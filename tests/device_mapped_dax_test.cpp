// Headless tests for the device-mapped DAX backend building blocks
// (src/core/DaxDevicePipeline): SPSC ring, playout buffer underrun / drift /
// overflow handling, streaming resampler, sample-format and channel
// conversion, and virtual-cable detection / mapping suggestions.
// No audio devices are touched.  Exit code 0 when everything passes.

#include "core/DaxDevicePipeline.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;
constexpr double kPi = 3.141592653589793;

void check(bool ok, const char* what, double value = 0.0)
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %-62s %.4f\n", ok ? " OK " : "FAIL", what, value);
}

void checkStr(bool ok, const char* what, const QString& value)
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %-62s \"%s\"\n", ok ? " OK " : "FAIL", what, qPrintable(value));
}

// Single-bin amplitude estimate (Hann window), normalised to sine amplitude.
double toneAmplitude(const std::vector<float>& x, int stride, int offset,
                     double freq, double rate, size_t skip = 0)
{
    const size_t frames = x.size() / stride;
    if (frames <= skip + 16) {
        return 0.0;
    }
    const size_t n = frames - skip;
    double re = 0.0;
    double im = 0.0;
    double wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        const double v = x[(i + skip) * stride + offset];
        re += w * v * std::cos(2.0 * kPi * freq * i / rate);
        im += w * v * std::sin(2.0 * kPi * freq * i / rate);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

std::vector<float> stereoTone(int frames, double freq, double rate, float amp, int phaseOffset = 0)
{
    std::vector<float> v(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float s = amp * static_cast<float>(std::sin(2.0 * kPi * freq * (i + phaseOffset) / rate));
        v[static_cast<size_t>(i) * 2] = s;
        v[static_cast<size_t>(i) * 2 + 1] = s;
    }
    return v;
}

// ── Ring ────────────────────────────────────────────────────────────────────

void testRing()
{
    std::printf("\n-- DaxSpscRing --\n");
    DaxSpscRing ring(2, 1000);
    check(ring.capacityFrames() == 1024, "capacity rounds up to power of two", ring.capacityFrames());

    // Wrap-around integrity: push/pull odd-sized chunks many times.
    std::vector<float> in(2 * 300);
    std::vector<float> out(2 * 300);
    int nextWrite = 0;
    int nextRead = 0;
    bool ok = true;
    for (int iter = 0; iter < 200; ++iter) {
        const int w = 37 + (iter * 53) % 263;
        for (int i = 0; i < w; ++i) {
            in[static_cast<size_t>(i) * 2] = static_cast<float>(nextWrite + i);
            in[static_cast<size_t>(i) * 2 + 1] = -static_cast<float>(nextWrite + i);
        }
        nextWrite += ring.write(in.data(), w);
        const int r = ring.read(out.data(), 29 + (iter * 31) % 250);
        for (int i = 0; i < r; ++i) {
            ok = ok && out[static_cast<size_t>(i) * 2] == static_cast<float>(nextRead + i)
                    && out[static_cast<size_t>(i) * 2 + 1] == -static_cast<float>(nextRead + i);
        }
        nextRead += r;
    }
    check(ok, "wrap-around preserves order and L/R pairing");
    check(ring.availableFrames() == nextWrite - nextRead, "available = written - read",
          ring.availableFrames());

    DaxSpscRing small(1, 64);
    std::vector<float> big(100, 1.0f);
    const int written = small.write(big.data(), 100);
    check(written == 64, "full ring drops newest frames", written);
    check(small.skip(10) == 10 && small.availableFrames() == 54, "skip discards frames",
          small.availableFrames());
}

void testRingThreaded()
{
    std::printf("\n-- DaxSpscRing concurrent producer/consumer --\n");
    DaxSpscRing ring(2, 4096);
    constexpr int kTotal = 2'000'000;
    std::atomic<bool> bad{false};

    std::thread producer([&]() {
        std::vector<float> buf(2 * 173);
        int n = 0;
        while (n < kTotal) {
            const int chunk = std::min(173, kTotal - n);
            for (int i = 0; i < chunk; ++i) {
                buf[static_cast<size_t>(i) * 2] = static_cast<float>((n + i) % 1000003);
                buf[static_cast<size_t>(i) * 2 + 1] = 0.5f;
            }
            int done = 0;
            while (done < chunk) {
                done += ring.write(buf.data() + static_cast<size_t>(done) * 2, chunk - done);
                if (done < chunk) {
                    std::this_thread::yield();
                }
            }
            n += chunk;
        }
    });

    std::vector<float> out(2 * 211);
    int got = 0;
    while (got < kTotal) {
        const int r = ring.read(out.data(), 211);
        for (int i = 0; i < r; ++i) {
            if (out[static_cast<size_t>(i) * 2] != static_cast<float>((got + i) % 1000003)
                || out[static_cast<size_t>(i) * 2 + 1] != 0.5f) {
                bad = true;
            }
        }
        got += r;
        if (r == 0) {
            std::this_thread::yield();
        }
    }
    producer.join();
    check(!bad.load(), "2M frames across threads: no loss, no reordering", got);
}

// ── Playout buffer ──────────────────────────────────────────────────────────

void testPlayoutBasics()
{
    std::printf("\n-- DaxPlayoutBuffer basics --\n");
    const auto cfg = DaxPlayoutBuffer::defaultConfig(48000, 40);
    check(cfg.targetFrames == 1920, "target 40 ms @ 48 kHz", cfg.targetFrames);
    DaxPlayoutBuffer pb(2, cfg);

    std::vector<float> out(2 * 480);
    std::vector<float> in(2 * 480);
    for (size_t i = 0; i < in.size(); ++i) {
        in[i] = 0.25f;
    }

    // Priming: less than target buffered → silence, nothing consumed.
    pb.push(in.data(), 480);
    pb.pull(out.data(), 480);
    bool silent = std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; });
    check(silent && pb.isPriming(), "priming outputs silence below target");
    check(pb.bufferedFrames() == 480, "priming does not consume", pb.bufferedFrames());

    for (int i = 0; i < 6; ++i) {
        pb.push(in.data(), 480);
    }
    pb.pull(out.data(), 480);
    const bool full = std::all_of(out.begin(), out.end(), [](float v) { return v == 0.25f; });
    check(full && !pb.isPriming(), "plays once target reached");
    check(pb.effectiveTargetFrames() == cfg.targetFrames + 480,
          "effective target = margin + device period", pb.effectiveTargetFrames());
    check(pb.bufferedFrames() == pb.effectiveTargetFrames() - 480,
          "priming overshoot trimmed to target", pb.bufferedFrames());

    // Underrun: drain and ask for more than is there.
    while (pb.bufferedFrames() >= 480) {
        pb.pull(out.data(), 480);
    }
    const int remaining = pb.bufferedFrames();
    pb.pull(out.data(), 480);
    const int tailStart = remaining;
    bool padded = true;
    for (int f = tailStart; f < 480; ++f) {
        padded = padded && out[static_cast<size_t>(f) * 2] == 0.0f;
    }
    check(padded, "underrun pads with silence");
    check(pb.underruns() == 1 && pb.isPriming(), "underrun counted and re-primes",
          static_cast<double>(pb.underruns()));

    // Overflow while playing: dump 1 s in (a GUI stall) → resync to target.
    std::vector<float> sec(2 * 48000, 0.1f);
    pb.push(sec.data(), 4800);
    pb.pull(out.data(), 480);   // priming completes
    pb.push(sec.data(), 48000);
    pb.pull(out.data(), 480);
    const int afterResync = pb.bufferedFrames();
    check(pb.resyncs() == 1, "overflow triggers one resync", static_cast<double>(pb.resyncs()));
    check(afterResync == pb.effectiveTargetFrames() - 480, "resync lands on target",
          afterResync);

    // Flush.
    pb.requestFlush();
    pb.pull(out.data(), 480);
    check(pb.bufferedFrames() == 0 && pb.isPriming(), "flush empties and re-primes");
}

