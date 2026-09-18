// Rate-plumbing tests for Resampler as used by the digital voice engines:
// 24 kHz stereo ↔ 8/16 kHz mono conversions must preserve duration, tone
// frequency and amplitude — for both the high-quality default profile and
// the LowLatencyVoice profile used by RADEEngine / FreeDvEngine.  Also
// measures (and bounds) each profile's streaming latency and checks the
// low-latency profile's pass band / alias rejection.  Exit code 0 on all pass.

#include "core/Resampler.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;
constexpr double kPi = 3.141592653589793;

void check(bool ok, const char* what, double value)
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %-55s %.4f\n", ok ? " OK " : "FAIL", what, value);
}

// Goertzel-style single-bin magnitude, normalized to sine amplitude.
double toneAmplitude(const float* x, int n, double freq, double rate)
{
    double re = 0.0;
    double im = 0.0;
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));  // Hann
        re += w * x[i] * std::cos(2.0 * kPi * freq * i / rate);
        im += w * x[i] * std::sin(2.0 * kPi * freq * i / rate);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (0.5 * n);
}

const char* profileName(Resampler::Profile p)
{
    return p == Resampler::Profile::LowLatencyVoice ? "low-latency" : "high-quality";
}

// Latency bound per profile.  Measured on r8brain as vendored: high quality
// 297 ms (8 kHz) / 148.5 ms (16 kHz); low-latency 16.4 ms / 8.2 ms.
double maxLatencySec(Resampler::Profile p)
{
    return p == Resampler::Profile::LowLatencyVoice ? 0.025 : 0.4;
}

void testStereoToMono(double dstRate, Resampler::Profile profile)
{
    constexpr double srcRate = 24000.0;
    constexpr double tone = 1000.0;
    constexpr int blocks = 500;  // 10 s
    constexpr int blockFrames = 480;
    Resampler rs(srcRate, dstRate, 4096, profile);
    std::vector<float> stereo(blockFrames * 2);
    std::vector<float> out;
    long n = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < blockFrames; ++i, ++n) {
            const auto v = static_cast<float>(0.5 * std::sin(2.0 * kPi * tone * n / srcRate));
            stereo[2 * i] = v;
            stereo[2 * i + 1] = v;
        }
        QByteArray o = rs.processStereoToMono(stereo.data(), blockFrames);
        const auto* f = reinterpret_cast<const float*>(o.constData());
        out.insert(out.end(), f, f + o.size() / static_cast<int>(sizeof(float)));
    }
    const double expected = blocks * blockFrames * dstRate / srcRate;
    std::printf("24k stereo -> %.0f mono (%s)\n", dstRate, profileName(profile));
    // r8brain holds back its filter latency; nothing is invented.  The
    // held-back amount IS the streaming latency of this stage.
    const double missingSec = (expected - static_cast<double>(out.size())) / dstRate;
    check(missingSec >= 0.0 && missingSec < maxLatencySec(profile),
          "latency = samples held back (s)", missingSec);
    const int tail = static_cast<int>(dstRate / 2);  // last 0.5 s (past filter latency)
    const double amp = toneAmplitude(out.data() + out.size() - tail, tail, tone, dstRate);
    check(std::abs(amp - 0.5) < 0.05, "1 kHz tone amplitude preserved", amp);
    const double wrong = toneAmplitude(out.data() + out.size() - tail, tail, tone * 1.5, dstRate);
    check(wrong < 0.05, "no energy at wrong frequency", wrong);
}

void testMonoToStereo(double srcRate, Resampler::Profile profile)
{
    constexpr double dstRate = 24000.0;
    constexpr double tone = 700.0;
    constexpr int blocks = 500;  // 10 s
    const int blockSamples = static_cast<int>(srcRate / 50.0);
    Resampler rs(srcRate, dstRate, 4096, profile);
    std::vector<float> mono(blockSamples);
    std::vector<float> left;
    bool channelsMatch = true;
    long n = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < blockSamples; ++i, ++n) {
            mono[i] = static_cast<float>(0.4 * std::sin(2.0 * kPi * tone * n / srcRate));
        }
        QByteArray o = rs.processMonoToStereo(mono.data(), blockSamples);
        const auto* f = reinterpret_cast<const float*>(o.constData());
        const int frames = static_cast<int>(o.size() / (2 * sizeof(float)));
        for (int i = 0; i < frames; ++i) {
            channelsMatch = channelsMatch && (f[2 * i] == f[2 * i + 1]);
            left.push_back(f[2 * i]);
        }
    }
    const double expected = blocks * blockSamples * dstRate / srcRate;
    std::printf("%.0f mono -> 24k stereo (%s)\n", srcRate, profileName(profile));
    const double missingSec = (expected - static_cast<double>(left.size())) / dstRate;
    check(missingSec >= 0.0 && missingSec < maxLatencySec(profile),
          "latency = samples held back (s)", missingSec);
    check(channelsMatch, "left == right", channelsMatch ? 1.0 : 0.0);
    const int tail = 12000;
    const double amp = toneAmplitude(left.data() + left.size() - tail, tail, tone, dstRate);
    check(std::abs(amp - 0.4) < 0.04, "700 Hz tone amplitude preserved", amp);
}


