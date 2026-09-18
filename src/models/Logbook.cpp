#include "Logbook.h"

#include "core/AppSettings.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringDecoder>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcLogbook, "aether.logbook", QtWarningMsg)

namespace {

constexpr int    kSchemaVersion   = 1;
constexpr int    kImportBatchSize = 2000;
constexpr qint64 kDupeWindowSecs  = 60;      // import / ingest duplicate window
constexpr double kEarthRadiusKm   = 6371.0;
constexpr double kPi              = 3.14159265358979323846;

// Column order used by INSERT / UPDATE.  `id` is not in the list.
const QStringList& columns()
{
    static const QStringList cols = {
        "call", "band", "mode", "submode", "mode_group", "freq", "freq_rx",
        "time_on", "time_off", "rst_sent", "rst_rcvd", "name", "qth", "grid",
        "state", "country", "dxcc_prefix", "dxcc", "continent", "cqz", "ituz",
        "comment", "station_call", "operator", "my_grid", "tx_pwr",
        "contest_id", "srx", "stx", "srx_string", "stx_string", "slice_id",
        "so2r_role", "recording",
        "up_lotw", "up_lotw_date", "up_qrz", "up_qrz_date",
        "up_clublog", "up_clublog_date", "up_eqsl", "up_eqsl_date", "extra",
    };
    return cols;
}

QString programVersion()
{
    const QString v = QCoreApplication::applicationVersion();
    return v.isEmpty() ? QStringLiteral("unknown") : v;
}

QVariant dateVar(const QDate& d) { return d.isValid() ? QVariant(d.toString(Qt::ISODate)) : QVariant(QString()); }
QVariant timeVar(const QDateTime& t) { return t.isValid() ? QVariant(t.toSecsSinceEpoch()) : QVariant(qint64(0)); }

QVariantList toRow(const LogQso& q)
{
    QJsonObject extra;
    for (auto it = q.extra.cbegin(); it != q.extra.cend(); ++it) {
        extra.insert(it.key(), it.value());
    }
    const QString extraJson = extra.isEmpty()
        ? QString()
        : QString::fromUtf8(QJsonDocument(extra).toJson(QJsonDocument::Compact));

    return {
        q.call, q.band, q.mode, q.submode, LogUtil::modeGroup(q.mode, q.submode),
        q.freqMhz, q.freqRxMhz, timeVar(q.timeOn), timeVar(q.timeOff),
        q.rstSent, q.rstRcvd, q.name, q.qth, q.gridsquare, q.state,
        q.country, q.dxccPrefix, q.dxcc, q.continent, q.cqZone, q.ituZone,
        q.comment, q.stationCallsign, q.operatorCall, q.myGrid, q.txPowerW,
        q.contestId, q.srx, q.stx, q.srxString, q.stxString, q.sliceId,
        q.so2rRole, q.recordingPath,
        static_cast<int>(q.upload[0]), dateVar(q.uploadDate[0]),
        static_cast<int>(q.upload[1]), dateVar(q.uploadDate[1]),
        static_cast<int>(q.upload[2]), dateVar(q.uploadDate[2]),
        static_cast<int>(q.upload[3]), dateVar(q.uploadDate[3]),
        extraJson,
    };
}

QDateTime fromSecs(qint64 s)
{
    return s > 0 ? QDateTime::fromSecsSinceEpoch(s, QTimeZone::UTC) : QDateTime();
}

LogQso fromQuery(const QSqlQuery& r)
{
    LogQso q;
    q.id          = r.value("id").toLongLong();
    q.call        = r.value("call").toString();
    q.band        = r.value("band").toString();
    q.mode        = r.value("mode").toString();
    q.submode     = r.value("submode").toString();
    q.freqMhz     = r.value("freq").toDouble();
    q.freqRxMhz   = r.value("freq_rx").toDouble();
    q.timeOn      = fromSecs(r.value("time_on").toLongLong());
    q.timeOff     = fromSecs(r.value("time_off").toLongLong());
    q.rstSent     = r.value("rst_sent").toString();
    q.rstRcvd     = r.value("rst_rcvd").toString();
    q.name        = r.value("name").toString();
    q.qth         = r.value("qth").toString();
    q.gridsquare  = r.value("grid").toString();
    q.state       = r.value("state").toString();
    q.country     = r.value("country").toString();
    q.dxccPrefix  = r.value("dxcc_prefix").toString();
    q.dxcc        = r.value("dxcc").toInt();
    q.continent   = r.value("continent").toString();
    q.cqZone      = r.value("cqz").toInt();
    q.ituZone     = r.value("ituz").toInt();
    q.comment     = r.value("comment").toString();
    q.stationCallsign = r.value("station_call").toString();
    q.operatorCall    = r.value("operator").toString();
    q.myGrid      = r.value("my_grid").toString();
    q.txPowerW    = r.value("tx_pwr").toDouble();
    q.contestId   = r.value("contest_id").toString();
    q.srx         = r.value("srx").toInt();
    q.stx         = r.value("stx").toInt();
    q.srxString   = r.value("srx_string").toString();
    q.stxString   = r.value("stx_string").toString();
    q.sliceId     = r.value("slice_id").toInt();
    q.so2rRole    = r.value("so2r_role").toString();
    q.recordingPath = r.value("recording").toString();
    static const char* const upCols[kUploadServiceCount][2] = {
        {"up_lotw", "up_lotw_date"}, {"up_qrz", "up_qrz_date"},
        {"up_clublog", "up_clublog_date"}, {"up_eqsl", "up_eqsl_date"}};
    for (int i = 0; i < kUploadServiceCount; ++i) {
        q.upload[i] = static_cast<UploadState>(r.value(upCols[i][0]).toInt());
        q.uploadDate[i] = QDate::fromString(r.value(upCols[i][1]).toString(), Qt::ISODate);
    }
    const QString extra = r.value("extra").toString();
    if (!extra.isEmpty()) {
        const QJsonObject o = QJsonDocument::fromJson(extra.toUtf8()).object();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            q.extra.insert(it.key(), it.value().toString());
        }
    }
    return q;
}

QString insertSql()
{
    const QStringList& cols = columns();
    QStringList ph;
    ph.reserve(cols.size());
    for (int i = 0; i < cols.size(); ++i) {
        ph << QStringLiteral("?");
    }
    return QStringLiteral("INSERT INTO qso (%1, created) VALUES (%2, ?)")
        .arg(cols.join(", "), ph.join(", "));
}

bool execInsert(QSqlDatabase& db, const LogQso& q, qint64* newId, QString* err)
{
    QSqlQuery ins(db);
    ins.prepare(insertSql());
    for (const QVariant& v : toRow(q)) {
        ins.addBindValue(v);
    }
    ins.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!ins.exec()) {
        if (err) {
            *err = ins.lastError().text();
        }
        return false;
    }
    if (newId) {
        *newId = ins.lastInsertId().toLongLong();
    }
    return true;
}

