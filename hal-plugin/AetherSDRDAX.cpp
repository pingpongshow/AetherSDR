// AetherSDR DAX — Core Audio HAL Audio Server Plug-In
//
// Creates 4 virtual audio output devices ("AetherSDR DAX 1" through "AetherSDR DAX 4")
// for receiving DAX audio from the radio, plus 1 virtual input device ("AetherSDR TX")
// for sending TX audio to the radio.
//
// Each device reads/writes PCM audio via a POSIX shared memory ring buffer shared
// with AetherSDR's VirtualAudioBridge.
//
// Format: stereo float32, 24 kHz (matches FlexRadio DAX native rate).

#include <aspl/Driver.hpp>
#include <aspl/DriverRequestHandler.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Device.hpp>
#include <aspl/Stream.hpp>
#include <aspl/Context.hpp>
#include <aspl/Tracer.hpp>
#include <aspl/Client.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdint>

// ── Shared memory layout — must match VirtualAudioBridge.h ──────────────────

struct DaxShmBlock {
    std::atomic<uint32_t> writePos;
    std::atomic<uint32_t> readPos;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t active;
    uint32_t reserved[3];
    static constexpr uint32_t RING_SIZE = 24000 * 2 * 2;  // ~2 sec @ 24kHz stereo
    float ringBuffer[RING_SIZE];
};

// Maps an AetherSDR-created segment.  Returns nullptr unless the object is
// already sized: AetherSDR creates it with shm_open(O_CREAT) and sizes it
// with a separate ftruncate(), and touching a mapping past the end of a
// not-yet-sized object raises SIGBUS inside coreaudiod.
static DaxShmBlock* mapDaxShm(const char* name)
{
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) return nullptr;

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(DaxShmBlock))) {
        ::close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, sizeof(DaxShmBlock), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    ::close(fd);
    return ptr == MAP_FAILED ? nullptr : static_cast<DaxShmBlock*>(ptr);
}

// IO callbacks for different clients can run concurrently, so the lazily
// created mapping is published with a CAS (the loser unmaps its copy) and
// the retry throttle is an atomic timestamp.
class DaxShmMapping {
public:
    ~DaxShmMapping()
    {
        if (DaxShmBlock* b = m_block.exchange(nullptr)) {
            munmap(b, sizeof(DaxShmBlock));
        }
    }

    DaxShmBlock* get(const char* name)
    {
        if (DaxShmBlock* b = m_block.load(std::memory_order_acquire)) {
            return b;
        }
        // Retry periodically — AetherSDR may not have created the segment yet.
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = m_lastRetryMs.load(std::memory_order_relaxed);
        if (now - last < 1000
            || !m_lastRetryMs.compare_exchange_strong(last, now)) {
            return nullptr;
        }
        DaxShmBlock* mapped = mapDaxShm(name);
        if (!mapped) return nullptr;
        DaxShmBlock* expected = nullptr;
        if (!m_block.compare_exchange_strong(expected, mapped)) {
            munmap(mapped, sizeof(DaxShmBlock));
            return expected;
        }
        return mapped;
    }

private:
    std::atomic<DaxShmBlock*> m_block{nullptr};
    std::atomic<int64_t> m_lastRetryMs{INT64_MIN / 2};
};

// ── DAX RX Handler: reads from shared memory → output to apps ───────────────

class DaxRxHandler : public aspl::IORequestHandler {
public:
    explicit DaxRxHandler(int channel)
    {
        snprintf(m_shmName, sizeof(m_shmName), "/aethersdr-dax-%d", channel);
    }

