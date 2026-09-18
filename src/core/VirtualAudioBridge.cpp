#include "VirtualAudioBridge.h"
#include "DeviceMappedDax.h"
#include "AppSettings.h"
#include "LogManager.h"

#ifdef HAVE_PIPEWIRE
#include "PipeWireAudioBridge.h"
#endif

#include <QTimer>

#ifdef Q_OS_MAC
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif
#include <cmath>
#include <algorithm>

namespace AetherSDR {

// ── Backend selection ───────────────────────────────────────────────────────

QString VirtualAudioBridge::backendName(Backend b)
{
    return b == Backend::DeviceMapped ? QStringLiteral("DeviceMapped")
                                      : QStringLiteral("Native");
}

VirtualAudioBridge::Backend VirtualAudioBridge::backendFromName(const QString& name,
                                                                Backend fallback)
{
    if (name.compare(QLatin1String("DeviceMapped"), Qt::CaseInsensitive) == 0) {
        return Backend::DeviceMapped;
    }
    if (name.compare(QLatin1String("Native"), Qt::CaseInsensitive) == 0) {
        return Backend::Native;
    }
    return fallback;
}

bool VirtualAudioBridge::nativeBackendAvailable()
{
#if defined(Q_OS_MAC) || defined(HAVE_PIPEWIRE)
    return true;
#else
    return false;   // Windows: no signed WDM driver (see docs/windows-dax.md)
#endif
}

VirtualAudioBridge::Backend VirtualAudioBridge::defaultBackend()
{
    return nativeBackendAvailable() ? Backend::Native : Backend::DeviceMapped;
}

VirtualAudioBridge::Backend VirtualAudioBridge::configuredBackend()
{
    const QString v = AppSettings::instance()
                          .value(backendSettingKey(), backendName(defaultBackend()))
                          .toString();
    const Backend b = backendFromName(v, defaultBackend());
    if (b == Backend::Native && !nativeBackendAvailable()) {
        return Backend::DeviceMapped;
    }
    return b;
}

VirtualAudioBridge::VirtualAudioBridge(QObject* parent)
    : VirtualAudioBridge(configuredBackend(), parent)
{}

VirtualAudioBridge::VirtualAudioBridge(Backend backend, QObject* parent)
    : QObject(parent)
    , m_backend(backend)
{
    if (m_backend == Backend::Native && !nativeBackendAvailable()) {
        m_backend = Backend::DeviceMapped;
    }
}

VirtualAudioBridge::~VirtualAudioBridge()
{
    close();
}

void VirtualAudioBridge::setGain(float g)
{
    m_gain = std::clamp(g, 0.0f, 1.0f);
    if (m_mapped) {
        m_mapped->setGain(m_gain);
    }
#ifdef HAVE_PIPEWIRE
    if (m_pipewire) {
        m_pipewire->setGain(m_gain);
    }
#endif
}

void VirtualAudioBridge::setChannelGain(int channel, float g)
{
    if (channel < 1 || channel > NUM_CHANNELS) {
        return;
    }
    m_channelGain[channel - 1] = std::clamp(g, 0.0f, 1.0f);
    if (m_mapped) {
        m_mapped->setChannelGain(channel, g);
    }
#ifdef HAVE_PIPEWIRE
    if (m_pipewire) {
        m_pipewire->setChannelGain(channel, g);
    }
#endif
}

void VirtualAudioBridge::setTxGain(float g)
{
    m_txGain = std::clamp(g, 0.0f, 1.0f);
    if (m_mapped) {
        m_mapped->setTxGain(m_txGain);
    }
#ifdef HAVE_PIPEWIRE
    if (m_pipewire) {
        m_pipewire->setTxGain(m_txGain);
    }
#endif
}

void VirtualAudioBridge::reloadDeviceMapping()
{
    if (m_mapped) {
        m_mapped->reloadDeviceMapping();
    }
}

bool VirtualAudioBridge::open()
{
    if (m_open) return true;
    m_lastError.clear();

    if (m_backend == Backend::DeviceMapped) {
        return openDelegate();
    }
#if defined(Q_OS_MAC)
    return openSharedMemory();
#elif defined(HAVE_PIPEWIRE)
    return openDelegate();
#else
    m_lastError = QStringLiteral("No native DAX driver on this platform.");
    return false;
#endif
}

bool VirtualAudioBridge::openDelegate()
{
    if (m_backend == Backend::DeviceMapped) {
        m_mapped = std::make_unique<DeviceMappedDax>();
        connect(m_mapped.get(), &DeviceMappedDax::txAudioReady,
                this, &VirtualAudioBridge::txAudioReady);
        connect(m_mapped.get(), &DeviceMappedDax::daxRxLevel,
                this, &VirtualAudioBridge::daxRxLevel);
        connect(m_mapped.get(), &DeviceMappedDax::daxTxLevel,
                this, &VirtualAudioBridge::daxTxLevel);
        for (int ch = 1; ch <= NUM_CHANNELS; ++ch) {
            m_mapped->setChannelGain(ch, m_channelGain[ch - 1]);
        }
        m_mapped->setTxGain(m_txGain);
        if (!m_mapped->open()) {
            m_lastError = m_mapped->lastError();
            m_mapped.reset();
            return false;
        }
        m_lastError = m_mapped->lastError();   // partial-open warnings, if any
        m_open = true;
        qCInfo(lcDax) << "VirtualAudioBridge: using device-mapped DAX backend";
        return true;
    }

#ifdef HAVE_PIPEWIRE
    // Linux Native: behaviour is exactly PipeWireAudioBridge's.
    m_pipewire = new PipeWireAudioBridge(this);
    connect(m_pipewire, &PipeWireAudioBridge::txAudioReady,
            this, &VirtualAudioBridge::txAudioReady);
    connect(m_pipewire, &PipeWireAudioBridge::daxRxLevel,
            this, &VirtualAudioBridge::daxRxLevel);
    connect(m_pipewire, &PipeWireAudioBridge::daxTxLevel,
            this, &VirtualAudioBridge::daxTxLevel);
    for (int ch = 1; ch <= NUM_CHANNELS; ++ch) {
        m_pipewire->setChannelGain(ch, m_channelGain[ch - 1]);
    }
    m_pipewire->setTxGain(m_txGain);
    if (!m_pipewire->open()) {
        m_lastError = QStringLiteral("Could not create the PulseAudio/PipeWire DAX devices "
                                     "(is pactl installed?).");
        delete m_pipewire;
        m_pipewire = nullptr;
        return false;
    }
    m_open = true;
    return true;
#else
    return false;
#endif
}

QString VirtualAudioBridge::shmName(int channel)
{
    return QStringLiteral("/aethersdr-dax-%1").arg(channel);
}

#ifdef Q_OS_MAC
static bool openShmSegment(const char* name, int& fd, DaxShmBlock*& block)
{
    // Try to open existing segment first (HAL plugin may already have it mapped).
    fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) {
        // Does not exist yet — create it.
        fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            qCWarning(lcDax) << "VirtualAudioBridge: shm_open failed for" << name;
            return false;
        }
        if (ftruncate(fd, sizeof(DaxShmBlock)) != 0) {
            qCWarning(lcDax) << "VirtualAudioBridge: ftruncate failed for" << name;
            ::close(fd);
            fd = -1;
            return false;
        }
    }
    // else: segment already exists — reuse it so the HAL plugin's existing
    // mmap stays valid.  If a previous run died between shm_open(O_CREAT)
    // and ftruncate() the object is still 0 bytes; mapping and writing it
    // would SIGBUS, so size it now.
    struct stat st{};
    if (fstat(fd, &st) != 0) {
        qCWarning(lcDax) << "VirtualAudioBridge: fstat failed for" << name;
        ::close(fd);
        fd = -1;
        return false;
    }
    if (st.st_size < static_cast<off_t>(sizeof(DaxShmBlock))
        && ftruncate(fd, sizeof(DaxShmBlock)) != 0) {
        qCWarning(lcDax) << "VirtualAudioBridge: shm segment" << name
                         << "is too small (" << st.st_size << "bytes) and cannot be resized";
        ::close(fd);
        fd = -1;
        return false;
    }

    void* ptr = mmap(nullptr, sizeof(DaxShmBlock), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        qCWarning(lcDax) << "VirtualAudioBridge: mmap failed for" << name;
        ::close(fd);
        fd = -1;
        return false;
    }

    // Re-initialize the header fields (resets write/read positions).
    auto* b = static_cast<DaxShmBlock*>(ptr);
    b->writePos.store(0, std::memory_order_relaxed);
    b->readPos.store(0, std::memory_order_relaxed);
    b->sampleRate = 24000;
    b->channels = 2;
    block = b;
    return true;
}