struct DriftResult {
    uint64_t underruns;
    uint64_t drops;
    uint64_t inserts;
    uint64_t resyncs;
    int minFill;
    int maxFill;
};

// Simulates `seconds` of a producer whose clock runs at rate*(1+ppm) with
// bursty VITA-49-like delivery, and a device pulling 10 ms periods.
DriftResult simulateDrift(double ppm, int seconds, unsigned seed, int pullFrames = 480)
{
    constexpr int kRate = 48000;
    const auto cfg = DaxPlayoutBuffer::defaultConfig(kRate, 40);
    DaxPlayoutBuffer pb(1, cfg);
    std::mt19937 rng(seed);
    // 0–3 packets per 5 ms slot, never more than ~20 ms held back: a busy
    // Wi-Fi link.  Wired LAN DAX jitter is a few ms.
    std::uniform_int_distribution<int> jitter(0, 3);

    std::vector<float> packet(256, 0.0f);   // 256 frames @48k = 128 @24k
    std::vector<float> out(static_cast<size_t>(pullFrames));
    double consumerOwed = 0.0;
    double producerOwed = 0.0;
    int backlogPackets = 0;
    DriftResult r{0, 0, 0, 0, 1 << 30, 0};
    const double producerFramesPer5ms = kRate * 0.005 * (1.0 + ppm * 1e-6);

    for (int ms5 = 0; ms5 < seconds * 200; ++ms5) {
        // Producer: accrue frames, deliver in bursts of whole packets.
        producerOwed += producerFramesPer5ms;
        while (producerOwed >= 256.0) {
            producerOwed -= 256.0;
            ++backlogPackets;
        }
        int deliver = std::min(backlogPackets, jitter(rng));
        if (backlogPackets > 4) {
            deliver = backlogPackets;
        }
        for (int p = 0; p < deliver; ++p) {
            pb.push(packet.data(), 256);
        }
        backlogPackets -= deliver;

        // Consumer: device pulls `pullFrames` whenever that much time has passed.
        consumerOwed += kRate * 0.005;
        while (consumerOwed >= pullFrames) {
            consumerOwed -= pullFrames;
            pb.pull(out.data(), pullFrames);
            if (ms5 > 400) {   // after 1 s settle
                r.minFill = std::min(r.minFill, pb.bufferedFrames());
                r.maxFill = std::max(r.maxFill, pb.bufferedFrames());
            }
        }
        if (ms5 == 400) {
            // Ignore start-up underruns while the burst pattern settles.
            r.underruns = pb.underruns();
        }
    }
    r.underruns = pb.underruns() - r.underruns;
    r.drops = pb.drops();
    r.inserts = pb.inserts();
    r.resyncs = pb.resyncs();
    return r;
}

