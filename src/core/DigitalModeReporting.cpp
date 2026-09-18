#include "DigitalModeReporting.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QTimeZone>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr quint16 kReceiverDataSetId = 0x9992;
constexpr quint16 kSenderDataSetId = 0x9993;
constexpr qint64 kReporterDedupWindowMs = 5 * 60 * 1000;
constexpr qint64 kSpotDedupWindowMs = 2 * 60 * 1000;
constexpr qint64 kTemplateRefreshMs = 60 * 60 * 1000;
constexpr qint64 kLookupRetryMs = 60 * 1000;
constexpr int kMaxRecordsPerPacket = 70;
constexpr int kMaxPendingReports = 1000;   // bound memory while the server is unreachable

QString normalizedMode(const QString& mode)
{
    return reporterModeName(mode).trimmed().toUpper();
}

bool isPlausibleCallsign(const QString& text)
{
    static const QRegularExpression callRx(
        QStringLiteral(R"(^(?:[A-Z0-9]{1,4}/)?[A-Z0-9]{0,3}[0-9][A-Z0-9]{1,6}(?:/[A-Z0-9]{1,4})?$)"));
    const QString candidate = text.trimmed().toUpper();
    return !candidate.isEmpty() && callRx.match(candidate).hasMatch();
}

QString normalizeTextBuffer(const QString& text)
{
    QString normalized = text.toUpper();
    normalized.replace(QChar::CarriageReturn, QLatin1Char(' '));
    normalized.replace(QChar::LineFeed, QLatin1Char(' '));
    normalized.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    return normalized.trimmed();
}

QString extractGrid(const QString& text)
{
    static const QRegularExpression gridRx(
        QStringLiteral(R"(\b([A-R]{2}[0-9]{2}(?:[A-X]{2})?)\b)"));
    const auto match = gridRx.match(text);
    return match.hasMatch() ? match.captured(1).toUpper() : QString{};
}

QString extractSpottedCallsign(const QString& text)
{
    static const QRegularExpression cqDxRx(
        QStringLiteral(R"(\bCQ(?:\s+[A-Z0-9/]+)?\s+(?:DE\s+)?([A-Z0-9/]{3,16})\b)"));
    static const QRegularExpression deRx(
        QStringLiteral(R"(\bDE\s+([A-Z0-9/]{3,16})(?:\s+\1)?\b)"));

    const auto cqMatch = cqDxRx.match(text);
    if (cqMatch.hasMatch() && isPlausibleCallsign(cqMatch.captured(1)))
        return cqMatch.captured(1).toUpper();

    const auto deMatch = deRx.match(text);
    if (deMatch.hasMatch() && isPlausibleCallsign(deMatch.captured(1)))
        return deMatch.captured(1).toUpper();

    return {};
}

QString logFilePathFor(const QString& fileName)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/AetherSDR/") + fileName;
}

QString dedupKeyForReport(const ReceptionReport& report)
{
    const quint64 freqBucket = report.frequencyHz / 10;
    return QStringLiteral("%1|%2|%3|%4")
        .arg(report.receiverCallsign.trimmed().toUpper(),
             report.senderCallsign.trimmed().toUpper(),
             normalizedMode(report.mode),
             QString::number(freqBucket));
}

qint64 currentUtcMs()
{
    return QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
}

QString displayModeForSpot(const QString& mode)
{
    const QString normalized = normalizedMode(mode);
    if (normalized == QLatin1String("PSK31"))
        return QStringLiteral("BPSK-31");
    if (normalized == QLatin1String("PSK63"))
        return QStringLiteral("BPSK-63");
    if (normalized == QLatin1String("PSK125"))
        return QStringLiteral("BPSK-125");
    return normalized;
}

} // namespace