    // Each CoreAudio client (WSJT-X, fldigi, a recorder, ...) gets its own
    // read cursor into the shared ring. Previously every client advanced the
    // one shared readPos, so two apps on the same DAX channel each received
    // roughly half the samples padded with silence (slow/choppy waterfalls).
    //
    // The ring's readPos is now only a resync marker written by AetherSDR
    // (e.g. to skip buffered silence after TX); when it moves, all cursors
    // jump forward to it.
    void OnReadClientInput(const std::shared_ptr<aspl::Client>& client,
                           const std::shared_ptr<aspl::Stream>& stream,
                           Float64 zeroTimestamp,
                           Float64 timestamp,
                           void* bytes,
                           UInt32 bytesCount) override
    {
        auto* dst = static_cast<float*>(bytes);
        const UInt32 totalSamples = bytesCount / sizeof(float);

        auto* block = m_shm.get(m_shmName);
        if (!block || !block->active) {
            std::memset(dst, 0, bytesCount);
            return;
        }

        ClientCursor* cur = cursorFor(client ? client->GetClientID() : 0);
        const uint32_t wp = block->writePos.load(std::memory_order_acquire);
        const uint32_t marker = block->readPos.load(std::memory_order_acquire);
        if (!cur) {
            std::memset(dst, 0, bytesCount);
            return;
        }

        if (!cur->primed) {
            cur->rp = startCursor(wp);
            cur->lastMarker = marker;
            cur->primed = true;
        } else if (marker != cur->lastMarker) {
            cur->lastMarker = marker;
            if (static_cast<uint32_t>(wp - marker) < static_cast<uint32_t>(wp - cur->rp)) {
                cur->rp = marker;  // AetherSDR asked readers to skip ahead
            }
        }

        uint32_t available = wp - cur->rp;
        if (available > kMaxLatencySamples) {
            // Fell behind (client stalled, or ring overwritten): resync close
            // to live audio instead of replaying stale data.
            cur->rp = startCursor(wp);
            available = wp - cur->rp;
        }

        // Whole stereo frames only, so the next buffer still starts on L.
        const uint32_t toRead = std::min(available, totalSamples) & ~1u;
        uint32_t rp = cur->rp;
        for (uint32_t i = 0; i < toRead; ++i) {
            dst[i] = block->ringBuffer[rp % DaxShmBlock::RING_SIZE];
            ++rp;
        }
        if (toRead < totalSamples) {
            std::memset(dst + toRead, 0, (totalSamples - toRead) * sizeof(float));
        }
        cur->rp = rp;
    }

private:
    // ~50 ms of stereo float samples between the writer and each reader absorbs
    // radio packet jitter vs. the HAL IO cycle without audible latency.
    static constexpr uint32_t kTargetLatencySamples = 2400;
    static constexpr uint32_t kMaxLatencySamples = 24000;   // 0.5 s
    static constexpr int kMaxClients = 16;
    static constexpr int64_t kStaleClientNs = 3'000'000'000;

    struct ClientCursor {
        std::atomic<UInt32> clientId{0};
        std::atomic<int64_t> lastUseNs{0};
        uint32_t rp{0};
        uint32_t lastMarker{0};
        bool primed{false};
    };

    static uint32_t startCursor(uint32_t wp)
    {
        // Keep the cursor on a stereo frame boundary (writer writes whole frames).
        const uint32_t back = std::min<uint32_t>(wp, kTargetLatencySamples) & ~1u;
        return wp - back;
    }

    static int64_t nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Lock-free per-client slot lookup; called on the IO thread.
    ClientCursor* cursorFor(UInt32 id)
    {
        const UInt32 key = id + 1;  // 0 marks a free slot
        const int64_t now = nowNs();
        for (auto& c : m_cursors) {
            if (c.clientId.load(std::memory_order_relaxed) == key) {
                c.lastUseNs.store(now, std::memory_order_relaxed);
                return &c;
            }
        }
        for (auto& c : m_cursors) {
            UInt32 current = c.clientId.load(std::memory_order_relaxed);
            const bool stale = current != 0
                && now - c.lastUseNs.load(std::memory_order_relaxed) > kStaleClientNs;
            if ((current == 0 || stale)
                && c.clientId.compare_exchange_strong(current, key)) {
                c.primed = false;
                c.lastUseNs.store(now, std::memory_order_relaxed);
                return &c;
            }
        }
        return nullptr;
    }

    char m_shmName[32]{};
    DaxShmMapping m_shm;
    std::array<ClientCursor, kMaxClients> m_cursors{};
};

// ── DAX TX Handler: receives audio from apps → writes to shared memory ──────

class DaxTxHandler : public aspl::IORequestHandler {
public:
    DaxTxHandler() = default;

    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
                            Float64 zeroTimestamp,
                            Float64 timestamp,
                            const void* bytes,
                            UInt32 bytesCount) override
    {
        auto* block = m_shm.get("/aethersdr-dax-tx");
        if (!block) return;
        const auto* src = static_cast<const float*>(bytes);
        const UInt32 totalSamples = bytesCount / sizeof(float);

        uint32_t wp = block->writePos.load(std::memory_order_acquire);

        for (uint32_t i = 0; i < totalSamples; ++i) {
            block->ringBuffer[wp % DaxShmBlock::RING_SIZE] = src[i];
            ++wp;
        }

        block->writePos.store(wp, std::memory_order_release);
        block->active = 1;
    }