// Is an equivalent QSO (same call, band, mode group, time within window) present?
bool existsSimilar(QSqlDatabase& db, const LogQso& q)
{
    QSqlQuery s(db);
    s.prepare("SELECT 1 FROM qso WHERE call = ? AND band = ? AND mode_group = ? "
              "AND time_on BETWEEN ? AND ? LIMIT 1");
    const qint64 t = q.timeOn.isValid() ? q.timeOn.toSecsSinceEpoch() : 0;
    s.addBindValue(q.call);
    s.addBindValue(q.band);
    s.addBindValue(LogUtil::modeGroup(q.mode, q.submode));
    s.addBindValue(t - kDupeWindowSecs);
    s.addBindValue(t + kDupeWindowSecs);
    return s.exec() && s.next();
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// LogUtil
// ═══════════════════════════════════════════════════════════════════════════

namespace LogUtil {

QString freqToBand(double mhz)
{
    struct Band { double lo; double hi; const char* name; };
    // ADIF 3.1.4 Band enumeration (subset relevant to HF/VHF/UHF + LF/MF).
    static constexpr Band kBands[] = {
        {0.1357, 0.1378, "2190m"}, {0.472, 0.479, "630m"}, {0.501, 0.504, "560m"},
        {1.8, 2.0, "160m"}, {3.5, 4.0, "80m"}, {5.06, 5.45, "60m"},
        {7.0, 7.3, "40m"}, {10.1, 10.15, "30m"}, {14.0, 14.35, "20m"},
        {18.068, 18.168, "17m"}, {21.0, 21.45, "15m"}, {24.890, 24.99, "12m"},
        {28.0, 29.7, "10m"}, {40.0, 45.0, "8m"}, {50.0, 54.0, "6m"},
        {54.000001, 69.9, "5m"}, {70.0, 71.0, "4m"}, {144.0, 148.0, "2m"},
        {222.0, 225.0, "1.25m"}, {420.0, 450.0, "70cm"}, {902.0, 928.0, "33cm"},
        {1240.0, 1300.0, "23cm"}, {2300.0, 2450.0, "13cm"},
    };
    for (const Band& b : kBands) {
        if (mhz >= b.lo && mhz <= b.hi) {
            return QString::fromLatin1(b.name);
        }
    }
    return {};
}

QString modeGroup(const QString& modeIn, const QString& submodeIn)
{
    const QString m = modeIn.trimmed().toUpper();
    const QString sub = submodeIn.trimmed().toUpper();
    if (m == "CW") {
        return QStringLiteral("CW");
    }
    if (m == "SSB" || m == "USB" || m == "LSB" || m == "AM" || m == "FM" ||
        m == "DIGITALVOICE" || m == "DSTAR" || m == "C4FM" || m == "DMR" ||
        m == "FREEDV" || sub == "FREEDV") {
        return QStringLiteral("PHONE");
    }
    if (m.isEmpty()) {
        return {};
    }
    return QStringLiteral("DATA");
}

void flexModeToAdif(const QString& flexMode, QString& mode, QString& submode)
{
    const QString f = flexMode.trimmed().toUpper();
    submode.clear();
    if (f == "USB" || f == "LSB") {
        mode = QStringLiteral("SSB");
        submode = f;
    } else if (f == "CW" || f == "CWL" || f == "CWU") {
        mode = QStringLiteral("CW");
    } else if (f == "AM" || f == "SAM") {
        mode = QStringLiteral("AM");
    } else if (f == "FM" || f == "NFM" || f == "DFM") {
        mode = QStringLiteral("FM");
    } else if (f == "RTTY") {
        mode = QStringLiteral("RTTY");
    } else if (f == "FDV" || f == "FDVU" || f == "FDVL" || f == "RADE") {
        // ADIF 3.1.x: FreeDV is SUBMODE FREEDV of MODE DIGITALVOICE.
        mode = QStringLiteral("DIGITALVOICE");
        submode = QStringLiteral("FREEDV");
    } else if (f == "DIGU" || f == "DIGL") {
        // The slice mode does not say which digital mode is in use; the
        // user-selectable default applies (maintainer review: default FT8).
        const QString def = AppSettings::instance()
                                .value("LogbookDigiModeDefault", "FT8").toString().toUpper();
        if (def == "FT4" || def == "JS8") {
            mode = QStringLiteral("MFSK");
            submode = def;
        } else if (def == "PSK31" || def == "PSK63") {
            mode = QStringLiteral("PSK");
            submode = def;
        } else {
            mode = def;
        }
    } else {
        mode = f;
    }
}

QString normalizeCall(const QString& call)
{
    return call.trimmed().toUpper();
}

QString baseCall(const QString& callIn)
{
    const QString call = normalizeCall(callIn);
    if (!call.contains('/')) {
        return call;
    }
    // The base call is the longest segment that contains a digit and a
    // letter; ties resolved to the first.  Handles W1AW/P, VP2E/K1ABC, K1ABC/VP2E.
    QString best;
    for (const QString& part : call.split('/', Qt::SkipEmptyParts)) {
        bool hasDigit = false;
        bool hasAlpha = false;
        for (const QChar c : part) {
            hasDigit = hasDigit || c.isDigit();
            hasAlpha = hasAlpha || c.isLetter();
        }
        if (hasDigit && hasAlpha && part.size() > best.size()) {
            best = part;
        }
    }
    return best.isEmpty() ? call : best;
}

bool isValidGrid(const QString& gridIn)
{
    const QString g = gridIn.trimmed().toUpper();
    if (g.size() != 4 && g.size() != 6 && g.size() != 8) {
        return false;
    }
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') {
        return false;
    }
    if (!g[2].isDigit() || !g[3].isDigit()) {
        return false;
    }
    if (g.size() >= 6 && (g[4] < 'A' || g[4] > 'X' || g[5] < 'A' || g[5] > 'X')) {
        return false;
    }
    if (g.size() == 8 && (!g[6].isDigit() || !g[7].isDigit())) {
        return false;
    }
    return true;
}

bool gridToLatLon(const QString& gridIn, double& lat, double& lon)
{
    if (!isValidGrid(gridIn)) {
        return false;
    }
    const QString g = gridIn.trimmed().toUpper();
    lon = (g[0].unicode() - 'A') * 20.0 - 180.0;
    lat = (g[1].unicode() - 'A') * 10.0 - 90.0;
    lon += (g[2].unicode() - '0') * 2.0;
    lat += (g[3].unicode() - '0') * 1.0;
    double lonSize = 2.0;
    double latSize = 1.0;
    if (g.size() >= 6) {
        lonSize = 2.0 / 24.0;
        latSize = 1.0 / 24.0;
        lon += (g[4].unicode() - 'A') * lonSize;
        lat += (g[5].unicode() - 'A') * latSize;
    }
    if (g.size() == 8) {
        lonSize /= 10.0;
        latSize /= 10.0;
        lon += (g[6].unicode() - '0') * lonSize;
        lat += (g[7].unicode() - '0') * latSize;
    }
    lon += lonSize / 2.0;
    lat += latSize / 2.0;
    return true;
}

double distanceKm(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = lat1 * kPi / 180.0;
    const double p2 = lat2 * kPi / 180.0;
    const double dp = (lat2 - lat1) * kPi / 180.0;
    const double dl = (lon2 - lon1) * kPi / 180.0;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0 * kEarthRadiusKm * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = lat1 * kPi / 180.0;
    const double p2 = lat2 * kPi / 180.0;
    const double dl = (lon2 - lon1) * kPi / 180.0;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double b = std::atan2(y, x) * 180.0 / kPi;
    if (b < 0) {
        b += 360.0;
    }
    return b;
}

QString uploadServiceName(UploadService s)
{
    switch (s) {
    case UploadService::Lotw:    return QStringLiteral("LoTW");
    case UploadService::Qrz:     return QStringLiteral("QRZ");
    case UploadService::ClubLog: return QStringLiteral("Club Log");
    case UploadService::Eqsl:    return QStringLiteral("eQSL");
    }
    return {};
}

} // namespace LogUtil

// ═══════════════════════════════════════════════════════════════════════════
// ADIF codec
// ═══════════════════════════════════════════════════════════════════════════

