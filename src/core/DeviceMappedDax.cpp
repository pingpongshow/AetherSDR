#include "DeviceMappedDax.h"
#include "DaxDevicePipeline.h"
#include "AppSettings.h"
#include "LogManager.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

namespace AetherSDR {

namespace {

// Playout target per RX device.  40 ms absorbs VITA-49 packet jitter and
// the device's pull period while keeping WSJT-X DT well inside ±0.1 s.
constexpr int kRxTargetLatencyMs = 40;

// Qt device buffer sizes.  WASAPI shared mode runs a 10 ms engine period;
// three periods avoid glitches on busy machines.  CoreAudio / PulseAudio
// are happy with less.
#ifdef Q_OS_WIN
constexpr int kSinkBufferMs = 30;
constexpr int kSourceBufferMs = 30;
#else
constexpr int kSinkBufferMs = 20;
constexpr int kSourceBufferMs = 20;
#endif

// TX low-latency guard — same thresholds as the macOS shared-memory TX path
// (VirtualAudioBridge::readTxAudio): if more than ~32 ms is queued, skip to
// the newest ~10.7 ms so TX never lags the modem's PTT.
constexpr int kTxMaxBacklogMs = 32;
constexpr int kTxKeepBacklogUs = 10700;
// Emit TX in 128-frame (5.3 ms @ 24 kHz) chunks, matching the shm poller.
constexpr int kTxChunkFrames = 128;

DaxSampleFormat toDaxFormat(QAudioFormat::SampleFormat f)
{
    switch (f) {
    case QAudioFormat::UInt8: return DaxSampleFormat::UInt8;
    case QAudioFormat::Int16: return DaxSampleFormat::Int16;
    case QAudioFormat::Int32: return DaxSampleFormat::Int32;
    default:                  return DaxSampleFormat::Float;
    }
}

QAudioDevice findDevice(const QList<QAudioDevice>& devices, const QString& name)
{
    if (name.isEmpty()) {
        return {};
    }
    for (const QAudioDevice& d : devices) {
        if (d.description() == name) {
            return d;
        }
    }
    // Windows prefixes duplicate endpoint names ("2- CABLE Input (...)") and
    // the number can change between boots; match without it.
    static const QRegularExpression dupPrefix(QStringLiteral("^\\s*\\d+\\s*-\\s*"));
    auto bare = [](QString n) { return n.remove(dupPrefix).trimmed(); };
    const QString want = bare(name);
    for (const QAudioDevice& d : devices) {
        if (bare(d.description()).compare(want, Qt::CaseInsensitive) == 0) {
            return d;
        }
    }
    return {};
}

bool devicePresent(const QList<QAudioDevice>& devices, const QString& name)
{
    return !findDevice(devices, name).isNull();
}

// Formats to try, best first: the device's own rate (no OS resampling),
// then 48 kHz; float before integer; stereo when the device has it.
QList<QAudioFormat> candidateFormats(const QAudioDevice& dev)
{
    const QAudioFormat pref = dev.preferredFormat();
    QList<int> rates;
    if (pref.sampleRate() >= DeviceMappedDax::kDaxSampleRate) {
        rates << pref.sampleRate();
    }
    for (int r : {48000, 44100, DeviceMappedDax::kDaxSampleRate}) {
        if (!rates.contains(r)) {
            rates << r;
        }
    }
    QList<QAudioFormat::SampleFormat> formats{QAudioFormat::Float, QAudioFormat::Int16,
                                              QAudioFormat::Int32};
    const int maxCh = std::max(1, dev.maximumChannelCount());
    QList<int> channels{std::min(2, maxCh)};
    if (channels.first() == 2 && dev.minimumChannelCount() <= 1) {
        channels << 1;
    }

    QList<QAudioFormat> out;
    for (int ch : channels) {
        for (int rate : rates) {
            for (QAudioFormat::SampleFormat sf : formats) {
                QAudioFormat f;
                f.setSampleRate(rate);
                f.setChannelCount(ch);
                f.setSampleFormat(sf);
#ifdef Q_OS_WIN
                // Qt's isFormatSupported() is unreliable for WASAPI shared
                // mode (VB-Cable, Voicemeeter, FlexRadio DAX all report false
                // for formats that open fine — see AudioEngine).  Try them all
                // and let start() decide.
                out << f;
#else
                if (dev.isFormatSupported(f)) {
                    out << f;
                }
#endif
            }
        }
    }
    return out;
}

qsizetype bufferBytes(const QAudioFormat& f, int ms)
{
    return static_cast<qsizetype>(f.bytesPerFrame()) * f.sampleRate() * ms / 1000;
}

float rmsOfChannel0(const float* interleaved, int frames, int channels)
{
    if (frames <= 0) {
        return 0.0f;
    }
    double sum = 0.0;
    for (int i = 0; i < frames; ++i) {
        const float v = interleaved[static_cast<size_t>(i) * channels];
        sum += static_cast<double>(v) * v;
    }
    return static_cast<float>(std::sqrt(sum / frames));
}

// Pull-mode source for one QAudioSink.  readData() may run on a Qt audio
// thread; it only touches the SPSC consumer side of the playout buffer and
// its own scratch, never blocks, and always returns the full request
// (padding with silence) so the sink never idles.  The whole request goes to
// the playout buffer in one pull so it can size its target to the device
// period (Qt 6.9 CoreAudio pull mode asks for 2048 frames at a time).  The
// scratch grows only when a larger request than any before arrives — in
// practice once, on the first read.
class DaxPullDevice : public QIODevice {
public:
    DaxPullDevice(DaxPlayoutBuffer& buffer, DaxSampleFormat fmt)
        : m_buffer(buffer)
        , m_format(fmt)
        , m_channels(buffer.channels())
        , m_scratch(static_cast<size_t>(kChunkFrames) * buffer.channels())
    {}

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return QIODevice::bytesAvailable()
             + static_cast<qint64>(kChunkFrames) * m_channels * daxBytesPerSample(m_format);
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        const int bpf = m_channels * daxBytesPerSample(m_format);
        const int frames = static_cast<int>(std::min<qint64>(maxlen / bpf, kMaxPullFrames));
        if (frames <= 0) {
            return 0;
        }
        const size_t need = static_cast<size_t>(frames) * m_channels;
        if (m_scratch.size() < need) {
            m_scratch.resize(need);
        }
        m_buffer.pull(m_scratch.data(), frames);
        daxConvertFromFloat(m_scratch.data(), data, frames * m_channels, m_format);
        return static_cast<qint64>(frames) * bpf;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    static constexpr int kChunkFrames = 4096;       // initial scratch
    static constexpr int kMaxPullFrames = 48000;    // cap one pull at ~1 s
    DaxPlayoutBuffer& m_buffer;
    DaxSampleFormat m_format;
    int m_channels;
    std::vector<float> m_scratch;
};

} // namespace

// ── Channel state ───────────────────────────────────────────────────────────

struct DeviceMappedDax::RxChannel {
    QString deviceName;
    QAudioFormat format;
    DaxSampleFormat sampleFormat{DaxSampleFormat::Float};
    bool failed{false};
    std::unique_ptr<DaxPlayoutBuffer> buffer;
    std::unique_ptr<DaxStreamResampler> resampler;
    std::unique_ptr<DaxPullDevice> io;
    std::unique_ptr<QAudioSink> sink;
    std::vector<float> gained;
    std::vector<float> remapped;
    std::vector<float> resampled;

