// Loopback tests for FreeDvEngine (codec2 FreeDV 700E / 1600 / 700D).
// Synthetic voiced "speech" (24 kHz stereo float32) → feedTxAudio → modem
// (24 kHz) → small AWGN → feedRxAudio → decoded speech.  Asserts the demod
// syncs, decoded speech carries energy once synced, the 24 kHz ↔ modem/speech
// rate plumbing preserves duration, and the text channel round-trips.
//
// Build: produced by CMake as `freedv_engine_test` when codec2 is found.
// Exit code 0 on all pass.  No radio, no network.

#include "core/FreeDvEngine.h"

#include <QByteArray>
#include <QString>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const QString& detail = QString())
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s %s\n", ok ? " OK " : "FAIL", what, qPrintable(detail));
}

constexpr int kRate = 24000;
constexpr int kChunkFrames = 480;  // 20 ms, typical DAX/mic block
constexpr double kPi = 3.141592653589793;

// Voiced synthetic speech: harmonics of a slowly gliding pitch with a crude
// formant envelope and syllable-rate amplitude modulation.
QByteArray makeSpeech(double seconds)
{
    const int frames = static_cast<int>(seconds * kRate);
    QByteArray out(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* d = reinterpret_cast<float*>(out.data());
    double phase = 0.0;
    for (int n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kRate;
        const double f0 = 130.0 + 30.0 * std::sin(2.0 * kPi * 0.7 * t);
        phase += 2.0 * kPi * f0 / kRate;
        double s = 0.0;
        for (int h = 1; h <= 20; ++h) {
            const double fh = f0 * h;
            if (fh > 3500.0) {
                break;
            }
            const double formant = std::exp(-std::pow((fh - 700.0) / 400.0, 2.0))
                                 + 0.6 * std::exp(-std::pow((fh - 1800.0) / 500.0, 2.0));
            s += (0.15 + formant) * std::sin(h * phase) / h;
        }
        const double env = 0.55 + 0.45 * std::sin(2.0 * kPi * 3.0 * t);
        const auto v = static_cast<float>(0.25 * env * s);
        d[2 * n] = v;
        d[2 * n + 1] = v;
    }
    return out;
}

double rmsStereo(const QByteArray& pcm)
{
    const auto* d = reinterpret_cast<const float*>(pcm.constData());
    const qsizetype n = pcm.size() / static_cast<qsizetype>(sizeof(float));
    if (n == 0) {
        return 0.0;
    }
    double acc = 0.0;
    for (qsizetype i = 0; i < n; ++i) {
        acc += static_cast<double>(d[i]) * d[i];
    }
    return std::sqrt(acc / static_cast<double>(n));
}

struct LoopResult {
    bool   everSynced{false};
    double modemRms{0.0};
    double decodedRms{0.0};
    double txDurationRatio{0.0};
    double rxDurationRatio{0.0};
    QStringList texts;
    float  lastSnr{-99.0f};
    int    modemRate{0};
    int    speechRate{0};
};

LoopResult runLoopback(DigitalVoiceMode mode, double seconds, const QString& txText)
{
    LoopResult r;
    FreeDvEngine tx(mode);
    FreeDvEngine rx(mode);
    if (!tx.start() || !rx.start()) {
        return r;
    }
    r.modemRate = tx.modemSampleRate();
    r.speechRate = tx.speechSampleRate();
    tx.setTxText(txText);
    // Squelch on with a low threshold: unsynced output is silence, so any
    // decoded energy must come from synced frames.
    rx.setSquelch(true, -10.0f);

    QByteArray modem;
    QObject::connect(&tx, &DigitalVoiceEngine::txModemReady,
                     [&modem](const QByteArray& pcm) { modem.append(pcm); });
    QByteArray decoded;
    QObject::connect(&rx, &DigitalVoiceEngine::rxSpeechReady,
                     [&decoded](const QByteArray& pcm) { decoded.append(pcm); });
    QObject::connect(&rx, &DigitalVoiceEngine::syncChanged,
                     [&r](bool s) { r.everSynced = r.everSynced || s; });
    QObject::connect(&rx, &DigitalVoiceEngine::snrChanged,
                     [&r](float snr) { r.lastSnr = snr; });
    QObject::connect(&rx, &DigitalVoiceEngine::textReceived,
                     [&r](const QString& t) { r.texts.append(t); });

    const QByteArray speech = makeSpeech(seconds);
    const int chunkBytes = kChunkFrames * 2 * static_cast<int>(sizeof(float));
    for (qsizetype off = 0; off + chunkBytes <= speech.size(); off += chunkBytes) {
        tx.feedTxAudio(speech.mid(off, chunkBytes));
    }
    r.txDurationRatio = static_cast<double>(modem.size()) / static_cast<double>(speech.size());
    r.modemRms = rmsStereo(modem);

    // Small AWGN (~20 dB below modem RMS), same noise on both channels.
    std::mt19937 rng(1234);
    std::normal_distribution<float> gauss(0.0f, static_cast<float>(r.modemRms * 0.1));
    auto* md = reinterpret_cast<float*>(modem.data());
    const qsizetype modemFrames = modem.size() / (2 * static_cast<qsizetype>(sizeof(float)));
    for (qsizetype i = 0; i < modemFrames; ++i) {
        const float nz = gauss(rng);
        md[2 * i] += nz;
        md[2 * i + 1] += nz;
    }

    for (qsizetype off = 0; off + chunkBytes <= modem.size(); off += chunkBytes) {
        rx.feedRxAudio(1, modem.mid(off, chunkBytes));
    }
    r.rxDurationRatio = modem.isEmpty() ? 0.0
        : static_cast<double>(decoded.size()) / static_cast<double>(modem.size());
    r.decodedRms = rmsStereo(decoded);
    tx.stop();
    rx.stop();
    return r;
}

void testMode(DigitalVoiceMode mode, double seconds, bool requireText)
{
    const QString name = digitalVoiceModeName(mode);
    std::printf("--- FreeDV %s loopback (%.0f s) ---\n", qPrintable(name), seconds);
    const LoopResult r = runLoopback(mode, seconds, QStringLiteral("K1ABC"));

    check(r.modemRate == 8000, "modem sample rate is 8 kHz", QString::number(r.modemRate));
    check(r.speechRate == 8000, "speech sample rate is 8 kHz", QString::number(r.speechRate));
    check(r.modemRms > 0.02, "TX produced modem signal", QString("rms=%1").arg(r.modemRms));
    check(std::abs(r.txDurationRatio - 1.0) < 0.1, "TX 24k in ≈ 24k modem out",
          QString("ratio=%1").arg(r.txDurationRatio));
    check(std::abs(r.rxDurationRatio - 1.0) < 0.1, "RX 24k modem in ≈ 24k speech out",
          QString("ratio=%1").arg(r.rxDurationRatio));
    check(r.everSynced, "RX acquired sync");
    check(r.lastSnr > 5.0f, "SNR estimate reported", QString("snr=%1 dB").arg(r.lastSnr));
    check(r.decodedRms > 0.01, "decoded speech energy", QString("rms=%1").arg(r.decodedRms));
    const bool gotText = r.texts.contains(QStringLiteral("K1ABC"));
    if (requireText) {
        check(gotText, "text channel round-trip", r.texts.join('|'));
    } else {
        std::printf("[INFO] text channel: %s\n", gotText ? "K1ABC received" : qPrintable(r.texts.join('|')));
    }
}

} // namespace

int main()
{
    check(FreeDvEngine::isSupported(), "codec2 support compiled in");

    // Unsupported mode must fail cleanly.
    {
        FreeDvEngine bad(DigitalVoiceMode::RadeV1);
        check(!bad.start() && !bad.isActive(), "non-codec2 mode refuses to start");
    }

    testMode(DigitalVoiceMode::FreeDv700E, 20.0, true);
    testMode(DigitalVoiceMode::FreeDv1600, 12.0, true);
    testMode(DigitalVoiceMode::FreeDv700D, 20.0, false);

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