namespace Adif {

namespace {

QString fmtFreq(double mhz)
{
    QString s = QString::number(mhz, 'f', 6);
    while (s.endsWith('0') && !s.endsWith(".0")) {
        s.chop(1);
    }
    return s;
}

// ADIF upload-status letter for each service/state.
QString statusLetter(UploadService svc, UploadState st)
{
    const bool qslStyle = (svc == UploadService::Lotw || svc == UploadService::Eqsl);
    switch (st) {
    case UploadState::Uploaded:    return QStringLiteral("Y");
    case UploadState::Modified:    return qslStyle ? QStringLiteral("Y") : QStringLiteral("M");
    case UploadState::Pending:     return qslStyle ? QStringLiteral("R") : QString();
    case UploadState::DoNotUpload: return QStringLiteral("N");
    case UploadState::Failed:      return qslStyle ? QStringLiteral("R") : QString();
    case UploadState::NotSent:     return {};
    }
    return {};
}

UploadState stateFromLetter(const QString& l)
{
    const QString u = l.trimmed().toUpper();
    if (u == "Y") { return UploadState::Uploaded; }
    if (u == "M") { return UploadState::Modified; }
    if (u == "R" || u == "Q") { return UploadState::Pending; }
    if (u == "N") { return UploadState::DoNotUpload; }
    return UploadState::NotSent;
}

struct UploadFieldNames { const char* status; const char* date; };
constexpr UploadFieldNames kUploadFields[kUploadServiceCount] = {
    {"LOTW_QSL_SENT", "LOTW_QSLSDATE"},
    {"QRZCOM_QSO_UPLOAD_STATUS", "QRZCOM_QSO_UPLOAD_DATE"},
    {"CLUBLOG_QSO_UPLOAD_STATUS", "CLUBLOG_QSO_UPLOAD_DATE"},
    {"EQSL_QSL_SENT", "EQSL_QSLSDATE"},
};

// Byte index of the end of `nChars` UTF-8 characters starting at `start`.
qint64 utf8CharSpan(const QByteArray& d, qint64 start, qint64 nChars)
{
    qint64 p = start;
    const qint64 n = d.size();
    for (qint64 c = 0; c < nChars && p < n; ++c) {
        const auto b = static_cast<unsigned char>(d[p]);
        int len = 1;
        if (b >= 0xF0) { len = 4; }
        else if (b >= 0xE0) { len = 3; }
        else if (b >= 0xC0) { len = 2; }
        p += len;
    }
    return std::min(p, n);
}

bool followedByTag(const QByteArray& d, qint64 p)
{
    const qint64 n = d.size();
    while (p < n && (d[p] == ' ' || d[p] == '\r' || d[p] == '\n' || d[p] == '\t')) {
        ++p;
    }
    return p >= n || d[p] == '<';
}

QString decodeValue(const QByteArray& raw)
{
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString s = dec.decode(raw);
    if (dec.hasError()) {
        return QString::fromLatin1(raw);   // legacy Latin-1 loggers
    }
    return s;
}

QDateTime parseDateTime(const QString& date, const QString& time)
{
    const QDate d = QDate::fromString(date.trimmed(), "yyyyMMdd");
    if (!d.isValid()) {
        return {};
    }
    QString t = time.trimmed();
    QTime tm;
    if (t.size() >= 6) {
        tm = QTime::fromString(t.left(6), "HHmmss");
    } else if (t.size() >= 4) {
        tm = QTime::fromString(t.left(4), "HHmm");
    }
    if (!tm.isValid()) {
        tm = QTime(0, 0);
    }
    return QDateTime(d, tm, QTimeZone::UTC);
}

} // namespace

QByteArray field(const QString& name, const QString& value)
{
    if (value.isEmpty()) {
        return {};
    }
    const QByteArray v = value.toUtf8();
    QByteArray out;
    out.reserve(v.size() + name.size() + 8);
    out += '<';
    out += name.toUpper().toLatin1();
    out += ':';
    out += QByteArray::number(v.size());
    out += '>';
    out += v;
    out += ' ';
    return out;
}

QByteArray header(const QString& programVersion)
{
    QByteArray h;
    h += "AetherSDR ADIF export\n";
    h += field("ADIF_VER", QString::fromLatin1(kAdifVersion));
    h += field("PROGRAMID", QStringLiteral("AetherSDR"));
    h += field("PROGRAMVERSION", programVersion);
    h += field("CREATED_TIMESTAMP",
               QDateTime::currentDateTimeUtc().toString("yyyyMMdd HHmmss"));
    h += "\n<EOH>\n";
    return h;
}

QByteArray record(const LogQso& q, const WriteOptions& opt)
{
    QByteArray r;
    r.reserve(512);
    auto add = [&r](const char* n, const QString& v) { r += field(QString::fromLatin1(n), v); };
    auto addInt = [&add](const char* n, int v) { if (v > 0) { add(n, QString::number(v)); } };

    add("CALL", q.call);
    if (q.timeOn.isValid()) {
        const QDateTime t = q.timeOn.toUTC();
        add("QSO_DATE", t.toString("yyyyMMdd"));
        add("TIME_ON", t.toString("HHmmss"));
    }
    if (q.timeOff.isValid()) {
        const QDateTime t = q.timeOff.toUTC();
        add("QSO_DATE_OFF", t.toString("yyyyMMdd"));
        add("TIME_OFF", t.toString("HHmmss"));
    }
    add("BAND", q.band);
    if (q.freqMhz > 0) {
        add("FREQ", fmtFreq(q.freqMhz));
    }
    if (q.freqRxMhz > 0 && std::abs(q.freqRxMhz - q.freqMhz) > 1e-7) {
        add("FREQ_RX", fmtFreq(q.freqRxMhz));
        add("BAND_RX", LogUtil::freqToBand(q.freqRxMhz));
    }
    add("MODE", q.mode);
    add("SUBMODE", q.submode);
    add("RST_SENT", q.rstSent);
    add("RST_RCVD", q.rstRcvd);
    add("NAME", q.name);
    add("QTH", q.qth);
    add("GRIDSQUARE", q.gridsquare);
    add("STATE", q.state);
    add("COUNTRY", q.country);
    addInt("DXCC", q.dxcc);
    add("CONT", q.continent);
    addInt("CQZ", q.cqZone);
    addInt("ITUZ", q.ituZone);
    add("COMMENT", q.comment);
    add("STATION_CALLSIGN", q.stationCallsign);
    add("OPERATOR", q.operatorCall);
    add("MY_GRIDSQUARE", q.myGrid);
    if (q.txPowerW > 0) {
        add("TX_PWR", QString::number(q.txPowerW, 'g', 6));
    }
    add("CONTEST_ID", q.contestId);
    addInt("SRX", q.srx);
    addInt("STX", q.stx);
    add("SRX_STRING", q.srxString);
    add("STX_STRING", q.stxString);

    if (opt.includeUploadStatus) {
        for (int i = 0; i < kUploadServiceCount; ++i) {
            const QString letter = statusLetter(static_cast<UploadService>(i), q.upload[i]);
            add(kUploadFields[i].status, letter);
            if (!letter.isEmpty() && q.uploadDate[i].isValid()) {
                add(kUploadFields[i].date, q.uploadDate[i].toString("yyyyMMdd"));
            }
        }
    }
    if (opt.includeAppFields) {
        if (q.sliceId >= 0) {
            add("APP_AETHERSDR_SLICE", QString::number(q.sliceId));
        }
        add("APP_AETHERSDR_SO2R", q.so2rRole);
        add("APP_AETHERSDR_RECORDING", q.recordingPath);
    }
    if (opt.includeExtra) {
        for (auto it = q.extra.cbegin(); it != q.extra.cend(); ++it) {
            if (!opt.includeAppFields && it.key().startsWith("APP_")) {
                continue;
            }
            r += field(it.key(), it.value());
        }
    }
    r += "<EOR>\n";
    return r;
}

qint64 parse(const QByteArray& d, const RecordFn& fn,
             const std::function<void(qint64)>& progress)
{
    const qint64 n = d.size();
    qint64 p = 0;
    qint64 delivered = 0;
    qint64 lastProgress = 0;
    QMap<QString, QString> fields;

    // Skip the header: everything up to <EOH> (case-insensitive, searched in
    // the first 64 KiB).  Header-less files start directly with a field.
    const qint64 eoh = d.left(64 * 1024).toUpper().indexOf("<EOH>");
    if (eoh >= 0) {
        p = eoh + 5;
    } else if (n > 0 && d[0] != '<') {
        const qint64 lt = d.indexOf('<');
        p = lt < 0 ? n : lt;
    }

    while (p < n) {
        const qint64 lt = d.indexOf('<', p);
        if (lt < 0) {
            break;
        }
        const qint64 gt = d.indexOf('>', lt + 1);
        if (gt < 0) {
            break;
        }
        const QByteArray tag = d.mid(lt + 1, gt - lt - 1);
        p = gt + 1;
        const QList<QByteArray> parts = tag.split(':');
        const QString name = QString::fromLatin1(parts.value(0)).trimmed().toUpper();
        if (name.isEmpty()) {
            continue;
        }
        if (name == "EOR") {
            if (!fields.isEmpty()) {
                ++delivered;
                if (!fn(fields)) {
                    return delivered;
                }
                fields.clear();
            }
            if (progress && p - lastProgress > 256 * 1024) {
                lastProgress = p;
                progress(p);
            }
            continue;
        }
        if (name == "EOH") {
            fields.clear();
            continue;
        }
        if (parts.size() < 2) {
            continue;   // malformed tag without length
        }
        bool ok = false;
        const qint64 len = parts[1].trimmed().toLongLong(&ok);
        if (!ok || len < 0) {
            continue;
        }
        qint64 end = std::min(p + len, n);
        // Tolerate writers that count characters rather than bytes: if the
        // byte interpretation does not land on a tag boundary but the
        // character interpretation does, use characters.
        if (!followedByTag(d, end)) {
            const qint64 charEnd = utf8CharSpan(d, p, len);
            if (charEnd != end && followedByTag(d, charEnd)) {
                end = charEnd;
            }
        }
        fields.insert(name, decodeValue(d.mid(p, end - p)));
        p = end;
    }
    if (progress) {
        progress(n);
    }
    return delivered;
}

LogQso toQso(const QMap<QString, QString>& in)
{
    QMap<QString, QString> f = in;
    auto take = [&f](const char* k) { return f.take(QString::fromLatin1(k)).trimmed(); };

    LogQso q;
    q.call = LogUtil::normalizeCall(take("CALL"));
    const QString date = take("QSO_DATE");
    const QString timeOn = take("TIME_ON");
    q.timeOn = parseDateTime(date, timeOn);
    QString dateOff = take("QSO_DATE_OFF");
    const QString timeOff = take("TIME_OFF");
    if (dateOff.isEmpty() && !timeOff.isEmpty()) {
        dateOff = date;
    }
    if (!timeOff.isEmpty()) {
        q.timeOff = parseDateTime(dateOff, timeOff);
        if (q.timeOff.isValid() && q.timeOn.isValid() && q.timeOff < q.timeOn &&
            dateOff == date) {
            q.timeOff = q.timeOff.addDays(1);   // crossed midnight UTC
        }
    }
    q.freqMhz = take("FREQ").toDouble();
    q.freqRxMhz = take("FREQ_RX").toDouble();
    q.band = take("BAND").toLower();
    f.remove("BAND_RX");
    if (!q.band.isEmpty() && !q.band.endsWith('m')) {
        // Some loggers write bare numbers ("20")
        static const QHash<QString, QString> bareBands = {
            {"160", "160m"}, {"80", "80m"}, {"60", "60m"}, {"40", "40m"},
            {"30", "30m"}, {"20", "20m"}, {"17", "17m"}, {"15", "15m"},
            {"12", "12m"}, {"10", "10m"}, {"6", "6m"}, {"4", "4m"}, {"2", "2m"},
        };
        q.band = bareBands.value(q.band, q.band);
    }
    if (q.band.isEmpty() && q.freqMhz > 0) {
        q.band = LogUtil::freqToBand(q.freqMhz);
    }
    q.mode = take("MODE").toUpper();
    q.submode = take("SUBMODE").toUpper();
    q.rstSent = take("RST_SENT");
    q.rstRcvd = take("RST_RCVD");
    q.name = take("NAME");
    q.qth = take("QTH");
    q.gridsquare = take("GRIDSQUARE").toUpper();
    q.state = take("STATE").toUpper();
    q.country = take("COUNTRY");
    q.dxcc = take("DXCC").toInt();
    q.continent = take("CONT").toUpper();
    q.cqZone = take("CQZ").toInt();
    q.ituZone = take("ITUZ").toInt();
    q.comment = take("COMMENT");
    if (q.comment.isEmpty()) {
        q.comment = f.value("NOTES");
    }
    q.stationCallsign = LogUtil::normalizeCall(take("STATION_CALLSIGN"));
    q.operatorCall = LogUtil::normalizeCall(take("OPERATOR"));
    if (q.stationCallsign.isEmpty()) {
        q.stationCallsign = q.operatorCall;
    }
    q.myGrid = take("MY_GRIDSQUARE").toUpper();
    q.txPowerW = take("TX_PWR").toDouble();
    q.contestId = take("CONTEST_ID");
    q.srx = take("SRX").toInt();
    q.stx = take("STX").toInt();
    q.srxString = take("SRX_STRING");
    q.stxString = take("STX_STRING");
    const QString slice = take("APP_AETHERSDR_SLICE");
    q.sliceId = slice.isEmpty() ? -1 : slice.toInt();
    q.so2rRole = take("APP_AETHERSDR_SO2R");
    q.recordingPath = take("APP_AETHERSDR_RECORDING");
    for (int i = 0; i < kUploadServiceCount; ++i) {
        q.upload[i] = stateFromLetter(take(kUploadFields[i].status));
        q.uploadDate[i] = QDate::fromString(take(kUploadFields[i].date), "yyyyMMdd");
    }
    // Everything else is preserved verbatim.
    for (auto it = f.cbegin(); it != f.cend(); ++it) {
        if (!it.value().isEmpty()) {
            q.extra.insert(it.key(), it.value());
        }
    }
    return q;
}

} // namespace Adif