bool VirtualAudioBridge::openSharedMemory()
{
    // Open 4 RX shared memory segments
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        int ch = i + 1;
        QByteArray name = shmName(ch).toUtf8();

        if (!openShmSegment(name.constData(), m_shmFds[i], m_blocks[i])) {
            close();
            return false;
        }
        m_blocks[i]->active = 1;
    }

    // Open TX shared memory segment
    if (!openShmSegment("/aethersdr-dax-tx", m_txShmFd, m_txBlock)) {
        close();
        return false;
    }
    m_txBlock->active = 0;  // HAL plugin sets this to 1 when apps write

    // Poll TX shared memory for incoming audio.
    // Use a short interval and fixed-size reads so TX stays close to real-time.
    m_txPollTimer = new QTimer(this);
    m_txPollTimer->setInterval(2);
    connect(m_txPollTimer, &QTimer::timeout, this, [this]() {
        static int pollNum = 0;
        ++pollNum;

        // Diagnostic: log shm state every second regardless of data
        if (m_txBlock && pollNum % 500 == 0) {
            uint32_t wp = m_txBlock->writePos.load(std::memory_order_relaxed);
            uint32_t rp = m_txBlock->readPos.load(std::memory_order_relaxed);
            qCDebug(lcDax) << "TX shm poll#" << pollNum
                     << "wp=" << wp << "rp=" << rp
                     << "avail=" << (wp - rp) << "active=" << m_txBlock->active;
        }

        // Drain multiple small chunks each tick to avoid stale backlog.
        constexpr int FRAMES_PER_READ = 128;   // ~5.3ms @ 24kHz
        constexpr int MAX_CHUNKS_PER_TICK = 8; // keep UI responsive under load
        for (int i = 0; i < MAX_CHUNKS_PER_TICK; ++i) {
            QByteArray audio = readTxAudio(FRAMES_PER_READ);
            if (audio.isEmpty()) break;

            static int txPollCount = 0;
            ++txPollCount;
            if (txPollCount <= 10 || txPollCount % 200 == 0)
                qCDebug(lcDax) << "VirtualAudioBridge: TX audio from shm, bytes=" << audio.size()
                         << "(chunk #" << txPollCount << ")"
                         << "wp=" << m_txBlock->writePos.load(std::memory_order_relaxed)
                         << "active=" << m_txBlock->active;
            emit txAudioReady(audio);
        }
    });
    m_txPollTimer->start();

    m_open = true;
    qCInfo(lcDax) << "VirtualAudioBridge: opened 4 RX + 1 TX shared memory segments";
    return true;
}

