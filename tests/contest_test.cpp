// Tests for ContestEngine (exchange parsing, serials, dupes, scoring,
// spot annotations, macro expansion + TX guard) and So2rController.
//
// No radio is involved: CwxModel / SliceModel are standalone objects whose
// commandReady() signals are captured locally and never reach a radio.
//
// Build: `contest_test`.  Run: ./build/contest_test [path/to/cty.dat]

#include "core/ContestEngine.h"
#include "models/CwxModel.h"
#include "models/SliceModel.h"
#include "models/SpotModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;
QString g_ctyPath;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

std::string s(const QString& q) { return q.toStdString(); }

const QDateTime kT0(QDate(2026, 11, 28), QTime(0, 0), QTimeZone::UTC);

qint64 logContact(Logbook& lb, ContestEngine& ce, const QString& call, double mhz,
                  const QString& flexMode, const QString& exch, int minute)
{
    LogQso q = lb.draftFromFrequency(mhz, flexMode);
    q.call = call;
    q.timeOn = kT0.addSecs(minute * 60);
    const int sn = ce.nextSerial();
    ce.applyExchange(q, ce.parseExchange(exch), sn);
    return lb.addQso(q);
}

void testExchangeParsing(ContestEngine& ce)
{
    ce.setProfile(ContestType::CqWw);
    ce.setModeVariant("CW");
    report("CQWW contest id", ce.contestId() == "CQ-WW-CW", s(ce.contestId()));
    auto ex = ce.parseExchange("5nn 14");
    report("CQWW '5nn 14' → RST 599 ZONE 14", ex.value("RST") == "599" && ex.value("ZONE") == "14",
           s(ex.value("RST") + "/" + ex.value("ZONE")));
    ex = ce.parseExchange("14");
    report("CQWW '14' → ZONE only", ex.value("ZONE") == "14" && !ex.contains("RST"));
    ex = ce.parseExchange("599 T5");
    report("CQWW cut zone 'T5' → 5", ex.value("ZONE") == "5");

    ce.setProfile(ContestType::ArrlFieldDay);
    report("FD contest id", ce.contestId() == "ARRL-FIELD-DAY");
    ex = ce.parseExchange("1a ema");
    report("FD '1A EMA'", ex.value("CLASS") == "1A" && ex.value("SECT") == "EMA");

    ce.setProfile(ContestType::Naqp);
    ex = ce.parseExchange("bob ma");
    report("NAQP 'BOB MA'", ex.value("NAME") == "BOB" && ex.value("STATE") == "MA");

    ce.setProfile(ContestType::NaSprint);
    ex = ce.parseExchange("012 BOB MA extra");
    report("Sprint '012 BOB MA' + rest", ex.value("SN") == "12" && ex.value("NAME") == "BOB" &&
           ex.value("STATE") == "MA" && ex.value("_REST") == "EXTRA");

    ce.setProfile(ContestType::ArrlDx);
    ex = ce.parseExchange("599 KW");
    report("ARRL DX '599 KW'", ex.value("RST") == "599" && ex.value("STATE") == "KW");

    const ContestProfile cp = ContestEngine::customProfile("RST SN *STATE", "MY-QSO-PARTY");
    report("custom profile fields", cp.rcvd.size() == 3 && cp.rcvd[2].isMult && cp.usesSerial &&
           cp.sentTemplate == "{RST} {SN} {STATE}");
}