    ~RxChannel()
    {
        // Stop the sink before its pull device / buffer go away.
        if (sink) {
            sink->disconnect();
            sink->stop();
        }
        sink.reset();
        io.reset();
    }
};

struct DeviceMappedDax::TxChannel {
    QString deviceName;
    QAudioFormat format;
    DaxSampleFormat sampleFormat{DaxSampleFormat::Float};
    bool failed{false};
    std::unique_ptr<QAudioSource> source;
    QIODevice* io{nullptr};   // owned by source
    std::unique_ptr<DaxStreamResampler> resampler;
    QByteArray pending;
    std::vector<float> deviceFloat;
    std::vector<float> stereo;
    std::vector<float> resampled;
    uint64_t skippedFrames{0};

    ~TxChannel()
    {
        if (source) {
            source->disconnect();
            source->stop();
        }
        io = nullptr;
        source.reset();
    }
};

// ── DeviceMappedDax ─────────────────────────────────────────────────────────

DeviceMappedDax::DeviceMappedDax(QObject* parent)
    : QObject(parent)
{
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    connect(m_reloadTimer, &QTimer::timeout, this, &DeviceMappedDax::reloadDeviceMapping);
}

DeviceMappedDax::~DeviceMappedDax()
{
    close();
}

QString DeviceMappedDax::rxDeviceSettingKey(int channel)
{
    return QStringLiteral("DaxRxDevice%1").arg(channel);
}

QString DeviceMappedDax::txDeviceSettingKey()
{
    return QStringLiteral("DaxTxDevice");
}

bool DeviceMappedDax::hasAnyMapping()
{
    const auto& s = AppSettings::instance();
    for (int ch = 1; ch <= NUM_CHANNELS; ++ch) {
        if (!s.value(rxDeviceSettingKey(ch), QString()).toString().isEmpty()) {
            return true;
        }
    }
    return !s.value(txDeviceSettingKey(), QString()).toString().isEmpty();
}

bool DeviceMappedDax::open()
{
    if (m_open) {
        return true;
    }
    m_lastError.clear();

    const auto& s = AppSettings::instance();
    QStringList problems;
    int mapped = 0;
    int opened = 0;
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        const QString name = s.value(rxDeviceSettingKey(i + 1), QString()).toString();
        if (name.isEmpty()) {
            continue;
        }
        ++mapped;
        if (openRx(i, name)) {
            ++opened;
        } else {
            problems << QStringLiteral("DAX RX %1: cannot open \"%2\"").arg(i + 1).arg(name);
        }
    }
    const QString txName = s.value(txDeviceSettingKey(), QString()).toString();
    if (!txName.isEmpty()) {
        ++mapped;
        if (openTx(txName)) {
            ++opened;
        } else {
            problems << QStringLiteral("DAX TX: cannot open \"%1\"").arg(txName);
        }
    }

