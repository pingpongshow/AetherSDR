// 2G ALE modem loopback tests: modulate calls / sounds / AMD, add noise and
// timing offset, demodulate with Ale2g::Demodulator + FrameAssembler and with
// the existing Ale2gDecoder (RX dialog decoder) and check exact content.
//
// Build: `ale2g_modem_test` (Qt6::Core).  Exit code 0 on all pass.

#include "core/Ale2gDecoder.h"
#include "core/Ale2gFrame.h"
#include "core/Ale2gModem.h"

#include <QByteArray>
#include <QStringList>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::Ale2g;

namespace {

int g_failures = 0;

void check(bool ok, const QString& what)
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.toUtf8().constData());
}

// Signal at amplitude 0.5 has power 0.125.  Noise sigma chosen for the given
// SNR in a 3 kHz bandwidth (white noise spread over 12 kHz Nyquist band).
std::vector<float> channel(const std::vector<float>& tx, int leadSamples, int tailSamples,
                           double snr3kDb, unsigned seed)
{
    std::mt19937 gen(seed);
    const double sigPower = 0.125;
    const double noise3k = sigPower / std::pow(10.0, snr3kDb / 10.0);
    const double sigma = std::sqrt(noise3k * 4.0);
    std::normal_distribution<double> nd(0.0, sigma);
    std::vector<float> out(static_cast<size_t>(leadSamples) + tx.size() + static_cast<size_t>(tailSamples));
    for (size_t i = 0; i < out.size(); ++i) {
        double s = 0.0;
        if (i >= static_cast<size_t>(leadSamples) && i - leadSamples < tx.size()) {
            s = tx[i - leadSamples];
        }
        out[i] = static_cast<float>(s + (snr3kDb < 90.0 ? nd(gen) : 0.0));
    }
    return out;
}

struct RxResult {
    std::vector<Frame> frames;
    QStringList words;
};

RxResult receive(const std::vector<float>& audio, int blockSize = 480)
{
    RxResult r;
    Demodulator demod;
    FrameAssembler assembler;
    std::vector<Demodulator::Event> events;
    for (size_t pos = 0; pos < audio.size(); pos += static_cast<size_t>(blockSize)) {
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(blockSize), audio.size() - pos));
        events.clear();
        demod.process(audio.data() + pos, n, &events);
        for (const auto& e : events) {
            if (e.type == Demodulator::Event::Type::Word) {
                r.words << wordToString(e.word.word) + (e.word.repeat ? QStringLiteral("*") : QString());
                if (auto f = assembler.addWord(e.word)) {
                    r.frames.push_back(*f);
                }
            } else if (auto f = assembler.flush()) {
                r.frames.push_back(*f);
            }
        }
    }
    if (auto f = assembler.flush()) {
        r.frames.push_back(*f);
    }
    return r;
}

