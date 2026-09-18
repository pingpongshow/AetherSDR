#pragma once

// Device-independent building blocks for the "device-mapped" DAX backend
// (DeviceMappedDax): lock-free SPSC ring, playout buffer with underrun and
// clock-drift handling, a small streaming polyphase resampler, sample-format
// / channel-layout conversion, and virtual-cable device-name matching.
//
// Nothing in here touches Qt Multimedia or real audio devices, so it is
// unit-tested headlessly by tests/device_mapped_dax_test.cpp.

#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace AetherSDR {

// ── Sample format / channel conversion ──────────────────────────────────────

// Mirrors the QAudioFormat::SampleFormat values we can drive, without pulling
// Qt Multimedia into this header.
enum class DaxSampleFormat { UInt8, Int16, Int32, Float };

int daxBytesPerSample(DaxSampleFormat fmt);

// Interleaved float [-1,1] → device format. `samples` = frames * channels.
// Out-of-range floats are clamped for the integer formats.
void daxConvertFromFloat(const float* src, void* dst, int samples, DaxSampleFormat fmt);

// Device format → interleaved float [-1,1].
void daxConvertToFloat(const void* src, float* dst, int samples, DaxSampleFormat fmt);

// Channel layout remap for `frames` interleaved frames:
//   N → N   copy
//   1 → N   duplicate to every channel
//   ≥2 → 1  average of the first two channels (same downmix as the Linux
//           PipeWire bridge: (L + R) / 2)
//   2 → N>2 L, R, then silence in the extra channels
//   N>2 → 2 first two channels
// src and dst must not alias.
void daxRemapChannels(const float* src, int srcChannels,
                      float* dst, int dstChannels, int frames);

// ── Lock-free single-producer / single-consumer ring of float frames ────────

class DaxSpscRing {
public:
    // Capacity is rounded up to a power of two frames.
    DaxSpscRing(int channels, int capacityFrames);

    int channels() const { return m_channels; }
    int capacityFrames() const { return static_cast<int>(m_mask + 1); }

    // Either side may call these; the value is a snapshot.
    int availableFrames() const;
    int freeFrames() const;

    // Producer only. Writes up to `frames` frames; returns frames written.
    // When full the newest frames are dropped (the consumer's drift logic
    // resynchronises long before a 2 s ring ever fills).
    int write(const float* interleaved, int frames);

    // Consumer only. Returns frames read (≤ requested).
    int read(float* interleaved, int frames);
    // Consumer only. Discards up to `frames` frames; returns frames skipped.
    int skip(int frames);

private:
    int m_channels;
    uint64_t m_mask;
    std::vector<float> m_buf;
    alignas(64) std::atomic<uint64_t> m_write{0};
    alignas(64) std::atomic<uint64_t> m_read{0};
};

// ── Playout (jitter) buffer ─────────────────────────────────────────────────
// Producer: the thread that receives DAX VITA-49 audio (GUI thread).
// Consumer: whatever thread the audio device pulls from.
//
//  * Target fill = config targetFrames (jitter margin) + the largest pull
//    the device has made (its period), measured just before each pull.
//  * Starts "priming": outputs silence until the target is buffered.
//  * Underrun: outputs what is there, pads with silence, re-primes.  The
//    device clock therefore never stalls (important for WSJT-X DT and VARA,
//    same reason the native bridges feed silence during TX).
//  * Drift: the radio's 24 kHz clock and the device clock differ by a few
//    tens of ppm.  A slowly smoothed fill level is compared with the target;
//    outside ± hysteresis (target/8) one frame is dropped or duplicated, at most once per
//    adjustIntervalFrames (≈ 50 ms → ≤ 0.04 % correction, inaudible for
//    digital modes).
//  * Overflow: if the fill exceeds maxFrames (e.g. after a GUI stall) it
//    skips straight back to the target.
class DaxPlayoutBuffer {
public:
    static constexpr int kMaxChannels = 8;

    struct Config {
        int targetFrames{0};
        int hysteresisFrames{0};
        int maxFrames{0};
        int adjustIntervalFrames{0};
        int smoothingFrames{0};   // time constant of the fill-level EMA
    };

    // Sensible defaults for a device running at `sampleRate`.
    static Config defaultConfig(int sampleRate, int targetLatencyMs = 40);

    DaxPlayoutBuffer(int channels, const Config& cfg);

    int channels() const { return m_ring.channels(); }
    const Config& config() const { return m_cfg; }

    // Producer side.
    int push(const float* interleaved, int frames);

    // Consumer side: always fills exactly `frames` frames.
    void pull(float* interleaved, int frames);

    // Any thread: drop everything buffered and re-prime on the next pull.
    void requestFlush() { m_flushRequested.store(true, std::memory_order_release); }

    int bufferedFrames() const { return m_ring.availableFrames(); }
    // targetFrames + largest device pull seen so far (any thread).
    int effectiveTargetFrames() const { return m_effectiveTarget.load(std::memory_order_relaxed); }
    bool isPriming() const { return m_priming.load(std::memory_order_relaxed); }

