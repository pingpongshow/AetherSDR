#include "FreeDvReporterProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace AetherSDR::FreeDvReporterProtocol {

bool isValidForReporting(const Identity& id)
{
    return !id.callsign.trimmed().isEmpty() && !id.gridSquare.trimmed().isEmpty();
}

QString osString()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MAC)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("other");
#endif
}

QJsonObject authPayload(const Identity& id, bool reporting)
{
    QJsonObject auth;
    if (!reporting || !isValidForReporting(id)) {
        auth[QStringLiteral("role")] = QStringLiteral("view");
    } else {
        auth[QStringLiteral("role")] = QStringLiteral("report");
        auth[QStringLiteral("callsign")] = id.callsign.trimmed().toUpper();
        auth[QStringLiteral("grid_square")] = id.gridSquare.trimmed();
        auth[QStringLiteral("version")] = id.version;
        auth[QStringLiteral("rx_only")] = id.rxOnly;
        auth[QStringLiteral("os")] = osString();
    }
    auth[QStringLiteral("protocol_version")] = kProtocolVersion;
    return auth;
}

QJsonObject freqChangePayload(quint64 freqHz)
{
    QJsonObject o;
    // Integer Hz (freedv-backend: yyjson_mut_obj_add_uint).  QJsonValue holds
    // a qint64 exactly and serializes it without a decimal point.
    o[QStringLiteral("freq")] = static_cast<qint64>(freqHz);
    return o;
}

QJsonObject txReportPayload(const QString& mode, bool transmitting)
{
    QJsonObject o;
    o[QStringLiteral("mode")] = mode;
    o[QStringLiteral("transmitting")] = transmitting;
    return o;
}

QJsonObject rxReportPayload(const QString& callsign, const QString& mode, int snr)
{
    QJsonObject o;
    o[QStringLiteral("callsign")] = callsign;
    o[QStringLiteral("mode")] = mode;
    o[QStringLiteral("snr")] = snr;
    return o;
}

QJsonObject messageUpdatePayload(const QString& message)
{
    QJsonObject o;
    o[QStringLiteral("message")] = message;
    return o;
}

QString encodeConnect(const QJsonObject& auth)
{
    return QStringLiteral("40")
         + QString::fromUtf8(QJsonDocument(auth).toJson(QJsonDocument::Compact));
}

QString encodeEvent(const QString& name, const QJsonObject& payload)
{
    QJsonArray arr;
    arr.append(name);
    arr.append(payload);
    return QStringLiteral("42")
         + QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QString encodeEvent(const QString& name)
{
    QJsonArray arr;
    arr.append(name);
    return QStringLiteral("42")
         + QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

bool looksLikeCallsign(const QString& text)
{
    static const QRegularExpression re(
        QStringLiteral("^([A-Z0-9]{1,4}/)?[A-Z0-9]{0,3}[0-9][A-Z0-9]{0,4}[A-Z](/[A-Z0-9]{1,4})?$"));
    const QString t = text.trimmed().toUpper();
    return t.size() >= 3 && t.size() <= 16 && re.match(t).hasMatch();
}

QStringList showSelfFrames(const SessionState& st)
{
    return {encodeEvent(QStringLiteral("show_self")),
            encodeEvent(QStringLiteral("freq_change"), freqChangePayload(st.freqHz)),
            encodeEvent(QStringLiteral("tx_report"), txReportPayload(st.txMode, st.transmitting)),
            encodeEvent(QStringLiteral("message_update"), messageUpdatePayload(st.message))};
}

QStringList connectionSuccessfulFrames(const SessionState& st)
{
    if (st.hidden) {
        return {hideSelfFrame()};
    }
    return {encodeEvent(QStringLiteral("freq_change"), freqChangePayload(st.freqHz)),
            encodeEvent(QStringLiteral("tx_report"), txReportPayload(st.txMode, st.transmitting)),
            encodeEvent(QStringLiteral("message_update"), messageUpdatePayload(st.message))};
}

QString hideSelfFrame()
{
    return encodeEvent(QStringLiteral("hide_self"));
}

bool sendsSyncedRxReport(const QString& reporterMode)
{
    return reporterMode == QStringLiteral("RADEV1");
}

} // namespace AetherSDR::FreeDvReporterProtocol
