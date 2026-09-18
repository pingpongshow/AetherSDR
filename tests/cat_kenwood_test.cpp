// Unit tests for the SmartSDR-CAT-compatible Kenwood/ZZ CAT protocol
// (src/core/CatKenwoodProtocol.*) and its TCP / PTY transports.
//
// All tests run against FakeRadio — no RadioModel, no network traffic to a
// radio. The TCP test uses a loopback socket; the PTY test uses openpty().
//
// Build: produced by CMake as `cat_kenwood_test`. Exit code 0 on all pass.

#include "core/CatKenwoodProtocol.h"
#include "core/CatKenwoodServer.h"
#include "core/CatKenwoodPty.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpSocket>

#include <cstdio>
#include <functional>
#include <map>
#include <string>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace AetherSDR;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
}

void expectEq(const QByteArray& got, const QByteArray& want, const std::string& what)
{
    const bool ok = (got == want);
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %-44s got '%s'%s%s%s\n", ok ? " OK " : "FAIL", what.c_str(),
                got.constData(), ok ? "" : " want '", ok ? "" : want.constData(), ok ? "" : "'");
}

// ── Fake radio ──────────────────────────────────────────────────────────────

class FakeRadio : public CatRadioInterface {
public:
    FakeRadio()
    {
        CatSliceState a;
        a.id = 0;
        a.freqHz = 14074000;
        a.mode = "DIGU";
        a.modeList = {"LSB", "USB", "CW", "AM", "SAM", "FM", "NFM", "DFM", "DIGU", "DIGL", "RTTY"};
        a.filterLow = 0;
        a.filterHigh = 3000;
        a.active = true;
        a.tx = true;
        a.agcMode = "med";
        a.sMeterDbm = -73.0f;
        slices[0] = a;
    }

    CatSliceState& addSlice(int id, long long hz, const QString& mode)
    {
        CatSliceState s = slices[0];
        s.id = id;
        s.freqHz = hz;
        s.mode = mode;
        s.active = false;
        s.tx = false;
        slices[id] = s;
        return slices[id];
    }

    void moveTx(int id)
    {
        for (auto& [sid, s] : slices) {
            s.tx = (sid == id);
        }
    }

    // State
    bool isConnected() const override { return connected; }
    std::vector<int> sliceIds() const override
    {
        std::vector<int> ids;
        for (const auto& [id, s] : slices) {
            ids.push_back(id);
        }
        return ids;
    }
    std::optional<CatSliceState> slice(int id) const override
    {
        auto it = slices.find(id);
        return it == slices.end() ? std::nullopt : std::optional<CatSliceState>(it->second);
    }
    int  activeSliceId() const override { return active; }
    bool isTransmitting() const override { return transmitting; }
    bool isTuning() const override { return tuning; }
    int  rfPower() const override { return power; }
    int  micLevel() const override { return mic; }
    int  cwSpeed() const override { return wpm; }
    int  cwPitch() const override { return pitch; }
    bool voxEnabled() const override { return vox; }
    bool binaural() const override { return bin; }
    QString radioModelName() const override { return model; }

    // Actions (log + optimistic update like SliceModel's setters)
    void setFrequency(int id, long long hz) override { log("freq", id, hz); slices[id].freqHz = hz; }
    void setMode(int id, const QString& m) override { log("mode:" + m.toStdString(), id, 0); slices[id].mode = m; }
    void setFilter(int id, int lo, int hi) override
    {
        log("filter", id, lo * 100000LL + hi);
        slices[id].filterLow = lo;
        slices[id].filterHigh = hi;
        lastFilterLo = lo;
        lastFilterHi = hi;
    }
    void setAudioGain(int id, int g) override { log("gain", id, g); slices[id].audioGain = g; }
    void setAudioPan(int id, int p) override { log("pan", id, p); slices[id].audioPan = p; }
    void setMute(int id, bool m) override { log("mute", id, m); slices[id].mute = m; }
    void setRit(int id, bool on, int hz) override { log("rit", id, hz); slices[id].ritOn = on; slices[id].ritHz = hz; }
    void setXit(int id, bool on, int hz) override { log("xit", id, hz); slices[id].xitOn = on; slices[id].xitHz = hz; }
    void setAgcMode(int id, const QString& m) override { log("agc:" + m.toStdString(), id, 0); slices[id].agcMode = m; }
    void setAgcThreshold(int id, int v) override { log("agct", id, v); slices[id].agcThreshold = v; }
    void setNr(int id, bool on) override { log("nr", id, on); slices[id].nr = on; }
    void setWnb(int id, bool on) override { log("wnb", id, on); slices[id].wnb = on; }
    void setWnbLevel(int id, int l) override { log("wnbl", id, l); slices[id].wnbLevel = l; }
    void setDiversity(int id, bool on) override { log("div", id, on); slices[id].diversity = on; }
    void setSquelch(int id, bool on, int l) override { log("sql", id, l); slices[id].squelchOn = on; slices[id].squelchLevel = l; }
    void setTxSlice(int id, bool on) override
    {
        log(on ? "txslice" : "txslice-off", id, 0);
        // Radio behaviour: exactly one TX slice; status echo arrives later,
        // but for the fake apply immediately unless deferTxSlice is set.
        if (!deferTxSlice) {
            if (on) {
                moveTx(id);
            } else {
                slices[id].tx = false;
            }
        }
    }
    void setActiveSlice(int id) override { log("active", id, 0); active = id; }
    void requestSplit(int rx, bool on, double mhz) override
    {
        log(on ? "split-on" : "split-off", rx, std::llround(mhz * 1e6));
    }
    void setTransmit(bool tx) override { log(tx ? "xmit1" : "xmit0", -1, 0); transmitting = tx; }
    void setTune(bool on) override { log(on ? "tune1" : "tune0", -1, 0); tuning = on; }
    void setRfPower(int p) override { log("rfpower", -1, p); power = p; }
    void setMicLevel(int l) override { log("mic", -1, l); mic = l; }
    void setCwSpeed(int w) override { log("wpm", -1, w); wpm = w; }
    void setCwPitch(int hz) override { log("pitch", -1, hz); pitch = hz; }
    void setVox(bool on) override { log("vox", -1, on); vox = on; }
    void setBinaural(bool on) override { log("binaural", -1, on); bin = on; }
    void sendCw(const QString& t) override { log("cw:" + t.toStdString(), -1, 0); }
    void clearCw() override { log("cwclear", -1, 0); }

