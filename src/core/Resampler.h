#pragma once

#include <QByteArray>
#include <memory>
#include <vector>
#include <cstdint>

namespace r8b { class CDSPResampler; }

namespace AetherSDR {

// High-quality sample rate converter using r8brain-free-src (MIT).
// Wraps r8b::CDSPResampler24 with float32 ↔ double conversion.
//
// Each instance handles one fixed rate ratio. Create separate instances
// for upsample and downsample paths.
//
// Thread safety: NOT thread-safe. Use one instance per thread/path.

class Resampler {
public:
    // Filter profile.
    //  HighQuality:     r8brain CDSPResampler24 defaults (2 % transition band,
    //                   180 dB stop band).  Transparent, but at 8 kHz the
    //                   linear-phase filter holds back ~297 ms per stage.
    //  LowLatencyVoice: 10 % transition band, 100 dB stop band.  Pass band
    //                   to 90 % of the lower Nyquist (3.6 kHz at 8 kHz,
    //                   7.2 kHz at 16 kHz) — ample for FreeDV/RADE modem
    //                   signals (< 3 kHz) and 16 kHz speech; ~16 ms per stage
    //                   at 8 kHz, ~8 ms at 16 kHz (resampler_rate_test).
    enum class Profile { HighQuality, LowLatencyVoice };

    // maxBlockSamples: max mono samples per process() call
    Resampler(double srcRate, double dstRate, int maxBlockSamples = 4096,
              Profile profile = Profile::HighQuality);
    ~Resampler();

    // Resample mono float32 PCM. Returns resampled mono float32.
    QByteArray process(const float* in, int numSamples);

    // Convenience: stereo float32 → mono downsample → resampled mono float32
    QByteArray processStereoToMono(const float* stereoIn, int numStereoFrames);

    // Convenience: mono float32 → resampled → duplicated to stereo float32
    QByteArray processMonoToStereo(const float* monoIn, int numSamples);

    // Convenience: stereo float32 → downmix to mono → resample → duplicate to stereo float32
    QByteArray processStereoToStereo(const float* stereoIn, int numStereoFrames);

    double srcRate() const { return m_srcRate; }
    double dstRate() const { return m_dstRate; }
    Profile profile() const { return m_profile; }

private:
    // Resample m_inBuf[0..numSamples) in chunks of at most m_maxBlock samples
    // (r8brain's scratch buffers are sized from MaxInLen and never grow, so a
    // larger single call overruns the heap). Output is written as float32 with
    // each sample repeated `channels` times.
    QByteArray resampleInBuf(int numSamples, int channels);

    double m_srcRate;
    double m_dstRate;
    int m_maxBlock;
    Profile m_profile;
    std::unique_ptr<r8b::CDSPResampler> m_resampler;
    std::vector<double> m_inBuf;   // float32 → double conversion buffer
};

} // namespace AetherSDR
