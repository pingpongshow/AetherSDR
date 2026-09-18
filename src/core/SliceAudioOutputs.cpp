#include "SliceAudioOutputs.h"

#include "LogManager.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QRegularExpression>

#include <algorithm>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr int kSinkBufferMs = 80;
constexpr int kMixerTargetMs = 60;
constexpr int kMaxPullFrames = 48000;

// Match by description; Windows may prefix duplicate endpoint names with
// "2- " and the number can change between boots (same rule as the
// device-mapped DAX backend).
QAudioDevice findOutput(const QString& name)
{
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    for (const QAudioDevice& d : outputs) {
        if (d.description() == name) {
            return d;
        }
    }
    static const QRegularExpression dupPrefix(QStringLiteral("^\\s*\\d+\\s*-\\s*"));
    auto bare = [](QString n) { return n.remove(dupPrefix).trimmed(); };
    const QString want = bare(name);
    for (const QAudioDevice& d : outputs) {
        if (bare(d.description()).compare(want, Qt::CaseInsensitive) == 0) {
            return d;
        }
    }
    return {};
}

QList<QAudioFormat> candidateFormats(const QAudioDevice& dev)
{
    QList<int> rates;
    const int pref = dev.preferredFormat().sampleRate();
    if (pref >= 16000) {
        rates << pref;
    }
    for (int r : {48000, 44100, 24000}) {
        if (!rates.contains(r)) {
            rates << r;
        }
    }
    const int ch = std::min(2, std::max(1, dev.maximumChannelCount()));
    QList<QAudioFormat> out;
    for (int rate : rates) {
        for (QAudioFormat::SampleFormat sf : {QAudioFormat::Float, QAudioFormat::Int16}) {
            QAudioFormat f;
            f.setSampleRate(rate);
            f.setChannelCount(ch);
            f.setSampleFormat(sf);
#ifdef Q_OS_WIN
            out << f;   // WASAPI shared mode misreports support; let start() decide
#else
            if (dev.isFormatSupported(f)) {
                out << f;
            }
#endif
        }
    }
    return out;
}

// Pull-mode source: QAudioSink reads, we mix.  readData() may run on a Qt
// audio thread; SliceMixer::pull is lock-free.
class MixerPullDevice : public QIODevice {
public:
    MixerPullDevice(SliceMixer& mixer, int channels, QAudioFormat::SampleFormat fmt)
        : m_mixer(mixer), m_channels(channels), m_format(fmt)
    {
        m_scratch.resize(static_cast<size_t>(4096) * static_cast<size_t>(channels));
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override
    {
        return QIODevice::bytesAvailable() + 4096LL * m_channels * bytesPerSample();
    }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        const int bpf = m_channels * bytesPerSample();
        const int frames = static_cast<int>(std::min<qint64>(maxlen / bpf, kMaxPullFrames));
        if (frames <= 0) {
            return 0;
        }
        const size_t need = static_cast<size_t>(frames) * static_cast<size_t>(m_channels);
        if (m_scratch.size() < need) {
            m_scratch.resize(need);
        }
        m_mixer.pull(m_scratch.data(), frames, m_channels);
        if (m_format == QAudioFormat::Float) {
            std::memcpy(data, m_scratch.data(), need * sizeof(float));
        } else {
            auto* dst = reinterpret_cast<qint16*>(data);
            for (size_t i = 0; i < need; ++i) {
                const float v = std::clamp(m_scratch[i], -1.0f, 1.0f);
                dst[i] = static_cast<qint16>(v * 32767.0f);
            }
        }
        return static_cast<qint64>(frames) * bpf;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    int bytesPerSample() const { return m_format == QAudioFormat::Float ? 4 : 2; }

    SliceMixer& m_mixer;
    int m_channels;
    QAudioFormat::SampleFormat m_format;
    std::vector<float> m_scratch;
};

} // namespace

struct SliceAudioOutputs::DeviceOut {
    QString name;
    QVector<int> sliceIds;
    std::unique_ptr<SliceMixer> mixer;
    std::unique_ptr<MixerPullDevice> io;
    std::unique_ptr<QAudioSink> sink;

    ~DeviceOut()
    {
        if (sink) {
            sink->stop();   // stop before io/mixer go away
        }
        sink.reset();
        io.reset();
        mixer.reset();
    }
};

SliceAudioOutputs::SliceAudioOutputs()
{
    m_routeForDax.fill(-1);
}

SliceAudioOutputs::~SliceAudioOutputs() = default;

