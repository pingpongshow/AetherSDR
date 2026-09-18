// Unit tests for SliceModel status parsing and setter commands for the FM
// options (fm_deviation, fm_tone_burst, dfm_pre_de_emphasis) and tx_ant_list.
// Key names/formats verified against FlexLib Slice.cs and a FLEX-6700 on
// firmware 4.1.5 ("sub slice all" status).
//
// Build: produced by CMake as `slice_model_status_test`. Exit code 0 on all pass.

#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

using AetherSDR::SliceModel;

namespace {
int g_failures = 0;

void expect(bool ok, const char* what)
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
}

QMap<QString, QString> kv(std::initializer_list<std::pair<QString, QString>> pairs)
{
    QMap<QString, QString> m;
    for (const auto& p : pairs) {
        m.insert(p.first, p.second);
    }
    return m;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Status parsing ────────────────────────────────────────────────────
    {
        SliceModel s(0);
        int burstSignals = 0;
        int emphSignals = 0;
        int txAntSignals = 0;
        QObject::connect(&s, &SliceModel::fmToneBurstChanged, [&](bool) { ++burstSignals; });
        QObject::connect(&s, &SliceModel::dfmPreDeEmphasisChanged, [&](bool) { ++emphSignals; });
        QObject::connect(&s, &SliceModel::txAntListChanged, [&](const QStringList&) { ++txAntSignals; });

        expect(s.txAntList().isEmpty(), "tx_ant_list empty before status");

        // Exact values reported by FLEX-6700 fw 4.1.5
        s.applyStatus(kv({{"fm_tone_burst", "0"}, {"fm_deviation", "5000"},
                          {"dfm_pre_de_emphasis", "0"},
                          {"tx_ant_list", "ANT1,ANT2,XVTR"}}));
        expect(!s.fmToneBurst(), "fm_tone_burst=0 parsed");
        expect(!s.dfmPreDeEmphasis(), "dfm_pre_de_emphasis=0 parsed");
        expect(s.fmDeviation() == 5000, "fm_deviation=5000 parsed");
        expect(s.txAntList() == QStringList({"ANT1", "ANT2", "XVTR"}), "tx_ant_list parsed");
        expect(burstSignals == 0 && emphSignals == 0, "no signal when value unchanged");
        expect(txAntSignals == 1, "txAntListChanged emitted once");

        s.applyStatus(kv({{"fm_tone_burst", "1"}, {"dfm_pre_de_emphasis", "1"},
                          {"fm_deviation", "2500"}, {"tx_ant_list", "ANT1,ANT2,XVTR"}}));
        expect(s.fmToneBurst(), "fm_tone_burst=1 parsed");
        expect(s.dfmPreDeEmphasis(), "dfm_pre_de_emphasis=1 parsed");
        expect(s.fmDeviation() == 2500, "fm_deviation=2500 parsed");
        expect(burstSignals == 1 && emphSignals == 1, "change signals emitted");
        expect(txAntSignals == 1, "identical tx_ant_list does not re-emit");

        s.applyStatus(kv({{"tx_ant_list", "ANT1"}}));
        expect(s.txAntList() == QStringList({"ANT1"}), "tx_ant_list update parsed");
        expect(txAntSignals == 2, "txAntListChanged emitted on change");
    }

    // ── Setter commands ───────────────────────────────────────────────────
    {
        SliceModel s(2);
        QStringList cmds;
        QObject::connect(&s, &SliceModel::commandReady, [&](const QString& c) { cmds << c; });

        s.setFmToneBurst(true);
        s.setFmToneBurst(true);  // no-op
        s.setDfmPreDeEmphasis(true);
        s.setFmDeviation(2500);
        s.setFmToneBurst(false);

        expect(cmds.size() == 4, "one command per effective change");
        expect(cmds.value(0) == "slice set 2 fm_tone_burst=1", "fm_tone_burst=1 command");
        expect(cmds.value(1) == "slice set 2 dfm_pre_de_emphasis=1", "dfm_pre_de_emphasis=1 command");
        expect(cmds.value(2) == "slice set 2 fm_deviation=2500", "fm_deviation command");
        expect(cmds.value(3) == "slice set 2 fm_tone_burst=0", "fm_tone_burst=0 command");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