    if (mapped == 0) {
        m_lastError = QStringLiteral("No DAX audio devices are mapped. Choose a virtual "
                                     "cable for each DAX channel in DAX Audio Devices.");
        qCWarning(lcDax) << "DeviceMappedDax:" << m_lastError;
        return false;
    }
    m_lastError = problems.join(QLatin1Char('\n'));
    if (opened == 0) {
        qCWarning(lcDax) << "DeviceMappedDax: no mapped device could be opened:" << m_lastError;
        close();
        return false;
    }
    if (!problems.isEmpty()) {
        qCWarning(lcDax) << "DeviceMappedDax: partially opened:" << m_lastError;
    }

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    // Hot-plug: reopen channels whose device (dis)appears.  Not on Linux —
    // see AudioEngine: PipeWire can crash in pw_stream_connect when
    // audioOutputsChanged fires during enumeration.
    if (!m_mediaDevices) {
        m_mediaDevices = new QMediaDevices(this);
        connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
                this, &DeviceMappedDax::onDevicesChanged);
        connect(m_mediaDevices, &QMediaDevices::audioInputsChanged,
                this, &DeviceMappedDax::onDevicesChanged);
    }
#endif

    m_open = true;
    qCInfo(lcDax) << "DeviceMappedDax: opened" << opened << "of" << mapped << "mapped devices";
    for (const QString& line : statusLines()) {
        qCInfo(lcDax).noquote() << "  " << line;
    }
    return true;
}

void DeviceMappedDax::close()
{
    m_reloadTimer->stop();
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        closeRx(i);
    }
    closeTx();
    m_transmitting = false;
    m_open = false;
}