QString modeSettingSuffix(const QString& mode)
{
    const QString normalized = normalizedMode(mode);
    QString out;
    out.reserve(normalized.size());
    for (const QChar ch : normalized) {
        if (ch.isLetterOrNumber())
            out += ch;
        else if (!out.endsWith(QLatin1Char('_')))
            out += QLatin1Char('_');
    }
    while (out.endsWith(QLatin1Char('_')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("UNKNOWN") : out;
}

QString spotEnabledSettingKey(const QString& mode)
{
    return QStringLiteral("DigitalSpotEnabled_%1").arg(modeSettingSuffix(mode));
}

QString reporterEnabledSettingKey(const QString& mode)
{
    return QStringLiteral("PskReporterEnabled_%1").arg(modeSettingSuffix(mode));
}

QString reporterModeName(const QString& mode)
{
    QString normalized = mode.trimmed().toUpper();
    normalized.remove(QLatin1Char(' '));
    normalized.remove(QLatin1Char('_'));
    normalized.remove(QLatin1Char('-'));

    if (normalized == QLatin1String("BPSK31") || normalized == QLatin1String("PSK31"))
        return QStringLiteral("PSK31");
    if (normalized == QLatin1String("BPSK63") || normalized == QLatin1String("PSK63"))
        return QStringLiteral("PSK63");
    if (normalized == QLatin1String("BPSK125") || normalized == QLatin1String("PSK125"))
        return QStringLiteral("PSK125");
    if (normalized == QLatin1String("QPSK31"))
        return QStringLiteral("QPSK31");
    if (normalized == QLatin1String("QPSK63"))
        return QStringLiteral("QPSK63");
    if (normalized == QLatin1String("QPSK125"))
        return QStringLiteral("QPSK125");
    if (normalized == QLatin1String("FT8"))
        return QStringLiteral("FT8");
    if (normalized == QLatin1String("FT4"))
        return QStringLiteral("FT4");
    if (normalized == QLatin1String("JT65"))
        return QStringLiteral("JT65");
    if (normalized == QLatin1String("WSPR"))
        return QStringLiteral("WSPR");
    if (normalized == QLatin1String("RTTY"))
        return QStringLiteral("RTTY");
    return mode.trimmed().toUpper();
}

std::optional<ReceptionReport> parseTextReception(const QString& textBuffer,
                                                  const QString& receiverCallsign,
                                                  const QString& receiverLocator,
                                                  quint64 frequencyHz,
                                                  const QString& mode,
                                                  int snr)
{
    const QString normalized = normalizeTextBuffer(textBuffer);
    if (normalized.isEmpty() || frequencyHz == 0)
        return std::nullopt;

    const QString senderCallsign = extractSpottedCallsign(normalized);
    if (senderCallsign.isEmpty())
        return std::nullopt;

    ReceptionReport report;
    report.receiverCallsign = receiverCallsign.trimmed().toUpper();
    report.receiverLocator = receiverLocator.trimmed().toUpper();
    report.senderCallsign = senderCallsign;
    report.senderLocator = extractGrid(normalized);
    report.frequencyHz = frequencyHz;
    report.snr = std::clamp(snr, -128, 127);
    report.mode = reporterModeName(mode);
    report.flowStartSeconds = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    report.message = normalized.left(200);
    return report;
}

DigitalModeSpotClient::DigitalModeSpotClient(QObject* parent)
    : QObject(parent)
    , m_logFile(new QFile(this))
{
    qRegisterMetaType<AetherSDR::ReceptionReport>("AetherSDR::ReceptionReport");

    m_logFile->setFileName(logFilePath());
    QDir().mkpath(QFileInfo(*m_logFile).absolutePath());
    m_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

DigitalModeSpotClient::~DigitalModeSpotClient()
{
    if (m_logFile)
        m_logFile->close();
}

QString DigitalModeSpotClient::logFilePath() const
{
    return logFilePathFor(QStringLiteral("digital-spots.log"));
}

void DigitalModeSpotClient::submitReport(const ReceptionReport& report)
{
    if (report.senderCallsign.trimmed().isEmpty() || report.frequencyHz == 0)
        return;

    const qint64 nowMs = currentUtcMs();
    for (auto it = m_recentReports.begin(); it != m_recentReports.end();) {
        if ((nowMs - it.value()) > kSpotDedupWindowMs)
            it = m_recentReports.erase(it);
        else
            ++it;
    }

    const QString key = dedupKeyForReport(report);
    if (m_recentReports.contains(key))
        return;
    m_recentReports.insert(key, nowMs);

    DxSpot spot;
    spot.spotterCall = report.receiverCallsign.trimmed().toUpper();
    if (spot.spotterCall.isEmpty())
        spot.spotterCall = QStringLiteral("AetherSDR");
    spot.freqMhz = static_cast<double>(report.frequencyHz) / 1000000.0;
    spot.dxCall = report.senderCallsign.trimmed().toUpper();
    spot.comment = report.message.trimmed().isEmpty()
                       ? QStringLiteral("%1 reception").arg(displayModeForSpot(report.mode))
                       : report.message.trimmed();
    spot.utcTime = QDateTime::fromSecsSinceEpoch(report.flowStartSeconds, QTimeZone::UTC).time();
    spot.source = reporterModeName(report.mode);
    spot.color = spotColorForMode(report.mode, report.snr);
    spot.snr = report.snr;
    spot.lifetimeSec = std::max(30, report.lifetimeSec);

    const QString line = lineForReport(report);
    if (m_logFile && m_logFile->isOpen()) {
        m_logFile->write(line.toUtf8());
        m_logFile->write("\n");
        m_logFile->flush();
    }

    emit rawLineReceived(line);
    emit spotReceived(spot);
}

QString DigitalModeSpotClient::lineForReport(const ReceptionReport& report) const
{
    return QStringLiteral("[%1] %2 %3 MHz %4 %5")
        .arg(QDateTime::fromSecsSinceEpoch(report.flowStartSeconds, QTimeZone::UTC)
                 .toString(QStringLiteral("HH:mm:ss 'UTC'")),
             reporterModeName(report.mode),
             QString::number(static_cast<double>(report.frequencyHz) / 1000000.0, 'f', 6),
             report.senderCallsign.trimmed().toUpper(),
             report.message.trimmed());
}

QString DigitalModeSpotClient::spotColorForMode(const QString& mode, int snr) const
{
    QString rgb = QStringLiteral("FFFFFF");
    const QString normalized = reporterModeName(mode);
    if (normalized == QLatin1String("FT8"))
        rgb = QStringLiteral("29B6F6");
    else if (normalized == QLatin1String("FT4"))
        rgb = QStringLiteral("26A69A");
    else if (normalized == QLatin1String("JT65"))
        rgb = QStringLiteral("F4B400");
    else if (normalized == QLatin1String("WSPR"))
        rgb = QStringLiteral("FB8C00");
    else if (normalized.startsWith(QLatin1String("PSK")) || normalized.startsWith(QLatin1String("QPSK")))
        rgb = QStringLiteral("66BB6A");
    else if (normalized == QLatin1String("RTTY"))
        rgb = QStringLiteral("EF5350");

    int alpha = 208;
    if (snr <= -24)
        alpha = 96;
    else if (snr >= 10)
        alpha = 255;
    else if (snr != 0)
        alpha = 96 + static_cast<int>(std::lround((snr + 24.0) * (159.0 / 34.0)));

    return QStringLiteral("#%1%2")
        .arg(alpha, 2, 16, QLatin1Char('0'))
        .arg(rgb)
        .toUpper();
}

PskReporterUploader::PskReporterUploader(QObject* parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_flushTimer(new QTimer(this))
    , m_observationDomainId(QRandomGenerator::global()->generate())
{
    m_flushTimer->setSingleShot(true);
    connect(m_flushTimer, &QTimer::timeout, this, &PskReporterUploader::flush);
}

PskReporterUploader::~PskReporterUploader()
{
    if (m_lookupId >= 0)
        QHostInfo::abortHostLookup(m_lookupId);
    flush();   // non-blocking: sends only if the server address is already known
}

void PskReporterUploader::startHostLookup(qint64 nowMs)
{
    if (m_lookupId >= 0)
        return;
    if (m_lastLookupMs != 0 && (nowMs - m_lastLookupMs) < kLookupRetryMs)
        return;
    m_lastLookupMs = nowMs;
    m_lookupId = QHostInfo::lookupHost(QStringLiteral("report.pskreporter.info"), this,
                                       [this](const QHostInfo& info) {
        m_lookupId = -1;
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty())
            return;
        m_serverAddress = info.addresses().constFirst();
        if (!m_pending.isEmpty() && !m_flushTimer->isActive())
            scheduleNextFlush();
    });
}

void PskReporterUploader::submitReport(const ReceptionReport& report)
{
    if (report.senderCallsign.trimmed().isEmpty()
        || report.receiverCallsign.trimmed().isEmpty()
        || report.receiverLocator.trimmed().isEmpty()
        || report.frequencyHz == 0) {
        return;
    }

    const qint64 nowMs = currentUtcMs();
    for (auto it = m_recentReports.begin(); it != m_recentReports.end();) {
        if ((nowMs - it.value()) > kReporterDedupWindowMs)
            it = m_recentReports.erase(it);
        else
            ++it;
    }

    const QString key = dedupKeyForReport(report);
    if (m_recentReports.contains(key))
        return;
    m_recentReports.insert(key, nowMs);

    if (!m_pending.isEmpty()) {
        const ReceptionReport& last = m_pending.constLast();
        if (last.receiverCallsign.trimmed().compare(report.receiverCallsign.trimmed(), Qt::CaseInsensitive) != 0
            || last.receiverLocator.trimmed().compare(report.receiverLocator.trimmed(), Qt::CaseInsensitive) != 0) {
            flush();
        }
    }

    if (m_pending.size() >= kMaxPendingReports)
        m_pending.removeFirst();
    m_pending.append(report);
    if (m_serverAddress.isNull())
        startHostLookup(nowMs);   // resolve early so the first flush can send
    if (m_pending.size() >= kMaxRecordsPerPacket && !m_serverAddress.isNull()) {
        flush();
        return;
    }

    if (!m_flushTimer->isActive())
        scheduleNextFlush();
}

void PskReporterUploader::flush()
{
    if (m_pending.isEmpty())
        return;

    const qint64 nowMs = currentUtcMs();
    if (m_serverAddress.isNull() || (nowMs - m_lastLookupMs) >= kTemplateRefreshMs)
        startHostLookup(nowMs);
    if (m_serverAddress.isNull()) {
        scheduleNextFlush();
        return;
    }

    while (!m_pending.isEmpty()) {
        // One datagram per receiver identity: the receiver record describes every
        // sender record in the packet, so reports queued under a different
        // callsign/locator (e.g. while the server was unreachable) must not be
        // attributed to the first one.
        const ReceptionReport& first = m_pending.constFirst();
        int count = 1;
        while (count < m_pending.size() && count < kMaxRecordsPerPacket
               && m_pending.at(count).receiverCallsign.trimmed().compare(
                      first.receiverCallsign.trimmed(), Qt::CaseInsensitive) == 0
               && m_pending.at(count).receiverLocator.trimmed().compare(
                      first.receiverLocator.trimmed(), Qt::CaseInsensitive) == 0) {
            ++count;
        }

        const bool sendTemplates = shouldSendTemplates(nowMs);
        QByteArray datagram;
        datagram.resize(16);

        if (sendTemplates)
            appendTemplateSets(&datagram);

        appendReceiverRecord(&datagram, first);

        const int senderSetLengthOffset = datagram.size() + 2;
        appendU16(&datagram, kSenderDataSetId);
        appendU16(&datagram, 0);
        for (int i = 0; i < count; ++i)
            appendSenderRecord(&datagram, m_pending.at(i));
        appendPadding(&datagram);
        const quint16 senderSetLength = static_cast<quint16>(datagram.size() - senderSetLengthOffset + 2);
        datagram[senderSetLengthOffset] = static_cast<char>((senderSetLength >> 8) & 0xff);
        datagram[senderSetLengthOffset + 1] = static_cast<char>(senderSetLength & 0xff);

        const quint16 totalLength = static_cast<quint16>(datagram.size());
        const quint32 exportTime = static_cast<quint32>(QDateTime::currentSecsSinceEpoch());
        datagram[0] = 0x00;
        datagram[1] = 0x0A;
        datagram[2] = static_cast<char>((totalLength >> 8) & 0xff);
        datagram[3] = static_cast<char>(totalLength & 0xff);
        datagram[4] = static_cast<char>((exportTime >> 24) & 0xff);
        datagram[5] = static_cast<char>((exportTime >> 16) & 0xff);
        datagram[6] = static_cast<char>((exportTime >> 8) & 0xff);
        datagram[7] = static_cast<char>(exportTime & 0xff);
        datagram[8] = static_cast<char>((m_sequenceNumber >> 24) & 0xff);
        datagram[9] = static_cast<char>((m_sequenceNumber >> 16) & 0xff);
        datagram[10] = static_cast<char>((m_sequenceNumber >> 8) & 0xff);
        datagram[11] = static_cast<char>(m_sequenceNumber & 0xff);
        datagram[12] = static_cast<char>((m_observationDomainId >> 24) & 0xff);
        datagram[13] = static_cast<char>((m_observationDomainId >> 16) & 0xff);
        datagram[14] = static_cast<char>((m_observationDomainId >> 8) & 0xff);
        datagram[15] = static_cast<char>(m_observationDomainId & 0xff);

        m_socket->writeDatagram(datagram, m_serverAddress, 4739);

        m_sequenceNumber += static_cast<quint32>(count);
        m_pending.remove(0, count);
        ++m_packetsSent;
        if (sendTemplates)
            m_lastTemplateMs = nowMs;
    }
    scheduleNextFlush();
}

void PskReporterUploader::appendPadding(QByteArray* data) const
{
    while ((data->size() % 4) != 0)
        data->append('\0');
}

void PskReporterUploader::appendReceiverRecord(QByteArray* data, const ReceptionReport& report) const
{
    const int lengthOffset = data->size() + 2;
    appendU16(data, kReceiverDataSetId);
    appendU16(data, 0);
    appendVariableString(data, report.receiverCallsign.trimmed().toUpper());
    appendVariableString(data, report.receiverLocator.trimmed().toUpper());
    appendVariableString(
        data,
        QStringLiteral("AetherSDR %1")
            .arg(QCoreApplication::applicationVersion().isEmpty()
                     ? QStringLiteral("dev")
                     : QCoreApplication::applicationVersion()));
    appendPadding(data);
    const quint16 length = static_cast<quint16>(data->size() - lengthOffset + 2);
    (*data)[lengthOffset] = static_cast<char>((length >> 8) & 0xff);
    (*data)[lengthOffset + 1] = static_cast<char>(length & 0xff);
}

void PskReporterUploader::appendSenderRecord(QByteArray* data, const ReceptionReport& report) const
{
    appendVariableString(data, report.senderCallsign.trimmed().toUpper());
    appendU32(data, static_cast<quint32>(std::min<quint64>(report.frequencyHz, 0xffffffffu)));
    data->append(static_cast<char>(std::clamp(report.snr, -128, 127)));
    data->append(static_cast<char>(0));
    appendVariableString(data, reporterModeName(report.mode));
    data->append(static_cast<char>(1));
    appendVariableString(data, report.senderLocator.trimmed().toUpper());
    appendU32(data, static_cast<quint32>(std::max<qint64>(0, report.flowStartSeconds)));
}

void PskReporterUploader::appendTemplateSets(QByteArray* data) const
{
    static const unsigned char kReceiverTemplate[] = {
        0x00, 0x03, 0x00, 0x24, 0x99, 0x92, 0x00, 0x03,
        0x00, 0x01, 0x80, 0x02, 0xff, 0xff, 0x00, 0x00,
        0x76, 0x8f, 0x80, 0x04, 0xff, 0xff, 0x00, 0x00,
        0x76, 0x8f, 0x80, 0x08, 0xff, 0xff, 0x00, 0x00,
        0x76, 0x8f, 0x00, 0x00
    };
    static const unsigned char kSenderTemplate[] = {
        0x00, 0x02, 0x00, 0x44, 0x99, 0x93, 0x00, 0x08,
        0x80, 0x01, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f,
        0x80, 0x05, 0x00, 0x04, 0x00, 0x00, 0x76, 0x8f,
        0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8f,
        0x80, 0x07, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8f,
        0x80, 0x0a, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f,
        0x80, 0x0b, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8f,
        0x80, 0x03, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f,
        0x00, 0x96, 0x00, 0x04
    };
    data->append(reinterpret_cast<const char*>(kReceiverTemplate), sizeof(kReceiverTemplate));
    data->append(reinterpret_cast<const char*>(kSenderTemplate), sizeof(kSenderTemplate));
}

void PskReporterUploader::appendU16(QByteArray* data, quint16 value) const
{
    data->append(static_cast<char>((value >> 8) & 0xff));
    data->append(static_cast<char>(value & 0xff));
}

void PskReporterUploader::appendU32(QByteArray* data, quint32 value) const
{
    data->append(static_cast<char>((value >> 24) & 0xff));
    data->append(static_cast<char>((value >> 16) & 0xff));
    data->append(static_cast<char>((value >> 8) & 0xff));
    data->append(static_cast<char>(value & 0xff));
}

void PskReporterUploader::appendVariableString(QByteArray* data, const QString& text) const
{
    const QByteArray utf8 = text.left(120).toUtf8();
    data->append(static_cast<char>(utf8.size()));
    data->append(utf8);
}

void PskReporterUploader::scheduleNextFlush()
{
    if (m_pending.isEmpty()) {
        m_flushTimer->stop();
        return;
    }

    const int intervalMs = 240000 + QRandomGenerator::global()->bounded(60000);
    m_flushTimer->start(intervalMs);
}

bool PskReporterUploader::shouldSendTemplates(qint64 nowMs) const
{
    return m_packetsSent < 3 || (nowMs - m_lastTemplateMs) >= kTemplateRefreshMs;
}

} // namespace AetherSDR