// ═══════════════════════════════════════════════════════════════════════════
// WorkedStatus
// ═══════════════════════════════════════════════════════════════════════════

DxccStatus WorkedStatus::dxccStatus() const
{
    if (!dxccKnown) {
        return DxccStatus::Unknown;
    }
    if (!dxccWorked) {
        return DxccStatus::NewDxcc;
    }
    if (!dxccBandWorked) {
        return DxccStatus::NewBand;
    }
    if (!dxccBandModeWorked) {
        return DxccStatus::NewMode;
    }
    return DxccStatus::Worked;
}

// ═══════════════════════════════════════════════════════════════════════════
// Logbook
// ═══════════════════════════════════════════════════════════════════════════

Logbook::Logbook(QObject* parent)
    : QObject(parent)
    , m_connName(QStringLiteral("aether_logbook_") +
                 QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    qRegisterMetaType<AetherSDR::LogQso>();
    qRegisterMetaType<AetherSDR::ImportResult>();
}

Logbook::~Logbook()
{
    cancelImport();
    if (m_importThread) {
        m_importThread->wait();
        m_importThread.reset();
    }
    close();
}

QString Logbook::defaultPath()
{
    const QString custom = AppSettings::instance().value("LogbookPath", QString()).toString();
    if (!custom.isEmpty()) {
        return custom;
    }
    const QString dir = QFileInfo(AppSettings::instance().filePath()).absolutePath();
    return dir + QStringLiteral("/logbook.sqlite");
}