#endif // Q_OS_MAC

void VirtualAudioBridge::close()
{
    if (m_mapped) {
        m_mapped->close();
        m_mapped.reset();
    }
#ifdef HAVE_PIPEWIRE
    if (m_pipewire) {
        m_pipewire->close();
        delete m_pipewire;
        m_pipewire = nullptr;
    }
#endif
    closeSharedMemory();
    m_open = false;
}

void VirtualAudioBridge::closeSharedMemory()
{
    if (m_silenceTimer) {
        m_silenceTimer->stop();
        delete m_silenceTimer;
        m_silenceTimer = nullptr;
    }
    m_transmitting = false;

    if (m_txPollTimer) {
        m_txPollTimer->stop();
        delete m_txPollTimer;
        m_txPollTimer = nullptr;
    }

#ifdef Q_OS_MAC
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        if (m_blocks[i]) {
            m_blocks[i]->active = 0;
            munmap(m_blocks[i], sizeof(DaxShmBlock));
            m_blocks[i] = nullptr;
        }
        if (m_shmFds[i] >= 0) {
            ::close(m_shmFds[i]);
            m_shmFds[i] = -1;
        }
    }

    if (m_txBlock) {
        munmap(m_txBlock, sizeof(DaxShmBlock));
        m_txBlock = nullptr;
    }
    if (m_txShmFd >= 0) {
        ::close(m_txShmFd);
        m_txShmFd = -1;
    }
