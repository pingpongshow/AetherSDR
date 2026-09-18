#pragma once

#include "DxClusterClient.h"

#include <QObject>
#include <QHash>
#include <QHostAddress>
#include <QVector>

#include <optional>

class QFile;
class QTimer;
class QUdpSocket;

namespace AetherSDR {

struct ReceptionReport {
    QString receiverCallsign;
    QString receiverLocator;
    QString senderCallsign;
    QString senderLocator;
    quint64 frequencyHz{0};
    int snr{0};
    QString mode;
    qint64 flowStartSeconds{0};
    QString message;
    int lifetimeSec{120};
};

QString modeSettingSuffix(const QString& mode);
QString spotEnabledSettingKey(const QString& mode);
QString reporterEnabledSettingKey(const QString& mode);
QString reporterModeName(const QString& mode);
std::optional<ReceptionReport> parseTextReception(const QString& textBuffer,
                                                  const QString& receiverCallsign,
                                                  const QString& receiverLocator,
                                                  quint64 frequencyHz,
                                                  const QString& mode,
                                                  int snr = 0);

class DigitalModeSpotClient : public QObject {
    Q_OBJECT

public:
    explicit DigitalModeSpotClient(QObject* parent = nullptr);
    ~DigitalModeSpotClient() override;

    QString logFilePath() const;
    void submitReport(const ReceptionReport& report);

signals:
    void spotReceived(const DxSpot& spot);
    void rawLineReceived(const QString& line);

private:
    QString lineForReport(const ReceptionReport& report) const;
    QString spotColorForMode(const QString& mode, int snr) const;

    QFile* m_logFile{nullptr};
    QHash<QString, qint64> m_recentReports;
};

class PskReporterUploader : public QObject {
    Q_OBJECT

public:
    explicit PskReporterUploader(QObject* parent = nullptr);
    ~PskReporterUploader() override;

    void submitReport(const ReceptionReport& report);
    void flush();

private:
    void appendPadding(QByteArray* data) const;
    void appendReceiverRecord(QByteArray* data, const ReceptionReport& report) const;
    void appendSenderRecord(QByteArray* data, const ReceptionReport& report) const;
    void appendTemplateSets(QByteArray* data) const;
    void appendU16(QByteArray* data, quint16 value) const;
    void appendU32(QByteArray* data, quint32 value) const;
    void appendVariableString(QByteArray* data, const QString& text) const;
    void scheduleNextFlush();
    bool shouldSendTemplates(qint64 nowMs) const;
    void startHostLookup(qint64 nowMs);

    // report.pskreporter.info is resolved asynchronously and cached: this
    // object lives on the GUI thread, and QHostInfo::fromName() blocked it on
    // every flush (and on every report once 70 were pending while offline).
    QHostAddress m_serverAddress;
    int m_lookupId{-1};
    qint64 m_lastLookupMs{0};

    QUdpSocket* m_socket{nullptr};
    QTimer* m_flushTimer{nullptr};
    QVector<ReceptionReport> m_pending;
    QHash<QString, qint64> m_recentReports;
    quint32 m_sequenceNumber{0};
    quint32 m_observationDomainId{0};
    int m_packetsSent{0};
    qint64 m_lastTemplateMs{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ReceptionReport)