bool DeviceMappedDax::openRx(int index, const QString& deviceName)
{
    closeRx(index);
    const QAudioDevice dev = findDevice(QMediaDevices::audioOutputs(), deviceName);
    if (dev.isNull()) {
        qCWarning(lcDax) << "DeviceMappedDax: RX" << (index + 1)
                         << "output device not found:" << deviceName;
        return false;
    }

    for (const QAudioFormat& fmt : candidateFormats(dev)) {
        auto rx = std::make_unique<RxChannel>();
        rx->deviceName = dev.description();
        rx->format = fmt;
        rx->sampleFormat = toDaxFormat(fmt.sampleFormat());
        rx->buffer = std::make_unique<DaxPlayoutBuffer>(
            fmt.channelCount(),
            DaxPlayoutBuffer::defaultConfig(fmt.sampleRate(), kRxTargetLatencyMs));
        rx->resampler = std::make_unique<DaxStreamResampler>(
            fmt.channelCount(), kDaxSampleRate, fmt.sampleRate());
        rx->io = std::make_unique<DaxPullDevice>(*rx->buffer, rx->sampleFormat);
        rx->io->open(QIODevice::ReadOnly);
        rx->sink = std::make_unique<QAudioSink>(dev, fmt);
        rx->sink->setBufferSize(bufferBytes(fmt, kSinkBufferMs));
        rx->sink->start(rx->io.get());
        if (rx->sink->error() != QAudio::NoError || rx->sink->state() == QAudio::StoppedState) {
            qCDebug(lcDax) << "DeviceMappedDax: RX" << (index + 1) << deviceName
                           << "rejected" << fmt << "error" << rx->sink->error();
            continue;
        }
        connect(rx->sink.get(), &QAudioSink::stateChanged, this,
                [this, index](QAudio::State st) {
            RxChannel* ch = m_rx[static_cast<size_t>(index)].get();
            if (!ch || !ch->sink || st != QAudio::StoppedState
                || ch->sink->error() == QAudio::NoError) {
                return;
            }
            // WASAPI session loss / device removal / sleep: reopen later.
            qCWarning(lcDax) << "DeviceMappedDax: RX" << (index + 1) << ch->deviceName
                             << "stopped with error" << ch->sink->error() << "— will reopen";
            ch->failed = true;
            scheduleReload(1500);
        });
        m_rx[static_cast<size_t>(index)] = std::move(rx);
        return true;
    }
    qCWarning(lcDax) << "DeviceMappedDax: RX" << (index + 1)
                     << "no usable format on" << deviceName;
    return false;
}

bool DeviceMappedDax::openTx(const QString& deviceName)
{
    closeTx();
    const QAudioDevice dev = findDevice(QMediaDevices::audioInputs(), deviceName);
    if (dev.isNull()) {
        qCWarning(lcDax) << "DeviceMappedDax: TX input device not found:" << deviceName;
        return false;
    }
    // Safety: never capture TX from the far end of a cable we play RX into —
    // that would put received audio straight back on the air.
    // Check both the open RX devices and the configured ones (an RX device
    // that is unplugged now may come back later via hot-plug).
    QStringList rxNames;
    for (const auto& rx : m_rx) {
        if (rx) {
            rxNames << rx->deviceName;
        }
    }
    for (int ch = 1; ch <= NUM_CHANNELS; ++ch) {
        rxNames << AppSettings::instance().value(rxDeviceSettingKey(ch), QString()).toString();
    }
    for (const QString& rxName : std::as_const(rxNames)) {
        if (daxDevicesShareCable(rxName, dev.description())) {
            qCWarning(lcDax) << "DeviceMappedDax: refusing DAX TX from" << dev.description()
                             << "— same cable as DAX RX device" << rxName;
            return false;
        }
    }

    for (const QAudioFormat& fmt : candidateFormats(dev)) {
        auto tx = std::make_unique<TxChannel>();
        tx->deviceName = dev.description();
        tx->format = fmt;
        tx->sampleFormat = toDaxFormat(fmt.sampleFormat());
        tx->resampler = std::make_unique<DaxStreamResampler>(2, fmt.sampleRate(), kDaxSampleRate);
        tx->source = std::make_unique<QAudioSource>(dev, fmt);
        tx->source->setBufferSize(bufferBytes(fmt, kSourceBufferMs));
        tx->io = tx->source->start();
        if (!tx->io || tx->source->error() != QAudio::NoError
            || tx->source->state() == QAudio::StoppedState) {
            qCDebug(lcDax) << "DeviceMappedDax: TX" << deviceName
                           << "rejected" << fmt << "error" << tx->source->error();
            continue;
        }
        connect(tx->io, &QIODevice::readyRead, this, &DeviceMappedDax::readTx);
        connect(tx->source.get(), &QAudioSource::stateChanged, this, [this](QAudio::State st) {
            if (!m_tx || !m_tx->source || st != QAudio::StoppedState
                || m_tx->source->error() == QAudio::NoError) {
                return;
            }
            qCWarning(lcDax) << "DeviceMappedDax: TX" << m_tx->deviceName
                             << "stopped with error" << m_tx->source->error() << "— will reopen";
            m_tx->failed = true;
            scheduleReload(1500);
        });
        m_tx = std::move(tx);
        return true;
    }
    qCWarning(lcDax) << "DeviceMappedDax: TX no usable format on" << deviceName;
    return false;
}

