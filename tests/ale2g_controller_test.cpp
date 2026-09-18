// 2G ALE controller state-machine tests with two controllers connected by a
// simulated audio channel and a fake clock (no radio, no real time):
//   1. call -> response -> acknowledgment -> LINKED on both sides, AMD in the
//      link, TWAS link termination
//   2. called station with TX disabled: no response, caller times out
//   3. TX master switch off: no PTT and no audio ever emitted
//   4. radio never confirms PTT: controller unkeys and reports an error
//   5. TWAS sound is heard and logged with SNR/BER
//   6. disabling TX mid-frame unkeys immediately
//   7. scanning receiver (3 channels) is captured by an auto-length scanning call
//   8. radio already transmitting (operator MOX / other client): ALE neither
//      retunes nor keys, reports the error
//   9. channel list edited while a multi-channel call is queued: stale
//      indices are remapped by frequency (no out-of-range access, no TX on a
//      frequency the call was not queued for)
//
// Build: `ale2g_controller_test` (Qt6::Core).  Exit code 0 on all pass.

#include "core/Ale2gController.h"

#include <QByteArray>
#include <QCoreApplication>

#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <vector>

using namespace AetherSDR;
using State = Ale2gController::State;

namespace {

int g_failures = 0;
qint64 g_nowMs = 0;
bool g_verbose = false;
bool g_feedAudio = true;   // scenario 3 skips demodulation to stay fast

void check(bool ok, const QString& what)
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.toUtf8().constData());
}

struct Station {
    QString name;
    std::unique_ptr<Ale2gController> ctrl;
    std::deque<float> onAir;          // audio queued by the modem, played in real time
    bool keyed{false};
    bool confirmTx{true};
    qint64 tunedHz{0};
    int keyOnCount{0};
    int keyOffCount{0};
    int audioChunks{0};
    bool audioWhileUnkeyed{false};
    QStringList amds;
    QStringList errors;
    QStringList logs;
    QStringList links;

    explicit Station(const QString& n, const QString& self)
        : name(n)
        , ctrl(std::make_unique<Ale2gController>(nullptr, false))
    {
        ctrl->setClock([]() { return g_nowMs; });
        ctrl->setSelfAddresses({self});
        ctrl->setChannels({Ale2gChannel{QStringLiteral("07D"), 7102000, true}});
        ctrl->setDwellMs(500);
        ctrl->setListenBeforeTransmitMs(500);
        ctrl->setReplyTimeoutMs(6000);

        QObject::connect(ctrl.get(), &Ale2gController::requestTransmit, ctrl.get(), [this](bool on) {
            keyed = on;
            (on ? keyOnCount : keyOffCount) += 1;
            if (confirmTx) {
                ctrl->setRadioTransmitting(on);
            } else if (!on) {
                ctrl->setRadioTransmitting(false);
            }
        });
        QObject::connect(ctrl.get(), &Ale2gController::requestTune, ctrl.get(),
                         [this](qint64 hz) { tunedHz = hz; });
        QObject::connect(ctrl.get(), &Ale2gController::txAudio, ctrl.get(), [this](const QByteArray& pcm) {
            ++audioChunks;
            if (!keyed) {
                audioWhileUnkeyed = true;
            }
            const auto* s = reinterpret_cast<const float*>(pcm.constData());
            const int frames = pcm.size() / static_cast<int>(2 * sizeof(float));
            for (int i = 0; i < frames; ++i) {
                onAir.push_back(s[2 * i]);
            }
        });
        QObject::connect(ctrl.get(), &Ale2gController::amdReceived, ctrl.get(),
                         [this](const QString& from, const QString& text) { amds << from + QLatin1Char(':') + text; });
        QObject::connect(ctrl.get(), &Ale2gController::errorOccurred, ctrl.get(),
                         [this](const QString& e) { errors << e; });
        QObject::connect(ctrl.get(), &Ale2gController::linkEstablished, ctrl.get(),
                         [this](const QString& r) { links << r; });
        QObject::connect(ctrl.get(), &Ale2gController::logMessage, ctrl.get(), [this](const QString& m) {
            logs << m;
            if (g_verbose) {
                std::printf("   %6lld ms %s: %s\n", static_cast<long long>(g_nowMs),
                            name.toUtf8().constData(), m.toUtf8().constData());
            }
        });
    }
};