void testCqwwScoring(Logbook& lb, ContestEngine& ce)
{
    ce.setProfile(ContestType::CqWw);
    ce.setModeVariant("CW");
    ce.setStartTime(kT0);
    ce.setMyExchange({{"ZONE", "5"}});

    report("first serial == 1", ce.nextSerial() == 1);
    logContact(lb, ce, "DL1ABC", 14.025, "CW", "599 14", 1);
    report("serial increments", ce.nextSerial() == 2);
    logContact(lb, ce, "DL2XYZ", 14.026, "CW", "599 14", 2);
    logContact(lb, ce, "VE3ABC", 14.027, "CW", "599 4", 3);
    logContact(lb, ce, "K2ABC", 14.028, "CW", "599 5", 4);
    logContact(lb, ce, "JA1AAA", 21.025, "CW", "599 25", 5);
    logContact(lb, ce, "DL1ABC", 14.030, "CW", "599 14", 6);   // dupe (logged anyway)

    const ContestScore& sc = ce.score();
    // Points: DL 3 + DL 3 + VE (NA↔NA other country) 2 + K (same country) 0 + JA 3 = 11
    // Mults 20m: zones {14,4,5} + countries {DL,VE,K} = 6; 15m: zone 25 + JA = 2 → 8
    report("CQWW qsos/dupes", sc.qsos == 5 && sc.dupes == 1,
           std::to_string(sc.qsos) + "/" + std::to_string(sc.dupes));
    report("CQWW points == 11", sc.points == 11, std::to_string(sc.points));
    report("CQWW mults == 8", sc.mults == 8, std::to_string(sc.mults));
    report("CQWW score == 88", sc.score == 88, std::to_string(sc.score));
    report("CQWW mult categories", sc.multsByCategory.value("Zones") == 4 &&
           sc.multsByCategory.value("Countries") == 4);

    report("isDupe DL1ABC 20m CW", ce.isDupe("DL1ABC", "20m", "CW"));
    report("not dupe DL1ABC 40m", !ce.isDupe("DL1ABC", "40m", "CW"));
    report("not dupe outside contest mode (SSB)", !ce.isDupe("DL1ABC", "20m", "SSB"));

    // Stored exchange fields
    const LogQso j = lb.search({QString(), "JA1AAA", {}, {}, {}, {}, {}, 0}).value(0);
    report("stored CQZ/STX/strings", j.cqZone == 25 && j.stx == 5 && j.srxString == "599 25" &&
           j.stxString == "599 5" && j.contestId == "CQ-WW-CW",
           s(j.srxString + " | " + j.stxString));

    // Spot annotations
    SpotAnnotation a = ce.annotateSpot("DL1ABC", 14.021, "CW");
    report("spot DL1ABC 20m → DUPE", a.kind == SpotAnnotation::Kind::Dupe && a.label == "DUPE");
    a = ce.annotateSpot("OH2XX", 14.022, "CW");
    report("spot OH2XX 20m → MULT (new country)", a.kind == SpotAnnotation::Kind::NewMult);
    a = ce.annotateSpot("DL5AA", 14.023, "");
    report("spot DL5AA 20m → needed, not mult", a.kind == SpotAnnotation::Kind::NewCall, s(a.tooltip));
    a = ce.annotateSpot("DL5AA", 7.023, "CW");
    report("spot DL5AA 40m → MULT (new band)", a.kind == SpotAnnotation::Kind::NewMult);
    SpotData sd;
    sd.callsign = "DL1ABC";
    sd.rxFreqMhz = 14.021;
    sd.mode = "CW";
    report("annotateSpot(SpotData) overload", ce.annotateSpot(sd).kind == SpotAnnotation::Kind::Dupe);
}

void testArrlDx(Logbook& lb, ContestEngine& ce)
{
    ce.setProfile(ContestType::ArrlDx);
    ce.setModeVariant("SSB");
    report("ARRL DX id", ce.contestId() == "ARRL-DX-SSB");
    logContact(lb, ce, "G3AAA", 14.200, "USB", "59 100", 10);
    logContact(lb, ce, "G3BBB", 14.210, "USB", "59 KW", 11);
    logContact(lb, ce, "F5CCC", 21.300, "USB", "59 500", 12);
    logContact(lb, ce, "W1DDD", 14.220, "USB", "59 1", 13);   // W↔W: 0 points
    const ContestScore& sc = ce.score();
    report("ARRL DX W/VE side: 9 pts, 2 mults, 18",
           sc.points == 9 && sc.mults == 2 && sc.score == 18,
           std::to_string(sc.points) + "/" + std::to_string(sc.mults));
    const LogQso g = lb.search({QString(), "G3BBB", {}, {}, {}, {}, {}, 0}).value(0);
    report("W/VE side stores RX_PWR", g.extra.value("RX_PWR") == "KW");
    ce.setMyExchange({{"STATE", "CT"}});
    report("sent exchange '59 CT'", ce.sentExchange(0) == "59 CT", s(ce.sentExchange(0)));
}