void testDrift()
{
    std::printf("\n-- DaxPlayoutBuffer clock drift (10 simulated minutes each) --\n");
    const auto fast = simulateDrift(+300.0, 600, 1);
    check(fast.drops > 0 && fast.inserts < fast.drops / 20,
          "radio clock +300 ppm → frames dropped", static_cast<double>(fast.drops));
    check(fast.underruns == 0 && fast.resyncs == 0, "  no underruns / resyncs",
          static_cast<double>(fast.underruns + fast.resyncs));
    check(fast.maxFill < 1920 * 3, "  fill stays bounded (< 120 ms)", fast.maxFill);
    // 300 ppm of 600 s at 48 kHz ≈ 8640 frames must have been removed.
    check(std::abs(static_cast<double>(fast.drops) - 8640.0) < 2500.0,
          "  drops ≈ accumulated drift (8640)", static_cast<double>(fast.drops));

    const auto slow = simulateDrift(-300.0, 600, 2);
    check(slow.inserts > 0 && slow.drops < slow.inserts / 20,
          "radio clock -300 ppm → frames inserted", static_cast<double>(slow.inserts));
    check(std::abs(static_cast<double>(slow.inserts) - static_cast<double>(slow.drops) - 8640.0)
              < 2500.0,
          "  net inserts ≈ accumulated drift (8640)",
          static_cast<double>(slow.inserts) - static_cast<double>(slow.drops));
    check(slow.underruns == 0 && slow.resyncs == 0, "  no underruns / resyncs",
          static_cast<double>(slow.underruns + slow.resyncs));
    check(slow.minFill > 0, "  never runs dry", slow.minFill);

    // Qt 6.9 CoreAudio pull mode: 2048-frame (43 ms) reads.
    const auto mac = simulateDrift(+100.0, 600, 4, 2048);
    check(mac.underruns == 0 && mac.resyncs == 0,
          "2048-frame device pulls (Qt/CoreAudio): no underruns",
          static_cast<double>(mac.underruns + mac.resyncs));
    const auto macSlow = simulateDrift(-100.0, 600, 5, 2048);
    check(macSlow.underruns == 0 && macSlow.minFill > 0,
          "2048-frame pulls, radio -100 ppm: never runs dry", macSlow.minFill);

    const auto same = simulateDrift(0.0, 600, 3);
    // Jitter makes the smoothed fill wander a little; < 1 single-frame
    // correction per 5 s is inaudible.
    check(same.underruns == 0 && same.drops + same.inserts < 120,
          "matched clocks → rare corrections (< 1 per 5 s)",
          static_cast<double>(same.drops + same.inserts));
}