    void log(const std::string& what, int id, long long v)
    {
        calls.push_back(what + "(" + std::to_string(id) + "," + std::to_string(v) + ")");
    }
    std::string callLog() const
    {
        std::string s;
        for (const auto& c : calls) {
            s += c + " ";
        }
        return s;
    }
    bool called(const std::string& c) const
    {
        for (const auto& x : calls) {
            if (x == c) {
                return true;
            }
        }
        return false;
    }

    std::map<int, CatSliceState> slices;
    std::vector<std::string> calls;
    bool connected{true};
    int  active{0};
    bool transmitting{false};
    bool tuning{false};
    int  power{50};
    int  mic{40};
    int  wpm{25};
    int  pitch{600};
    bool vox{false};
    bool bin{false};
    bool deferTxSlice{false};
    int  lastFilterLo{0};
    int  lastFilterHi{0};
    QString model{"FLEX-6700"};
};

// ── Tests ───────────────────────────────────────────────────────────────────

void testIdentityAndFraming()
{
    std::printf("\n-- identity / framing --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    expectEq(p.feed("ID;"), "ID904;", "ID FLEX-6700");
    r.model = "FLEX-8600M";
    expectEq(p.feed("ID;"), "ID911;", "ID FLEX-8600M");
    r.model = "FLEX-6400";
    expectEq(p.feed("ID;"), "ID908;", "ID FLEX-6400");
    r.model = "AU-520M";
    expectEq(p.feed("ID;"), "ID931;", "ID AU-520M");
    r.model = "FLEX-6700";
    expectEq(p.feed("PS;"), "PS1;", "PS power status");
    expectEq(p.feed("PS0;"), "", "PS0 ignored (never power off)");
    expectEq(p.feed("XX;"), "?;", "unknown command");
    expectEq(p.feed("ZZQQ;"), "?;", "unknown ZZ command");
    expectEq(p.feed("fa;"), "FA00014074000;", "lower-case command accepted");
    expectEq(p.feed("\r\nFA;"), "FA00014074000;", "leading CR/LF ignored");
    expectEq(p.feed("FA;MD;ZZMD;"), "FA00014074000;MD9;ZZMD07;", "batched commands");
    expectEq(p.feed("F"), "", "partial command buffered");
    expectEq(p.feed("A;"), "FA00014074000;", "partial command completed");
    expectEq(p.feed(";;"), "", "empty commands ignored");
    expectEq(p.feed(QByteArray(CatKenwoodProtocol::kMaxBufferBytes + 1, 'A')), "?;",
             "unterminated flood dropped");
    expectEq(p.feed("FA;"), "FA00014074000;", "recovers after flood");

    r.connected = false;
    expectEq(p.feed("FA;"), "?;", "FA without radio -> ?;");
    expectEq(p.feed("ID;"), "ID904;", "ID works without radio");
    expectEq(p.feed("TX;"), "?;", "TX without radio refused");
    check(!r.called("xmit1(-1,0)"), "no xmit when disconnected");
}

void testFrequency()
{
    std::printf("\n-- frequency --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    expectEq(p.feed("ZZFA;"), "ZZFA00014074000;", "ZZFA get 11 digits");
    expectEq(p.feed("FA00007074000;"), "", "FA set silent");
    check(r.slices[0].freqHz == 7074000, "FA set tuned slice 0 to 7.074 MHz");
    expectEq(p.feed("ZZFA00021074000;"), "", "ZZFA set");
    check(r.slices[0].freqHz == 21074000, "ZZFA set tuned slice 0");
    expectEq(p.feed("FA0000000000;"), "?;", "FA 0 Hz rejected");
    expectEq(p.feed("FA0001407400X;"), "?;", "FA non-digit rejected");
    expectEq(p.feed("FA000140740000;"), "?;", "FA 12 digits rejected");
    // [SSC] "May switch to 8 digit mode by doing an 8 digit set."
    expectEq(p.feed("FA14074000;"), "", "8-digit set");
    expectEq(p.feed("FA;"), "FA14074000;", "8-digit mode answer");
    expectEq(p.feed("FA00014074000;"), "", "11-digit set");
    expectEq(p.feed("FA;"), "FA00014074000;", "back to 11-digit answers");
    // No VFO B yet: FB mirrors VFO A, FB set is remembered (not applied).
    expectEq(p.feed("FB;"), "FB00014074000;", "FB without VFO B mirrors A");
    r.calls.clear();
    expectEq(p.feed("FB00014076000;"), "", "FB set without VFO B accepted");
    check(r.calls.empty(), "FB set without VFO B touches no slice");
    expectEq(p.feed("FB;"), "FB00014076000;", "FB reports pending value");
}

void testModes()
{
    std::printf("\n-- modes --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    expectEq(p.feed("MD;"), "MD9;", "MD DIGU = 9");
    expectEq(p.feed("MD1;"), "", "MD1 set");
    check(r.slices[0].mode == "LSB", "MD1 -> LSB");
    expectEq(p.feed("ZZMD;"), "ZZMD00;", "ZZMD LSB = 00");
    expectEq(p.feed("ZZMD04;"), "", "ZZMD04 set");
    check(r.slices[0].mode == "CW", "ZZMD04 -> CW");
    expectEq(p.feed("MD;"), "MD3;", "MD CW = 3");
    expectEq(p.feed("ZZMD;"), "ZZMD04;", "ZZMD CW = 04 (CWU)");
    expectEq(p.feed("ZZMD07;"), "", "ZZMD07 set");
    check(r.slices[0].mode == "DIGU", "ZZMD07 -> DIGU");
    expectEq(p.feed("ZZMD30;"), "", "ZZMD30 set");
    check(r.slices[0].mode == "RTTY", "ZZMD30 -> RTTY");
    expectEq(p.feed("MD;"), "MD6;", "MD RTTY folds to 6");
    r.calls.clear();
    expectEq(p.feed("MD6;"), "", "MD6 on RTTY");
    check(r.calls.empty(), "MD6 keeps RTTY (same family, no bounce)");
    r.slices[0].mode = "NFM";
    r.calls.clear();
    expectEq(p.feed("MD4;"), "", "MD4 on NFM");
    check(r.calls.empty(), "MD4 keeps NFM");
    expectEq(p.feed("ZZMD;"), "ZZMD11;", "ZZMD NFM = 11");
    expectEq(p.feed("MD5;"), "", "MD5 set");
    check(r.slices[0].mode == "AM", "MD5 -> AM");
    expectEq(p.feed("MD8;"), "?;", "MD8 invalid");
    expectEq(p.feed("ZZMD40;"), "?;", "ZZMD40 (DSTR) not in mode list");
    expectEq(p.feed("ZZMD99;"), "?;", "ZZMD99 invalid");
    expectEq(p.feed("MD12;"), "?;", "MD two digits invalid");
    expectEq(p.feed("ZZME;"), "?;", "ZZME without VFO B");
}

void testInfo()
{
    std::printf("\n-- IF / ZZIF --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    r.slices[0].ritOn = true;
    r.slices[0].ritHz = -150;
    const QByteArray ifr = p.feed("IF;");
    expectEq(ifr, "IF000140740000000-0015010000090000000;", "IF layout");
    check(ifr.size() == 38, "IF is 37 chars + ';' (Hamlib if_len 37)");
    check(ifr[28] == '0', "IF P8 (offset 28) = RX");
    r.transmitting = true;
    const QByteArray ifTx = p.feed("IF;");
    check(ifTx[28] == '1', "IF P8 (offset 28) = TX while transmitting");
    r.transmitting = false;
    r.slices[0].xitOn = true;
    r.slices[0].xitHz = 500;
    expectEq(p.feed("IF;"), "IF000140740000000+0050011000090000000;", "IF P3 carries XIT when XIT on");
    const QByteArray zz = p.feed("ZZIF;");
    expectEq(zz, "ZZIF000140740000000+00500110000070000000;", "ZZIF layout (2-digit mode)");
    check(zz.size() == 41, "ZZIF length");
    expectEq(p.feed("IF1;"), "?;", "IF with argument rejected");
}

void testPtt()
{
    std::printf("\n-- PTT / TX slice --\n");
    {
        FakeRadio r;
        r.addSlice(1, 7074000, "DIGU");
        CatKenwoodProtocol p(&r);
        p.setVfoASlice(1);
        r.calls.clear();
        expectEq(p.feed("TX;"), "", "TX keys (silent)");
        check(r.callLog() == "txslice(1,0) xmit1(-1,0) ",
              "auto-switch: TX flag moved to port slice BEFORE xmit 1 (" + r.callLog() + ")");
        expectEq(p.feed("RX;"), "", "RX unkeys");
        check(r.called("xmit0(-1,0)"), "RX -> setTransmit(false)");
    }
    {
        FakeRadio r;
        r.addSlice(1, 7074000, "DIGU");
        CatKenwoodProtocol p(&r);
        p.setVfoASlice(1);
        p.setAutoSwitchTxSlice(false);
        r.calls.clear();
        p.feed("TX1;");
        check(r.callLog() == "xmit1(-1,0) ", "auto-switch off: TX flag left alone");
    }
    {
        // Hamlib flex6k_set_ptt: "ZZTX1;ZZTX;" must read back 1 immediately.
        FakeRadio r;
        CatKenwoodProtocol p(&r);
        expectEq(p.feed("ZZTX;"), "ZZTX0;", "ZZTX get RX");
        expectEq(p.feed("ZZRX;"), "ZZRX1;", "ZZRX get = inverse of MOX");
        expectEq(p.feed("ZZTX1;ZZTX;"), "ZZTX1;", "ZZTX1;ZZTX; read-back");
        expectEq(p.feed("ZZRX;"), "ZZRX0;", "ZZRX while TX");
        expectEq(p.feed("ZZTX0;ZZTX;"), "ZZTX0;", "ZZTX0;ZZTX; read-back");
        expectEq(p.feed("ZZRX0;"), "?;", "ZZRX0 not supported ([SSC] 3.3.34)");
        expectEq(p.feed("ZZTX2;"), "?;", "ZZTX2 invalid");
        r.calls.clear();
        expectEq(p.feed("TX2;"), "", "TX2 = TUNE");
        check(r.called("tune1(-1,0)"), "TX2 -> tune");
        expectEq(p.feed("ZZTX;"), "ZZTX1;", "ZZTX reports tune as TX");
        expectEq(p.feed("RX;"), "", "RX during tune");
        check(r.called("tune0(-1,0)") && r.called("xmit0(-1,0)"), "RX stops tune and xmit");
        expectEq(p.feed("TX3;"), "?;", "TX3 invalid");
        // Hamlib TS-2000 kenwood_set_ptt sends "VX0;TX".
        r.calls.clear();
        expectEq(p.feed("VX0;TX;"), "", "TS-2000 VX0;TX;");
        check(r.called("vox(-1,0)") && r.called("xmit1(-1,0)"), "VX0 then key");
        expectEq(p.feed("VX;"), "VX0;", "VX get");
        p.feed("RX;");
    }
}

void testSplit()
{
    std::printf("\n-- split / VFO B --\n");
    {
        FakeRadio r;
        CatKenwoodProtocol p(&r);
        expectEq(p.feed("FT;"), "FT0;", "FT get simplex");
        expectEq(p.feed("FR1;"), "?;", "FR1 without VFO B -> ?;");
        r.calls.clear();
        // WSJT-X "Split: Rig" order: FB set, FT1.
        p.feed("FB00014076000;FT1;");
        check(r.called("split-on(0,14076000)"), "FT1 without B asks GUI for split with pending FB (" + r.callLog() + ")");
        // MainWindow::enableSplit creates slice 2 and moves TX to it.
        r.addSlice(2, 14076000, "DIGU");
        r.moveTx(2);
        expectEq(p.feed("FT;"), "FT1;", "FT1 after split slice appeared");
        expectEq(p.feed("ZZSW;"), "ZZSW1;", "ZZSW mirrors FT");
        check(p.vfoBSliceId() == 2, "split slice adopted as VFO B");
        expectEq(p.feed("FB;"), "FB00014076000;", "FB reads split slice");
        p.feed("FB00014077500;");
        check(r.slices[2].freqHz == 14077500, "FB set tunes split slice");
        check(r.slices[0].freqHz == 14074000, "FB set leaves VFO A alone");
        expectEq(p.feed("ZZME;"), "ZZME07;", "ZZME reads VFO B mode");
        const QByteArray ifr = p.feed("IF;");
        check(ifr[32] == '1', "IF P12 (offset 32) = split");
        r.calls.clear();
        p.feed("TX;");
        check(!r.called("txslice(0,0)") && r.called("xmit1(-1,0)"),
              "keying in split keeps TX on VFO B (" + r.callLog() + ")");
        p.feed("RX;");
        // FR1: VFO B becomes active slice
        r.calls.clear();
        expectEq(p.feed("FR1;"), "", "FR1 with VFO B");
        check(r.called("active(2,0)"), "FR1 activates VFO B slice");
        expectEq(p.feed("FR;"), "FR1;", "FR get");
        p.feed("FR0;");
        check(r.called("active(0,0)"), "FR0 re-activates VFO A slice");
        r.calls.clear();
        expectEq(p.feed("FR0;"), "", "FR0 again");
        check(r.calls.empty(), "repeated FR0 does not steal focus");
        // SO2R (default): FT0 moves TX back and removes split slice.
        r.calls.clear();
        p.feed("FT0;");
        check(r.called("txslice(0,0)") && r.called("split-off(0,0)"), "FT0 SO2R: TX->A + split off");
        check(p.vfoBSliceId() == -1, "VFO B forgotten after SO2R FT0");
    }
    {
        FakeRadio r;
        CatKenwoodProtocol p(&r);
        p.setSplitMode(CatKenwoodProtocol::SplitMode::So2v);
        p.feed("FT1;");
        r.addSlice(3, 14080000, "USB");
        r.moveTx(3);
        p.feed("FA;");   // any command adopts
        r.calls.clear();
        p.feed("FT0;");
        check(r.called("txslice(0,0)") && !r.called("split-off(0,0)"), "FT0 SO2V keeps split slice");
        expectEq(p.feed("FB;"), "FB00014080000;", "SO2V: VFO B still readable");
        r.calls.clear();
        p.feed("ZZFT;");
        check(r.called("txslice(3,0)"), "ZZFT toggles TX to VFO B");
    }
    {
        // FB after FT1 while the split slice is still being created retargets it.
        FakeRadio r;
        CatKenwoodProtocol p(&r);
        p.feed("FT1;");
        r.calls.clear();
        p.feed("FB00014078000;");
        check(r.called("split-on(0,14078000)"), "FB while split pending retargets split request");
    }
    {
        // Two ports: port 2 (slice 1) must not treat port 1's TX slice as VFO B.
        FakeRadio r;
        r.addSlice(1, 7074000, "DIGU");
        CatKenwoodProtocol p2(&r);
        p2.setVfoASlice(1);
        expectEq(p2.feed("FT;"), "FT0;", "other slice holding TX is not split");
        r.calls.clear();
        p2.feed("FB00007080000;");
        check(r.slices[0].freqHz == 14074000, "port 2 FB never retunes slice 0");
        p2.feed("TX;");
        check(r.called("txslice(1,0)"), "port 2 keying moves TX to slice 1");
        p2.feed("RX;");
    }
}

void testBinding()
{
    std::printf("\n-- slice binding --\n");
    FakeRadio r;
    r.addSlice(1, 3573000, "DIGU");
    CatKenwoodProtocol p(&r);
    p.setVfoASlice(1);
    expectEq(p.feed("FA;"), "FA00003573000;", "port bound to slice B");
    p.setVfoASlice(5);
    expectEq(p.feed("FA;"), "?;", "bound slice not present -> ?;");
    check(p.vfoASliceId() == -1, "vfoASliceId -1 when slice missing");
    p.setVfoASlice(CatKenwoodProtocol::kActiveSlice);
    r.active = 1;
    expectEq(p.feed("FA;"), "FA00003573000;", "active-slice binding follows active slice");
    r.active = 0;
    expectEq(p.feed("FA;"), "FA00014074000;", "active-slice binding switched");
    r.active = 7;
    expectEq(p.feed("FA;"), "FA00014074000;", "active binding falls back to first slice");
}

void testFilters()
{
    std::printf("\n-- filters --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    r.slices[0].mode = "LSB";
    r.slices[0].filterLow = -2900;
    r.slices[0].filterHigh = -100;
    expectEq(p.feed("ZZFI;"), "ZZFI02;", "[SSC] example: LSB 2.8k -> 02");
    r.slices[0].filterLow = -3001;
    expectEq(p.feed("ZZFI;"), "ZZFI01;", "LSB 2.901k -> 01");
    r.slices[0].mode = "USB";
    r.slices[0].filterLow = 100;
    r.slices[0].filterHigh = 2900;
    expectEq(p.feed("ZZFI05;"), "", "ZZFI05 set");
    check(r.lastFilterLo == 100 && r.lastFilterHi == 2200, "USB idx 5 = 2.1k keeps low edge");
    r.slices[0].mode = "CW";
    r.slices[0].filterLow = 350;
    r.slices[0].filterHigh = 850;
    expectEq(p.feed("ZZFI;"), "ZZFI03;", "CW 500 Hz -> 03 (400 < 500 <= 800)");
    p.feed("ZZFI05;");
    check(r.lastFilterLo == 475 && r.lastFilterHi == 725, "CW 250 Hz centred on passband");
    r.slices[0].mode = "DIGU";
    r.slices[0].filterLow = 0;
    r.slices[0].filterHigh = 3000;
    expectEq(p.feed("ZZFI;"), "ZZFI00;", "DIGU 3k -> 00");
    expectEq(p.feed("ZZFI08;"), "?;", "ZZFI08 invalid");
    expectEq(p.feed("ZZFJ;"), "?;", "ZZFJ without VFO B");
    // SH / SL
    r.slices[0].mode = "USB";
    r.slices[0].filterLow = 100;
    r.slices[0].filterHigh = 2800;
    expectEq(p.feed("SH;"), "SH07;", "SH USB 2800 -> 07");
    expectEq(p.feed("SL;"), "SL02;", "SL USB 100 -> 02");
    p.feed("SH10;");
    check(r.lastFilterLo == 100 && r.lastFilterHi == 4000, "SH10 -> 4000");
    p.feed("SL04;");
    check(r.lastFilterLo == 300 && r.lastFilterHi == 4000, "SL04 -> 300");
    r.slices[0].mode = "LSB";
    r.slices[0].filterLow = -2800;
    r.slices[0].filterHigh = -100;
    expectEq(p.feed("SH;"), "SH07;", "SH LSB mirrors");
    p.feed("SH05;");
    check(r.lastFilterLo == -2400 && r.lastFilterHi == -100, "SH05 on LSB -> -2400..-100");
    r.slices[0].mode = "AM";
    r.slices[0].filterLow = -3000;
    r.slices[0].filterHigh = 3000;
    expectEq(p.feed("SH;"), "SH01;", "SH AM table");
    expectEq(p.feed("SH04;"), "?;", "SH AM idx > 3 invalid");
    expectEq(p.feed("SH12;"), "?;", "SH idx 12 invalid");
}

void testAudioAgcDsp()
{
    std::printf("\n-- audio / AGC / DSP --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    r.slices[0].audioGain = 40;
    expectEq(p.feed("AG;"), "AG040;", "AG get (SmartSDR 0-100)");
    expectEq(p.feed("ZZAG;"), "ZZAG040;", "ZZAG get");
    p.feed("ZZAG075;");
    check(r.slices[0].audioGain == 75, "ZZAG set");
    p.feed("AG060;");
    check(r.slices[0].audioGain == 60, "AG set 3-digit");
    expectEq(p.feed("AG0;"), "AG0153;", "TS-2000 AG0; (0-255 scale)");
    p.feed("AG0255;");
    check(r.slices[0].audioGain == 100, "TS-2000 AG0255 -> 100");
    expectEq(p.feed("AG101;"), "?;", "AG out of range");
    expectEq(p.feed("ZZLE;"), "?;", "ZZLE without VFO B");
    expectEq(p.feed("ZZLB;"), "ZZLB050;", "ZZLB pan");
    p.feed("ZZMA1;");
    check(r.slices[0].mute, "ZZMA1 mutes");
    expectEq(p.feed("ZZMA;"), "ZZMA1;", "ZZMA get");
    expectEq(p.feed("ZZMA2;"), "?;", "ZZMA2 invalid");
    expectEq(p.feed("GT;"), "GT3;", "GT med = 3");
    p.feed("GT4;");
    check(r.slices[0].agcMode == "fast", "GT4 -> fast");
    p.feed("ZZGT0;");
    check(r.slices[0].agcMode == "off", "ZZGT0 -> off");
    p.feed("GT020;");
    check(r.slices[0].agcMode == "slow", "TS-2000 GT020 -> slow");
    expectEq(p.feed("GT1;"), "?;", "GT1 invalid");
    p.feed("ZZAR065;");
    expectEq(p.feed("ZZAR;"), "ZZAR065;", "ZZAR AGC threshold");
    p.feed("NB1;");
    check(r.slices[0].wnb, "NB1 -> WNB on");
    expectEq(p.feed("NB;"), "NB1;", "NB get");
    p.feed("ZZNL070;");
    expectEq(p.feed("ZZNL;"), "ZZNL070;", "ZZNL WNB level");
    p.feed("NR2;");
    check(r.slices[0].nr, "TS-2000 NR2 -> NR on");
    expectEq(p.feed("ZZNR;"), "ZZNR1;", "ZZNR get");
    p.feed("ZZBI1;");
    check(r.bin, "ZZBI binaural");
    p.feed("ZZDE1;");
    check(r.slices[0].diversity, "ZZDE diversity");
    r.slices[0].squelchLevel = 20;
    expectEq(p.feed("SQ0;"), "SQ0051;", "SQ0 get 0-255");
    p.feed("SQ0255;");
    check(r.slices[0].squelchLevel == 100, "SQ0255 -> 100");
}

void testRitXit()
{
    std::printf("\n-- RIT / XIT --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    r.slices[0].mode = "USB";
    p.feed("RT1;");
    check(r.slices[0].ritOn, "RT1");
    expectEq(p.feed("RT;"), "RT1;", "RT get");
    p.feed("RU;");
    check(r.slices[0].ritHz == 50, "RU default 50 Hz in USB");
    r.slices[0].mode = "CW";
    p.feed("RD;");
    check(r.slices[0].ritHz == 40, "RD default 10 Hz in CW");
    p.feed("RU00210;");
    check(r.slices[0].ritHz == 250, "RU00210 (Hamlib has_rit2 form)");
    expectEq(p.feed("ZZRG;"), "ZZRG+00250;", "ZZRG get");
    p.feed("ZZRG-01000;");
    check(r.slices[0].ritHz == -1000, "ZZRG set negative");
    expectEq(p.feed("ZZRG01000;"), "?;", "ZZRG without sign rejected");
    p.feed("RC;");
    check(r.slices[0].ritHz == 0, "RC clears RIT");
    // XIT only: Hamlib kenwood_set_xit drives the shared offset via RU/RD.
    p.feed("RT0;XT1;");
    p.feed("RU00300;");
    check(r.slices[0].xitHz == 300 && r.slices[0].ritHz == 0, "RU drives XIT when only XIT is on");
    expectEq(p.feed("ZZXG;"), "ZZXG+00300;", "ZZXG get");
    expectEq(p.feed("ZZXS;"), "ZZXS1;", "ZZXS get");
    p.feed("ZZXC;");
    check(r.slices[0].xitHz == 0, "ZZXC clears XIT");
    p.feed("RU99999;RU00010;");
    check(r.slices[0].xitHz == 99999, "offset clamps at 99999");
    expectEq(p.feed("RU123456;"), "?;", "RU 6 digits rejected");
}

void testMetersAndTx()
{
    std::printf("\n-- meters / transmitter / CW --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    r.slices[0].sMeterDbm = -73.0f;
    expectEq(p.feed("SM;"), "SM015;", "SM S9 = 015");
    expectEq(p.feed("SM0;"), "SM00015;", "TS-2000 SM0 4 digits");
    expectEq(p.feed("ZZSM;"), "ZZSM134;", "ZZSM (-73+140)*2 = 134");
    r.slices[0].sMeterDbm = -13.0f;
    expectEq(p.feed("SM;"), "SM030;", "SM S9+60 = 030");
    r.slices[0].sMeterDbm = -150.0f;
    expectEq(p.feed("ZZSM;"), "ZZSM000;", "ZZSM clamps low");
    expectEq(p.feed("PC;"), "PC050;", "PC get");
    p.feed("ZZPC075;");
    check(r.power == 75, "ZZPC set");
    expectEq(p.feed("PC101;"), "?;", "PC > 100 rejected");
    expectEq(p.feed("ZZMG;"), "ZZMG040;", "ZZMG get");
    expectEq(p.feed("KS;"), "KS025;", "KS get");
    p.feed("KS030;");
    check(r.wpm == 30, "KS set");
    expectEq(p.feed("KS004;"), "?;", "KS below 5 rejected");
    p.feed("PT050;");
    check(r.pitch == 600, "PT < 100 silently ignored");
    p.feed("PT700;");
    check(r.pitch == 700, "PT set");
    expectEq(p.feed("KY;"), "KY0;", "KY buffer query");
    r.calls.clear();
    p.feed("KY CQ TEST DE K1ABC       ;");
    check(r.called("cw:CQ TEST DE K1ABC(-1,0)"), "KY sends padded text trimmed (" + r.callLog() + ")");
    p.feed("KY0;");
    check(r.called("cwclear(-1,0)"), "KY0 (Hamlib stop_morse) clears CWX");
}

void testAutoInfo()
{
    std::printf("\n-- auto-information --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    expectEq(p.feed("AI;"), "AI0;", "AI default off");
    expectEq(p.pollAutoInfo(), "", "no push while AI off");
    p.feed("AI1;");
    check(p.autoInfoEnabled(), "AI1 enables");
    expectEq(p.pollAutoInfo(), "", "no initial dump");
    r.slices[0].freqHz = 14075000;
    expectEq(p.pollAutoInfo(), "FA00014075000;", "freq change pushed as FA");
    expectEq(p.pollAutoInfo(), "", "no duplicate push");
    r.slices[0].mode = "USB";
    expectEq(p.pollAutoInfo(), "MD2;", "mode change pushed as MD");
    r.transmitting = true;
    const QByteArray push = p.pollAutoInfo();
    check(push.startsWith("IF") && push.size() == 38 && push[28] == '1', "TX edge pushed as IF");
    r.transmitting = false;
    p.pollAutoInfo();
    p.feed("FT1;");
    r.addSlice(2, 14080000, "USB");
    r.moveTx(2);
    const QByteArray split = p.pollAutoInfo();
    check(split.contains("FB00014080000;"), "new split slice pushed as FB (" + std::string(split.constData()) + ")");
    p.feed("AI0;");
    r.slices[0].freqHz = 14000000;
    expectEq(p.pollAutoInfo(), "", "AI0 stops pushes");
    expectEq(p.feed("ZZAI1;ZZAI;"), "ZZAI1;", "ZZAI set/get");
    expectEq(p.feed("AI4;"), "?;", "AI4 invalid");
}

void testHamlibOpenSequence()
{
    std::printf("\n-- Hamlib FlexRadio 6xxx / TS-2000 open + poll --\n");
    FakeRadio r;
    CatKenwoodProtocol p(&r);
    // flexradio_open: ID, then kenwood_get_trn "AI" and set "AI0".
    expectEq(p.feed("ID;"), "ID904;", "open: ID");
    expectEq(p.feed("AI;"), "AI0;", "open: AI query");
    expectEq(p.feed("AI0;"), "", "open: AI0");
    // WSJT-X poll: FA, MD + ZZFI (flex6k_get_mode), IF (get_vfo/split/ptt).
    expectEq(p.feed("FA;"), "FA00014074000;", "poll FA");
    expectEq(p.feed("MD;"), "MD9;", "poll MD");
    expectEq(p.feed("ZZFI;"), "ZZFI00;", "poll ZZFI");
    check(p.feed("IF;").size() == 38, "poll IF");
    // flex6k_set_mode: "MD9;" then "ZZFI01;" (dig 2000 Hz).
    p.feed("MD9;ZZFI01;");
    check(r.lastFilterHi - r.lastFilterLo == 2000, "set_mode PKTUSB 2000 Hz");
}

// ── Transport tests (loopback only) ─────────────────────────────────────────

bool waitFor(const std::function<bool()>& cond, int ms = 2000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return cond();
}

void testTcpServer()
{
    std::printf("\n-- TCP server (127.0.0.1) --\n");
    FakeRadio r;
    CatKenwoodServer server(&r);
    CatKenwoodPortConfig cfg;
    cfg.vfoASlice = 0;
    server.setPortConfig(cfg);
    check(server.start(0), "server listens on ephemeral port");
    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, server.port());
    check(sock.waitForConnected(2000), "client connects");
    check(waitFor([&] { return server.clientCount() == 1; }), "server sees 1 client");
    sock.write("ID;FA;");
    QByteArray got;
    waitFor([&] { got += sock.readAll(); return got.size() >= 20; });
    expectEq(got, "ID904;FA00014074000;", "TCP round trip");
    sock.write("AI1;");
    waitFor([&] { return false; }, 100);
    r.slices[0].freqHz = 7074000;
    server.notifyRadioStateChanged();
    got.clear();
    waitFor([&] { got += sock.readAll(); return got.size() >= 14; });
    expectEq(got, "FA00007074000;", "TCP AI push");
    sock.disconnectFromHost();
    check(waitFor([&] { return server.clientCount() == 0; }), "client disconnect tracked");
    server.stop();
    check(!server.isRunning(), "server stopped");
}

void testPty()
{
#ifndef Q_OS_WIN
    std::printf("\n-- PTY --\n");
    FakeRadio r;
    CatKenwoodPty pty(&r);
    const QString link = QString("/tmp/AetherSDR-KCAT-test-%1").arg(::getpid());
    pty.setSymlinkPath(link);
    check(pty.start(), "pty started");
    const int fd = ::open(pty.slavePath().toLocal8Bit().constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    check(fd >= 0, "slave opens");
    if (fd >= 0) {
        const char cmd[] = "ID;ZZFA;";
        check(::write(fd, cmd, sizeof(cmd) - 1) == static_cast<ssize_t>(sizeof(cmd) - 1), "write to slave");
        QByteArray got;
        waitFor([&] {
            char buf[256];
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                got.append(buf, static_cast<qsizetype>(n));
            }
            return got.size() >= 22;
        });
        expectEq(got, "ID904;ZZFA00014074000;", "PTY round trip");
        ::close(fd);
    }
    char target[512] = {};
    const ssize_t len = ::readlink(link.toLocal8Bit().constData(), target, sizeof(target) - 1);
    check(len > 0 && QString::fromLocal8Bit(target, static_cast<int>(len)) == pty.slavePath(),
          "symlink points at slave");
    pty.stop();
    check(::access(link.toLocal8Bit().constData(), F_OK) != 0, "symlink removed on stop");
#endif
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testIdentityAndFraming();
    testFrequency();
    testModes();
    testInfo();
    testPtt();
    testSplit();
    testBinding();
    testFilters();
    testAudioAgcDsp();
    testRitXit();
    testMetersAndTx();
    testAutoInfo();
    testHamlibOpenSequence();
    testTcpServer();
    testPty();

    std::printf("\n%s (%d checks, %d failures)\n",
                g_failures == 0 ? "ALL PASS" : "FAILURES", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