bool Logbook::open(const QString& pathIn)
{
    close();
    m_path = pathIn.isEmpty() ? defaultPath() : pathIn;
    QDir().mkpath(QFileInfo(m_path).absolutePath());

    if (!QSqlDatabase::isDriverAvailable("QSQLITE")) {
        m_lastError = tr("Qt SQLite driver (QSQLITE) not available");
        qCWarning(lcLogbook) << m_lastError;
        return false;
    }
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
        db.setDatabaseName(m_path);
        if (!db.open()) {
            m_lastError = db.lastError().text();
            qCWarning(lcLogbook) << "open failed:" << m_path << m_lastError;
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(m_connName);
            return false;
        }
        QSqlQuery p(db);
        p.exec("PRAGMA journal_mode=WAL");
        p.exec("PRAGMA synchronous=NORMAL");
        p.exec("PRAGMA busy_timeout=5000");
    }
    if (!createSchema()) {
        close();
        return false;
    }
    if (!m_cty.isLoaded()) {
        loadCtyDat();
    }
    rebuildCache();
    return true;
}

void Logbook::close()
{
    if (!QSqlDatabase::contains(m_connName)) {
        return;
    }
    {
        QSqlDatabase db = QSqlDatabase::database(m_connName, false);
        if (db.isOpen()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connName);
    m_byCall.clear();
    m_dxcc.clear();
}

bool Logbook::isOpen() const
{
    return QSqlDatabase::contains(m_connName) &&
           QSqlDatabase::database(m_connName, false).isOpen();
}

bool Logbook::loadCtyDat(const QString& pathOrResource)
{
    const bool ok = pathOrResource.startsWith(':')
        ? m_cty.loadFromResource(pathOrResource)
        : m_cty.loadFromFile(pathOrResource);
    if (!ok) {
        qCWarning(lcLogbook) << "cty.dat not loaded from" << pathOrResource;
    }
    return ok;
}

bool Logbook::createSchema()
{
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    q.exec("PRAGMA user_version");
    const int ver = q.next() ? q.value(0).toInt() : 0;
    if (ver >= kSchemaVersion) {
        return true;
    }
    const char* ddl[] = {
        "CREATE TABLE IF NOT EXISTS qso ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " call TEXT NOT NULL, band TEXT, mode TEXT, submode TEXT, mode_group TEXT,"
        " freq REAL, freq_rx REAL, time_on INTEGER, time_off INTEGER,"
        " rst_sent TEXT, rst_rcvd TEXT, name TEXT, qth TEXT, grid TEXT, state TEXT,"
        " country TEXT, dxcc_prefix TEXT, dxcc INTEGER, continent TEXT,"
        " cqz INTEGER, ituz INTEGER, comment TEXT,"
        " station_call TEXT, operator TEXT, my_grid TEXT, tx_pwr REAL,"
        " contest_id TEXT, srx INTEGER, stx INTEGER, srx_string TEXT, stx_string TEXT,"
        " slice_id INTEGER, so2r_role TEXT, recording TEXT,"
        " up_lotw INTEGER DEFAULT 0, up_lotw_date TEXT,"
        " up_qrz INTEGER DEFAULT 0, up_qrz_date TEXT,"
        " up_clublog INTEGER DEFAULT 0, up_clublog_date TEXT,"
        " up_eqsl INTEGER DEFAULT 0, up_eqsl_date TEXT,"
        " extra TEXT, created INTEGER)",
        "CREATE INDEX IF NOT EXISTS idx_qso_call ON qso(call)",
        "CREATE INDEX IF NOT EXISTS idx_qso_time ON qso(time_on)",
        "CREATE INDEX IF NOT EXISTS idx_qso_contest ON qso(contest_id, time_on)",
        "CREATE INDEX IF NOT EXISTS idx_qso_dxcc ON qso(dxcc_prefix, band, mode_group)",
    };
    db.transaction();
    for (const char* s : ddl) {
        if (!q.exec(QString::fromLatin1(s))) {
            m_lastError = q.lastError().text();
            qCWarning(lcLogbook) << "schema:" << m_lastError;
            db.rollback();
            return false;
        }
    }
    q.exec(QStringLiteral("PRAGMA user_version=%1").arg(kSchemaVersion));
    return db.commit();
}

quint8 Logbook::modeBit(const QString& g)
{
    if (g == "CW") { return 1; }
    if (g == "PHONE") { return 2; }
    if (g == "DATA") { return 4; }
    return 0;
}

void Logbook::cacheAdd(const LogQso& q)
{
    CacheEntry e;
    e.exactCall = q.call;
    e.band = q.band;
    e.modeGroup = modeBit(LogUtil::modeGroup(q.mode, q.submode));
    e.contestId = q.contestId;
    e.timeOn = q.timeOn.isValid() ? q.timeOn.toSecsSinceEpoch() : 0;
    m_byCall[LogUtil::baseCall(q.call)].append(e);
    if (!q.dxccPrefix.isEmpty()) {
        m_dxcc[q.dxccPrefix][q.band] |= e.modeGroup ? e.modeGroup : quint8(0x80);
    }
}

void Logbook::rebuildCache()
{
    m_byCall.clear();
    m_dxcc.clear();
    if (!isOpen()) {
        return;
    }
    QSqlQuery s(QSqlDatabase::database(m_connName));
    s.setForwardOnly(true);
    s.exec("SELECT call, band, mode, submode, contest_id, time_on, dxcc_prefix FROM qso");
    while (s.next()) {
        LogQso q;
        q.call = s.value(0).toString();
        q.band = s.value(1).toString();
        q.mode = s.value(2).toString();
        q.submode = s.value(3).toString();
        q.contestId = s.value(4).toString();
        q.timeOn = fromSecs(s.value(5).toLongLong());
        q.dxccPrefix = s.value(6).toString();
        cacheAdd(q);
    }
}

QString Logbook::stationCallsign() const
{
    const QString s = AppSettings::instance().value("LogbookStationCallsign", QString())
                          .toString().trimmed().toUpper();
    return s.isEmpty() ? m_stationCallsign.trimmed().toUpper() : s;
}

void Logbook::fillDxcc(LogQso& q) const
{
    if (!m_cty.isLoaded() || q.call.isEmpty()) {
        return;
    }
    const QString pfx = m_cty.resolvePrimaryPrefix(q.call);
    if (pfx.isEmpty()) {
        return;
    }
    if (q.dxccPrefix.isEmpty()) {
        q.dxccPrefix = pfx;
    }
    const DxccEntity* e = m_cty.entityByPrefix(pfx);
    if (!e) {
        return;
    }
    if (q.country.isEmpty()) { q.country = e->name; }
    if (q.continent.isEmpty()) { q.continent = e->continent; }
    if (q.cqZone == 0) { q.cqZone = e->cqZone; }
    if (q.ituZone == 0) { q.ituZone = e->ituZone; }
}

void Logbook::fillStation(LogQso& q) const
{
    auto& s = AppSettings::instance();
    if (q.stationCallsign.isEmpty()) {
        q.stationCallsign = stationCallsign();
    }
    if (q.operatorCall.isEmpty()) {
        q.operatorCall = s.value("LogbookOperator", QString()).toString().trimmed().toUpper();
    }
    if (q.myGrid.isEmpty()) {
        q.myGrid = s.value("StationGridSquare", QString()).toString().trimmed().toUpper();
    }
    if (q.txPowerW <= 0) {
        q.txPowerW = s.value("LogbookTxPowerDefault", 0).toDouble();
    }
}

LogQso Logbook::draftFromFrequency(double freqMhz, const QString& flexMode, int sliceId) const
{
    LogQso q;
    q.freqMhz = freqMhz;
    q.band = LogUtil::freqToBand(freqMhz);
    LogUtil::flexModeToAdif(flexMode, q.mode, q.submode);
    const QString g = LogUtil::modeGroup(q.mode, q.submode);
    const QString rst = (g == "PHONE") ? QStringLiteral("59") : QStringLiteral("599");
    if (g != "DATA") {
        q.rstSent = rst;
        q.rstRcvd = rst;
    }
    q.timeOn = QDateTime::currentDateTimeUtc();
    q.sliceId = sliceId;
    fillStation(q);
    return q;
}

LogQso Logbook::draftFromSlice(const SliceModel* slice) const
{
    if (!slice) {
        return draftFromFrequency(0.0, QString());
    }
    return draftFromFrequency(slice->frequency(), slice->mode(), slice->sliceId());
}

qint64 Logbook::addQso(LogQso q)
{
    if (!isOpen()) {
        m_lastError = tr("Logbook not open");
        return 0;
    }
    q.call = LogUtil::normalizeCall(q.call);
    if (q.call.isEmpty()) {
        m_lastError = tr("Callsign is required");
        return 0;
    }
    if (q.band.isEmpty() && q.freqMhz > 0) {
        q.band = LogUtil::freqToBand(q.freqMhz);
    }
    if (!q.timeOn.isValid()) {
        q.timeOn = QDateTime::currentDateTimeUtc();
    }
    fillDxcc(q);
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    qint64 id = 0;
    if (!execInsert(db, q, &id, &m_lastError)) {
        qCWarning(lcLogbook) << "insert failed:" << m_lastError;
        return 0;
    }
    q.id = id;
    cacheAdd(q);
    emit qsoAdded(q);
    return id;
}

bool Logbook::updateQso(const LogQso& in)
{
    if (!isOpen() || in.id <= 0) {
        return false;
    }
    bool ok = false;
    const LogQso old = qso(in.id, &ok);
    if (!ok) {
        return false;
    }
    LogQso q = in;
    q.call = LogUtil::normalizeCall(q.call);
    if (q.call != old.call) {
        q.dxccPrefix.clear();
        q.country.clear();
        q.continent.clear();
        q.cqZone = 0;
        q.ituZone = 0;
        q.dxcc = 0;
    }
    fillDxcc(q);
    // Edited after upload → QRZ / Club Log need a replace; LoTW/eQSL are
    // left alone (LoTW has no "modified" state — re-sign manually).
    for (int i = 0; i < kUploadServiceCount; ++i) {
        const auto svc = static_cast<UploadService>(i);
        if ((svc == UploadService::Qrz || svc == UploadService::ClubLog) &&
            old.upload[i] == UploadState::Uploaded && q.upload[i] == UploadState::Uploaded) {
            q.upload[i] = UploadState::Modified;
        }
    }
    QStringList sets;
    for (const QString& c : columns()) {
        sets << c + QStringLiteral(" = ?");
    }
    QSqlQuery u(QSqlDatabase::database(m_connName));
    u.prepare(QStringLiteral("UPDATE qso SET %1 WHERE id = ?").arg(sets.join(", ")));
    for (const QVariant& v : toRow(q)) {
        u.addBindValue(v);
    }
    u.addBindValue(q.id);
    if (!u.exec()) {
        m_lastError = u.lastError().text();
        qCWarning(lcLogbook) << "update failed:" << m_lastError;
        return false;
    }
    rebuildCache();
    emit qsoUpdated(q);
    return true;
}

bool Logbook::deleteQso(qint64 id)
{
    return deleteQsos({id});
}

bool Logbook::deleteQsos(const QVector<qint64>& ids)
{
    if (!isOpen() || ids.isEmpty()) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    db.transaction();
    QSqlQuery d(db);
    d.prepare("DELETE FROM qso WHERE id = ?");
    for (qint64 id : ids) {
        d.addBindValue(id);
        if (!d.exec()) {
            m_lastError = d.lastError().text();
            db.rollback();
            return false;
        }
    }
    if (!db.commit()) {
        m_lastError = db.lastError().text();
        return false;
    }
    rebuildCache();
    if (ids.size() == 1) {
        emit qsoDeleted(ids.first());
    } else {
        emit logReset();
    }
    return true;
}

LogQso Logbook::qso(qint64 id, bool* ok) const
{
    if (ok) {
        *ok = false;
    }
    if (!isOpen()) {
        return {};
    }
    QSqlQuery s(QSqlDatabase::database(m_connName));
    s.prepare("SELECT * FROM qso WHERE id = ?");
    s.addBindValue(id);
    if (!s.exec() || !s.next()) {
        return {};
    }
    if (ok) {
        *ok = true;
    }
    return fromQuery(s);
}

QVector<LogQso> Logbook::search(const LogFilter& f) const
{
    QVector<LogQso> out;
    if (!isOpen()) {
        return out;
    }
    QStringList where;
    QVariantList binds;
    if (!f.call.isEmpty()) {
        where << "call = ?";
        binds << LogUtil::normalizeCall(f.call);
    } else if (!f.text.trimmed().isEmpty()) {
        where << "(call LIKE ? ESCAPE '\\' OR name LIKE ? ESCAPE '\\' OR "
                 "comment LIKE ? ESCAPE '\\' OR qth LIKE ? ESCAPE '\\' OR "
                 "country LIKE ? ESCAPE '\\')";
        QString t = f.text.trimmed();
        t.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
        const QString like = '%' + t + '%';
        for (int i = 0; i < 5; ++i) {
            binds << like;
        }
    }
    if (!f.band.isEmpty()) {
        where << "band = ?";
        binds << f.band.toLower();
    }
    if (!f.modeGroup.isEmpty()) {
        where << "mode_group = ?";
        binds << f.modeGroup.toUpper();
    }
    if (!f.contestId.isEmpty()) {
        where << "contest_id = ?";
        binds << f.contestId;
    }
    if (f.from.isValid()) {
        where << "time_on >= ?";
        binds << f.from.toSecsSinceEpoch();
    }
    if (f.to.isValid()) {
        where << "time_on <= ?";
        binds << f.to.toSecsSinceEpoch();
    }
    QString sql = QStringLiteral("SELECT * FROM qso");
    if (!where.isEmpty()) {
        sql += QStringLiteral(" WHERE ") + where.join(" AND ");
    }
    sql += QStringLiteral(" ORDER BY time_on DESC, id DESC");
    if (f.limit > 0) {
        sql += QStringLiteral(" LIMIT %1").arg(f.limit);
    }
    QSqlQuery s(QSqlDatabase::database(m_connName));
    s.setForwardOnly(true);
    s.prepare(sql);
    for (const QVariant& b : binds) {
        s.addBindValue(b);
    }
    if (!s.exec()) {
        qCWarning(lcLogbook) << "search failed:" << s.lastError().text();
        return out;
    }
    while (s.next()) {
        out.append(fromQuery(s));
    }
    return out;
}

qint64 Logbook::count() const
{
    if (!isOpen()) {
        return 0;
    }
    QSqlQuery s(QSqlDatabase::database(m_connName));
    s.exec("SELECT COUNT(*) FROM qso");
    return s.next() ? s.value(0).toLongLong() : 0;
}

bool Logbook::setUploadState(qint64 id, UploadService svc, UploadState st, const QDate& date)
{
    if (!isOpen()) {
        return false;
    }
    static const char* const cols[kUploadServiceCount][2] = {
        {"up_lotw", "up_lotw_date"}, {"up_qrz", "up_qrz_date"},
        {"up_clublog", "up_clublog_date"}, {"up_eqsl", "up_eqsl_date"}};
    const int i = static_cast<int>(svc);
    QSqlQuery u(QSqlDatabase::database(m_connName));
    u.prepare(QStringLiteral("UPDATE qso SET %1 = ?, %2 = ? WHERE id = ?")
                  .arg(QLatin1String(cols[i][0]), QLatin1String(cols[i][1])));
    u.addBindValue(static_cast<int>(st));
    u.addBindValue(date.isValid() ? date.toString(Qt::ISODate) : QString());
    u.addBindValue(id);
    if (!u.exec()) {
        m_lastError = u.lastError().text();
        return false;
    }
    bool ok = false;
    const LogQso q = qso(id, &ok);
    if (ok) {
        emit qsoUpdated(q);
    }
    return true;
}

QVector<LogQso> Logbook::qsosWithUploadState(UploadService svc,
                                             const QSet<UploadState>& states,
                                             int limit) const
{
    QVector<LogQso> out;
    if (!isOpen() || states.isEmpty()) {
        return out;
    }
    static const char* const cols[kUploadServiceCount] = {
        "up_lotw", "up_qrz", "up_clublog", "up_eqsl"};
    QStringList vals;
    for (UploadState st : states) {
        vals << QString::number(static_cast<int>(st));
    }
    QString sql = QStringLiteral("SELECT * FROM qso WHERE %1 IN (%2) ORDER BY time_on ASC")
                      .arg(QLatin1String(cols[static_cast<int>(svc)]), vals.join(','));
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT %1").arg(limit);
    }
    QSqlQuery s(QSqlDatabase::database(m_connName));
    s.setForwardOnly(true);
    if (s.exec(sql)) {
        while (s.next()) {
            out.append(fromQuery(s));
        }
    }
    return out;
}

