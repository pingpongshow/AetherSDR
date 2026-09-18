#pragma once

// Per-slice RX audio routing — pure logic (no Qt Multimedia, no settings
// I/O), unit-tested by tests/slice_audio_routing_test.cpp.
//
// Why DAX: the radio's remote_audio_rx stream ("stream create
// type=remote_audio_rx") is ONE mix of every unmuted slice, already scaled by
// each slice's audio_level / audio_pan.  The only per-slice audio the radio
// sends is DAX RX ("stream create type=dax_rx dax_channel=N"), which carries
// the slices whose `dax=` equals N, independent of audio_level / audio_mute
// (the digital-voice path relies on that: it mutes the slice and still
// decodes its DAX audio).  So:
//   * main output only (default) — unchanged: the mixed stream, nothing here.
//   * separate output device / extra DAX target — fed from the slice's own
//     DAX RX channel.  The slice must have a DAX channel that no other slice
//     shares, otherwise its audio cannot be told apart.
//   * "not on main output" — the mix cannot be un-mixed client-side; the
//     controller must mute the slice radio-side while the route is active
//     (plan entry removeFromMain) and restore it afterwards.
// Per-slice volume / pan stay radio-authoritative (audio_level/audio_pan,
// never persisted); for DAX-fed outputs they are mirrored client-side with
// sliceOutputGains() because DAX ignores them.

#include <QChar>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace AetherSDR {

class Resampler;

constexpr int kMaxRoutedSlices = 8;     // slices A-H (FLEX-6700/8600)
constexpr int kSliceDaxRate = 24000;    // DAX RX: 24 kHz float32 stereo

// Client-side routing choice for one slice (persisted per slice letter in
// AppSettings as SliceAudioRoute_<letter>; absent = default).
struct SliceAudioRoute {
    bool    toMain{true};     // keep in the main (mixed remote_audio_rx) output
    QString outputDevice;     // extra output device (QAudioDevice description); empty = none
    int     daxChannel{0};    // extra VirtualAudioBridge DAX channel 1-4; 0 = none

    bool isDefault() const { return toMain && outputDevice.isEmpty() && daxChannel == 0; }
    bool operator==(const SliceAudioRoute& o) const
    {
        return toMain == o.toMain && outputDevice == o.outputDevice && daxChannel == o.daxChannel;
    }
    bool operator!=(const SliceAudioRoute& o) const { return !(*this == o); }

    // Compact JSON: {"main":true,"device":"...","dax":0}.  Garbage → default.
    QString toSetting() const;
    static SliceAudioRoute fromSetting(const QString& s);
    // "SliceAudioRoute_A" … "SliceAudioRoute_H".
    static QString settingsKey(QChar sliceLetter);
};

// What the controller knows about a slice (all radio-side state, mirrored).
struct SliceAudioInfo {
    int   sliceId{-1};
    QChar letter;
    int   radioDaxChannel{0};   // slice dax= (0 = none)
};

struct SliceAudioPlanEntry {
    int     sliceId{-1};
    QChar   letter;
    int     sourceDaxChannel{0};  // radio DAX channel carrying this slice alone
    QString outputDevice;         // empty = none
    int     bridgeChannel{0};     // VirtualAudioBridge channel to feed; 0 = none
    bool    removeFromMain{false};// controller mutes the slice radio-side
};

struct SliceAudioPlan {
    QVector<SliceAudioPlanEntry> entries;
    QVector<int> slicesNeedingDax;   // routed slice without a radio DAX channel
    QStringList  warnings;           // human readable, for the dialog

    bool isEmpty() const { return entries.isEmpty(); }
    QStringList outputDevices() const;
    // Entries fed by a given radio DAX channel (at most one after planning).
    const SliceAudioPlanEntry* entryForDaxChannel(int daxChannel) const;
};

// bridgeFeedsRadioDax: the DAX bridge is running and already receives every
// radio DAX channel on the same-numbered virtual channel (DaxController
// wires daxAudioReady → feedDaxAudio).  Feeding a second source into such a
// channel would double its sample rate (see #1679), so it is refused.
SliceAudioPlan planSliceAudioRouting(const QMap<QChar, SliceAudioRoute>& routes,
                                     const QVector<SliceAudioInfo>& slices,
                                     bool bridgeFeedsRadioDax);

// Client-side mirror of the radio's slice volume/pan for DAX-fed outputs.
// audioLevel 0-100 (linear amplitude, 100 = unity), audioPan 0-100
// (50 = centre, linear balance like the radio's pan control).
struct StereoGain {
    float left{1.0f};
    float right{1.0f};
};
StereoGain sliceOutputGains(float audioLevel, int audioPan, bool muted);

// ── Real-time mixer for one output device ───────────────────────────────────
//
// One slot per routed slice (fixed at construction; rebuild on route
// change).  Producer: push() with the slice's 24 kHz stereo DAX audio, on
// one thread (AudioEngine's).  Consumer: pull() at the device rate, from
// whatever thread the QAudioSink reads its QIODevice on.  Lock-free: each
// slot is a single-producer/single-consumer ring of mono frames at the
// device rate; gains are atomics.
//
// Jitter/drift per slot: output silence until targetFrames are buffered
// ("priming"); on underrun re-prime; if the fill exceeds 3x target (radio
// clock faster than the device, or a stall) skip back to the target.
class SliceMixer {
public:
    SliceMixer(const QVector<int>& sliceIds, int deviceRate, int targetLatencyMs = 60);
    ~SliceMixer();

    SliceMixer(const SliceMixer&) = delete;
    SliceMixer& operator=(const SliceMixer&) = delete;

    int deviceRate() const { return m_deviceRate; }
    int targetFrames() const { return m_targetFrames; }
    bool hasSlice(int sliceId) const;

    // Any thread.
    void setGain(int sliceId, StereoGain g);

    // Producer: 24 kHz interleaved stereo float32.  Returns frames queued
    // (device rate), or -1 if the slice has no slot here.
    int push(int sliceId, const float* stereo24k, int frames);

    // Consumer: fills exactly `frames` interleaved frames of `channels`
    // (1 = mono downmix, 2 = L/R, >2 = L/R then silence).
    void pull(float* out, int frames, int channels);

    // Statistics (consumer-maintained).
    uint64_t underruns() const { return m_underruns.load(std::memory_order_relaxed); }
    uint64_t skippedFrames() const { return m_skipped.load(std::memory_order_relaxed); }
    int bufferedFrames(int sliceId) const;

private:
    struct Slot;
    Slot* slot(int sliceId) const;

    int m_deviceRate;
    int m_targetFrames;
    std::array<int, kMaxRoutedSlices> m_slotIndex{};   // sliceId → slot, -1 = none
    std::vector<std::unique_ptr<Slot>> m_slots;
    std::vector<float> m_scratch;                       // consumer only
    std::atomic<uint64_t> m_underruns{0};
    std::atomic<uint64_t> m_skipped{0};
};

} // namespace AetherSDR