// ── Resampler ───────────────────────────────────────────────────────────────

std::vector<float> resampleChunked(DaxStreamResampler& rs, const std::vector<float>& in,
                                   int channels, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> sizes(1, 700);
    std::vector<float> out;
    const int frames = static_cast<int>(in.size()) / channels;
    int pos = 0;
    while (pos < frames) {
        const int n = std::min(sizes(rng), frames - pos);
        rs.process(in.data() + static_cast<size_t>(pos) * channels, n, out);
        pos += n;
    }
    return out;
}

void testResampler()
{
    std::printf("\n-- DaxStreamResampler --\n");
    struct Case { int in; int out; };
    for (const Case c : {Case{24000, 48000}, Case{48000, 24000}, Case{24000, 44100},
                         Case{44100, 24000}, Case{24000, 24000}}) {
        const int frames = c.in * 2;   // 2 s
        const auto tone = stereoTone(frames, 1000.0, c.in, 0.5f);
        DaxStreamResampler rs(2, c.in, c.out);
        const auto out = resampleChunked(rs, tone, 2, 7);
        const double expected = static_cast<double>(frames) * c.out / c.in;
        const double outFrames = static_cast<double>(out.size() / 2);
        char label[96];
        std::snprintf(label, sizeof(label), "%d → %d: length within 64 frames", c.in, c.out);
        check(std::abs(outFrames - expected) <= 64.0, label, outFrames - expected);
        const double ampL = toneAmplitude(out, 2, 0, 1000.0, c.out, 256);
        const double ampR = toneAmplitude(out, 2, 1, 1000.0, c.out, 256);
        std::snprintf(label, sizeof(label), "%d → %d: 1 kHz amplitude preserved (L)", c.in, c.out);
        check(std::abs(ampL - 0.5) < 0.01, label, ampL);
        std::snprintf(label, sizeof(label), "%d → %d: L/R identical", c.in, c.out);
        check(std::abs(ampL - ampR) < 1e-4, label, ampL - ampR);
    }

    // Chunking must not change the output.
    {
        const auto tone = stereoTone(24000, 1234.0, 24000, 0.3f);
        DaxStreamResampler a(2, 24000, 48000);
        DaxStreamResampler b(2, 24000, 48000);
        std::vector<float> oneShot;
        a.process(tone.data(), 24000, oneShot);
        const auto chunked = resampleChunked(b, tone, 2, 11);
        double maxDiff = 0.0;
        const size_t n = std::min(oneShot.size(), chunked.size());
        for (size_t i = 0; i < n; ++i) {
            maxDiff = std::max(maxDiff, static_cast<double>(std::abs(oneShot[i] - chunked[i])));
        }
        check(oneShot.size() == chunked.size() && maxDiff < 1e-6,
              "chunked == one-shot output", maxDiff);
    }

    // Anti-aliasing: 15 kHz at 48 kHz must not fold to 9 kHz at 24 kHz.
    {
        const auto tone = stereoTone(48000, 15000.0, 48000, 0.5f);
        DaxStreamResampler rs(2, 48000, 24000);
        const auto out = resampleChunked(rs, tone, 2, 5);
        const double alias = toneAmplitude(out, 2, 0, 9000.0, 24000, 256);
        const double db = 20.0 * std::log10(std::max(alias, 1e-9) / 0.5);
        check(db < -50.0, "48k→24k: 15 kHz alias rejected (dB)", db);
    }

    // DC gain exact.
    {
        std::vector<float> dc(2 * 4800, 0.75f);
        DaxStreamResampler rs(2, 24000, 44100);
        std::vector<float> out;
        rs.process(dc.data(), 4800, out);
        double maxErr = 0.0;
        for (size_t i = 200; i + 200 < out.size(); ++i) {
            maxErr = std::max(maxErr, static_cast<double>(std::abs(out[i] - 0.75f)));
        }
        check(maxErr < 1e-4, "DC gain exact (24k→44.1k)", maxErr);
    }
}

