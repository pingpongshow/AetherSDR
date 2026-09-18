// Scanner state-machine tests with a fake slice, a fake S-meter and a
// virtual clock (no radio, no real time, no AppSettings):
//   1. stops on a signal above threshold, logs the hit and its peak
//   2. meter samples inside the settle window after a retune are ignored
//   3. resume modes: Time (resume after N s), Carrier (resume after drop), Hold
//   4. skip list: skipCurrent locks a channel out; locked channels never tuned;
//      all-locked-out list refuses to start
//   5. priority channel is checked every N s, scan returns; a signal on the
//      priority channel stops the scan there
//   6. memory mode/filter applied per channel; original freq/mode/filter
//      restored on user stop; tuneTo stops without restoring
//   7. TX / MOX pauses the scan with zero retunes; resumes afterwards; start
//      refused while transmitting; stop during TX defers restore to TX end
//   8. squelch-open based detection
//   9. list builders: band channels, current band, memory selection, list parse
//  10. slice disappears mid-scan -> abort without touching it
//
// Build: `scanner_test` (Qt6::Core). Exit code 0 on all pass.

#include "core/Scanner.h"
#include "models/BandDefs.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

using namespace AetherSDR;
using State = Scanner::State;

namespace {

int    g_failures = 0;
qint64 g_nowMs = 0;

void check(bool ok, const QString& what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) {
        ++g_failures;
    }
}

// Shared state of the fake slice, owned by the test (the Scanner owns the
// ScanTarget wrapper, which only references this).
struct FakeSliceState {
    int     id{0};
    bool    valid{true};
    bool    locked{false};
    double  freq{14.100};
    QString mode{"USB"};
    int     filterLow{100};
    int     filterHigh{2800};
    int     tuneCount{0};
    int     modeCount{0};
    int     filterCount{0};
    std::vector<double> tuneLog;
};

class FakeTarget : public ScanTarget {
public:
    explicit FakeTarget(FakeSliceState* s) : m_s(s) {}
    bool    isValid() const override { return m_s->valid; }
    int     sliceId() const override { return m_s->id; }
    bool    isLocked() const override { return m_s->locked; }
    double  frequencyMhz() const override { return m_s->freq; }
    QString mode() const override { return m_s->mode; }
    int     filterLow() const override { return m_s->filterLow; }
    int     filterHigh() const override { return m_s->filterHigh; }
    void tune(double mhz) override
    {
        m_s->freq = mhz;
        ++m_s->tuneCount;
        m_s->tuneLog.push_back(mhz);
    }
    void setMode(const QString& mode) override
    {
        m_s->mode = mode;
        ++m_s->modeCount;
    }
    void setFilter(int low, int high) override
    {
        m_s->filterLow = low;
        m_s->filterHigh = high;
        ++m_s->filterCount;
    }

private:
    FakeSliceState* m_s;
};

// Fake RF world: signal level per frequency (kHz key), noise elsewhere.
struct FakeWorld {
    std::map<qint64, float> carriers;   // key = Hz
    float noise{-125.0f};
    void set(double mhz, float dbm) { carriers[Scanner::channelKey(mhz)] = dbm; }
    void clear(double mhz) { carriers.erase(Scanner::channelKey(mhz)); }
    float level(double mhz) const
    {
        auto it = carriers.find(Scanner::channelKey(mhz));
        return it == carriers.end() ? noise : it->second;
    }
};

struct Rig {
    FakeSliceState slice;
    FakeWorld      world;
    std::unique_ptr<Scanner> scanner;
    std::vector<ScanHit> hits;
    std::map<int, float> peaks;
    int finishedCount{0};
    bool lastFinishedRestored{false};
    QStringList errors;
    bool meterOn{true};

