#pragma once

#include <QByteArray>
#include <memory>

struct DenoiseState;

namespace AetherSDR {

class Resampler;

// Client-side RNN noise suppression using Mozilla/Xiph RNNoise.
// Processes 24kHz stereo Int16 audio by upsampling to 48kHz mono,
// running RNNoise, and downsampling back to 24kHz stereo.
//
// RNNoise requires 48kHz mono float input in 480-sample (10ms) frames.

class RNNoiseFilter {
public:
    RNNoiseFilter();
    ~RNNoiseFilter();

    // Process a block of 24kHz stereo Int16 PCM.
    // Returns the processed block (same format, same size).
    QByteArray process(const QByteArray& pcm24kStereo);

    // Returns true if rnnoise_create() succeeded.
    bool isValid() const { return m_state != nullptr; }

    // Reset internal state (e.g., on band change).
    void reset();

private:
    DenoiseState* m_state{nullptr};
    std::unique_ptr<Resampler> m_up;    // 24kHz mono → 48kHz mono
    std::unique_ptr<Resampler> m_down;  // 48kHz mono → 24kHz mono
    QByteArray    m_inAccum;            // accumulate 48kHz mono float input
    QByteArray    m_outAccum;           // accumulate 24kHz stereo int16 output
};

} // namespace AetherSDR