private:
    DaxShmMapping m_shm;
};

// ── AudioStreamBasicDescription helper ──────────────────────────────────────

static AudioStreamBasicDescription makePCMFormat(Float64 sampleRate, UInt32 channels)
{
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = sampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 32;
    fmt.mChannelsPerFrame = channels;
    fmt.mBytesPerFrame    = channels * sizeof(float);
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;
    return fmt;
}

// ── DriverRequestHandler: defers device creation until host is ready ─────────

class DaxDriverHandler : public aspl::DriverRequestHandler {
public:
    DaxDriverHandler(std::shared_ptr<aspl::Context> ctx,
                     std::shared_ptr<aspl::Plugin> plug)
        : m_context(std::move(ctx))
        , m_plugin(std::move(plug))
    {}

    OSStatus OnInitialize() override
    {
        // Called by HAL after driver is fully initialized and host is available.
        // Safe to add devices here — PropertiesChanged notifications will work.

        // 4 DAX RX input devices (radio → apps receive audio)
        for (int ch = 1; ch <= 4; ++ch) {
            char name[64];
            snprintf(name, sizeof(name), "AetherSDR DAX %d", ch);

            char uid[64];
            snprintf(uid, sizeof(uid), "com.aethersdr.dax.rx.%d", ch);

            auto handler = std::make_shared<DaxRxHandler>(ch);

            aspl::DeviceParameters devParams;
            devParams.Name         = name;
            devParams.Manufacturer = "AetherSDR";
            devParams.DeviceUID    = uid;
            devParams.ModelUID     = "com.aethersdr.dax";
            devParams.SampleRate   = 24000;
            devParams.ChannelCount = 2;
            devParams.EnableMixing = true;

            auto device = std::make_shared<aspl::Device>(m_context, devParams);
            device->SetIOHandler(handler);

            aspl::StreamParameters streamParams;
            streamParams.Direction = aspl::Direction::Input;
            streamParams.Format    = makePCMFormat(24000, 2);

            device->AddStreamWithControlsAsync(streamParams);
            m_plugin->AddDevice(device);

            // Keep shared_ptrs alive
            m_handlers.push_back(handler);
            m_devices.push_back(device);
        }

        // 1 TX output device (apps send audio → radio)
        {
            auto txHandler = std::make_shared<DaxTxHandler>();

            aspl::DeviceParameters txParams;
            txParams.Name         = "AetherSDR TX";
            txParams.Manufacturer = "AetherSDR";
            txParams.DeviceUID    = "com.aethersdr.dax.tx";
            txParams.ModelUID     = "com.aethersdr.dax";
            txParams.SampleRate   = 24000;
            txParams.ChannelCount = 2;
            txParams.EnableMixing = true;

            auto txDevice = std::make_shared<aspl::Device>(m_context, txParams);
            txDevice->SetIOHandler(txHandler);

            aspl::StreamParameters txStreamParams;
            txStreamParams.Direction = aspl::Direction::Output;
            txStreamParams.Format    = makePCMFormat(24000, 2);

            txDevice->AddStreamWithControlsAsync(txStreamParams);
            m_plugin->AddDevice(txDevice);

            m_handlers.push_back(txHandler);
            m_devices.push_back(txDevice);
        }

        return kAudioHardwareNoError;
    }

private:
    std::shared_ptr<aspl::Context> m_context;
    std::shared_ptr<aspl::Plugin> m_plugin;
    std::vector<std::shared_ptr<aspl::IORequestHandler>> m_handlers;
    std::vector<std::shared_ptr<aspl::Device>> m_devices;
};

// ── Driver entry point ──────────────────────────────────────────────────────

extern "C" void* AetherSDRDAX_Create(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    auto context = std::make_shared<aspl::Context>(
        std::make_shared<aspl::Tracer>());

    auto plugin = std::make_shared<aspl::Plugin>(context);

    // Devices are created in OnInitialize() after host is ready
    auto driverHandler = std::make_shared<DaxDriverHandler>(context, plugin);

    static auto driver = std::make_shared<aspl::Driver>(context, plugin);
    driver->SetDriverHandler(driverHandler);

    // Keep handler alive
    static auto handlerRef = driverHandler;

    return driver->GetReference();
}