// AudioEngine's 24k→48k sink path resamples L and R with two independent
// instances and interleaves the outputs.  That relies on identically
// configured instances emitting identical sample counts per call, and it
// must keep a hard-panned signal panned (processStereoToStereo downmixes).
void testDualInstanceStereoKeepsPan()
{
    constexpr int blockFrames = 128;
    constexpr int blocks = 400;
    Resampler left(24000.0, 48000.0);
    Resampler right(24000.0, 48000.0);
    Resampler downmix(24000.0, 48000.0);
    std::vector<float> l(blockFrames);
    std::vector<float> r(blockFrames, 0.0f);
    std::vector<float> stereo(blockFrames * 2);
    bool countsMatch = true;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    double downmixRightEnergy = 0.0;
    long n = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < blockFrames; ++i, ++n) {
            l[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * n / 24000.0));
            stereo[2 * i] = l[i];
            stereo[2 * i + 1] = 0.0f;
        }
        const QByteArray lo = left.process(l.data(), blockFrames);
        const QByteArray ro = right.process(r.data(), blockFrames);
        const QByteArray dm = downmix.processStereoToStereo(stereo.data(), blockFrames);
        countsMatch = countsMatch && lo.size() == ro.size();
        const auto* lf = reinterpret_cast<const float*>(lo.constData());
        const auto* rf = reinterpret_cast<const float*>(ro.constData());
        for (qsizetype i = 0; i < lo.size() / static_cast<qsizetype>(sizeof(float)); ++i) {
            leftEnergy += static_cast<double>(lf[i]) * lf[i];
        }
        for (qsizetype i = 0; i < ro.size() / static_cast<qsizetype>(sizeof(float)); ++i) {
            rightEnergy += static_cast<double>(rf[i]) * rf[i];
        }
        const auto* df = reinterpret_cast<const float*>(dm.constData());
        for (qsizetype i = 1; i < dm.size() / static_cast<qsizetype>(sizeof(float)); i += 2) {
            downmixRightEnergy += static_cast<double>(df[i]) * df[i];
        }
    }
    check(countsMatch, "dual-instance L/R output counts match per block", countsMatch ? 1.0 : 0.0);
    check(leftEnergy > 100.0 && rightEnergy == 0.0, "dual-instance hard-left pan preserved", rightEnergy);
    check(downmixRightEnergy > 10.0, "processStereoToStereo downmixes (why AE avoids it)", downmixRightEnergy);
}

// Low-latency profile quality for the DV band: flat to 3 kHz at 8 kHz
// (FreeDV/RADE modem signals sit below 3 kHz), and a 24 kHz input tone that
// would alias into that band (5 kHz → 3 kHz at 8 kHz) is rejected.
double steadyToneAmp(double srcRate, double dstRate, double tone, Resampler::Profile profile)
{
    Resampler rs(srcRate, dstRate, 4096, profile);
    const int block = static_cast<int>(srcRate / 50.0);
    std::vector<float> in(static_cast<size_t>(block));
    std::vector<float> out;
    long n = 0;
    for (int b = 0; b < 150; ++b) {  // 3 s
        for (int i = 0; i < block; ++i, ++n) {
            in[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * tone * n / srcRate));
        }
        const QByteArray o = rs.process(in.data(), block);
        const auto* f = reinterpret_cast<const float*>(o.constData());
        out.insert(out.end(), f, f + o.size() / static_cast<int>(sizeof(float)));
    }
    const int tail = static_cast<int>(dstRate);  // last 1 s
    // Measure at the frequency the tone lands on after resampling (alias
    // folding for tones above the destination Nyquist).
    double f = tone;
    if (f > dstRate / 2.0) {
        f = dstRate - f;
    }
    return toneAmplitude(out.data() + out.size() - tail, tail, f, dstRate);
}

void testLowLatencyBand()
{
    constexpr auto kLow = Resampler::Profile::LowLatencyVoice;
    std::printf("low-latency profile pass band / alias rejection\n");
    for (double f : {300.0, 1500.0, 3000.0}) {
        const double down = steadyToneAmp(24000.0, 8000.0, f, kLow);
        const double up = steadyToneAmp(8000.0, 24000.0, f, kLow);
        const double db = 20.0 * std::log10(std::min(down, up) / 0.5);
        check(std::abs(db) < 0.1, "24k<->8k pass band ripple (dB) up to 3 kHz", db);
    }
    const double alias = steadyToneAmp(24000.0, 8000.0, 5000.0, kLow);
    const double aliasDb = 20.0 * std::log10(std::max(alias, 1e-12) / 0.5);
    check(aliasDb < -80.0, "24k->8k: 5 kHz alias into 3 kHz rejected (dB)", aliasDb);
    const double speech = steadyToneAmp(16000.0, 24000.0, 6000.0, kLow);
    check(std::abs(20.0 * std::log10(speech / 0.5)) < 0.1,
          "16k->24k: 6 kHz speech content flat (dB)", 20.0 * std::log10(speech / 0.5));
}

} // namespace

int main()
{
    for (Resampler::Profile p : {Resampler::Profile::HighQuality,
                                 Resampler::Profile::LowLatencyVoice}) {
        testStereoToMono(8000.0, p);
        testStereoToMono(16000.0, p);
        testMonoToStereo(8000.0, p);
        testMonoToStereo(16000.0, p);
    }
    testLowLatencyBand();
    testDualInstanceStereoKeepsPan();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
