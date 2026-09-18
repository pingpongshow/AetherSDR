#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace AetherSDR {

// Pure message builders for the FreeDV Reporter socket.io protocol
// (qso.freedv.org, Engine.IO v4 / Socket.IO v4, protocol_version 2).
//
// Mirrors the official client: tmiw/freedv-backend
// src/reporting/FreeDVReporter.cpp @ 8990bd09 (used by freedv-gui master).
// Kept free of sockets so the payloads can be unit tested offline.
namespace FreeDvReporterProtocol {

constexpr int kProtocolVersion = 2;

struct Identity {
    QString callsign;
    QString gridSquare;
    QString version;      // e.g. "AetherSDR 0.8.19"
    bool    rxOnly{false};
};

// A reporter without callsign or grid is treated as view-only (same rule as
// FreeDVReporter::isValidForReporting()).
bool isValidForReporting(const Identity& id);

// "windows" | "linux" | "macos" | "other"
QString osString();

// Socket.IO connect auth.  View-only: {"role":"view","protocol_version":2}.
// Reporting: role "report", callsign, grid_square, version, rx_only, os,
// protocol_version.
QJsonObject authPayload(const Identity& id, bool reporting);

QJsonObject freqChangePayload(quint64 freqHz);                                  // freq_change
QJsonObject txReportPayload(const QString& mode, bool transmitting);            // tx_report
QJsonObject rxReportPayload(const QString& callsign, const QString& mode, int snr); // rx_report
QJsonObject messageUpdatePayload(const QString& message);                       // message_update

// Socket.IO frames over Engine.IO: "40<auth>" and "42[\"event\",{...}]".
QString encodeConnect(const QJsonObject& auth);
QString encodeEvent(const QString& name, const QJsonObject& payload);
QString encodeEvent(const QString& name);   // event without payload (hide_self/show_self)

// Loose callsign check used before sending rx_report for decoded text.
bool looksLikeCallsign(const QString& text);

// ── Visibility / session semantics (freedv-backend FreeDVReporter.cpp) ──
//
// The reporter is "hidden" while the modem is stopped: freedv-gui
// MainFrame::stopRxStream() calls hideFromView() ("hide_self"), and starting
// the modem calls showOurselves() ("show_self" followed by freq_change,
// tx_report and message_update with the last known values).  On
// connection_successful a hidden reporter sends only "hide_self"; a visible
// one re-sends freq_change / tx_report / message_update.
struct SessionState {
    bool    hidden{false};
    quint64 freqHz{0};
    QString txMode;
    bool    transmitting{false};
    QString message;
};

// Frames for connection_successful (FreeDVReporter::onConnectionSuccessful_).
QStringList connectionSuccessfulFrames(const SessionState& st);
// Frames for showOurselves() (FreeDVReporter::showOurselvesImpl_).
QStringList showSelfFrames(const SessionState& st);
// Frame for hideFromView() (FreeDVReporter::hideFromViewImpl_).
QString hideSelfFrame();

// RADE "receiving something" report.  freedv-gui MainFrame::OnTimer (100 ms
// plot timer): while RADE is synced and no callsign was decoded, every 10th
// tick sends rx_report with an EMPTY callsign, mode "RADEV1" and the rounded
// SNR, i.e. once per second.  Only for RADE (codec2 modes carry a callsign
// in their text channel).
constexpr int kSyncedRxReportIntervalMs = 1000;
bool sendsSyncedRxReport(const QString& reporterMode);

} // namespace FreeDvReporterProtocol
} // namespace AetherSDR
