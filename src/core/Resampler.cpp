#include "Resampler.h"

#include "CDSPResampler.h"

#include <algorithm>

namespace AetherSDR {

namespace {

std::unique_ptr<r8b::CDSPResampler> makeR8b(double src, double dst, int maxBlock,
                                            Resampler::Profile profile)
{
    if (profile == Resampler::Profile::LowLatencyVoice) {
        constexpr double kTransBandPct = 10.0;
        constexpr double kStopBandDb = 100.0;
        return std::make_unique<r8b::CDSPResampler>(src, dst, maxBlock, kTransBandPct,
                                                    kStopBandDb, r8b::fprLinearPhase);
    }
    return std::make_unique<r8b::CDSPResampler24>(src, dst, maxBlock);
}

} // namespace

Resampler::Resampler(double srcRate, double dstRate, int maxBlockSamples, Profile profile)
    : m_srcRate(srcRate)
    , m_dstRate(dstRate)
    , m_maxBlock(std::max(1, maxBlockSamples))
    , m_profile(profile)
    , m_resampler(makeR8b(srcRate, dstRate, m_maxBlock, profile))
{
    m_inBuf.reserve(m_maxBlock);
}

Resampler::~Resampler() = default;

QByteArray Resampler::resampleInBuf(int numSamples, int channels)
{
    QByteArray result;
    // Pre-size for the common single-chunk case; grows if needed.
    const double ratio = m_dstRate / m_srcRate;
    result.reserve(static_cast<qsizetype>((numSamples * ratio + 64) * channels * sizeof(float)));

    for (int offset = 0; offset < numSamples; offset += m_maxBlock) {
        const int chunk = std::min(m_maxBlock, numSamples - offset);
        double* outPtr = nullptr;
        const int outLen = m_resampler->process(m_inBuf.data() + offset, chunk, outPtr);
        if (outLen <= 0 || !outPtr) {
            continue;
        }

        const qsizetype start = result.size();
        result.resize(start + static_cast<qsizetype>(outLen) * channels * sizeof(float));
        auto* dst = reinterpret_cast<float*>(result.data() + start);
        for (int i = 0; i < outLen; ++i) {
            const float s = static_cast<float>(outPtr[i]);
            for (int c = 0; c < channels; ++c) {
                *dst++ = s;
            }
        }
    }
    return result;
}

QByteArray Resampler::process(const float* in, int numSamples)
{
    if (numSamples <= 0) return {};

    m_inBuf.resize(numSamples);
    for (int i = 0; i < numSamples; ++i)
        m_inBuf[i] = static_cast<double>(in[i]);

    return resampleInBuf(numSamples, 1);
}

QByteArray Resampler::processStereoToMono(const float* stereoIn, int numStereoFrames)
{
    if (numStereoFrames <= 0) return {};

    m_inBuf.resize(numStereoFrames);
    for (int i = 0; i < numStereoFrames; ++i)
        m_inBuf[i] = (stereoIn[2 * i] + stereoIn[2 * i + 1]) * 0.5;

    return resampleInBuf(numStereoFrames, 1);
}

QByteArray Resampler::processMonoToStereo(const float* monoIn, int numSamples)
{
    if (numSamples <= 0) return {};

    m_inBuf.resize(numSamples);
    for (int i = 0; i < numSamples; ++i)
        m_inBuf[i] = static_cast<double>(monoIn[i]);

    return resampleInBuf(numSamples, 2);
}

QByteArray Resampler::processStereoToStereo(const float* stereoIn, int numStereoFrames)
{
    if (numStereoFrames <= 0) return {};

    m_inBuf.resize(numStereoFrames);
    for (int i = 0; i < numStereoFrames; ++i)
        m_inBuf[i] = (stereoIn[2 * i] + stereoIn[2 * i + 1]) * 0.5;

    return resampleInBuf(numStereoFrames, 2);
}

} // namespace AetherSDR
