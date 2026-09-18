#pragma once

#include <QString>

#include <functional>

namespace AetherSDR {

// Offline IQ WAV → .aesr spectrum recording, so a recorded IQ file can be
// reviewed with SpectrumPlayer ("Play in waterfall"): seek, 1-8x, loop.
//
// Each output frame is a Welch average of up to `maxAverages` Blackman-Harris
// windowed FFTs taken from that frame's slice of the file, fft-shifted so bin 0
// is centre − fs/2, in dBFS (+ dbOffset). The pan state keyframe is
// centre = auxi/filename centre, bandwidth = sample rate, and a dBm range
// estimated from a pre-scan of the file (noise floor − 10 dB … peak + 10 dB).
//
// Uses a self-contained radix-2 FFT rather than FFTW: the FFTW *planner* is not
// thread-safe and the project's shared planner lock (SpectralNR::s_fftwMutex)
// is private, while this converter is meant to run on a background thread.
// fftSize must be a power of two (256…65536).
//
// Blocking; run it off the GUI thread (e.g. QThread::create). The progress
// callback receives 0…1 and may return false to cancel.
class IqSpectrumConverter {
public:
    struct Options {
        int    fftSize{4096};
        double framesPerSecond{20.0};
        int    maxAverages{8};
        double dbOffset{0.0};
    };

    static bool convert(const QString& wavPath, const QString& outPath, const Options& opts,
                        const std::function<bool(double)>& progress = {},
                        QString* err = nullptr);
};

} // namespace AetherSDR
