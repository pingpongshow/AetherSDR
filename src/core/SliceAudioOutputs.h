#pragma once

#include "SliceAudioRouting.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>
#include <memory>
#include <vector>

namespace AetherSDR {

// One routed slice as applied at run time (built by the controller from a
// SliceAudioPlanEntry plus the slice's mirrored audio_level / audio_pan).
struct SliceRouteRuntime {
    int        sliceId{-1};
    int        sourceDaxChannel{0};   // radio DAX channel carrying the slice
    QString    outputDevice;          // extra output device; empty = none
    int        bridgeChannel{0};      // VirtualAudioBridge channel to feed; 0 = none
    StereoGain gain;                  // applied to the device output only
};

// Owns the per-device outputs for per-slice routing: one QAudioSink (pull
// mode) + SliceMixer (per-slice resampler and lock-free ring) per device.
// Lives on AudioEngine's thread; every method is called there.  Idle (no
// sinks, feed() returns immediately) when no route is configured.
class SliceAudioOutputs {
public:
    SliceAudioOutputs();
    ~SliceAudioOutputs();

    SliceAudioOutputs(const SliceAudioOutputs&) = delete;
    SliceAudioOutputs& operator=(const SliceAudioOutputs&) = delete;

    // Replace the routing table.  Devices whose slice set is unchanged keep
    // running; others are (re)opened or closed.
    void setRoutes(const QVector<SliceRouteRuntime>& routes);
    void setGain(int sliceId, StereoGain gain);
    bool isEmpty() const { return m_routes.isEmpty(); }

    // DAX RX audio (24 kHz stereo float32) for a radio DAX channel.  Pushes
    // it to the routed slice's output device and returns the bridge channel
    // the caller should forward it to (0 = none).
    int feed(int daxChannel, const QByteArray& pcm);

    // Names of devices currently open (for the dialog / logging).
    QStringList openDevices() const;

private:
    struct DeviceOut;
    std::unique_ptr<DeviceOut> openDevice(const QString& name, const QVector<int>& sliceIds);

    QVector<SliceRouteRuntime> m_routes;
    std::array<int, 5> m_routeForDax{};   // DAX channel 1-4 → index in m_routes, -1 = none
    std::vector<std::unique_ptr<DeviceOut>> m_devices;
};

} // namespace AetherSDR
