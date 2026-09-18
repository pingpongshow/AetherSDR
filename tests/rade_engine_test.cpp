// RADE V1 tests (no radio, no network, nothing transmitted):
//  1. RadeEooText codec: encode/decode round trip, charset handling, noise
//     and phase-free AWGN robustness, CRC rejection of corrupted frames.
//  2. RADEEngine loopback with the real bundled RADE library: synthetic
//     speech → feedTxAudio → modem (24 kHz) → requestEndOfOver() (EOO frame
//     carrying the callsign) → small AWGN → feedRxAudio → sync, decoded
//     speech, and the EOO callsign arriving via textReceived().
//
// Build: `rade_engine_test` (needs HAVE_RADE).  Exit code 0 on all pass.

#include "core/RADEEngine.h"
#include "core/RadeEooText.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

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
constexpr int kChunkFrames = 480;  // 20 ms
constexpr double kPi = 3.141592653589793;
constexpr int kEooBits = 180;      // typical rade_n_eoo_bits(); codec only needs >= 112

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
        for (int h = 1; h <= 40; ++h) {
            const double fh = f0 * h;
            if (fh > 7000.0) {
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

double rms(const QByteArray& pcm)
{
    const auto* d = reinterpret_cast<const float*>(pcm.constData());
    const qsizetype n = pcm.size() / static_cast<qsizetype>(sizeof(float));
    double acc = 0.0;
    for (qsizetype i = 0; i < n; ++i) {
        acc += static_cast<double>(d[i]) * d[i];
    }
    return n > 0 ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

void testEooCodec()
{
    std::printf("--- RADE EOO callsign codec ---\n");
    for (const char* call : {"K1ABC", "VK5DGR", "W1AW/7", "N0", "KK7GWY"}) {
        std::vector<float> bits(kEooBits);
        const bool enc = RadeEooText::encode(QString::fromLatin1(call), bits.data(), kEooBits);
        const auto res = RadeEooText::decode(bits.data(), kEooBits);
        check(enc && res.ok && res.callsign == QLatin1String(call), "clean round trip",
              QStringLiteral("%1 -> %2").arg(QLatin1String(call), res.callsign));
    }
    check(RadeEooText::sanitize(QStringLiteral(" kk7gwy-12 ")) == QStringLiteral("KK7GWY-1"),
          "sanitize: upper-case, 8 chars max", RadeEooText::sanitize(QStringLiteral(" kk7gwy-12 ")));
    check(RadeEooText::sanitize(QStringLiteral("a b_c")) == QStringLiteral("ABC"),
          "sanitize: drops chars outside the OTA charset",
          RadeEooText::sanitize(QStringLiteral("a b_c")));
    std::vector<float> small(100);
    check(!RadeEooText::encode(QStringLiteral("K1ABC"), small.data(), 100),
          "encode refuses < 112 EOO bits");

    // AWGN on the soft symbols, random amplitude scale (receiver gain).
    std::mt19937 rng(42);
    for (double snrDb : {12.0, 6.0, 3.0}) {
        const double sigma = std::sqrt(0.5 * std::pow(10.0, -snrDb / 10.0));
        std::normal_distribution<float> g(0.0f, static_cast<float>(sigma));
        int ok = 0;
        constexpr int kTrials = 200;
        for (int t = 0; t < kTrials; ++t) {
            std::vector<float> bits(kEooBits);
            RadeEooText::encode(QStringLiteral("VK5DGR"), bits.data(), kEooBits);
            for (float& b : bits) {
                b = 0.37f * (b + g(rng));
            }
            const auto res = RadeEooText::decode(bits.data(), kEooBits);
            ok += (res.ok && res.callsign == QStringLiteral("VK5DGR")) ? 1 : 0;
        }
        const double rate = static_cast<double>(ok) / kTrials;
        const double need = snrDb >= 6.0 ? 0.99 : 0.80;
        check(rate >= need, "decode rate under AWGN",
              QStringLiteral("Es/N0=%1 dB: %2 %").arg(snrDb).arg(rate * 100.0, 0, 'f', 1));
    }

    // Garbage must never produce a callsign (LDPC + CRC gate).
    std::normal_distribution<float> noise(0.0f, 1.0f);
    int falseAccepts = 0;
    for (int t = 0; t < 500; ++t) {
        std::vector<float> bits(kEooBits);
        for (float& b : bits) {
            b = noise(rng);
        }
        falseAccepts += RadeEooText::decode(bits.data(), kEooBits).ok ? 1 : 0;
    }
    check(falseAccepts == 0, "pure noise never decodes", QString::number(falseAccepts));
}

void testEngineLoopback()
{
    std::printf("--- RADEEngine loopback (with EOO callsign) ---\n");
    RADEEngine tx;
    RADEEngine rx;
    if (!tx.start() || !rx.start()) {
        check(false, "RADE engines start");
        return;
    }
    tx.setTxText(QStringLiteral("vk5dgr"));
    check(tx.txCallsign() == QStringLiteral("VK5DGR"), "TX callsign sanitised", tx.txCallsign());

    QByteArray modem;
    int eooSignals = 0;
    QObject::connect(&tx, &DigitalVoiceEngine::txModemReady,
                     [&modem](const QByteArray& pcm) { modem.append(pcm); });
    QObject::connect(&tx, &DigitalVoiceEngine::endOfOverSent, [&eooSignals] { ++eooSignals; });

    const QByteArray speech = makeSpeech(6.0);
    const int chunkBytes = kChunkFrames * 2 * static_cast<int>(sizeof(float));
    for (qsizetype off = 0; off + chunkBytes <= speech.size(); off += chunkBytes) {
        tx.feedTxAudio(speech.mid(off, chunkBytes));
    }
    const qsizetype voiceBytes = modem.size();
    tx.requestEndOfOver();
    const qsizetype eooBytes = modem.size() - voiceBytes;
    check(eooSignals == 1, "endOfOverSent emitted once", QString::number(eooSignals));
    check(eooBytes > 0, "EOO frame + tail emitted",
          QStringLiteral("%1 ms").arg(eooBytes / (2.0 * sizeof(float)) / kRate * 1000.0, 0, 'f', 1));
    // After EOO, mic audio is ignored until resetTx().
    tx.feedTxAudio(speech.left(chunkBytes * 20));
    check(modem.size() == voiceBytes + eooBytes, "TX gated after EOO until resetTx()");
    tx.requestEndOfOver();
    check(eooSignals == 2 && modem.size() == voiceBytes + eooBytes,
          "second EOO request is a no-op that still signals");
    check(std::abs(static_cast<double>(voiceBytes) / speech.size() - 1.0) < 0.1,
          "TX 24k speech ≈ 24k modem duration",
          QString::number(static_cast<double>(voiceBytes) / speech.size()));

    // ~20 dB SNR AWGN, then 1 s of silence so the RX sees the EOO and drops out.
    const double modemRms = rms(modem.left(voiceBytes));
    modem.append(QByteArray(kRate * 2 * static_cast<int>(sizeof(float)), '\0'));
    std::mt19937 rng(7);
    std::normal_distribution<float> gauss(0.0f, static_cast<float>(modemRms * 0.1));
    auto* md = reinterpret_cast<float*>(modem.data());
    const qsizetype frames = modem.size() / (2 * static_cast<qsizetype>(sizeof(float)));
    for (qsizetype i = 0; i < frames; ++i) {
        const float nz = gauss(rng);
        md[2 * i] += nz;
        md[2 * i + 1] += nz;
    }

    bool everSynced = false;
    QStringList texts;
    QByteArray decoded;
    QObject::connect(&rx, &DigitalVoiceEngine::syncChanged,
                     [&everSynced](bool s) { everSynced = everSynced || s; });
    QObject::connect(&rx, &DigitalVoiceEngine::textReceived,
                     [&texts](const QString& t) { texts.append(t); });
    QObject::connect(&rx, &DigitalVoiceEngine::rxSpeechReady,
                     [&decoded](const QByteArray& pcm) { decoded.append(pcm); });
    // Pad to whole chunks.
    const qsizetype pad = (chunkBytes - modem.size() % chunkBytes) % chunkBytes;
    modem.append(QByteArray(pad, '\0'));
    for (qsizetype off = 0; off + chunkBytes <= modem.size(); off += chunkBytes) {
        rx.feedRxAudio(1, modem.mid(off, chunkBytes));
    }
    check(everSynced, "RX acquired sync");
    check(rms(decoded) > 0.005, "decoded speech energy", QString::number(rms(decoded)));
    check(texts.contains(QStringLiteral("VK5DGR")), "EOO callsign decoded by RX",
          texts.join(QLatin1Char('|')));

    tx.resetTx();
    const qsizetype before = modem.size();
    QByteArray more;
    QObject::connect(&tx, &DigitalVoiceEngine::txModemReady,
                     [&more](const QByteArray& pcm) { more.append(pcm); });
    for (int i = 0; i < 20; ++i) {
        tx.feedTxAudio(speech.mid(i * chunkBytes, chunkBytes));
    }
    Q_UNUSED(before);
    check(!more.isEmpty(), "resetTx() re-enables TX after EOO");
    tx.stop();
    rx.stop();
}

} // namespace

int main()
{
    testEooCodec();
    testEngineLoopback();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