// ── Formats / channels ──────────────────────────────────────────────────────

void testFormats()
{
    std::printf("\n-- Sample format / channel conversion --\n");
    const std::vector<float> src{0.0f, 0.5f, -0.5f, 0.999f, -1.0f, 1.5f, -1.5f};
    const int n = static_cast<int>(src.size());

    struct F { DaxSampleFormat fmt; const char* name; double tol; };
    for (const F f : {F{DaxSampleFormat::Int16, "Int16", 1.0 / 32768},
                      F{DaxSampleFormat::Int32, "Int32", 1e-6},
                      F{DaxSampleFormat::UInt8, "UInt8", 1.0 / 128},
                      F{DaxSampleFormat::Float, "Float", 0.0}}) {
        std::vector<unsigned char> raw(static_cast<size_t>(n) * daxBytesPerSample(f.fmt));
        std::vector<float> back(static_cast<size_t>(n));
        daxConvertFromFloat(src.data(), raw.data(), n, f.fmt);
        daxConvertToFloat(raw.data(), back.data(), n, f.fmt);
        double maxErr = 0.0;
        for (int i = 0; i < 5; ++i) {   // in-range values
            maxErr = std::max(maxErr, static_cast<double>(std::abs(back[static_cast<size_t>(i)]
                                                                   - src[static_cast<size_t>(i)])));
        }
        char label[80];
        std::snprintf(label, sizeof(label), "%s round trip within 1 LSB", f.name);
        check(maxErr <= f.tol + 1e-9, label, maxErr);
        if (f.fmt != DaxSampleFormat::Float) {
            std::snprintf(label, sizeof(label), "%s clamps out-of-range", f.name);
            check(back[5] <= 1.0f && back[5] > 0.98f && back[6] >= -1.0f && back[6] < -0.98f,
                  label, back[5]);
        }
    }
    {
        const int16_t full = [] {
            const float one = 1.0f;
            int16_t v = 0;
            daxConvertFromFloat(&one, &v, 1, DaxSampleFormat::Int16);
            return v;
        }();
        check(full == 32767, "Int16 +1.0 → 32767 (no wrap)", full);
    }

    const std::vector<float> stereo{0.2f, 0.4f, -1.0f, 1.0f};
    std::vector<float> mono(2);
    daxRemapChannels(stereo.data(), 2, mono.data(), 1, 2);
    check(std::abs(mono[0] - 0.3f) < 1e-6 && mono[1] == 0.0f, "2→1 averages L+R", mono[0]);

    std::vector<float> dup(4);
    const std::vector<float> m{0.7f, -0.2f};
    daxRemapChannels(m.data(), 1, dup.data(), 2, 2);
    check(dup[0] == 0.7f && dup[1] == 0.7f && dup[2] == -0.2f && dup[3] == -0.2f,
          "1→2 duplicates");

    std::vector<float> quad(8, 9.0f);
    daxRemapChannels(stereo.data(), 2, quad.data(), 4, 2);
    check(quad[0] == 0.2f && quad[1] == 0.4f && quad[2] == 0.0f && quad[3] == 0.0f
              && quad[4] == -1.0f && quad[5] == 1.0f,
          "2→4 carries L/R, silences extras");
}