bool Logbook::setRecordingPath(qint64 id, const QString& path)
{
    if (!isOpen()) {
        return false;
    }
    QSqlQuery u(QSqlDatabase::database(m_connName));
    u.prepare("UPDATE qso SET recording = ? WHERE id = ?");
    u.addBindValue(path);
    u.addBindValue(id);
    if (!u.exec()) {
        return false;
    }
    bool ok = false;
    const LogQso q = qso(id, &ok);
    if (ok) {
        emit qsoUpdated(q);
    }
    return true;
}

int Logbook::linkRecording(const QString& path, const QDateTime& startUtc,
                           const QDateTime& endUtc)
{
    if (!isOpen() || path.isEmpty() || !startUtc.isValid()) {
        return 0;
    }
    constexpr qint64 kSlackSecs = 120;
    const qint64 a = startUtc.toSecsSinceEpoch() - kSlackSecs;
    const qint64 b = (endUtc.isValid() ? endUtc : startUtc).toSecsSinceEpoch() + kSlackSecs;
    QSqlQuery s(QSqlDatabase::database(m_connName));
    s.prepare("SELECT id FROM qso WHERE time_on BETWEEN ? AND ? "
              "AND (recording IS NULL OR recording = '')");
    s.addBindValue(a);
    s.addBindValue(b);
    QVector<qint64> ids;
    if (s.exec()) {
        while (s.next()) {
            ids << s.value(0).toLongLong();
        }
    }
    int n = 0;
    for (qint64 id : ids) {
        n += setRecordingPath(id, path) ? 1 : 0;
    }
    return n;
}

