// CW skimmer tests: synthesize DAX-IQ-like complex baseband containing
// several keyed CW carriers at different offsets and speeds, run it through
// CwSkimmerEngine (directly, and through the threaded CwSkimmer facade with
// big-endian VITA payloads), and verify the right callsigns are spotted at
// the right frequencies.  Exit code 0 on all pass.
//
// Usage: cw_skimmer_test [path/to/cty.dat]

#include "core/CwSkimmer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {})
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %-60s %s\n", ok ? " OK " : "FAIL", what.c_str(), detail.c_str());
}

const std::map<char, const char*>& morse()
{
    static const std::map<char, const char*> m = {
        {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},  {'E', "."},
        {'F', "..-."}, {'G', "--."},  {'H', "...."}, {'I', ".."},   {'J', ".---"},
        {'K', "-.-"},  {'L', ".-.."}, {'M', "--"},   {'N', "-."},   {'O', "---"},
        {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
        {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"}, {'Y', "-.--"},
        {'Z', "--.."},
        {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
        {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
        {'/', "-..-."},
    };
    return m;
}

struct CwSource {
    double      offsetHz;
    int         wpm;
    double      startSec;
    std::string text;
    double      amplitude{1.0};
};

// Keying envelope (0/1 with 5 ms raised-cosine edges) sampled at fs.
std::vector<float> keyingEnvelope(const CwSource& s, int fs, std::size_t total)
{
    std::vector<float> env(total, 0.0f);
    const double unit = 1.2 / s.wpm;
    double t = s.startSec;
    std::vector<std::pair<double, double>> marks;
    for (char c : s.text) {
        if (c == ' ') {
            t += 4 * unit;  // + 3 already added after the previous char = 7
            continue;
        }
        const auto it = morse().find(c);
        if (it == morse().end()) {
            continue;
        }
        for (const char* p = it->second; *p; ++p) {
            const double len = (*p == '.') ? unit : 3 * unit;
            marks.emplace_back(t, t + len);
            t += len + unit;
        }
        t += 2 * unit;
    }
    const double edge = 0.005;
    for (const auto& [a, b] : marks) {
        const std::size_t i0 = static_cast<std::size_t>(std::max(0.0, a * fs));
        const std::size_t i1 = std::min(total, static_cast<std::size_t>((b + edge) * fs));
        for (std::size_t i = i0; i < i1; ++i) {
            const double tt = static_cast<double>(i) / fs;
            double g = 1.0;
            if (tt < a + edge) {
                g = 0.5 - 0.5 * std::cos(std::numbers::pi * (tt - a) / edge);
            } else if (tt > b) {
                g = 0.5 + 0.5 * std::cos(std::numbers::pi * (tt - b) / edge);
            }
            env[i] = std::max(env[i], static_cast<float>(g));
        }
    }
    return env;
}

std::vector<std::complex<float>> synthesize(const std::vector<CwSource>& srcs, int fs,
                                            double seconds, double noiseSnrDbIn150Hz,
                                            unsigned seed)
{
    const std::size_t total = static_cast<std::size_t>(seconds * fs);
    std::vector<std::complex<float>> iq(total);
    for (const CwSource& s : srcs) {
        const std::vector<float> env = keyingEnvelope(s, fs, total);
        const double w = 2.0 * std::numbers::pi * s.offsetHz / fs;
        for (std::size_t i = 0; i < total; ++i) {
            if (env[i] > 0.0f) {
                const double ph = w * static_cast<double>(i);
                iq[i] += std::complex<float>(static_cast<float>(s.amplitude * env[i] * std::cos(ph)),
                                             static_cast<float>(s.amplitude * env[i] * std::sin(ph)));
            }
        }
    }
    // Complex white noise: total power P over fs, so 150 Hz holds P*150/fs.
    const double noiseIn150 = 1.0 / std::pow(10.0, noiseSnrDbIn150Hz / 10.0);
    const double totalPower = noiseIn150 * fs / 150.0;
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, static_cast<float>(std::sqrt(totalPower / 2.0)));
    for (std::complex<float>& v : iq) {
        v += std::complex<float>(nd(rng), nd(rng));
    }
    return iq;
}

struct Expect {
    const char* call;
    double      offsetHz;
    int         wpm;
    const char* dxcc;
};

void verifySpots(const char* tag, const std::vector<CwSkimmerSpot>& spots,
                 const std::vector<Expect>& expect, double centerHz, bool ctyLoaded,
                 const std::vector<const char*>& mustNotSpot)
{
    for (const Expect& e : expect) {
        const CwSkimmerSpot* hit = nullptr;
        for (const CwSkimmerSpot& s : spots) {
            if (s.callsign == QLatin1String(e.call)) {
                hit = &s;
                break;
            }
        }
        char buf[200];
        if (!hit) {
            check(false, std::string(tag) + ": spotted " + e.call, "not found");
            continue;
        }
        const double ferr = hit->freqHz - (centerHz + e.offsetHz);
        std::snprintf(buf, sizeof buf, "f err %+.1f Hz, %d wpm, snr %.1f dB, cq=%d, dxcc=%s",
                      ferr, hit->wpm, hit->snrDb, hit->isCq ? 1 : 0,
                      hit->dxccPrefix.toLatin1().constData());
        const bool wpmOk = std::abs(hit->wpm - e.wpm) <= std::max(3, e.wpm / 5);
        const bool dxccOk = !ctyLoaded || hit->dxccPrefix == QLatin1String(e.dxcc);
        check(std::abs(ferr) < 25.0 && wpmOk && hit->isCq && dxccOk,
              std::string(tag) + ": " + e.call + " at right offset/speed", buf);
    }
    for (const char* bad : mustNotSpot) {
        bool found = false;
        for (const CwSkimmerSpot& s : spots) {
            found = found || s.callsign == QLatin1String(bad);
        }
        check(!found, std::string(tag) + ": did not spot " + bad);
    }
    int unexpected = 0;
    std::string names;
    for (const CwSkimmerSpot& s : spots) {
        bool known = false;
        for (const Expect& e : expect) {
            known = known || s.callsign == QLatin1String(e.call);
        }
        if (!known) {
            ++unexpected;
            names += s.callsign.toStdString() + " ";
        }
    }
    check(unexpected == 0, std::string(tag) + ": no false callsigns", names);
}

const std::vector<CwSource> kSources = {
    {-7985.0, 18, 0.30, "CQ TEST W1AW W1AW  CQ TEST W1AW"},
    {+3020.0, 25, 0.80, "CQ CQ DE K9XYZ K9XYZ K  W3LPL DE K9XYZ TU"},
    {+12470.0, 32, 1.20, "CQ TEST DL1ABC DL1ABC  CQ DL1ABC TEST"},
};
const std::vector<Expect> kExpect = {
    {"W1AW", -7985.0, 18, "K"},
    {"K9XYZ", 3020.0, 25, "K"},
    {"DL1ABC", 12470.0, 32, "DL"},
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Callsign pattern ────────────────────────────────────────────────
    {
        bool ok = true;
        for (const char* c : {"W1AW", "K9XYZ", "DL1ABC", "VK2XYZ", "9A1A", "JA1ABC",
                              "E71A", "G4ABC/P", "PA/DL1ABC", "3DA0RS"}) {
            if (!CwSkimmerEngine::looksLikeCallsign(QString::fromLatin1(c))) {
                std::printf("   rejected valid %s\n", c);
                ok = false;
            }
        }
        for (const char* c : {"CQ", "TEST", "5NN", "599", "73", "TU", "K", "DE", "R5", "QRZ"}) {
            if (CwSkimmerEngine::looksLikeCallsign(QString::fromLatin1(c))) {
                std::printf("   accepted non-call %s\n", c);
                ok = false;
            }
        }
        check(ok, "callsign regex accepts calls, rejects CW jargon");
    }

    CtyDatParser cty;
    const bool ctyLoaded = argc > 1 && cty.loadFromFile(QString::fromLocal8Bit(argv[1]));
    std::printf("cty.dat %s\n", ctyLoaded ? "loaded" : "not supplied — skipping DXCC checks");

    const double centerHz = 14'030'000.0;

    // ── Engine direct, 48 kHz, 20 dB SNR in 150 Hz ──────────────────────
    for (int fs : {48000, 96000, 192000}) {
        const std::vector<std::complex<float>> iq = synthesize(kSources, fs, 32.0, 20.0, 1234);
        CwSkimmerEngine eng(fs);
        eng.setCenterFrequencyHz(centerHz);
        if (ctyLoaded) {
            eng.setCtyParser(&cty);
        }
        std::vector<CwSkimmerSpot> spots;
        std::map<int, std::string> text;
        eng.onSpot([&](const CwSkimmerSpot& s) { spots.push_back(s); });
        eng.onText([&](int id, double, const QString& t) { text[id] += t.toStdString(); });

        QElapsedTimer timer;
        timer.start();
        // Feed in DAX-IQ-sized packets (~128 complex samples at 48 kHz scaled)
        const std::size_t pkt = static_cast<std::size_t>(fs / 375);
        for (std::size_t i = 0; i < iq.size(); i += pkt) {
            eng.process(iq.data() + i, std::min(pkt, iq.size() - i));
        }
        const double cpu = timer.nsecsElapsed() / 1e9;

        const std::string tag = std::to_string(fs / 1000) + "k";
        for (const CwSkimmerChannelInfo& c : eng.channels()) {
            std::printf("   ch%-2d %+9.1f Hz %5.1f dB %4.1f wpm | %s\n", c.id, c.offsetHz,
                        c.snrDb, c.wpm, text[c.id].c_str());
        }
        std::printf("   %s: %.2f s CPU for 32 s of IQ (%.1f%% of real time), %zu channels\n",
                    tag.c_str(), cpu, 100.0 * cpu / 32.0, eng.channels().size());
        verifySpots(tag.c_str(), spots, kExpect, centerHz, ctyLoaded, {"W3LPL"});
        check(eng.channels().size() <= 6, tag + ": no channel explosion from noise",
              std::to_string(eng.channels().size()));
    }

    // ── Weaker signals: 12 dB in 150 Hz ─────────────────────────────────
    {
        const int fs = 48000;
        const std::vector<std::complex<float>> iq = synthesize(kSources, fs, 32.0, 12.0, 99);
        CwSkimmerEngine eng(fs);
        eng.setCenterFrequencyHz(centerHz);
        if (ctyLoaded) {
            eng.setCtyParser(&cty);
        }
        std::vector<CwSkimmerSpot> spots;
        std::map<int, std::string> text;
        eng.onSpot([&](const CwSkimmerSpot& s) { spots.push_back(s); });
        eng.onText([&](int id, double, const QString& t) { text[id] += t.toStdString(); });
        eng.process(iq.data(), iq.size());
        for (const CwSkimmerChannelInfo& c : eng.channels()) {
            std::printf("   ch%-2d %+9.1f Hz %4.1f wpm | %s\n", c.id, c.offsetHz, c.wpm,
                        text[c.id].c_str());
        }
        verifySpots("48k@12dB", spots, kExpect, centerHz, ctyLoaded, {"W3LPL"});
    }

    // ── Pure noise: no channels, no spots ───────────────────────────────
    {
        const int fs = 48000;
        const std::vector<std::complex<float>> iq = synthesize({}, fs, 20.0, 20.0, 4242);
        CwSkimmerEngine eng(fs);
        int spots = 0;
        std::size_t maxChannels = 0;
        eng.onSpot([&](const CwSkimmerSpot&) { ++spots; });
        const std::size_t blk = 4800;
        for (std::size_t i = 0; i < iq.size(); i += blk) {
            eng.process(iq.data() + i, std::min(blk, iq.size() - i));
            maxChannels = std::max(maxChannels, eng.channels().size());
        }
        check(spots == 0 && maxChannels == 0, "pure noise: no carriers, no spots",
              "max channels " + std::to_string(maxChannels));
    }

    // ── Channel cap is honoured ─────────────────────────────────────────
    {
        const int fs = 24000;
        std::vector<CwSource> many;
        for (int i = 0; i < 10; ++i) {
            many.push_back({-10000.0 + i * 2000.0 + 13.0, 20, 0.1 * i, "CQ TEST N0CALL"});
        }
        const std::vector<std::complex<float>> iq = synthesize(many, fs, 5.0, 20.0, 7);
        CwSkimmerConfig cfg;
        cfg.maxChannels = 4;
        CwSkimmerEngine eng(fs, cfg);
        eng.process(iq.data(), iq.size());
        check(eng.channels().size() <= 4, "maxChannels cap (10 carriers, cap 4)",
              std::to_string(eng.channels().size()));
    }

    // ── Threaded facade with big-endian VITA payloads ───────────────────
    {
        const int fs = 48000;
        const std::vector<std::complex<float>> iq = synthesize(kSources, fs, 32.0, 20.0, 555);
        CwSkimmer sk;
        if (argc > 1) {
            sk.setCtyDatPath(QString::fromLocal8Bit(argv[1]));
        }
        sk.setDaxIqChannel(2);
        sk.setCenterFrequencyMhz(centerHz / 1e6);
        std::vector<CwSkimmerSpot> spots;
        int tableUpdates = 0;
        QObject::connect(&sk, &CwSkimmer::spotDetected,
                         [&](const CwSkimmerSpot& s) { spots.push_back(s); });
        QObject::connect(&sk, &CwSkimmer::channelsUpdated,
                         [&](const QVector<CwSkimmerChannelInfo>&) { ++tableUpdates; });
        sk.start();

        const std::size_t pkt = 128;  // complex samples per DAX IQ packet
        QByteArray payload(static_cast<int>(pkt * 8), Qt::Uninitialized);
        for (std::size_t i = 0; i + pkt <= iq.size(); i += pkt) {
            auto* dst = reinterpret_cast<uchar*>(payload.data());
            for (std::size_t k = 0; k < pkt; ++k) {
                quint32 bi = 0;
                quint32 bq = 0;
                const float fi = iq[i + k].real();
                const float fq = iq[i + k].imag();
                std::memcpy(&bi, &fi, 4);
                std::memcpy(&bq, &fq, 4);
                qToBigEndian(bi, dst + 8 * k);
                qToBigEndian(bq, dst + 8 * k + 4);
            }
            while (sk.backlog() > CwSkimmer::kMaxBacklogPackets / 2) {
                QThread::usleep(200);
            }
            sk.feedRawIqPacket(2, payload, fs);
            if ((i / pkt) % 50 == 0) {
                sk.feedRawIqPacket(1, payload, fs);  // other DAX IQ channel: ignored
            }
            if ((i / pkt) % 64 == 0) {
                QCoreApplication::processEvents();
            }
        }
        QElapsedTimer t;
        t.start();
        while ((sk.backlog() > 0 || spots.size() < kExpect.size()) && t.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        QCoreApplication::processEvents();
        verifySpots("threaded", spots, kExpect, centerHz, argc > 1 && ctyLoaded, {"W3LPL"});
        check(tableUpdates > 10, "threaded: channel table published", std::to_string(tableUpdates));
        sk.stop();
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