std::mt19937 g_rng(1234);

// One 10 ms step: tick both controllers, then deliver 240 samples of each
// station's on-air audio (plus light noise) to the other one.
void step(Station& a, Station& b)
{
    constexpr int kStepMs = 10;
    constexpr int kSamples = 240;
    g_nowMs += kStepMs;
    a.ctrl->tick();
    b.ctrl->tick();
    if (!g_feedAudio) {
        a.onAir.clear();
        b.onAir.clear();
        return;
    }
    std::normal_distribution<float> noise(0.0f, 0.02f);
    std::vector<float> toB(kSamples);
    std::vector<float> toA(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        float sa = 0.0f;
        float sb = 0.0f;
        if (!a.onAir.empty()) {
            sa = a.onAir.front();
            a.onAir.pop_front();
        }
        if (!b.onAir.empty()) {
            sb = b.onAir.front();
            b.onAir.pop_front();
        }
        const bool sameChannel = a.tunedHz == b.tunedHz;
        toB[static_cast<size_t>(i)] = (sameChannel ? sa : 0.0f) + noise(g_rng);
        toA[static_cast<size_t>(i)] = (sameChannel ? sb : 0.0f) + noise(g_rng);
    }
    b.ctrl->feedRxMono(toB.data(), kSamples);
    a.ctrl->feedRxMono(toA.data(), kSamples);
}

template <typename Pred>
bool runUntil(Station& a, Station& b, qint64 maxMs, Pred pred)
{
    const qint64 end = g_nowMs + maxMs;
    while (g_nowMs < end) {
        step(a, b);
        if (pred()) {
            return true;
        }
    }
    return false;
}