bool Logbook::isDupe(const QString& callIn, const QString& band, const QString& mode,
                     const DupeRules& rules) const
{
    const QString call = LogUtil::normalizeCall(callIn);
    const auto it = m_byCall.constFind(LogUtil::baseCall(call));
    if (it == m_byCall.cend()) {
        return false;
    }
    // `mode` may be an ADIF mode or already a group name.
    QString group = mode.toUpper();
    if (group != "CW" && group != "PHONE" && group != "DATA") {
        group = LogUtil::modeGroup(mode);
    }
    const quint8 bit = modeBit(group);
    const qint64 since = rules.since.isValid() ? rules.since.toSecsSinceEpoch() : 0;
    for (const CacheEntry& e : it.value()) {
        if (e.exactCall != call) {
            continue;
        }
        if (!rules.contestId.isEmpty() && e.contestId != rules.contestId) {
            continue;
        }
        if (since > 0 && e.timeOn < since) {
            continue;
        }
        if (rules.perBand && e.band != band.toLower()) {
            continue;
        }
        if (rules.perMode && bit != 0 && e.modeGroup != 0 && e.modeGroup != bit) {
            continue;
        }
        return true;
    }
    return false;
}

WorkedStatus Logbook::workedStatus(const QString& callIn, const QString& bandIn,
                                   const QString& mode) const
{
    WorkedStatus w;
    const QString call = LogUtil::normalizeCall(callIn);
    const QString band = bandIn.toLower();
    QString group = mode.toUpper();
    if (group != "CW" && group != "PHONE" && group != "DATA") {
        group = LogUtil::modeGroup(mode);
    }
    const quint8 bit = modeBit(group);

    if (m_cty.isLoaded() && !call.isEmpty()) {
        w.dxccPrefix = m_cty.resolvePrimaryPrefix(call);
        if (const DxccEntity* e = m_cty.entityByPrefix(w.dxccPrefix)) {
            w.dxccKnown = true;
            w.country = e->name;
            w.cqZone = e->cqZone;
            w.ituZone = e->ituZone;
            w.continent = e->continent;
            w.lat = e->latitude;
            w.lon = -e->longitude;   // cty.dat longitude is +West
        }
    }
    if (w.dxccKnown) {
        const auto d = m_dxcc.constFind(w.dxccPrefix);
        if (d != m_dxcc.cend()) {
            w.dxccWorked = true;
            const auto b = d->constFind(band);
            if (b != d->cend()) {
                w.dxccBandWorked = true;
                w.dxccBandModeWorked = (bit == 0) || (b.value() & bit);
            }
        }
    }
    const auto c = m_byCall.constFind(LogUtil::baseCall(call));
    if (c != m_byCall.cend()) {
        for (const CacheEntry& e : c.value()) {
            w.callWorked = true;
            ++w.callQsoCount;
            if (e.band == band) {
                w.callBandWorked = true;
                if (bit == 0 || e.modeGroup == bit) {
                    w.callBandModeWorked = true;
                }
            }
        }
    }
    return w;
}

DxccStatus Logbook::dxccStatus(const QString& dxccPrefix, const QString& band,
                               const QString& modeGroup) const
{
    if (dxccPrefix.isEmpty()) {
        return DxccStatus::Unknown;
    }
    const auto d = m_dxcc.constFind(dxccPrefix);
    if (d == m_dxcc.cend()) {
        return DxccStatus::NewDxcc;
    }
    const auto b = d->constFind(band.toLower());
    if (b == d->cend()) {
        return DxccStatus::NewBand;
    }
    const quint8 bit = modeBit(modeGroup.toUpper());
    if (bit != 0 && !(b.value() & bit)) {
        return DxccStatus::NewMode;
    }
    return DxccStatus::Worked;
}

int Logbook::maxSerialSent(const QString& contestId) const
{
    if (!isOpen()) {
        return 0;
    }
    QSqlQuery s(QSqlDatabase::database(m_connName));
    s.prepare("SELECT MAX(stx) FROM qso WHERE contest_id = ?");
    s.addBindValue(contestId);
    return (s.exec() && s.next()) ? s.value(0).toInt() : 0;
}

// ── ADIF import / export ──────────────────────────────────────────────────