void DeviceMappedDax::closeRx(int index)
{
    m_rx[static_cast<size_t>(index)].reset();
}

void DeviceMappedDax::closeTx()
{
    m_tx.reset();
}

void DeviceMappedDax::setGain(float g)
{
    m_gain = std::clamp(g, 0.0f, 1.0f);
}

void DeviceMappedDax::setChannelGain(int channel, float g)
{
    if (channel >= 1 && channel <= NUM_CHANNELS) {
        m_channelGain[static_cast<size_t>(channel - 1)] = std::clamp(g, 0.0f, 1.0f);
    }
}

void DeviceMappedDax::setTxGain(float g)
{
    m_txGain = std::clamp(g, 0.0f, 1.0f);
}

QString DeviceMappedDax::rxDeviceName(int channel) const
{
    if (channel < 1 || channel > NUM_CHANNELS) {
        return {};
    }
    const auto& rx = m_rx[static_cast<size_t>(channel - 1)];
    return rx ? rx->deviceName : QString();
}

QString DeviceMappedDax::txDeviceName() const
{
    return m_tx ? m_tx->deviceName : QString();
}

QStringList DeviceMappedDax::statusLines() const
{
    QStringList lines;
    auto fmtText = [](const QAudioFormat& f) {
        static const char* names[] = {"?", "u8", "s16", "s32", "f32"};
        const int idx = std::clamp(static_cast<int>(f.sampleFormat()), 0, 4);
        return QStringLiteral("%1 Hz %2ch %3").arg(f.sampleRate()).arg(f.channelCount())
                                             .arg(QLatin1String(names[idx]));
    };
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        const auto& rx = m_rx[static_cast<size_t>(i)];
        if (!rx) {
            continue;
        }
        lines << QStringLiteral("DAX RX %1 → %2 [%3] buffered %4 ms, underruns %5, "
                                "drift +%6/-%7, resyncs %8")
                     .arg(i + 1).arg(rx->deviceName, fmtText(rx->format))
                     .arg(rx->buffer->bufferedFrames() * 1000 / std::max(1, rx->format.sampleRate()))
                     .arg(rx->buffer->underruns())
                     .arg(rx->buffer->inserts()).arg(rx->buffer->drops())
                     .arg(rx->buffer->resyncs());
    }
    if (m_tx) {
        lines << QStringLiteral("DAX TX ← %1 [%2] skipped %3 frames")
                     .arg(m_tx->deviceName, fmtText(m_tx->format))
                     .arg(m_tx->skippedFrames);
    }
    return lines;
}

void DeviceMappedDax::feedDaxAudio(int channel, const QByteArray& pcm)
{
    if (!m_open || channel < 1 || channel > NUM_CHANNELS) {
        return;
    }
    RxChannel* rx = m_rx[static_cast<size_t>(channel - 1)].get();
    if (!rx || rx->failed) {
        return;
    }

    // Input: float32 stereo @ 24 kHz (PanadapterStream DAX, fw v1.4.0.0+).
    // Whole stereo frames only, like the shm bridge.
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    const int numSamples = (pcm.size() / static_cast<int>(sizeof(float))) & ~1;
    const int frames = numSamples / 2;
    if (frames <= 0) {
        return;
    }

    // RX level meter (~every 10 packets), pre-gain left channel like the shm bridge.
    int& meter = m_rxMeterCount[static_cast<size_t>(channel - 1)];
    if (++meter % 10 == 0) {
        emit daxRxLevel(channel, rmsOfChannel0(samples, frames, 2));
    }

    const float g = m_channelGain[static_cast<size_t>(channel - 1)];
    rx->gained.resize(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i) {
        rx->gained[static_cast<size_t>(i)] = samples[i] * g;
    }

    const int devCh = rx->format.channelCount();
    const float* stage = rx->gained.data();
    if (devCh != 2) {
        rx->remapped.resize(static_cast<size_t>(frames) * devCh);
        daxRemapChannels(stage, 2, rx->remapped.data(), devCh, frames);
        stage = rx->remapped.data();
    }

    rx->resampled.clear();
    const int outFrames = rx->resampler->process(stage, frames, rx->resampled);
    rx->buffer->push(rx->resampled.data(), outFrames);
}