std::unique_ptr<SliceAudioOutputs::DeviceOut>
SliceAudioOutputs::openDevice(const QString& name, const QVector<int>& sliceIds)
{
    const QAudioDevice dev = findOutput(name);
    if (dev.isNull()) {
        qCWarning(lcAudio) << "SliceAudioOutputs: output device not found:" << name;
        return nullptr;
    }
    for (const QAudioFormat& fmt : candidateFormats(dev)) {
        auto out = std::make_unique<DeviceOut>();
        out->name = name;
        out->sliceIds = sliceIds;
        out->mixer = std::make_unique<SliceMixer>(sliceIds, fmt.sampleRate(), kMixerTargetMs);
        out->io = std::make_unique<MixerPullDevice>(*out->mixer, fmt.channelCount(),
                                                    fmt.sampleFormat());
        out->io->open(QIODevice::ReadOnly);
        out->sink = std::make_unique<QAudioSink>(dev, fmt);
        out->sink->setBufferSize(static_cast<qsizetype>(fmt.bytesPerFrame()) * fmt.sampleRate()
                                 * kSinkBufferMs / 1000);
        out->sink->start(out->io.get());
        if (out->sink->error() != QAudio::NoError
            || out->sink->state() == QAudio::StoppedState) {
            qCDebug(lcAudio) << "SliceAudioOutputs:" << name << "rejected" << fmt;
            continue;
        }
        qCInfo(lcAudio) << "SliceAudioOutputs: opened" << name << fmt << "for slices" << sliceIds;
        return out;
    }
    qCWarning(lcAudio) << "SliceAudioOutputs: no usable format on" << name;
    return nullptr;
}

void SliceAudioOutputs::setRoutes(const QVector<SliceRouteRuntime>& routes)
{
    m_routes = routes;
    m_routeForDax.fill(-1);
    // Slices per device, in route order.
    QMap<QString, QVector<int>> wanted;
    for (int i = 0; i < m_routes.size(); ++i) {
        const SliceRouteRuntime& r = m_routes[i];
        if (r.sourceDaxChannel >= 1 && r.sourceDaxChannel <= 4) {
            m_routeForDax[static_cast<size_t>(r.sourceDaxChannel)] = i;
        }
        if (!r.outputDevice.isEmpty()) {
            wanted[r.outputDevice].append(r.sliceId);
        }
    }

    // Keep devices whose slice set is unchanged; drop the rest.
    std::vector<std::unique_ptr<DeviceOut>> kept;
    for (std::unique_ptr<DeviceOut>& d : m_devices) {
        if (d && d->sink && d->sink->state() != QAudio::StoppedState
            && wanted.value(d->name) == d->sliceIds) {
            wanted.remove(d->name);
            kept.push_back(std::move(d));
        }
    }
    m_devices = std::move(kept);   // destroys (stops) the rest
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        if (std::unique_ptr<DeviceOut> d = openDevice(it.key(), it.value())) {
            m_devices.push_back(std::move(d));
        }
    }
    for (const SliceRouteRuntime& r : m_routes) {
        setGain(r.sliceId, r.gain);
    }
}

void SliceAudioOutputs::setGain(int sliceId, StereoGain gain)
{
    for (SliceRouteRuntime& r : m_routes) {
        if (r.sliceId == sliceId) {
            r.gain = gain;
        }
    }
    for (const std::unique_ptr<DeviceOut>& d : m_devices) {
        d->mixer->setGain(sliceId, gain);
    }
}

int SliceAudioOutputs::feed(int daxChannel, const QByteArray& pcm)
{
    if (m_routes.isEmpty() || daxChannel < 1 || daxChannel > 4) {
        return 0;
    }
    const int idx = m_routeForDax[static_cast<size_t>(daxChannel)];
    if (idx < 0) {
        return 0;
    }
    const SliceRouteRuntime& r = m_routes[idx];
    if (!r.outputDevice.isEmpty()) {
        const auto* f = reinterpret_cast<const float*>(pcm.constData());
        const int frames = static_cast<int>(pcm.size() / (2 * static_cast<qsizetype>(sizeof(float))));
        for (const std::unique_ptr<DeviceOut>& d : m_devices) {
            if (d->name == r.outputDevice) {
                d->mixer->push(r.sliceId, f, frames);
            }
        }
    }
    return r.bridgeChannel;
}

QStringList SliceAudioOutputs::openDevices() const
{
    QStringList out;
    for (const std::unique_ptr<DeviceOut>& d : m_devices) {
        out.append(d->name);
    }
    return out;
}

} // namespace AetherSDR
