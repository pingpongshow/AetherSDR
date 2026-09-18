// Offline tests for FreeDV Reporter payload builders (no network).
// Expected shapes follow tmiw/freedv-backend src/reporting/FreeDVReporter.cpp.
// Exit code 0 on all pass.

#include "core/FreeDvReporterProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;
namespace P = AetherSDR::FreeDvReporterProtocol;

namespace {
int g_failures = 0;

void check(bool ok, const char* what, const QString& detail = QString())
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s %s\n", ok ? " OK " : "FAIL", what, qPrintable(detail));
}

QJsonArray decodeEvent(const QString& frame)
{
    if (!frame.startsWith(QStringLiteral("42"))) {
        return {};
    }
    return QJsonDocument::fromJson(frame.mid(2).toUtf8()).array();
}
} // namespace

int main()
{
    P::Identity id;
    id.callsign = QStringLiteral(" k1abc ");
    id.gridSquare = QStringLiteral("FN42");
    id.version = QStringLiteral("AetherSDR 0.8.19");

    // View-only auth
    {
        const QJsonObject a = P::authPayload(id, false);
        check(a.size() == 2, "view auth has exactly 2 keys", QString::number(a.size()));
        check(a.value("role").toString() == "view", "view auth role");
        check(a.value("protocol_version").toInt() == 2, "view auth protocol_version=2");
        const QString frame = P::encodeConnect(a);
        check(frame.startsWith("40{") && frame.endsWith("}"), "connect frame is 40{...}", frame);
    }

    // Reporting auth
    {
        const QJsonObject a = P::authPayload(id, true);
        check(a.value("role").toString() == "report", "report auth role");
        check(a.value("callsign").toString() == "K1ABC", "callsign trimmed+upper", a.value("callsign").toString());
        check(a.value("grid_square").toString() == "FN42", "grid_square");
        check(a.value("version").toString() == "AetherSDR 0.8.19", "version");
        check(a.value("rx_only").isBool() && !a.value("rx_only").toBool(), "rx_only bool false");
        const QString os = a.value("os").toString();
        check(os == "macos" || os == "linux" || os == "windows" || os == "other", "os string", os);
        check(a.value("protocol_version").toInt() == 2, "report auth protocol_version=2");
        check(a.size() == 7, "report auth has 7 keys", QString::number(a.size()));
    }

    // Reporting without grid falls back to view-only
    {
        P::Identity noGrid = id;
        noGrid.gridSquare.clear();
        check(!P::isValidForReporting(noGrid), "missing grid is not valid for reporting");
        check(P::authPayload(noGrid, true).value("role").toString() == "view",
              "missing grid -> view role");
    }

    // freq_change
    {
        const QString f = P::encodeEvent("freq_change", P::freqChangePayload(14236000ULL));
        check(f == R"(42["freq_change",{"freq":14236000}])", "freq_change frame exact", f);
        const QJsonArray arr = decodeEvent(f);
        check(arr.size() == 2 && arr.at(1).toObject().value("freq").toInteger() == 14236000,
              "freq_change round-trips integer Hz");
        const QString big = P::encodeEvent("freq_change", P::freqChangePayload(1296250000ULL));
        check(big == R"(42["freq_change",{"freq":1296250000}])", "freq_change > 2^31 Hz stays integer", big);
    }

    // tx_report
    {
        const QString f = P::encodeEvent("tx_report", P::txReportPayload("700E", true));
        const QJsonObject o = decodeEvent(f).at(1).toObject();
        check(decodeEvent(f).at(0).toString() == "tx_report", "tx_report event name");
        check(o.value("mode").toString() == "700E" && o.value("transmitting").isBool()
                  && o.value("transmitting").toBool(),
              "tx_report payload", f);
        check(o.size() == 2, "tx_report has 2 keys");
    }

    // rx_report
    {
        const QString f = P::encodeEvent("rx_report", P::rxReportPayload("W1AW", "RADEV1", 7));
        const QJsonObject o = decodeEvent(f).at(1).toObject();
        check(o.value("callsign").toString() == "W1AW" && o.value("mode").toString() == "RADEV1",
              "rx_report callsign/mode", f);
        check(f.contains(R"("snr":7)") && !f.contains("7."), "rx_report snr is integer", f);
        check(o.size() == 3, "rx_report has 3 keys");
    }

    // message_update / no-payload events
    {
        const QString f = P::encodeEvent("message_update", P::messageUpdatePayload(QString()));
        check(f == R"(42["message_update",{"message":""}])", "message_update frame", f);
        check(P::encodeEvent("hide_self") == R"(42["hide_self"])", "hide_self frame", P::encodeEvent("hide_self"));
    }

    // hide/show + session restore (freedv-backend FreeDVReporter.cpp)
    {
        P::SessionState st;
        st.freqHz = 14236000ULL;
        st.txMode = "RADEV1";
        st.transmitting = false;
        st.hidden = true;
        const QStringList hidden = P::connectionSuccessfulFrames(st);
        check(hidden == QStringList{R"(42["hide_self"])"}, "hidden: connect sends only hide_self",
              hidden.join(' '));
        st.hidden = false;
        const QStringList vis = P::connectionSuccessfulFrames(st);
        check(vis.size() == 3 && vis[0].startsWith(R"(42["freq_change")")
                  && vis[1].startsWith(R"(42["tx_report")") && vis[2].startsWith(R"(42["message_update")"),
              "visible: connect sends freq/tx/message", vis.join(' '));
        const QStringList show = P::showSelfFrames(st);
        check(show.size() == 4 && show[0] == R"(42["show_self"])"
                  && show[1] == R"(42["freq_change",{"freq":14236000}])",
              "show_self then freq/tx/message", show.join(' '));
        check(P::hideSelfFrame() == R"(42["hide_self"])", "hide_self frame (helper)");
    }

    // RADE synced "--" report
    {
        check(P::sendsSyncedRxReport("RADEV1"), "RADE sends empty-callsign rx_report while synced");
        check(!P::sendsSyncedRxReport("700D"), "codec2 modes do not");
        check(P::kSyncedRxReportIntervalMs == 1000, "interval 10 x 100 ms");
        const QString f = P::encodeEvent("rx_report", P::rxReportPayload(QString(), "RADEV1", 4));
        check(f == R"(42["rx_report",{"callsign":"","mode":"RADEV1","snr":4}])",
              "empty-callsign rx_report frame", f);
    }

    // Callsign heuristic
    {
        check(P::looksLikeCallsign("K1ABC"), "K1ABC is callsign");
        check(P::looksLikeCallsign("vk3tpm"), "vk3tpm is callsign");
        check(P::looksLikeCallsign("DL/W1AW/P"), "DL/W1AW/P is callsign");
        check(!P::looksLikeCallsign("HELLO"), "HELLO is not callsign");
        check(!P::looksLikeCallsign("12345"), "12345 is not callsign");
        check(!P::looksLikeCallsign(""), "empty is not callsign");
    }

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