#endif
}

void VirtualAudioBridge::setTransmitting(bool tx)
{
    if (m_mapped) {
        m_mapped->setTransmitting(tx);
        return;
    }
#ifdef HAVE_PIPEWIRE
    if (m_pipewire) {
        m_pipewire->setTransmitting(tx);
        return;
    }
#endif
    m_transmitting = tx;

    if (tx) {
        // Start a timer that feeds silence into all RX shared memory channels
        // at ~20 ms intervals (~480 stereo samples per tick @ 24 kHz).
        // This keeps the HAL plugin's ring buffer advancing so CoreAudio and
        // WSJT-X/VARA don't see a stalled audio source.
        if (!m_silenceTimer) {
            m_silenceTimer = new QTimer(this);
            m_silenceTimer->setInterval(20);
            connect(m_silenceTimer, &QTimer::timeout,
                    this, &VirtualAudioBridge::feedSilenceToAllChannels);
        }
        m_silenceElapsed.start();
        m_silenceFramesWritten = 0;
        m_silenceTimer->start();
    }
    // NOTE: we do NOT stop the silence timer on TX→RX here.
    // The radio hasn't resumed DAX RX audio yet at this point — the
    // interlock is still transitioning (UNKEY_REQUESTED → READY).
    // The timer is stopped in feedDaxAudio() when real audio arrives.
}

void VirtualAudioBridge::feedSilenceToAllChannels()
{
    if (!m_open) return;

    // Feed silence in whole stereo frames, tracking the total owed since TX
    // started so QTimer jitter causes no cumulative drift.  Writing an odd
    // sample count (the old per-tick elapsed*48000 truncation) shifted every
    // later frame by one sample, swapping L/R for the HAL plug-in readers.
    const qint64 framesDue = m_silenceElapsed.nsecsElapsed() * 24000LL / 1000000000LL;
    const qint64 frames = std::min<qint64>(framesDue - m_silenceFramesWritten,
                                           DaxShmBlock::RING_SIZE / 2);
    m_silenceFramesWritten = framesDue;
    if (frames <= 0) return;
    const int silenceSamples = static_cast<int>(frames) * 2;

    for (int i = 0; i < NUM_CHANNELS; ++i) {
        auto* block = m_blocks[i];
        if (!block || !block->active) continue;
        uint32_t wp = block->writePos.load(std::memory_order_relaxed);
        for (int s = 0; s < silenceSamples; ++s) {
            block->ringBuffer[wp % DaxShmBlock::RING_SIZE] = 0.0f;
            ++wp;
        }
        block->writePos.store(wp, std::memory_order_release);
    }
}