ImportResult Logbook::importInto(const QString& connName, const QByteArray& data,
                                 bool skipDupes, const std::atomic<bool>* cancel,
                                 const std::function<void(qint64, qint64)>& progress) const
{
    ImportResult res;
    QSqlDatabase db = QSqlDatabase::database(connName);
    if (!db.isOpen()) {
        res.errorString = tr("Logbook not open");
        return res;
    }
    int inBatch = 0;
    db.transaction();
    qint64 lastPos = 0;
    Adif::parse(data, [&](const QMap<QString, QString>& fields) {
        if (cancel && cancel->load()) {
            return false;
        }
        LogQso q = Adif::toQso(fields);
        if (q.call.isEmpty()) {
            ++res.errors;
            return true;
        }
        fillDxcc(q);
        if (skipDupes && existsSimilar(db, q)) {
            ++res.skippedDupes;
            return true;
        }
        QString err;
        if (!execInsert(db, q, nullptr, &err)) {
            ++res.errors;
            if (res.errorString.isEmpty()) {
                res.errorString = err;
            }
            return true;
        }
        ++res.added;
        if (++inBatch >= kImportBatchSize) {
            db.commit();
            db.transaction();
            inBatch = 0;
            if (progress) {
                progress(lastPos, res.added);
            }
        }
        return true;
    }, [&](qint64 pos) { lastPos = pos; });
    if (!db.commit()) {
        res.errorString = db.lastError().text();
    }
    if (progress) {
        progress(data.size(), res.added);
    }
    return res;
}

ImportResult Logbook::importAdifData(const QByteArray& data, bool skipDupes)
{
    const ImportResult r = importInto(m_connName, data, skipDupes, nullptr, {});
    rebuildCache();
    emit logReset();
    return r;
}

ImportResult Logbook::importAdif(const QString& filePath, bool skipDupes)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        ImportResult r;
        r.errorString = f.errorString();
        return r;
    }
    // mmap large files instead of copying them into memory.
    uchar* mapped = f.size() > 0 ? f.map(0, f.size()) : nullptr;
    if (mapped) {
        const QByteArray view = QByteArray::fromRawData(
            reinterpret_cast<const char*>(mapped), f.size());
        const ImportResult r = importAdifData(view, skipDupes);
        f.unmap(mapped);
        return r;
    }
    return importAdifData(f.readAll(), skipDupes);
}

bool Logbook::importAdifAsync(const QString& filePath, bool skipDupes)
{
    if (m_importThread || !isOpen()) {
        return false;
    }
    m_cancelImport = false;
    const QString dbPath = m_path;
    const QString workerConn = m_connName + QStringLiteral("_import");

    m_importThread.reset(QThread::create([this, filePath, dbPath, workerConn, skipDupes]() {
        ImportResult res;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", workerConn);
            db.setDatabaseName(dbPath);
            if (!db.open()) {
                res.errorString = db.lastError().text();
            } else {
                QSqlQuery(db).exec("PRAGMA busy_timeout=5000");
                QFile f(filePath);
                if (!f.open(QIODevice::ReadOnly)) {
                    res.errorString = f.errorString();
                } else {
                    const qint64 total = f.size();
                    uchar* mapped = total > 0 ? f.map(0, total) : nullptr;
                    QByteArray owned;
                    QByteArray view;
                    if (mapped) {
                        view = QByteArray::fromRawData(reinterpret_cast<const char*>(mapped), total);
                    } else {
                        owned = f.readAll();
                        view = owned;
                    }
                    res = importInto(workerConn, view, skipDupes, &m_cancelImport,
                        [this, total](qint64 pos, qint64 added) {
                            QMetaObject::invokeMethod(this, [this, pos, total, added]() {
                                emit importProgress(pos, total, added);
                            }, Qt::QueuedConnection);
                        });
                    if (mapped) {
                        f.unmap(mapped);
                    }
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(workerConn);
        QMetaObject::invokeMethod(this, [this, res]() {
            if (m_importThread) {
                m_importThread->wait();
                m_importThread.reset();
            }
            rebuildCache();
            emit logReset();
            emit importFinished(res);
        }, Qt::QueuedConnection);
    }));
    m_importThread->start();
    return true;
}

void Logbook::cancelImport()
{
    m_cancelImport = true;
}

bool Logbook::exportAdif(const QString& filePath, const QVector<qint64>& ids,
                         const Adif::WriteOptions& opt) const
{
    if (ids.isEmpty()) {
        return exportAdif(filePath, LogFilter{}, opt);
    }
    QSaveFile out(filePath);
    if (!out.open(QIODevice::WriteOnly)) {
        qCWarning(lcLogbook) << "export open failed:" << out.errorString();
        return false;
    }
    out.write(Adif::header(programVersion()));
    for (qint64 id : ids) {
        bool ok = false;
        const LogQso q = qso(id, &ok);
        if (ok) {
            out.write(Adif::record(q, opt));
        }
    }
    return out.commit();
}

bool Logbook::exportAdif(const QString& filePath, const LogFilter& filter,
                         const Adif::WriteOptions& opt) const
{
    QSaveFile out(filePath);
    if (!out.open(QIODevice::WriteOnly)) {
        qCWarning(lcLogbook) << "export open failed:" << out.errorString();
        return false;
    }
    out.write(Adif::header(programVersion()));
    QVector<LogQso> qsos = search(filter);
    // Chronological order in the file.
    std::reverse(qsos.begin(), qsos.end());
    for (const LogQso& q : qsos) {
        out.write(Adif::record(q, opt));
    }
    return out.commit();
}

qint64 Logbook::ingestAdifRecord(const QString& adif, bool skipDupes)
{
    if (!isOpen()) {
        return 0;
    }
    LogQso q;
    bool got = false;
    Adif::parse(adif.toUtf8(), [&](const QMap<QString, QString>& fields) {
        q = Adif::toQso(fields);
        got = true;
        return false;   // first record only
    });
    if (!got) {
        // Tolerate a single record without a trailing <EOR>
        Adif::parse(adif.toUtf8() + "<EOR>", [&](const QMap<QString, QString>& fields) {
            q = Adif::toQso(fields);
            got = true;
            return false;
        });
    }
    if (!got || q.call.isEmpty()) {
        m_lastError = tr("No callsign in ADIF record");
        return 0;
    }
    fillStation(q);
    fillDxcc(q);
    if (skipDupes) {
        QSqlDatabase db = QSqlDatabase::database(m_connName);
        if (existsSimilar(db, q)) {
            m_lastError = tr("Duplicate QSO ignored");
            return 0;
        }
    }
    return addQso(q);
}

QString Logbook::wsjtxLoggedAdif(const QByteArray& datagram)
{
    // WSJT-X NetworkMessage: quint32 magic 0xadbccbda, quint32 schema,
    // quint32 type, utf8 id, … ; type 12 = LoggedADIF, payload utf8 ADIF.
    // QDataStream big-endian, QByteArray = quint32 len + bytes (0xffffffff null).
    constexpr quint32 kMagic = 0xadbccbda;
    constexpr quint32 kLoggedAdif = 12;
    QDataStream in(datagram);
    in.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0;
    quint32 schema = 0;
    quint32 type = 0;
    in >> magic >> schema >> type;
    if (in.status() != QDataStream::Ok || magic != kMagic || type != kLoggedAdif) {
        return {};
    }
    in.setVersion(schema >= 3 ? QDataStream::Qt_5_4 : QDataStream::Qt_5_0);
    QByteArray id;
    QByteArray adif;
    in >> id >> adif;
    if (in.status() != QDataStream::Ok) {
        return {};
    }
    return QString::fromUtf8(adif);
}

} // namespace AetherSDR