    // Statistics (monotonic counters, readable from any thread).
    uint64_t underruns() const { return m_underruns.load(std::memory_order_relaxed); }
    uint64_t drops() const     { return m_drops.load(std::memory_order_relaxed); }
    uint64_t inserts() const   { return m_inserts.load(std::memory_order_relaxed); }
    uint64_t resyncs() const   { return m_resyncs.load(std::memory_order_relaxed); }
    uint64_t overflowFrames() const { return m_overflowFrames.load(std::memory_order_relaxed); }

private:
    DaxSpscRing m_ring;
    Config m_cfg;

    // Consumer-thread state.
    double m_smoothedFill{0.0};
    int m_framesSinceAdjust{0};
    int m_maxPullFrames{0};
    std::atomic<int> m_effectiveTarget{0};

    std::atomic<bool> m_priming{true};
    std::atomic<bool> m_flushRequested{false};
    std::atomic<uint64_t> m_underruns{0};
    std::atomic<uint64_t> m_drops{0};
    std::atomic<uint64_t> m_inserts{0};
    std::atomic<uint64_t> m_resyncs{0};
    std::atomic<uint64_t> m_overflowFrames{0};
};

// ── Streaming resampler ─────────────────────────────────────────────────────
// Windowed-sinc (Blackman) polyphase interpolator for interleaved audio with
// an arbitrary rate ratio.  Low latency (~16 input samples when upsampling),
// anti-aliased when downsampling, exact DC gain.  Equal rates pass through.
// Not thread-safe: one instance per stream.
class DaxStreamResampler {
public:
    DaxStreamResampler(int channels, int inRate, int outRate);

    int channels() const { return m_channels; }
    int inRate() const { return m_inRate; }
    int outRate() const { return m_outRate; }
    bool isPassthrough() const { return m_inRate == m_outRate; }

    // Appends resampled interleaved frames to `out`.  Returns frames appended.
    int process(const float* interleaved, int frames, std::vector<float>& out);

    void reset();

private:
    float kernel(double distance) const;

    int m_channels;
    int m_inRate;
    int m_outRate;
    double m_step{1.0};          // input samples per output sample
    int m_halfTaps{0};           // input samples each side of the centre
    double m_halfWidth{0.0};     // kernel support (input samples)
    int m_tableRes{256};         // table points per input sample
    std::vector<float> m_table;  // kernel(d) for d in [0, halfWidth]
    std::vector<float> m_hist;   // interleaved input history
    double m_pos{0.0};           // next output time, in m_hist frames
};

// ── Virtual cable detection ─────────────────────────────────────────────────

enum class DaxCableFamily {
    NotVirtual,       // physical / unknown device
    VbCable,          // VB-Audio Virtual Cable ("CABLE Input/Output")
    VbCableLettered,  // VB-Audio Cable A/B/C/D ("CABLE-A Input ...")
    VbHiFiCable,      // VB-Audio Hi-Fi Cable
    VirtualAudioCable,// Eugene Muzychenko VAC ("Line 1 (Virtual Audio Cable)")
    BlackHole,        // macOS BlackHole
    Loopback,         // Rogue Amoeba Loopback, Soundflower, ALSA snd-aloop
    Voicemeeter,      // VB-Audio Voicemeeter VAIO/AUX (a mixer; last resort)
};

// Classifies a device description.  AetherSDR's own macOS HAL devices
// ("AetherSDR DAX n", "AetherSDR TX") are NotVirtual so they are never
// suggested (the device-mapped backend must not feed its own driver).
DaxCableFamily daxClassifyDevice(const QString& name);

// Sort rank for suggestions (lower = preferred).  Only meaningful for
// virtual devices.
int daxCableRank(const QString& name);

// Normalised key identifying the cable a playback or capture endpoint
// belongs to: "CABLE Input (VB-Audio Virtual Cable)" and
// "CABLE Output (VB-Audio Virtual Cable)" → same key.  Strips the Windows
// duplicate-endpoint prefix ("2- ").
QString daxCableKey(const QString& name);

// True when the playback endpoint `outputName` and capture endpoint
// `inputName` are the two ends of the same virtual cable.  Mapping DAX TX to
// the far end of a DAX RX cable would transmit received audio.
bool daxDevicesShareCable(const QString& outputName, const QString& inputName);

struct DaxDeviceSuggestion {
    // AetherSDR side: DAX RX n plays to rx[n-1]; DAX TX captures from tx.
    std::array<QString, 4> rx;
    QString tx;
    // What the digital-mode app should select for the same channels
    // (the other end of each cable).
    std::array<QString, 4> rxAppDevice;
    QString txAppDevice;
    QStringList notes;
};

// Suggests a mapping from the available device descriptions.  Only cables
// with both a playback and a capture endpoint are used.  Assignment order:
// best cable → DAX RX 1, second → DAX TX, the rest → DAX RX 2..4.  A cable is
// never used for both RX and TX.
DaxDeviceSuggestion suggestDaxDeviceMapping(const QStringList& outputDevices,
                                            const QStringList& inputDevices);

} // namespace AetherSDR