// ── End-to-end RX chain (as DeviceMappedDax::feedDaxAudio + pull) ───────────

void testRxChain()
{
    std::printf("\n-- RX chain: 24k stereo float → 48k Int16 mono device --\n");
    const int devRate = 48000;
    const int devCh = 1;
    const float gain = 0.5f;
    DaxStreamResampler rs(devCh, 24000, devRate);
    DaxPlayoutBuffer pb(devCh, DaxPlayoutBuffer::defaultConfig(devRate, 40));

    std::vector<float> captured;
    std::vector<float> resampled;
    std::vector<float> mono(128);
    std::vector<float> pullBuf(480);
    std::vector<int16_t> dev(480);
    std::vector<float> back(480);
    int phase = 0;
    for (int tick = 0; tick < 300; ++tick) {   // 3 s of 10 ms ticks
        // Radio: ~10 ms = 240 frames @24k, in 128-frame packets (with remainder).
        for (int p = 0; p < 2; ++p) {
            const int frames = (p == 0) ? 128 : 112;
            auto pkt = stereoTone(frames, 700.0, 24000, 0.8f, phase);
            phase += frames;
            for (float& v : pkt) {
                v *= gain;
            }
            daxRemapChannels(pkt.data(), 2, mono.data(), devCh, frames);
            resampled.clear();
            const int n = rs.process(mono.data(), frames, resampled);
            pb.push(resampled.data(), n);
        }
        pb.pull(pullBuf.data(), 480);
        daxConvertFromFloat(pullBuf.data(), dev.data(), 480, DaxSampleFormat::Int16);
        daxConvertToFloat(dev.data(), back.data(), 480, DaxSampleFormat::Int16);
        captured.insert(captured.end(), back.begin(), back.end());
    }
    const double amp = toneAmplitude(captured, 1, 0, 700.0, devRate, 48000 / 2);
    check(std::abs(amp - 0.4) < 0.01, "700 Hz tone arrives at gain × amplitude (0.4)", amp);
    check(pb.underruns() == 0, "no underruns with steady 10 ms delivery",
          static_cast<double>(pb.underruns()));
}

// ── Device detection ────────────────────────────────────────────────────────