void run(Station& a, Station& b, qint64 ms)
{
    runUntil(a, b, ms, []() { return false; });
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    g_verbose = argc > 1 && QByteArray(argv[1]) == "-v";

    // ── 1. Full three-way handshake, AMD, termination ─────────────────────
    {
        Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
        Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
        a.ctrl->setTransmitEnabled(true);
        b.ctrl->setTransmitEnabled(true);
        a.ctrl->start();
        b.ctrl->start();
        run(a, b, 1000);

        check(a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("1: call() accepted"));
        check(a.ctrl->state() == State::Calling, QStringLiteral("1: caller in CALLING"));
        bool sawResponding = false;
        const bool linked = runUntil(a, b, 40000, [&]() {
            sawResponding = sawResponding || b.ctrl->state() == State::Responding;
            return a.ctrl->state() == State::Linked && b.ctrl->state() == State::Linked;
        });
        check(sawResponding, QStringLiteral("1: called station went RESPONDING"));
        check(linked, QStringLiteral("1: both stations LINKED (t=%1 ms)").arg(g_nowMs));
        check(a.ctrl->remoteAddress() == QStringLiteral("W2XYZ") && b.ctrl->remoteAddress() == QStringLiteral("K1ABC"),
              QStringLiteral("1: remote addresses correct"));
        check(a.keyOnCount == 2 && b.keyOnCount == 1,
              QStringLiteral("1: caller keyed twice (call, ack), called once (response): A=%1 B=%2")
                  .arg(a.keyOnCount)
                  .arg(b.keyOnCount));

        run(a, b, 1000);
        check(a.ctrl->sendAmd(QStringLiteral("QSL? 73 de K1ABC")), QStringLiteral("1: AMD queued in link"));
        const bool amdOk = runUntil(a, b, 20000, [&]() { return !b.amds.isEmpty(); });
        check(amdOk && b.amds.value(0) == QStringLiteral("K1ABC:QSL? 73 DE K1ABC"),
              QStringLiteral("1: AMD received exactly (got \"%1\")").arg(b.amds.join(QLatin1Char('|'))));
        check(b.ctrl->state() == State::Linked, QStringLiteral("1: AMD in link does not break link"));

        run(a, b, 3000);
        b.ctrl->terminateLink();
        const bool terminated = runUntil(a, b, 20000, [&]() {
            return a.ctrl->state() == State::Scanning && b.ctrl->state() == State::Scanning;
        });
        check(terminated, QStringLiteral("1: TWAS termination returns both to SCANNING"));
        run(a, b, 1000);
        check(!a.keyed && !b.keyed && a.keyOnCount == a.keyOffCount && b.keyOnCount == b.keyOffCount,
              QStringLiteral("1: every PTT on matched by PTT off"));
        check(!a.audioWhileUnkeyed && !b.audioWhileUnkeyed, QStringLiteral("1: no TX audio while unkeyed"));
        check(a.errors.isEmpty() && b.errors.isEmpty(),
              QStringLiteral("1: no errors (A: %1 | B: %2)").arg(a.errors.join(';'), b.errors.join(';')));
    }

    // ── 2. Called station has TX disabled -> caller times out ─────────────
    {
        Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
        Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
        a.ctrl->setTransmitEnabled(true);
        a.ctrl->start();
        b.ctrl->start();
        run(a, b, 500);
        check(a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("2: call() accepted"));
        run(a, b, 3000);
        const bool back = runUntil(a, b, 30000, [&]() { return a.ctrl->state() == State::Scanning; });
        check(back, QStringLiteral("2: caller returns to SCANNING after reply timeout"));
        check(a.errors.join(';').contains(QStringLiteral("no response")),
              QStringLiteral("2: caller reports no response"));
        check(b.keyOnCount == 0 && b.audioChunks == 0,
              QStringLiteral("2: TX-disabled station never keyed or produced audio"));
        check(b.logs.join(';').contains(QStringLiteral("transmit disabled, not responding")),
              QStringLiteral("2: TX-disabled station logged the call it could not answer"));
        check(b.ctrl->state() == State::Scanning, QStringLiteral("2: called station stays SCANNING"));
        check(!a.keyed && a.keyOnCount == a.keyOffCount, QStringLiteral("2: caller unkeyed"));
    }

    // ── 3. TX master switch off: nothing is ever transmitted ──────────────
    {
        Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
        Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
        a.ctrl->start();
        a.ctrl->setSoundingIntervalMinutes(30);
        check(!a.ctrl->transmitEnabled(), QStringLiteral("3: transmit disabled by default"));
        check(!a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("3: call() refused"));
        check(!a.ctrl->allCall(QStringLiteral("CQ")), QStringLiteral("3: allCall() refused"));
        check(!a.ctrl->soundNow(), QStringLiteral("3: soundNow() refused"));
        g_feedAudio = false;
        run(a, b, 31L * 60L * 1000L);   // past the sounding interval
        g_feedAudio = true;
        check(a.keyOnCount == 0 && a.keyOffCount == 0 && a.audioChunks == 0,
              QStringLiteral("3: no requestTransmit and no txAudio in 31 minutes"));
    }

    // ── 4. Radio never confirms PTT -> unkey + error ──────────────────────
    {
        Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
        Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
        a.confirmTx = false;
        a.ctrl->setTransmitEnabled(true);
        a.ctrl->start();
        check(a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("4: call() accepted"));
        run(a, b, 8000);
        check(a.keyOnCount == 1 && a.keyOffCount == 1 && !a.keyed,
              QStringLiteral("4: PTT requested once then released"));
        check(a.audioChunks == 0, QStringLiteral("4: no audio sent without TX confirmation"));
        check(a.errors.join(';').contains(QStringLiteral("did not confirm")),
              QStringLiteral("4: error reported"));
        check(a.ctrl->state() == State::Scanning, QStringLiteral("4: back to SCANNING"));
    }

    // ── 5. Sound heard + LQA entry ────────────────────────────────────────
    {
        Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
        Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
        a.ctrl->setTransmitEnabled(true);
        a.ctrl->setSoundLengthMs(3000);
        a.ctrl->start();
        b.ctrl->start();
        check(a.ctrl->soundNow(), QStringLiteral("5: soundNow() accepted"));
        const bool heard = runUntil(a, b, 20000, [&]() { return !b.ctrl->heardStations().isEmpty(); });
        check(heard && b.ctrl->heardStations().first().address == QStringLiteral("K1ABC")
                  && b.ctrl->heardStations().first().lastKind == QStringLiteral("TWAS sound"),
              QStringLiteral("5: TWAS sound from K1ABC logged in heard list"));
        if (heard) {
            const auto& h = b.ctrl->heardStations().first();
            std::printf("   heard %s on %lld Hz, SNR %.1f dB, BER %d\n", h.address.toUtf8().constData(),
                        static_cast<long long>(h.frequencyHz), h.snrDb, h.berCode);
        }
        run(a, b, 2000);
        check(a.ctrl->state() == State::Scanning && !a.keyed, QStringLiteral("5: sounder back to SCANNING, unkeyed"));
        check(b.keyOnCount == 0, QStringLiteral("5: TWAS sound does not trigger a response"));
    }

    // ── 6. TX disabled mid-frame -> immediate unkey ───────────────────────
    {
        Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
        Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
        a.ctrl->setTransmitEnabled(true);
        a.ctrl->start();
        a.ctrl->call(QStringLiteral("W2XYZ"));
        const bool sending = runUntil(a, b, 5000, [&]() { return a.audioChunks > 5; });
        check(sending && a.keyed, QStringLiteral("6: transmitting"));
        const int chunks = a.audioChunks;
        a.ctrl->setTransmitEnabled(false);
        check(!a.keyed && a.keyOffCount == 1, QStringLiteral("6: unkeyed synchronously when TX disabled"));
        run(a, b, 5000);
        check(a.audioChunks == chunks && a.keyOnCount == 1, QStringLiteral("6: no further audio or PTT"));
    }

    // ── 7. Multi-channel scanning: called station scans 3 channels ────────
    {
        const QVector<Ale2gChannel> chans{
            Ale2gChannel{QStringLiteral("07D"), 7102000, false},
            Ale2gChannel{QStringLiteral("10D"), 10145500, false},
            Ale2gChannel{QStringLiteral("14B"), 14109000, false},
        };
        Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
        Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
        a.ctrl->setChannels(chans);
        b.ctrl->setChannels(chans);
        a.ctrl->setTransmitEnabled(true);
        b.ctrl->setTransmitEnabled(true);
        a.ctrl->start();
        a.ctrl->stop();                 // caller parks on 07D, not scanning
        b.ctrl->start();                // called station scans 07D/10D/14B
        run(a, b, 1300);
        check(b.ctrl->state() == State::Scanning && a.ctrl->state() == State::Idle,
              QStringLiteral("7: receiver scanning, caller idle"));
        check(a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("7: call() accepted"));
        const bool linked = runUntil(a, b, 40000, [&]() {
            return a.ctrl->state() == State::Linked && b.ctrl->state() == State::Linked;
        });
        check(linked, QStringLiteral("7: scanning station captured by scanning call and LINKED"));
        check(a.tunedHz == 7102000 && b.tunedHz == 7102000, QStringLiteral("7: both on 07D"));
        run(a, b, 500);
        check(!a.keyed && !b.keyed, QStringLiteral("7: both unkeyed"));
    }

    // ── 8. Radio already transmitting from another source ────────────────
    {
        const QVector<Ale2gChannel> chans{
            Ale2gChannel{QStringLiteral("07D"), 7102000, false},
            Ale2gChannel{QStringLiteral("10D"), 10145500, false},
        };
        Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
        Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
        a.ctrl->setChannels(chans);
        a.ctrl->setMaxCallAttempts(2);
        a.ctrl->setTransmitEnabled(true);
        a.ctrl->start();
        run(a, b, 200);
        const qint64 tunedBefore = a.tunedHz;
        a.ctrl->setRadioTransmitting(true);   // operator keyed MOX on the radio
        check(a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("8: call() accepted (queued)"));
        run(a, b, 200);
        check(a.keyOnCount == 0 && a.audioChunks == 0,
              QStringLiteral("8: no PTT and no audio while radio is already transmitting"));
        check(a.tunedHz == tunedBefore, QStringLiteral("8: slice not retuned under a foreign transmission"));
        check(a.errors.join(';').contains(QStringLiteral("already transmitting")),
              QStringLiteral("8: error reported"));
        check(a.ctrl->state() == State::Scanning, QStringLiteral("8: back to SCANNING"));
        a.ctrl->setRadioTransmitting(false);
        check(a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("8: call accepted after radio unkeyed"));
        const bool keyed = runUntil(a, b, 5000, [&]() { return a.keyOnCount > 0; });
        check(keyed, QStringLiteral("8: keys normally once the radio is free"));
        a.ctrl->stop();
        check(!a.keyed, QStringLiteral("8: unkeyed on stop"));
    }

    // ── 9. Channel list edited while a call is queued ─────────────────────
    {
        const QVector<Ale2gChannel> chans{
            Ale2gChannel{QStringLiteral("07D"), 7102000, false},
            Ale2gChannel{QStringLiteral("10D"), 10145500, false},
            Ale2gChannel{QStringLiteral("14B"), 14109000, false},
        };
        // 9a: shrink to a list without the queued channels.
        {
            Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
            Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
            g_feedAudio = false;
            a.ctrl->setChannels(chans);
            a.ctrl->setMaxCallAttempts(3);
            a.ctrl->setTransmitEnabled(true);
            a.ctrl->start();
            check(a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("9a: call() accepted"));
            a.ctrl->setChannels({Ale2gChannel{QStringLiteral("18D"), 18106000, false}});
            run(a, b, 30000);
            check(a.keyOnCount == 0, QStringLiteral("9a: nothing transmitted on a channel the call was not queued for"));
            check(a.ctrl->state() == State::Scanning, QStringLiteral("9a: call cancelled, back to SCANNING"));
            g_feedAudio = true;
        }
        // 9b: reorder and drop the middle channel mid-call.
        {
            Station a(QStringLiteral("A"), QStringLiteral("K1ABC"));
            Station b(QStringLiteral("B"), QStringLiteral("W2XYZ"));
            g_feedAudio = false;
            QVector<qint64> keyedOn;
            QObject::connect(a.ctrl.get(), &Ale2gController::requestTransmit, a.ctrl.get(),
                             [&](bool on) { if (on) { keyedOn << a.tunedHz; } });
            a.ctrl->setChannels(chans);
            a.ctrl->setMaxCallAttempts(3);
            a.ctrl->setScanCallMs(2000);
            a.ctrl->setTransmitEnabled(true);
            a.ctrl->start();
            check(a.ctrl->call(QStringLiteral("W2XYZ")), QStringLiteral("9b: call() accepted"));
            a.ctrl->setChannels({chans.at(2), chans.at(0)});   // 14B, 07D
            run(a, b, 40000);
            check(keyedOn == QVector<qint64>{7102000, 14109000},
                  QStringLiteral("9b: attempts on 07D then 14B only (got %1 attempts)").arg(keyedOn.size()));
            check(!a.keyed && a.ctrl->state() == State::Scanning,
                  QStringLiteral("9b: unkeyed and back to SCANNING after no response"));
            g_feedAudio = true;
        }
    }

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