    Rig()
    {
        g_nowMs = 1000;
        scanner = std::make_unique<Scanner>(nullptr, false);
        scanner->setClock([]() { return g_nowMs; });
        scanner->setTarget(std::make_unique<FakeTarget>(&slice));
        QObject::connect(scanner.get(), &Scanner::hitAdded, [this](const ScanHit& h) {
            hits.push_back(h);
            peaks[h.id] = h.peakDbm;
        });
        QObject::connect(scanner.get(), &Scanner::hitPeakUpdated, [this](int id, float p) {
            peaks[id] = p;
        });
        QObject::connect(scanner.get(), &Scanner::finished, [this](bool restored) {
            ++finishedCount;
            lastFinishedRestored = restored;
        });
        QObject::connect(scanner.get(), &Scanner::errorOccurred, [this](const QString& e) {
            errors.append(e);
        });
    }

    // Advance virtual time in 10 ms steps; the radio meter reports every 50 ms
    // for whatever frequency the slice is on right now.
    void run(int ms)
    {
        for (int t = 0; t < ms; t += 10) {
            g_nowMs += 10;
            if (meterOn && g_nowMs % 50 == 0) {
                scanner->onSignalLevel(slice.id, world.level(slice.freq));
            }
            scanner->tick();
        }
    }

    // Run until the slice sits on `mhz` (or timeout). Returns true on success.
    bool runUntilOn(double mhz, int timeoutMs)
    {
        for (int t = 0; t < timeoutMs; t += 10) {
            if (Scanner::channelKey(slice.freq) == Scanner::channelKey(mhz)) {
                return true;
            }
            run(10);
        }
        return Scanner::channelKey(slice.freq) == Scanner::channelKey(mhz);
    }
};

ScanChannel chan(double mhz, const QString& mode = {}, int lo = 0, int hi = 0,
                 const QString& label = {})
{
    ScanChannel c;
    c.freqMhz = mhz;
    c.mode = mode;
    c.filterLow = lo;
    c.filterHigh = hi;
    c.label = label;
    return c;
}

QVector<ScanChannel> threeChannels()
{
    return {chan(7.100), chan(7.150), chan(7.200)};
}