void VirtualAudioBridge::feedDaxAudio(int channel, const QByteArray& pcm)
{
    if (m_mapped) {
        m_mapped->feedDaxAudio(channel, pcm);
        return;
    }
#ifdef HAVE_PIPEWIRE
    if (m_pipewire) {
        m_pipewire->feedDaxAudio(channel, pcm);
        return;
    }
#endif
    if (channel < 1 || channel > NUM_CHANNELS) return;

    auto* block = m_blocks[channel - 1];
    if (!block) return;

    // Real DAX audio has arrived from the radio — stop the silence fill timer.
    // This bridges the gap between "we asked radio to stop TX" and "radio
    // actually resumed sending RX audio".
    if (m_silenceTimer && m_silenceTimer->isActive()) {
        m_silenceTimer->stop();
        m_transmitting = false;

        // Skip all buffered silence so the modem hears real audio immediately.
        // During TX, the silence timer accumulated samples in the ring buffer.
        // Without this reset, the HAL plugin would read through all that silence
        // before reaching real audio — causing a perceivable delay after TX.
        for (int i = 0; i < NUM_CHANNELS; ++i) {
            if (m_blocks[i] && m_blocks[i]->active) {
                uint32_t wp = m_blocks[i]->writePos.load(std::memory_order_relaxed);
                m_blocks[i]->readPos.store(wp, std::memory_order_release);
            }
        }
    }

    // Input: float32 stereo PCM @ 24 kHz from the radio (native DAX rate).
    //        After the float32 migration (502b934, b0c49d7), both PCC_IF_NARROW
    //        and PCC_IF_NARROW_REDUCED emit float32 stereo from PanadapterStream.
    // Output: float32 stereo @ 24 kHz into the ring buffer — direct 1:1 copy.
    // HAL plugin also runs at 24 kHz, so no resampling needed.
    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    // Whole stereo frames only: a stray odd sample would shift every later
    // frame in the ring by one and swap L/R for the HAL plug-in readers.
    const int numSamples = (pcm.size() / static_cast<int>(sizeof(float))) & ~1;  // L,R,L,R,...

    const float chGain = m_channelGain[channel - 1];
    uint32_t wp = block->writePos.load(std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i) {
        block->ringBuffer[wp % DaxShmBlock::RING_SIZE] = samples[i] * chGain;
        ++wp;
    }

    block->writePos.store(wp, std::memory_order_release);

    // RX level meter (every ~100ms)
    static int meterCount[NUM_CHANNELS]{};
    if (++meterCount[channel - 1] % 10 == 0) {
        float sum = 0;
        for (int i = 0; i < numSamples; i += 2)
            sum += samples[i] * samples[i];
        float rms = std::sqrt(sum / std::max(1, numSamples / 2));
        emit daxRxLevel(channel, rms);
    }
}

QByteArray VirtualAudioBridge::readTxAudio(int maxFrames)
{
    if (!m_txBlock || !m_txBlock->active) return {};

    uint32_t rp = m_txBlock->readPos.load(std::memory_order_acquire);
    uint32_t wp = m_txBlock->writePos.load(std::memory_order_acquire);

    uint32_t available = wp - rp;
    if (available == 0) return {};

    // If writer has lapped the reader, skip ahead to avoid stale data.
    if (available > DaxShmBlock::RING_SIZE) {
        rp = wp - DaxShmBlock::RING_SIZE / 2;  // jump to recent data
        available = wp - rp;
    }

    // Low-latency guard: if backlog grows, skip stale audio and keep only
    // a small recent window near the writer head.
    constexpr uint32_t TARGET_BACKLOG_SAMPLES = 256 * 2; // 256 frames ≈ 10.7ms
    constexpr uint32_t MAX_BACKLOG_SAMPLES = 768 * 2;    // 768 frames ≈ 32ms
    if (available > MAX_BACKLOG_SAMPLES) {
        rp = wp - TARGET_BACKLOG_SAMPLES;
        available = wp - rp;
    }

    uint32_t totalSamples = available;

    if (maxFrames > 0)
        totalSamples = std::min(totalSamples, static_cast<uint32_t>(maxFrames * 2));

    QByteArray result(static_cast<int>(totalSamples * sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(result.data());

    for (uint32_t i = 0; i < totalSamples; ++i) {
        dst[i] = m_txBlock->ringBuffer[rp % DaxShmBlock::RING_SIZE];
        ++rp;
    }

    m_txBlock->readPos.store(rp, std::memory_order_release);

    // Apply TX gain
    if (m_txGain != 1.0f) {
        for (uint32_t i = 0; i < totalSamples; ++i)
            dst[i] *= m_txGain;
    }

    // TX level meter
    static int txMeterCount = 0;
    if (++txMeterCount % 10 == 0 && totalSamples > 0) {
        float sum = 0;
        for (uint32_t i = 0; i < totalSamples; i += 2)
            sum += dst[i] * dst[i];
        emit daxTxLevel(std::sqrt(sum / std::max(1u, totalSamples / 2)));
    }

    return result;
}

} // namespace AetherSDR