void testDetection()
{
    std::printf("\n-- Virtual cable detection --\n");
    check(daxClassifyDevice("CABLE Input (VB-Audio Virtual Cable)") == DaxCableFamily::VbCable,
          "VB-Cable playback endpoint");
    check(daxClassifyDevice("CABLE-A Output (VB-Audio Cable A)") == DaxCableFamily::VbCableLettered,
          "VB-Cable A capture endpoint");
    check(daxClassifyDevice("Line 1 (Virtual Audio Cable)") == DaxCableFamily::VirtualAudioCable,
          "VAC Line 1");
    check(daxClassifyDevice("VoiceMeeter Aux Input (VB-Audio VoiceMeeter AUX VAIO)")
              == DaxCableFamily::Voicemeeter, "Voicemeeter AUX");
    check(daxClassifyDevice("BlackHole 2ch") == DaxCableFamily::BlackHole, "BlackHole");
    check(daxClassifyDevice("VB-Cable") == DaxCableFamily::VbCable, "VB-Cable (macOS name)");
    check(daxClassifyDevice("Speakers (Realtek(R) Audio)") == DaxCableFamily::NotVirtual,
          "physical speakers ignored");
    check(daxClassifyDevice("Line In (Realtek(R) Audio)") == DaxCableFamily::NotVirtual,
          "physical line-in ignored");
    check(daxClassifyDevice("AetherSDR DAX 1") == DaxCableFamily::NotVirtual,
          "own HAL device never treated as a cable");

    checkStr(daxCableKey("2- CABLE Input (VB-Audio Virtual Cable)")
                 == daxCableKey("CABLE Output (VB-Audio Virtual Cable)"),
             "dup-prefixed Input/Output share key",
             daxCableKey("2- CABLE Input (VB-Audio Virtual Cable)"));
    check(daxDevicesShareCable("CABLE Input (VB-Audio Virtual Cable)",
                               "CABLE Output (VB-Audio Virtual Cable)"),
          "same-cable RX/TX detected (feedback guard)");
    check(!daxDevicesShareCable("CABLE Input (VB-Audio Virtual Cable)",
                                "CABLE-B Output (VB-Audio Cable B)"),
          "different cables are not flagged");
    check(daxDevicesShareCable("BlackHole 2ch", "BlackHole 2ch"),
          "BlackHole in/out is one cable");

    std::printf("\n-- Mapping suggestions --\n");
    const QStringList winPhysOut{"Speakers (Realtek(R) Audio)", "Headphones (USB Audio CODEC )"};
    const QStringList winPhysIn{"Microphone (Realtek(R) Audio)", "Microphone (USB Audio CODEC )"};

    {   // Single free VB-Cable: RX1 only, TX unmapped with a note.
        const auto s = suggestDaxDeviceMapping(
            winPhysOut + QStringList{"CABLE Input (VB-Audio Virtual Cable)"},
            winPhysIn + QStringList{"CABLE Output (VB-Audio Virtual Cable)"});
        checkStr(s.rx[0] == "CABLE Input (VB-Audio Virtual Cable)", "1 cable: RX1", s.rx[0]);
        checkStr(s.rxAppDevice[0] == "CABLE Output (VB-Audio Virtual Cable)",
                 "1 cable: app records CABLE Output", s.rxAppDevice[0]);
        checkStr(s.tx.isEmpty() && !s.notes.isEmpty(), "1 cable: TX unmapped + note", s.tx);
    }
    {   // VB-Cable + A + B.
        const auto s = suggestDaxDeviceMapping(
            winPhysOut + QStringList{"CABLE-B Input (VB-Audio Cable B)",
                                     "CABLE Input (VB-Audio Virtual Cable)",
                                     "CABLE-A Input (VB-Audio Cable A)"},
            winPhysIn + QStringList{"CABLE-A Output (VB-Audio Cable A)",
                                    "CABLE Output (VB-Audio Virtual Cable)",
                                    "CABLE-B Output (VB-Audio Cable B)"});
        checkStr(s.rx[0] == "CABLE Input (VB-Audio Virtual Cable)", "3 cables: RX1", s.rx[0]);
        checkStr(s.tx == "CABLE-A Output (VB-Audio Cable A)", "3 cables: TX", s.tx);
        checkStr(s.txAppDevice == "CABLE-A Input (VB-Audio Cable A)",
                 "3 cables: app plays to CABLE-A Input", s.txAppDevice);
        checkStr(s.rx[1] == "CABLE-B Input (VB-Audio Cable B)", "3 cables: RX2", s.rx[1]);
        checkStr(s.rx[2].isEmpty() && s.rx[3].isEmpty(), "3 cables: RX3/4 unmapped", s.rx[2]);
    }
    {   // VAC lines, including a Windows dup-prefix.
        const auto s = suggestDaxDeviceMapping(
            QStringList{"Line 3 (Virtual Audio Cable)", "Line 1 (Virtual Audio Cable)",
                        "Line 2 (Virtual Audio Cable)"},
            QStringList{"Line 2 (Virtual Audio Cable)", "Line 1 (Virtual Audio Cable)",
                        "Line 3 (Virtual Audio Cable)"});
        checkStr(s.rx[0] == "Line 1 (Virtual Audio Cable)" && s.tx == "Line 2 (Virtual Audio Cable)"
                     && s.rx[1] == "Line 3 (Virtual Audio Cable)",
                 "VAC: Line1→RX1, Line2→TX, Line3→RX2", s.rx[1]);
    }
    {   // Voicemeeter only: usable, but with a mixer warning.
        const auto s = suggestDaxDeviceMapping(
            QStringList{"VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)",
                        "VoiceMeeter Aux Input (VB-Audio VoiceMeeter AUX VAIO)"},
            QStringList{"VoiceMeeter Output (VB-Audio VoiceMeeter VAIO)",
                        "VoiceMeeter Aux Output (VB-Audio VoiceMeeter AUX VAIO)"});
        checkStr(s.rx[0] == "VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)"
                     && s.tx == "VoiceMeeter Aux Output (VB-Audio VoiceMeeter AUX VAIO)",
                 "Voicemeeter: VAIO→RX1, AUX→TX", s.tx);
        check(s.notes.join(' ').contains("mixer"), "Voicemeeter: mixer-bus warning");
    }
    {   // VB-Cable preferred over Voicemeeter even when listed later.
        const auto s = suggestDaxDeviceMapping(
            QStringList{"VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)",
                        "CABLE Input (VB-Audio Virtual Cable)"},
            QStringList{"VoiceMeeter Output (VB-Audio VoiceMeeter VAIO)",
                        "CABLE Output (VB-Audio Virtual Cable)"});
        checkStr(s.rx[0] == "CABLE Input (VB-Audio Virtual Cable)", "VB-Cable ranks above Voicemeeter",
                 s.rx[0]);
    }
    {   // This Mac: HAL devices excluded, VB-Cable, BlackHole 2ch / 16ch.
        const auto s = suggestDaxDeviceMapping(
            QStringList{"MacBook Pro Speakers", "BlackHole 16ch", "BlackHole 2ch", "AetherSDR TX",
                        "VB-Cable", "Microsoft Teams Audio"},
            QStringList{"MacBook Pro Microphone", "BlackHole 16ch", "BlackHole 2ch",
                        "AetherSDR DAX 1", "AetherSDR DAX 2", "VB-Cable", "Microsoft Teams Audio"});
        checkStr(s.rx[0] == "VB-Cable" && s.tx == "BlackHole 2ch" && s.rx[1] == "BlackHole 16ch",
                 "macOS: VB-Cable→RX1, BlackHole 2ch→TX, 16ch→RX2", s.tx);
        bool noOwn = true;
        for (const QString& r : s.rx) {
            noOwn = noOwn && !r.startsWith("AetherSDR");
        }
        check(noOwn && !s.tx.startsWith("AetherSDR"), "macOS: AetherSDR HAL devices never suggested");
    }
    {   // Nothing virtual.
        const auto s = suggestDaxDeviceMapping(winPhysOut, winPhysIn);
        check(s.rx[0].isEmpty() && s.tx.isEmpty() && !s.notes.isEmpty(),
              "no cables: empty mapping + install hint");
    }
    {   // Output-only endpoint (no capture side) is not suggested.
        const auto s = suggestDaxDeviceMapping(QStringList{"CABLE Input (VB-Audio Virtual Cable)"},
                                               QStringList{});
        check(s.rx[0].isEmpty(), "cable without capture endpoint skipped");
    }
    {   // Invariant: suggested TX is never the far end of any suggested RX.
        const auto s = suggestDaxDeviceMapping(
            QStringList{"CABLE Input (VB-Audio Virtual Cable)", "CABLE-A Input (VB-Audio Cable A)",
                        "BlackHole 2ch", "Line 1 (Virtual Audio Cable)", "Line 2 (Virtual Audio Cable)"},
            QStringList{"CABLE Output (VB-Audio Virtual Cable)", "CABLE-A Output (VB-Audio Cable A)",
                        "BlackHole 2ch", "Line 1 (Virtual Audio Cable)", "Line 2 (Virtual Audio Cable)"});
        bool safe = true;
        for (const QString& r : s.rx) {
            safe = safe && !daxDevicesShareCable(r, s.tx);
        }
        check(safe && !s.rx[3].isEmpty(), "5 cables: all 4 RX mapped, TX never loops back");
    }
}

} // namespace

int main()
{
    testRing();
    testRingThreaded();
    testPlayoutBasics();
    testDrift();
    testResampler();
    testFormats();
    testRxChain();
    testDetection();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
