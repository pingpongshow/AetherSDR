#pragma once

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QFile>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <atomic>
#include "DxClusterClient.h"  // for DxSpot
#include "FreeDvReporterProtocol.h"

namespace AetherSDR {

// FreeDV Reporter spot client — connects to qso.freedv.org via WebSocket
// with manual Engine.IO v4 / Socket.IO v4 framing. Tracks station state
// (callsign, frequency, grid, mode) and emits spotReceived() for each
// rx_report event (station heard another station).
//
// Default is view-only auth (no callsign, read-only).  When reporting is
// enabled via setReporting() it connects with role "report" and publishes
// this station's callsign/grid/frequency/TX state and decoded callsigns
// (freq_change / tx_report / rx_report), matching freedv-backend
// FreeDVReporter.cpp.  Reporting is OFF unless the user enables it.
class FreeDvClient : public QObject {
    Q_OBJECT

public:
    explicit FreeDvClient(QObject* parent = nullptr);
    ~FreeDvClient() override;

    void startConnection();
    void stopConnection();
    bool isConnected() const { return m_connected.load(); }

    QString logFilePath() const;

    bool isReporting() const { return m_reporting.load(); }

public slots:
    // Enable/disable publishing.  Reconnects if the auth role changes while
    // connected.  Does not open a connection by itself.
    void setReporting(bool enabled, const QString& callsign, const QString& gridSquare,
                      const QString& version);
    void reportFrequency(quint64 freqHz);
    void reportTransmit(const QString& mode, bool transmitting);
    void reportRx(const QString& callsign, const QString& mode, int snr);

    // Modem stopped / started (freedv-backend hideFromView()/showOurselves()).
    // Hidden: "hide_self" now and on every (re)connect instead of the
    // freq/tx/message state.  Showing sends "show_self" + current state.
    // Default is visible (pre-existing behaviour) until hideFromView().
    void hideFromView();
    void showOurselves();

    // Decoder sync / SNR for the RADE "receiving something" report: while
    // synced in a mode where sendsSyncedRxReport() is true, an rx_report with
    // an empty callsign is sent every kSyncedRxReportIntervalMs (freedv-gui).
    void setRxSyncState(bool synced, const QString& mode, int snr);

signals:
    void started();
    void stopped();
    void connectionError(const QString& error);
    void spotReceived(const DxSpot& spot);
    void rawLineReceived(const QString& line);

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessage(const QString& message);
    void onWsError(QAbstractSocket::SocketError err);
    void onReconnectTimer();
    void onSyncedRxReportTimer();

private:
    // Per-station state tracked by Socket.IO session ID
    struct StationInfo {
        QString callsign;
        QString gridSquare;
        double  freqMhz{0.0};
        QString mode;
        bool    rxOnly{false};
    };

    void handleEngineIO(const QString& raw);
    void handleSocketIO(const QString& payload);
    void handleEvent(const QString& eventName, const QJsonObject& data);
    void onNewConnection(const QJsonObject& data);
    void onFreqChange(const QJsonObject& data);
    void onRxReport(const QJsonObject& data);
    void onTxReport(const QJsonObject& data);
    void onRemoveConnection(const QJsonObject& data);
    void onBulkUpdate(const QJsonArray& pairs);
    void onConnectionSuccessful();
    void sendEvent(const QString& name, const QJsonObject& payload);
    void sendFrames(const QStringList& frames);
    bool canSend() const;
    FreeDvReporterProtocol::SessionState sessionState() const;
    void updateSyncedRxReportTimer();
    void reconnectWithCurrentAuth();

    QWebSocket*  m_ws;
    QTimer*      m_pingTimer;
    QTimer*      m_reconnectTimer;
    QTimer*      m_syncedRxReportTimer;
    QFile        m_logFile;

    QHash<QString, StationInfo> m_stations;  // sid → station info

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_reporting{false};
    bool    m_fullyConnected{false};   // server sent connection_successful
    FreeDvReporterProtocol::Identity m_identity;
    quint64 m_lastFreqHz{0};
    QString m_txMode;
    bool    m_transmitting{false};
    bool    m_hidden{false};
    bool    m_rxSynced{false};
    QString m_rxSyncMode;
    int     m_rxSyncSnr{0};
    bool    m_intentionalDisconnect{false};
    int     m_reconnectAttempts{0};
    int     m_pingIntervalMs{25000};

    static constexpr const char* WsUrl =
        "wss://qso.freedv.org/socket.io/?EIO=4&transport=websocket";
    static constexpr int MaxReconnectDelayMs     = 60000;
    static constexpr int InitialReconnectDelayMs  = 5000;
};

} // namespace AetherSDR