void testFieldDay(Logbook& lb, ContestEngine& ce)
{
    ce.setProfile(ContestType::ArrlFieldDay);
    ce.setFieldDayPowerMultiplier(2);
    logContact(lb, ce, "W1AW", 7.030, "CW", "3A CT", 20);
    logContact(lb, ce, "W1AW", 7.200, "LSB", "3A CT", 21);   // other mode → not dupe
    logContact(lb, ce, "K2BBB", 14.074, "DIGU", "1D ENY", 22);
    logContact(lb, ce, "W1AW", 7.031, "CW", "3A CT", 23);    // dupe
    const ContestScore& sc = ce.score();
    // CW 2 + PH 1 + DIG 2 = 5, ×2 power = 10
    report("FD points 5, dupes 1, score 10", sc.points == 5 && sc.dupes == 1 && sc.score == 10,
           std::to_string(sc.points) + "/" + std::to_string(sc.score));
    const LogQso w = lb.search({QString(), "K2BBB", {}, {}, {}, {}, {}, 0}).value(0);
    report("FD stores CLASS/ARRL_SECT", w.extra.value("CLASS") == "1D" &&
           w.extra.value("ARRL_SECT") == "ENY");
}

void testCustom(Logbook& lb, ContestEngine& ce)
{
    ce.setProfile(ContestEngine::customProfile("RST SN *STATE", "TEST-QP"));
    logContact(lb, ce, "N1AAA", 14.040, "CW", "599 1 MA", 30);
    logContact(lb, ce, "N1BBB", 14.041, "CW", "599 2 MA", 31);
    logContact(lb, ce, "N1CCC", 7.041, "CW", "599 3 NH", 32);
    const ContestScore& sc = ce.score();
    report("custom: 3 qsos, 2 mults, score 6", sc.qsos == 3 && sc.mults == 2 && sc.score == 6,
           std::to_string(sc.mults));
    report("custom: serial sent 1..3", ce.nextSerial() == 4);
    report("custom: new mult hint", ce.isNewMult("N1DDD", "40m", {{"STATE", "VT"}}) &&
           !ce.isNewMult("N1DDD", "20m", {{"STATE", "MA"}}));
}

void testNonContestAnnotation(Logbook& lb, ContestEngine& ce)
{
    ce.setProfile(ContestType::None);
    report("no-contest: VK → NEW DXCC",
           ce.annotateSpot("VK2ABC", 14.020, "CW").kind == SpotAnnotation::Kind::NewDxcc);
    report("no-contest: DL on 80m → NEW BAND",
           ce.annotateSpot("DL7ZZ", 3.520, "CW").kind == SpotAnnotation::Kind::NewBand);
    report("no-contest: DL on 20m SSB → NEW MODE",
           ce.annotateSpot("DL7ZZ", 14.250, "USB").kind == SpotAnnotation::Kind::NewMode);
    report("no-contest: DL1ABC 20m CW → Worked",
           ce.annotateSpot("DL1ABC", 14.025, "CW").kind == SpotAnnotation::Kind::Worked);
    report("no-contest: DL5QQ 20m CW → new call (entity worked)",
           ce.annotateSpot("DL5QQ", 14.025, "CW").kind == SpotAnnotation::Kind::NewCall);
    Q_UNUSED(lb);
}

