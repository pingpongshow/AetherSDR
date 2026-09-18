#pragma once

#include "gui/MainWindow.h"

#include <QObject>

#include <functional>
#include <QPointer>

namespace AetherSDR {

class WsjtxUdpServer;
class WsjtxPanel;

// Spot sources (DX cluster, RBN, POTA, SpotCollector, FreeDV Reporter, digital-mode reporters), spot forwarding to the radio and the SpotHub dialog.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class SpotController : public QObject {
public:
    explicit SpotController(MainWindow& mw);

    DxClusterDialog* showSpotHubDialog(const QString& initialTab = {});
    void submitDigitalReceptionReport(const AetherSDR::ReceptionReport& report);
    void initDxccColoring();
    void createSpotClients();
    void wireSpotForwarding();
    void shutdownSpotClients();
    void autoStartSpotSources();
    void stopSpotSources();
    void addManualSpot(double freqMhz, const QString& callsign, const QString& comment, int lifetimeSec, bool forwardToCluster);
#ifdef HAVE_WEBSOCKETS
    void setFreeDvReporterEnabled(bool on);
#endif
    // WSJT-X / JTDX UDP protocol: decodes → client-side spots, spot click → Reply.
    void wireWsjtx();
    // Forward a locally generated spot (e.g. CW skimmer) through the same
    // dedup + batched "spot add" path as cluster spots.
    void submitSpot(const DxSpot& spot, const QString& source);
    void showWsjtxPanel();
    WsjtxUdpServer* wsjtxServer() const { return m_wsjtxServer; }

private:
    MainWindow& m_mw;

    DxClusterClient*   m_dxCluster{nullptr};
    DxClusterClient*   m_rbnClient{nullptr};
#ifdef HAVE_MQTT
    MqttClient*        m_mqttClient{nullptr};
#endif
    DigitalModeSpotClient* m_digitalSpotClient{nullptr};
    PskReporterUploader* m_pskReporterUploader{nullptr};
    SpotCollectorClient* m_spotCollectorClient{nullptr};
    PotaClient*          m_potaClient{nullptr};
    QThread*           m_spotThread{nullptr};
    // Spot deduplication: callsign → {freqMhz, timestamp ms}
    struct SpotDedup {
        double freqMhz;
        qint64 addedMs;
    };
    QHash<QString, SpotDedup> m_spotDedup;
    // Batched spot add commands (flushed 1/sec)
    QStringList m_spotCmdBatch;
    // Modeless dialogs
    QPointer<DxClusterDialog> m_spotHubDialog;
    std::function<void(const DxSpot&, const QString&)> m_queueSpot;
    WsjtxUdpServer*      m_wsjtxServer{nullptr};   // main thread, parented to this
    QPointer<WsjtxPanel> m_wsjtxPanel;
};

} // namespace AetherSDR