const Frame* firstComplete(const RxResult& r)
{
    for (const Frame& f : r.frames) {
        if (f.complete) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    Modulator::Options opt;
    opt.amplitude = 0.5f;

    // ── Tone mapping (A.5.1.2).
    check(Modulator::bitsForToneIndex(0) == 0b000 && Modulator::bitsForToneIndex(2) == 0b011
              && Modulator::bitsForToneIndex(4) == 0b110 && Modulator::bitsForToneIndex(7) == 0b100,
          QStringLiteral("tone/bit Gray mapping matches A.5.1.2"));
    bool inverse = true;
    for (int i = 0; i < 8; ++i) {
        inverse = inverse && Modulator::toneIndexForBits(Modulator::bitsForToneIndex(i)) == i;
    }
    check(inverse, QStringLiteral("tone mapping inverse"));

    // ── Waveform timing: 49 symbols x 192 samples per redundant word = 392 ms.
    const QVector<std::uint32_t> one{makeWord(Preamble::Tis, 'A', 'B', 'C')};
    check(Modulator::modulate(one, opt).size() == 9408u, QStringLiteral("one redundant word = 9408 samples (392 ms)"));

    // ── Individual scanning call with AMD, K1ABC calls W2XYZ.
    const QString amdText = QStringLiteral("HELLO FROM AETHERSDR 73");
    const QVector<std::uint32_t> call = Frames::buildCall(QStringLiteral("W2XYZ"), QStringLiteral("K1ABC"),
                                                         true, 8, amdText);
    const std::vector<float> callPcm = Modulator::modulate(call, opt);

    for (double snr : {99.0, 12.0, 6.0}) {
        const RxResult r = receive(channel(callPcm, 1237, 12000, snr, 42u));
        const Frame* f = firstComplete(r);
        const QString tag = QStringLiteral("call+AMD @ SNR %1 dB: ").arg(snr >= 90 ? QStringLiteral("inf") : QString::number(snr));
        check(f != nullptr, tag + QStringLiteral("complete frame decoded"));
        if (!f) {
            std::printf("   words: %s\n", r.words.join(QStringLiteral(" | ")).toUtf8().constData());
            continue;
        }
        check(f->toAddresses == QStringList{QStringLiteral("W2XYZ")},
              tag + QStringLiteral("TO W2XYZ (got %1)").arg(f->toAddresses.join(QLatin1Char(','))));
        check(f->tis && f->fromAddress == QStringLiteral("K1ABC"),
              tag + QStringLiteral("TIS K1ABC (got %1)").arg(f->fromAddress));
        check(f->amdMessages == QStringList{amdText},
              tag + QStringLiteral("AMD text exact (got \"%1\")").arg(f->amdMessages.join(QLatin1Char('|'))));
        std::printf("   BER code %d, mean SNR %.1f dB, words %d\n", f->berCode(), f->meanSnrDb(), f->wordCount);
    }

    // ── Low SNR and sample-clock offset (radio vs. remote soundcard, 100 ppm).
    {
        std::vector<float> drifted;
        const double ratio = 1.0 + 100e-6;
        for (double x = 0.0; x + 1.0 < static_cast<double>(callPcm.size()); x += ratio) {
            const size_t i = static_cast<size_t>(x);
            const double frac = x - static_cast<double>(i);
            drifted.push_back(static_cast<float>((1.0 - frac) * callPcm[i] + frac * callPcm[i + 1]));
        }
        for (double snr : {3.0, 0.0}) {
            const RxResult r = receive(channel(drifted, 4321, 12000, snr, 1234u));
            const Frame* f = firstComplete(r);
            check(f && f->toAddresses == QStringList{QStringLiteral("W2XYZ")}
                      && f->fromAddress == QStringLiteral("K1ABC") && f->amdMessages == QStringList{amdText},
                  QStringLiteral("call+AMD @ SNR %1 dB (3 kHz) with 100 ppm clock offset").arg(snr));
            if (f) {
                std::printf("   BER code %d, mean SNR %.1f dB\n", f->berCode(), f->meanSnrDb());
            } else {
                std::printf("   words: %s\n", r.words.join(QStringLiteral(" | ")).toUtf8().constData());
            }
        }
    }

    // ── TWAS sound with 2-word address (TWAS W2X DATA YZ@ ...).
    {
        const QVector<std::uint32_t> sound = Frames::buildSound(QStringLiteral("W2XYZ"), false, 6);
        const RxResult r = receive(channel(Modulator::modulate(sound, opt), 555, 12000, 10.0, 7u));
        const Frame* f = firstComplete(r);
        check(f && f->isSound() && f->twas && f->fromAddress == QStringLiteral("W2XYZ"),
              QStringLiteral("TWAS sound W2XYZ decoded as sound (words: %1)").arg(r.words.join(QStringLiteral(" | "))));
    }

    // ── Response frame (no scanning call): TO K1ABC x2, TIS W2XYZ.
    {
        const QVector<std::uint32_t> resp = Frames::buildResponse(QStringLiteral("K1ABC"), QStringLiteral("W2XYZ"), true);
        check(resp.size() == 6, QStringLiteral("response = 2 x (TO K1A, DATA BC@) + TIS W2X, DATA YZ@"));
        const RxResult r = receive(channel(Modulator::modulate(resp, opt), 3000, 12000, 10.0, 99u));
        const Frame* f = firstComplete(r);
        check(f && f->toAddresses == QStringList{QStringLiteral("K1ABC")} && f->tis
                  && f->fromAddress == QStringLiteral("W2XYZ"),
              QStringLiteral("short response decoded (words: %1)").arg(r.words.join(QStringLiteral(" | "))));
    }

    // ── AllCall with AMD.
    {
        const QVector<std::uint32_t> ac = Frames::buildAllCall(QStringLiteral("K1ABC"), true, 4,
                                                               QStringLiteral("CQ CQ"));
        const RxResult r = receive(channel(Modulator::modulate(ac, opt), 100, 12000, 10.0, 5u));
        const Frame* f = firstComplete(r);
        check(f && f->allCall && f->toAddresses.isEmpty() && f->fromAddress == QStringLiteral("K1ABC")
                  && f->amdMessages == QStringList{QStringLiteral("CQ CQ")},
              QStringLiteral("AllCall @?@ with AMD decoded"));
    }

    // ── Existing RX dialog decoder (Ale2gDecoder) decodes the TX waveform.
    {
        Ale2gDecoder decoder;
        QStringList decoded;
        QObject::connect(&decoder, &Ale2gDecoder::wordDecoded, &decoder,
                         [&decoded](const QString&, const QString& pre, const QString& text, int, float,
                                    const QString&) { decoded << pre + QLatin1Char(' ') + text; });
        const std::vector<float> rx = channel(callPcm, 777, 12000, 15.0, 3u);
        for (size_t pos = 0; pos < rx.size(); pos += 480) {
            const int n = static_cast<int>(std::min<size_t>(480, rx.size() - pos));
            decoder.feedAudio(Modulator::toStereoFloat32(rx.data() + pos, n));
        }
        check(decoded.contains(QStringLiteral("TO W2X")) && decoded.contains(QStringLiteral("DATA YZ@"))
                  && decoded.contains(QStringLiteral("TIS K1A")) && decoded.contains(QStringLiteral("DATA BC@")),
              QStringLiteral("Ale2gDecoder (RX dialog) decodes TO W2X / DATA YZ@ / TIS K1A / DATA BC@ (got %1 words)")
                  .arg(decoded.size()));
    }

    // ── Noise only: no words.
    {
        std::vector<float> silence(24000 * 20, 0.0f);
        const RxResult r = receive(channel(silence, 0, 0, 0.0, 11u));
        check(r.words.isEmpty(), QStringLiteral("20 s of noise: no false words (got %1)").arg(r.words.size()));
    }

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