void testMacros(Logbook& lb, ContestEngine& ce)
{
    Q_UNUSED(lb);
    MacroContext c;
    c.myCall = "K1ABC";
    c.call = "DL1ABC";
    c.rst = "599";
    c.serial = 1;
    c.exchange = "599 001";
    c.cutNumbers = true;
    report("expand {CALL} {RST} {SN} cut", ContestEngine::expandMacro("{CALL} {RST} {SN}", c) ==
           "DL1ABC 5NN TT1", s(ContestEngine::expandMacro("{CALL} {RST} {SN}", c)));
    report("expand '!' and {MYCALL}", ContestEngine::expandMacro("! TU {MYCALL}", c) ==
           "DL1ABC TU K1ABC");
    report("'!' inside words untouched", ContestEngine::expandMacro("QRL? HI!", c) == "QRL? HI!");

    ce.setProfile(ContestType::CqWw);
    ce.setModeVariant("CW");
    ce.setMyExchange({{"ZONE", "5"}});
    report("default F1 contest CQ", ce.cwMacro(1) == "CQ TEST {MYCALL} {MYCALL} TEST");
    const MacroContext mc = ce.macroContext("oh2xx", "", 12);
    report("macro context exchange (CQWW, cut)", mc.exchange == "5NN 5" && mc.myCall == "K1ABC",
           s(mc.exchange));

    // TX guard — CwxModel is standalone, its commandReady is only captured here.
    CwxModel cwx;
    QStringList cmds;
    QObject::connect(&cwx, &CwxModel::commandReady, [&](const QString& cmd) { cmds << cmd; });
    ce.setCwxModel(&cwx);
    QString refused;
    QObject::connect(&ce, &ContestEngine::macroRefused, [&](const QString& r) { refused = r; });
    report("CW macro refused when not armed",
           !ce.sendCwMacro(2, "OH2XX", "", 12) && cmds.isEmpty() && !refused.isEmpty());
    ce.setMacroTxArmed(true);
    ce.setTxGuard([]() { return false; });
    report("CW macro refused when guard says no", !ce.sendCwMacro(2, "OH2XX", "", 12) && cmds.isEmpty());
    ce.setTxGuard([]() { return true; });
    const bool sent = ce.sendCwMacro(5, "OH2XX", "", 12);
    report("armed + guard → CWX command produced (not connected to a radio)",
           sent && cmds.size() == 1 && cmds[0].startsWith("cwx send \"OH2XX\""),
           s(cmds.value(0)));
    report("voice macro without DVK refused", !ce.sendVoiceMacro(1));
    ce.setMacroTxArmed(false);
    ce.setCwxModel(nullptr);
}

void testSo2r()
{
    SliceModel a(0);
    SliceModel b(1);
    QStringList cmds;
    QObject::connect(&a, &SliceModel::commandReady, [&](const QString& c) { cmds << c; });
    QObject::connect(&b, &SliceModel::commandReady, [&](const QString& c) { cmds << c; });
    So2rController so2r;
    so2r.setSlices(&a, &b);
    report("SO2R initial focus A", so2r.focusRadio() == 0 && so2r.focusedSlice() == &a);
    so2r.swapFocus();
    report("SO2R swapFocus → B", so2r.focusRadio() == 1);
    a.applyStatus({{"tx", "1"}});
    report("SO2R txRadio from slice status", so2r.txRadio() == 0);
    report("SO2R swapTx → slice 1 tx=1", so2r.swapTx() && cmds.value(0) == "slice set 1 tx=1",
           s(cmds.value(0)));
    LogQso q;
    so2r.stampQso(q);
    report("SO2R stamp: slice 1 S&P", q.sliceId == 1 && q.so2rRole == "S&P");
    so2r.toggleRole();
    so2r.stampQso(q);
    report("SO2R toggleRole → RUN", q.so2rRole == "RUN");
}

} // namespace

int main(int argc, char** argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
#ifdef AETHER_CTY_DAT_PATH
    g_ctyPath = QStringLiteral(AETHER_CTY_DAT_PATH);
#endif
    if (argc > 1) {
        g_ctyPath = QString::fromLocal8Bit(argv[1]);
    }
    if (g_ctyPath.isEmpty() || !QFile::exists(g_ctyPath)) {
        std::printf("cty.dat required (pass path as argv[1])\n");
        return 1;
    }
    QTemporaryDir dir;
    Logbook lb;
    lb.loadCtyDat(g_ctyPath);
    if (!lb.open(dir.path() + "/contest.sqlite")) {
        std::printf("cannot open logbook: %s\n", qPrintable(lb.lastError()));
        return 1;
    }
    lb.setStationCallsign("K1ABC");
    // Some non-contest history for worked-status annotations.
    LogQso h = lb.draftFromFrequency(14.025, "CW");
    h.call = "DL1ABC";
    h.timeOn = kT0.addDays(-30);
    lb.addQso(h);

    ContestEngine ce(&lb);
    testExchangeParsing(ce);
    testCqwwScoring(lb, ce);
    testArrlDx(lb, ce);
    testFieldDay(lb, ce);
    testCustom(lb, ce);
    testNonContestAnnotation(lb, ce);
    testMacros(lb, ce);
    testSo2r();
    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