Scanner::Config baseConfig()
{
    Scanner::Config cfg;
    cfg.dwellMs = 300;
    cfg.settleMs = 150;
    cfg.thresholdDbm = -100.0f;
    cfg.resume = Scanner::ResumeMode::Time;
    cfg.resumeMs = 3000;
    cfg.carrierDropMs = 1000;
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────

void testStopOnSignal()
{
    std::printf("\n[1] stop on signal\n");
    Rig r;
    r.scanner->setConfig(baseConfig());
    r.scanner->setChannels(threeChannels());
    r.world.set(7.150, -80.0f);

    check(r.scanner->start(), "start succeeds");
    check(r.slice.freq == 7.100, "first channel tuned");
    r.run(250);
    check(r.scanner->state() == State::Scanning, "quiet channel keeps scanning");
    r.run(400);
    check(r.scanner->state() == State::Stopped, "stops on the active channel");
    check(Scanner::channelKey(r.slice.freq) == Scanner::channelKey(7.150), "sits on 7.150");
    check(r.hits.size() == 1 && Scanner::channelKey(r.hits[0].freqMhz) == Scanner::channelKey(7.150),
          "activity log entry for 7.150");
    check(r.hits.size() == 1 && std::fabs(r.hits[0].peakDbm - (-80.0f)) < 0.01f,
          "hit carries level at stop");

    r.world.set(7.150, -70.0f);
    r.run(200);
    check(!r.hits.empty() && std::fabs(r.peaks[r.hits[0].id] - (-70.0f)) < 0.01f,
          "peak level updated while stopped");
    check(r.slice.tuneCount == 2, "exactly two retunes (7.100, 7.150)");
}

void testSettleWindow()
{
    std::printf("\n[2] settle window ignores stale meter samples\n");
    Rig r;
    Scanner::Config cfg = baseConfig();
    cfg.settleMs = 200;
    r.scanner->setConfig(cfg);
    r.scanner->setChannels(threeChannels());
    r.meterOn = false;

    check(r.scanner->start(), "start");
    // A strong reading belonging to the previous frequency arrives right
    // after the retune: it must not stop the scan.
    g_nowMs += 20;
    r.scanner->onSignalLevel(r.slice.id, -50.0f);
    r.scanner->tick();
    check(r.scanner->state() == State::Scanning, "stale sample during settle ignored");

    // A sample for another slice is ignored as well.
    g_nowMs += 250;
    r.scanner->onSignalLevel(r.slice.id + 1, -50.0f);
    r.scanner->tick();
    check(r.scanner->state() == State::Scanning, "other slice's meter ignored");

    // After settle, a real sample stops it.
    r.scanner->onSignalLevel(r.slice.id, -60.0f);
    check(r.scanner->state() == State::Stopped, "post-settle sample stops the scan");
}

void testResumeModes()
{
    std::printf("\n[3] resume modes\n");
    {
        Rig r;
        Scanner::Config cfg = baseConfig();
        cfg.resume = Scanner::ResumeMode::Time;
        cfg.resumeMs = 2000;
        r.scanner->setConfig(cfg);
        r.scanner->setChannels(threeChannels());
        r.world.set(7.100, -80.0f);
        r.scanner->start();
        r.run(300);
        check(r.scanner->state() == State::Stopped, "Time: stopped on 7.100");
        r.run(1500);
        check(r.scanner->state() == State::Stopped, "Time: still stopped before timeout");
        r.run(400);   // stopped at t=1150, resume at 3150, next dwell ends 3450
        check(Scanner::channelKey(r.slice.freq) == Scanner::channelKey(7.150),
              "Time: resumed to next channel after timeout although signal remains");
    }
    {
        Rig r;
        Scanner::Config cfg = baseConfig();
        cfg.resume = Scanner::ResumeMode::Carrier;
        cfg.carrierDropMs = 1000;
        r.scanner->setConfig(cfg);
        r.scanner->setChannels(threeChannels());
        r.world.set(7.100, -80.0f);
        r.scanner->start();
        r.run(300);
        check(r.scanner->state() == State::Stopped, "Carrier: stopped on 7.100");
        r.run(6000);
        check(r.scanner->state() == State::Stopped, "Carrier: holds while carrier present");
        r.world.set(7.100, -102.0f);   // within hysteresis: still "present"
        r.run(1500);
        check(r.scanner->state() == State::Stopped, "Carrier: hysteresis keeps the stop");
        r.world.clear(7.100);
        r.run(800);
        check(r.scanner->state() == State::Stopped, "Carrier: waits for drop delay");
        r.run(400);
        check(Scanner::channelKey(r.slice.freq) == Scanner::channelKey(7.150),
              "Carrier: resumes after carrier drop delay");
    }
    {
        Rig r;
        Scanner::Config cfg = baseConfig();
        cfg.resume = Scanner::ResumeMode::Hold;
        r.scanner->setConfig(cfg);
        r.scanner->setChannels(threeChannels());
        r.world.set(7.100, -80.0f);
        r.scanner->start();
        r.run(300);
        r.world.clear(7.100);
        r.run(20000);
        check(r.scanner->state() == State::Stopped
                  && Scanner::channelKey(r.slice.freq) == Scanner::channelKey(7.100),
              "Hold: stays on channel indefinitely");
        r.scanner->resume();
        check(r.scanner->state() == State::Scanning
                  && Scanner::channelKey(r.slice.freq) == Scanner::channelKey(7.150),
              "Hold: user resume moves on");
    }
}

void testSkipList()
{
    std::printf("\n[4] skip list\n");
    Rig r;
    r.scanner->setConfig(baseConfig());
    r.scanner->setChannels(threeChannels());
    r.world.set(7.150, -80.0f);
    int skipSignals = 0;
    QObject::connect(r.scanner.get(), &Scanner::skipListChanged, [&]() { ++skipSignals; });

    r.scanner->start();
    r.run(700);
    check(r.scanner->state() == State::Stopped, "stopped on birdie 7.150");
    r.scanner->skipCurrent();
    check(r.scanner->isSkipped(7.150), "7.150 added to skip list");
    check(skipSignals == 1, "skipListChanged emitted");
    check(Scanner::channelKey(r.slice.freq) == Scanner::channelKey(7.200), "moved on to 7.200");

    r.slice.tuneLog.clear();
    r.run(3000);
    bool visited = false;
    for (double f : r.slice.tuneLog) {
        if (Scanner::channelKey(f) == Scanner::channelKey(7.150)) {
            visited = true;
        }
    }
    check(!visited, "locked-out channel never tuned again");
    check(r.hits.size() == 1, "no further stops on the locked-out birdie");

    r.scanner->stop();
    r.scanner->setSkipList({Scanner::channelKey(7.100), Scanner::channelKey(7.150),
                            Scanner::channelKey(7.200)});
    check(!r.scanner->start(), "start refused when all channels locked out");
    check(!r.errors.isEmpty() && r.errors.last().contains("locked out"), "error reported");
    r.scanner->removeSkip(7.200);
    check(r.scanner->start() && Scanner::channelKey(r.slice.freq) == Scanner::channelKey(7.200),
          "un-skipped channel scannable again");
}

void testPriority()
{
    std::printf("\n[5] priority channel\n");
    Rig r;
    Scanner::Config cfg = baseConfig();
    cfg.priorityEnabled = true;
    cfg.priorityChannel = chan(146.520, "FM", 0, 0, "Calling");
    cfg.priorityIntervalMs = 2000;
    cfg.priorityDwellMs = 300;
    r.scanner->setConfig(cfg);
    r.scanner->setChannels(threeChannels());
    r.scanner->start();

    r.run(1900);
    int priVisits = 0;
    for (double f : r.slice.tuneLog) {
        priVisits += Scanner::channelKey(f) == Scanner::channelKey(146.520) ? 1 : 0;
    }
    check(priVisits == 0, "no priority check before the interval");
    r.run(200);
    check(Scanner::channelKey(r.slice.freq) == Scanner::channelKey(146.520)
              && r.scanner->isCheckingPriority(),
          "priority channel checked after interval");
    check(r.slice.mode == "FM", "priority channel mode applied");
    r.run(400);
    check(!r.scanner->isCheckingPriority()
              && Scanner::channelKey(r.slice.freq) != Scanner::channelKey(146.520),
          "quiet priority channel: scan returns");
    r.run(2000);
    priVisits = 0;
    for (double f : r.slice.tuneLog) {
        priVisits += Scanner::channelKey(f) == Scanner::channelKey(146.520) ? 1 : 0;
    }
    check(priVisits == 2, "priority re-checked every interval");

    r.world.set(146.520, -90.0f);
    check(r.runUntilOn(146.520, 3000), "back on priority channel");
    r.run(300);
    check(r.scanner->state() == State::Stopped && !r.hits.empty() && r.hits.back().priority,
          "signal on priority channel stops scan (priority hit)");
    check(r.scanner->currentChannel().label == "Calling", "current channel is priority");

    // Keep-mode channels go back to the pre-scan mode after an FM priority check.
    r.world.clear(146.520);
    r.scanner->resume();
    check(r.slice.mode == "USB", "pre-scan mode restored on keep-mode channel after priority");

    // Slice locked mid-scan (by the user): pause, no further retunes.
    r.slice.locked = true;
    const int tunes = r.slice.tuneCount;
    r.run(2000);
    check(r.scanner->state() == State::Paused && r.slice.tuneCount == tunes,
          "slice locked mid-scan pauses without retuning");
}

void testRestoreAndMemories()
{
    std::printf("\n[6] memory mode/filter + restore on stop\n");
    Rig r;
    r.slice.freq = 14.225;
    r.slice.mode = "USB";
    r.slice.filterLow = 100;
    r.slice.filterHigh = 2800;
    r.scanner->setConfig(baseConfig());
    r.scanner->setChannels({chan(7.074, "DIGU", 0, 3000, "FT8"),
                            chan(3.885, "AM", -5000, 5000, "AM net"),
                            chan(10.000)});
    r.scanner->start();
    check(r.slice.mode == "DIGU" && r.slice.filterLow == 0 && r.slice.filterHigh == 3000,
          "memory 1 mode + filter applied");
    r.run(350);
    check(r.slice.mode == "AM" && r.slice.filterLow == -5000 && r.slice.filterHigh == 5000,
          "memory 2 mode + filter applied");
    r.run(350);
    check(Scanner::channelKey(r.slice.freq) == Scanner::channelKey(10.000) && r.slice.mode == "USB"
              && r.slice.filterLow == 100 && r.slice.filterHigh == 2800,
          "channel without mode uses the pre-scan mode/filter");

    r.scanner->stop();
    check(r.scanner->state() == State::Idle, "idle after stop");
    check(Scanner::channelKey(r.slice.freq) == Scanner::channelKey(14.225)
              && r.slice.mode == "USB" && r.slice.filterLow == 100 && r.slice.filterHigh == 2800,
          "original freq/mode/filter restored");
    check(r.finishedCount == 1 && r.lastFinishedRestored, "finished(restored=true)");

    const int tunes = r.slice.tuneCount;
    r.run(2000);
    check(r.slice.tuneCount == tunes, "no radio changes after stop");

    // Click-to-tune from the log: stop without restoring, tune to the hit.
    r.scanner->start();
    r.run(100);
    check(r.scanner->tuneTo(chan(3.885, "AM")), "tuneTo accepted");
    check(r.scanner->state() == State::Idle
              && Scanner::channelKey(r.slice.freq) == Scanner::channelKey(3.885)
              && r.slice.mode == "AM",
          "tuneTo stops scan and stays on the chosen frequency (no restore)");

    // Locked slice refuses to start.
    r.slice.locked = true;
    check(!r.scanner->start(), "locked slice refuses to start");
}

void testPauseOnTx()
{
    std::printf("\n[7] pause on TX / MOX\n");
    Rig r;
    r.slice.freq = 14.225;
    r.scanner->setConfig(baseConfig());
    r.scanner->setChannels(threeChannels());
    r.scanner->start();
    r.run(500);

    r.scanner->setMox(true);
    check(r.scanner->state() == State::PausedTx, "MOX pauses the scan");
    const int tunes = r.slice.tuneCount;
    const double txFreq = r.slice.freq;
    r.world.set(txFreq, -20.0f);   // own TX leaking into the meter
    r.run(5000);
    check(r.slice.tuneCount == tunes && r.slice.freq == txFreq, "zero retunes during MOX");
    check(r.hits.empty(), "no stop logged from TX meter readings");
    r.scanner->skipCurrent();
    check(r.slice.tuneCount == tunes, "skip during TX does not retune");

    r.world.clear(txFreq);
    r.scanner->setMox(false);
    check(r.scanner->state() == State::Scanning, "resumes after MOX released");
    r.run(700);
    check(r.slice.tuneCount > tunes, "scanning continues after TX");

    // Interlock TX (e.g. other client / PTT) also pauses.
    r.scanner->setTransmitting(true);
    check(r.scanner->state() == State::PausedTx, "interlock TX pauses the scan");
    // User stops during TX: restore is deferred to TX end.
    const int tunesBefore = r.slice.tuneCount;
    r.scanner->stop();
    check(r.scanner->state() == State::Idle && r.slice.tuneCount == tunesBefore,
          "stop during TX does not retune");
    check(!r.scanner->start(), "start refused while transmitting");
    r.scanner->setTransmitting(false);
    check(Scanner::channelKey(r.slice.freq) == Scanner::channelKey(14.225),
          "deferred restore applied when TX ends");
    check(r.lastFinishedRestored, "finished(restored=true) after TX end");

    // User pause survives TX.
    r.scanner->start();
    r.scanner->pause();
    r.scanner->setMox(true);
    r.scanner->setMox(false);
    check(r.scanner->state() == State::Paused, "user pause is kept after TX ends");
}

void testSquelchDetection()
{
    std::printf("\n[8] squelch-based detection\n");
    Rig r;
    Scanner::Config cfg = baseConfig();
    cfg.detect = Scanner::DetectSource::Squelch;
    cfg.resume = Scanner::ResumeMode::Carrier;
    cfg.carrierDropMs = 500;
    r.scanner->setConfig(cfg);
    r.scanner->setChannels(threeChannels());
    r.world.set(7.100, -60.0f);   // strong S-meter reading, but squelch closed
    r.scanner->start();
    r.run(250);
    r.scanner->setSquelchOpen(false);
    r.run(100);
    check(r.scanner->state() == State::Scanning, "S-meter ignored in squelch mode");
    check(r.runUntilOn(7.200, 1000), "reaches 7.200");
    r.run(200);
    r.scanner->setSquelchOpen(true);
    check(r.scanner->state() == State::Stopped, "squelch open stops the scan");
    r.run(1000);
    check(r.scanner->state() == State::Stopped, "held while squelch open");
    r.scanner->setSquelchOpen(false);
    r.run(600);
    check(r.scanner->state() == State::Scanning, "resumes after squelch closes");
}

void testBuilders()
{
    std::printf("\n[9] list builders\n");
    const QVector<ScanChannel> band = Scanner::bandChannels(14.000, 14.010, 2500, "CW");
    check(band.size() == 5, "14.000-14.010 @ 2.5 kHz = 5 channels");
    check(band.size() == 5 && Scanner::channelKey(band[4].freqMhz) == 14010000
              && band[0].mode == "CW",
          "band channel values");
    check(Scanner::bandChannels(1.0, 30.0, 1, {}, 100).size() == 100, "channel cap honoured");
    check(Scanner::bandChannels(14.1, 14.0, 50000).size() == 3, "reversed start/stop swapped");

    const BandDef* b = Scanner::bandForFrequency(7.150);
    check(b && QString(b->name) == "40m", "7.150 is 40m");
    check(Scanner::bandForFrequency(8.0) == nullptr, "8 MHz is no amateur band");

    QVector<ScanChannel> mems;
    for (int i = 0; i < 4; ++i) {
        ScanChannel c = chan(7.0 + i * 0.1);
        c.memoryIndex = i;
        c.group = i < 2 ? "Nets" : "Beacons";
        mems.append(c);
    }
    check(Scanner::selectMemories(mems, Scanner::MemorySelect::All, {}, {}).size() == 4, "All");
    check(Scanner::selectMemories(mems, Scanner::MemorySelect::Group, "nets", {}).size() == 2,
          "Group (case-insensitive)");
    const QVector<ScanChannel> sel =
        Scanner::selectMemories(mems, Scanner::MemorySelect::Selected, {}, {1, 3});
    check(sel.size() == 2 && sel[0].memoryIndex == 1 && sel[1].memoryIndex == 3, "Selected");

    QStringList errs;
    const QVector<ScanChannel> list = Scanner::parseFrequencyList(
        "14.074 DIGU FT8 20m\n7074k digu\n# comment\n10136 kHz;bogus;146.52 fm Simplex", &errs);
    check(list.size() == 4, "4 entries parsed");
    check(list.size() == 4 && Scanner::channelKey(list[0].freqMhz) == 14074000
              && list[0].mode == "DIGU" && list[0].label == "FT8 20m",
          "freq + mode + label");
    check(list.size() == 4 && Scanner::channelKey(list[1].freqMhz) == 7074000
              && list[1].mode == "DIGU",
          "kHz suffix");
    check(list.size() == 4 && Scanner::channelKey(list[2].freqMhz) == 10136000
              && list[2].mode.isEmpty(),
          "separate kHz unit token");
    check(list.size() == 4 && list[3].mode == "FM" && list[3].label == "Simplex", "';' separator");
    check(errs.size() == 1 && errs[0] == "bogus", "bad entry reported");
}

void testSliceVanishes()
{
    std::printf("\n[10] slice disappears mid-scan\n");
    Rig r;
    r.scanner->setConfig(baseConfig());
    r.scanner->setChannels(threeChannels());
    r.scanner->start();
    r.run(200);
    r.slice.valid = false;
    const int tunes = r.slice.tuneCount;
    r.run(100);
    check(r.scanner->state() == State::Idle, "scan aborted");
    check(r.slice.tuneCount == tunes, "no commands to a vanished slice");
    check(r.finishedCount == 1 && !r.lastFinishedRestored, "finished(restored=false)");
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    testStopOnSignal();
    testSettleWindow();
    testResumeModes();
    testSkipList();
    testPriority();
    testRestoreAndMemories();
    testPauseOnTx();
    testSquelchDetection();
    testBuilders();
    testSliceVanishes();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