void DeviceMappedDax::setTransmitting(bool tx)
{
    m_transmitting = tx;
}

void DeviceMappedDax::readTx()
{
    TxChannel* tx = m_tx.get();
    if (!tx || !tx->io) {
        return;
    }
    tx->pending.append(tx->io->readAll());

    const int devCh = tx->format.channelCount();
    const int bpf = devCh * daxBytesPerSample(tx->sampleFormat);
    int frames = static_cast<int>(tx->pending.size() / bpf);
    if (frames <= 0) {
        return;
    }

    const int rate = tx->format.sampleRate();
    const int maxBacklog = std::max(rate * kTxMaxBacklogMs / 1000,
                                    2 * rate * kSourceBufferMs / 1000);
    int skip = 0;
    if (frames > maxBacklog) {
        const int keep = static_cast<int>(static_cast<qint64>(rate) * kTxKeepBacklogUs / 1000000);
        skip = frames - keep;
        tx->skippedFrames += static_cast<uint64_t>(skip);
        frames = keep;
    }

    const char* raw = tx->pending.constData() + static_cast<qsizetype>(skip) * bpf;
    tx->deviceFloat.resize(static_cast<size_t>(frames) * devCh);
    daxConvertToFloat(raw, tx->deviceFloat.data(), frames * devCh, tx->sampleFormat);
    tx->pending.remove(0, static_cast<qsizetype>(skip + frames) * bpf);

    // Keep stereo as captured (HAL TX path passes stereo through too); mono
    // devices are duplicated to L/R.
    tx->stereo.resize(static_cast<size_t>(frames) * 2);
    daxRemapChannels(tx->deviceFloat.data(), devCh, tx->stereo.data(), 2, frames);

    tx->resampled.clear();
    const int outFrames = tx->resampler->process(tx->stereo.data(), frames, tx->resampled);
    if (outFrames <= 0) {
        return;
    }
    if (m_txGain != 1.0f) {
        for (float& v : tx->resampled) {
            v *= m_txGain;
        }
    }

    for (int off = 0; off < outFrames; off += kTxChunkFrames) {
        const int n = std::min(kTxChunkFrames, outFrames - off);
        const float* p = tx->resampled.data() + static_cast<size_t>(off) * 2;
        QByteArray chunk(reinterpret_cast<const char*>(p),
                         static_cast<qsizetype>(n) * 2 * static_cast<qsizetype>(sizeof(float)));
        if (++m_txMeterCount % 10 == 0) {
            emit daxTxLevel(rmsOfChannel0(p, n, 2));
        }
        emit txAudioReady(chunk);
        if (!m_tx) {
            return;  // a slot closed us
        }
    }
}

void DeviceMappedDax::scheduleReload(int delayMs)
{
    m_reloadTimer->start(delayMs);
}

void DeviceMappedDax::onDevicesChanged()
{
    if (m_open) {
        scheduleReload(750);
    }
}

void DeviceMappedDax::reloadDeviceMapping()
{
    if (!m_open) {
        return;
    }
    const auto& s = AppSettings::instance();
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();

    for (int i = 0; i < NUM_CHANNELS; ++i) {
        const QString want = s.value(rxDeviceSettingKey(i + 1), QString()).toString();
        const auto& rx = m_rx[static_cast<size_t>(i)];
        const bool healthy = rx && !rx->failed && devicePresent(outputs, rx->deviceName);
        if (want.isEmpty()) {
            closeRx(i);
        } else if (!healthy || findDevice(outputs, want).description() != rx->deviceName) {
            if (devicePresent(outputs, want)) {
                qCInfo(lcDax) << "DeviceMappedDax: (re)opening RX" << (i + 1) << want;
                openRx(i, want);
            } else {
                closeRx(i);
            }
        }
    }

    const QString wantTx = s.value(txDeviceSettingKey(), QString()).toString();
    const bool txHealthy = m_tx && !m_tx->failed && devicePresent(inputs, m_tx->deviceName);
    if (wantTx.isEmpty()) {
        closeTx();
    } else if (!txHealthy || findDevice(inputs, wantTx).description() != m_tx->deviceName) {
        if (devicePresent(inputs, wantTx)) {
            qCInfo(lcDax) << "DeviceMappedDax: (re)opening TX" << wantTx;
            openTx(wantTx);
        } else {
            closeTx();
        }
    }
}

} // namespace AetherSDR
