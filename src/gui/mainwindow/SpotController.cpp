#include "gui/mainwindow/SpotController.h"

#include "core/WsjtxUdpServer.h"
#include "gui/WsjtxPanel.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/DigitalVoiceController.h"
#ifdef HAVE_MQTT
#include "gui/MqttApplet.h"
#endif
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "gui/SpectrumWidget.h"
#include "gui/AppletPanel.h"
#include "gui/DxClusterDialog.h"
#include "models/SliceModel.h"
#include "core/WeakSignalNativeDecoder.h"
#include <algorithm>
#include <cmath>
#include <QTimer>
#include <QDateTime>
#include <QDialog>
#include <QThread>
#include "core/AppSettings.h"

namespace AetherSDR {

static int digitalSpotLifetimeSeconds()
{
    auto& settings = AppSettings::instance();
    if (settings.contains(QStringLiteral("DigitalSpotLifetime")))
        return std::max(30, settings.value(QStringLiteral("DigitalSpotLifetime"), 120).toInt());
    return std::max(30, settings.value(QStringLiteral("WsjtxSpotLifetime"), 120).toInt());
}

SpotController::SpotController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void SpotController::initDxccColoring()
{
    // DXCC spot coloring (#330)
    m_mw.m_dxccProvider.loadCtyDat(":/cty.dat");
    {
        auto& s = AppSettings::instance();
        m_mw.m_dxccProvider.setEnabled(s.value("IsDxccColoringEnabled", "False").toString() == "True");
        m_mw.m_dxccProvider.colorNewDxcc = QColor(s.value("DxccColorNewEntity", "#FF3030").toString());
        m_mw.m_dxccProvider.colorNewBand = QColor(s.value("DxccColorNewBand", "#FF8C00").toString());
        m_mw.m_dxccProvider.colorNewMode = QColor(s.value("DxccColorNewMode", "#FFD700").toString());
        m_mw.m_dxccProvider.colorWorked  = QColor(s.value("DxccColorWorked", "#606060").toString());
        const QString adifPath = s.value("DxccAdifFilePath", "").toString();
        if (!adifPath.isEmpty()) {
            m_mw.m_dxccProvider.importAdifFile(adifPath);
            if (s.value("DxccAutoReloadAdif", "False").toString() == "True")
                m_mw.m_dxccProvider.setAutoReload(true, adifPath);
        }
    }
    connect(&m_mw.m_dxccProvider, &DxccColorProvider::importFinished,
            this, [this](int, int) { m_mw.m_radioModel.spotModel().refresh(); });
}

void SpotController::createSpotClients()
{
    // ── DX Cluster — forward parsed spots to radio ──────────────────────
    // ── Spot clients on worker thread ─────────────────────────────────────
    m_dxCluster = new DxClusterClient;
    m_rbnClient = new DxClusterClient;
    m_rbnClient->setLogFileName("rbn.log");
    m_digitalSpotClient = new DigitalModeSpotClient(&m_mw);
    m_pskReporterUploader = new PskReporterUploader(&m_mw);
    m_mw.m_weakSignalDecoder = new WeakSignalNativeDecoder(&m_mw);
    m_spotCollectorClient = new SpotCollectorClient;
    m_potaClient = new PotaClient;
#ifdef HAVE_WEBSOCKETS
    m_mw.m_freedvClient = new FreeDvClient;
#endif
#ifdef HAVE_MQTT
    m_mqttClient = new MqttClient(&m_mw);
    m_mw.m_appletPanel->mqttApplet()->setMqttClient(m_mqttClient);

    connect(m_mw.m_appletPanel->mqttApplet(), &MqttApplet::connectRequested,
            this, [this](const QString& host, quint16 port,
                         const QString& user, const QString& pass,
                         const QStringList& topics,
                         bool useTls, const QString& caFile) {
        m_mqttClient->connectToBroker(host, port, user, pass, useTls, caFile);
        for (const QString& t : topics) {
            m_mqttClient->subscribe(t);
        }
    });
    connect(m_mw.m_appletPanel->mqttApplet(), &MqttApplet::disconnectRequested,
            this, [this] { m_mqttClient->disconnect(); });

    // MQTT → panadapter overlay display
    connect(m_mw.m_appletPanel->mqttApplet(), &MqttApplet::displayValueChanged,
            this, [this](const QString& key, const QString& value) {
        if (auto* sw = m_mw.m_panStack->activeSpectrum()) {
            sw->setMqttDisplayValue(key, value);
        }
    });
    connect(m_mw.m_appletPanel->mqttApplet(), &MqttApplet::displayCleared,
            this, [this] {
        if (auto* sw = m_mw.m_panStack->activeSpectrum()) {
            sw->clearMqttDisplay();
        }
    });
#endif

    m_spotThread = new QThread(&m_mw);
    m_spotThread->setObjectName("SpotClients");
    m_dxCluster->moveToThread(m_spotThread);
    m_rbnClient->moveToThread(m_spotThread);
    m_spotCollectorClient->moveToThread(m_spotThread);
    m_potaClient->moveToThread(m_spotThread);
#ifdef HAVE_WEBSOCKETS
    m_mw.m_freedvClient->moveToThread(m_spotThread);
#endif
    m_spotThread->start();

    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            m_mw.m_weakSignalDecoder, &WeakSignalNativeDecoder::feedAudio);

#ifdef HAVE_WEBSOCKETS
    if (m_mw.m_modeFreeDvReporterAction) {
        connect(m_mw.m_freedvClient, &FreeDvClient::started, this, [this] {
            QSignalBlocker b(m_mw.m_modeFreeDvReporterAction);
            m_mw.m_modeFreeDvReporterAction->setChecked(true);
        });
        connect(m_mw.m_freedvClient, &FreeDvClient::stopped, this, [this] {
            QSignalBlocker b(m_mw.m_modeFreeDvReporterAction);
            m_mw.m_modeFreeDvReporterAction->setChecked(false);
#ifdef HAVE_DIGITAL_VOICE
            m_mw.m_digitalVoiceCtl->onFreeDvReporterStopped();
#endif
        });
        QSignalBlocker b(m_mw.m_modeFreeDvReporterAction);
        m_mw.m_modeFreeDvReporterAction->setChecked(false);
    }
#endif
}

void SpotController::wireSpotForwarding()
{
    // ── Spot forwarding: dedup + batch queue + 1/sec flush ────────────────

    // Dedup helper — returns true if spot should be skipped
    auto isDuplicateSpot = [this](const DxSpot& spot) -> bool {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto& as = AppSettings::instance();
        int lifetimeMs;
        if (spot.lifetimeSec > 0)
            lifetimeMs = spot.lifetimeSec * 1000;                      // source-provided
        else if (spot.source == "FreeDV")
            lifetimeMs = as.value("FreeDvSpotLifetime", 120).toInt() * 1000;  // HAVE_WEBSOCKETS
        else
        {
            int sec = as.value("DxClusterSpotLifetimeSec", 0).toInt();
            if (sec <= 0) sec = as.value("DxClusterSpotLifetime", 30).toInt() * 60;
            lifetimeMs = sec * 1000;
        }
        auto it = m_spotDedup.find(spot.dxCall);
        if (it != m_spotDedup.end()) {
            bool sameFreq = std::abs(it->freqMhz - spot.freqMhz) < 0.001;
            bool expired = (now - it->addedMs) > lifetimeMs;
            if (sameFreq && !expired)
                return true;
        }
        m_spotDedup[spot.dxCall] = {spot.freqMhz, now};
        return false;
    };

    // Build spot add command and queue for batch send
    auto queueSpotCmd = [this, isDuplicateSpot](const DxSpot& spot, const QString& source) {
        if (!m_mw.m_radioModel.isConnected()) return;
        if (isDuplicateSpot(spot)) return;
        QString call = QString(spot.dxCall).replace(' ', QChar(0x7f));
        QString freq = QString::number(spot.freqMhz, 'f', 6);
        QString cmd = "spot add callsign=" + call + " rx_freq=" + freq
                     + " tx_freq=" + freq
                     + " source=" + source
                     + " spotter_callsign=" + spot.spotterCall
                     + " lifetime_seconds=" + QString::number(
                           spot.lifetimeSec > 0 ? spot.lifetimeSec
                           : [&]() { int s = AppSettings::instance().value("DxClusterSpotLifetimeSec", 0).toInt();
                                     return s > 0 ? s : AppSettings::instance().value("DxClusterSpotLifetime", 30).toInt() * 60; }());
        if (!spot.comment.isEmpty())
            cmd += " comment=" + QString(spot.comment).replace(' ', QChar(0x7f));
        // Apply source-specific color if not already set
        QString spotColor = spot.color;
        if (spotColor.isEmpty()) {
            auto& as = AppSettings::instance();
            if (source == "DXCluster")
                spotColor = as.value("DxClusterSpotColor", "#D2B48C").toString();
            else if (source == "RBN")
                spotColor = as.value("RbnSpotColor", "#4488FF").toString();
            else if (source == "SpotCollector")
                spotColor = as.value("SpotCollectorSpotColor", "#FFD700").toString();
            else if (source == "FreeDV")
                spotColor = as.value("FreeDvSpotColor", "#FF8C00").toString();  // HAVE_WEBSOCKETS
        }
        if (!spotColor.isEmpty()) {
            if (spotColor.length() == 7) spotColor = "#FF" + spotColor.mid(1);
            cmd += " color=" + spotColor;
        }
        m_spotCmdBatch.append(cmd);
    };
    m_queueSpot = queueSpotCmd;

    // Flush batch: send queued spot commands to radio (1/sec)
    auto* spotCmdTimer = new QTimer(&m_mw);
    spotCmdTimer->start(1000);
    connect(spotCmdTimer, &QTimer::timeout, this, [this] {
        if (m_spotCmdBatch.isEmpty() || !m_mw.m_radioModel.isConnected()) return;
        // Send up to RbnRateLimit commands per tick
        int limit = AppSettings::instance().value("RbnRateLimit", 10).toInt();
        int count = std::min(static_cast<int>(m_spotCmdBatch.size()), limit);
        for (int i = 0; i < count; ++i)
            m_mw.m_radioModel.sendCommand(m_spotCmdBatch[i]);
        m_spotCmdBatch.remove(0, count);
    });

    connect(m_dxCluster, &DxClusterClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "DXCluster");
    });

    connect(m_rbnClient, &DxClusterClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "RBN");
    });

    connect(m_digitalSpotClient, &DigitalModeSpotClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, spot.source);
    });

    connect(m_spotCollectorClient, &SpotCollectorClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "SpotCollector");
    });

    connect(m_potaClient, &PotaClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "POTA");
    });

#ifdef HAVE_WEBSOCKETS
    connect(m_mw.m_freedvClient, &FreeDvClient::spotReceived,
            this, [queueSpotCmd](const DxSpot& spot) {
        queueSpotCmd(spot, "FreeDV");
    });
#endif

    wireWsjtx();
}

void SpotController::wireWsjtx()
{
    m_wsjtxServer = new WsjtxUdpServer(this);
    // WSJT-X spots are client-side only (like TCI spots): the radio never sees them.
    connect(m_wsjtxServer, &WsjtxUdpServer::spotUpsert, this,
            [this](int spotId, const QMap<QString, QString>& kvs) {
        m_mw.m_radioModel.spotModel().applySpotStatus(spotId, kvs);
    });
    connect(m_wsjtxServer, &WsjtxUdpServer::spotRemove, this, [this](int spotId) {
        m_mw.m_radioModel.spotModel().removeSpot(spotId);
    });
    // Optional: colour needed calls inside WSJT-X's Band Activity window.
    connect(m_wsjtxServer, &WsjtxUdpServer::decodeReceived, this,
            [this](const WsjtxDecodeEvent& ev) {
        if (!m_mw.m_dxccProvider.isEnabled() || !ev.decode.isNew || ev.parsed.deCall.isEmpty()
            || AppSettings::instance().value("WsjtxHighlightDxcc", "False").toString() != "True") {
            return;
        }
        const double mhz = static_cast<double>(ev.rfFrequencyHz) / 1e6;
        const DxccStatus st = m_mw.m_dxccProvider.statusForSpot(ev.parsed.deCall, mhz, ev.modeName);
        if (st == DxccStatus::NewDxcc || st == DxccStatus::NewBand || st == DxccStatus::NewMode) {
            m_wsjtxServer->sendHighlightCallsign(ev.instanceId, ev.parsed.deCall,
                m_mw.m_dxccProvider.colorForSpot(ev.parsed.deCall, mhz, ev.modeName),
                QColor(Qt::black));
        }
    });
    m_wsjtxServer->applySettings();
}

void SpotController::submitSpot(const DxSpot& spot, const QString& source)
{
    if (m_queueSpot) {
        m_queueSpot(spot, source);
    }
}

void SpotController::showWsjtxPanel()
{
    if (!m_wsjtxPanel) {
        m_wsjtxPanel = new WsjtxPanel(m_wsjtxServer, &m_mw);
        m_wsjtxPanel->setWindowFlag(Qt::Window);
        m_wsjtxPanel->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_wsjtxPanel->show();
    m_wsjtxPanel->raise();
    m_wsjtxPanel->activateWindow();
}

void SpotController::shutdownSpotClients()
{
    if (m_pskReporterUploader)
        m_pskReporterUploader->flush();

    // Stop spot client worker thread
    if (m_spotThread) {
        // Destroy the clients on the spot thread itself: deleteLater() on an
        // object whose thread is about to finish runs as the thread exits, so
        // their timers are stopped from the owning thread (deleting them here
        // after wait() warned "Timers cannot be stopped from another thread").
        QObject* clients[] = {m_dxCluster, m_rbnClient, m_spotCollectorClient, m_potaClient,
#ifdef HAVE_WEBSOCKETS
                              m_mw.m_freedvClient,
#endif
        };
        for (QObject* c : clients) {
            if (c) {
                c->deleteLater();
            }
        }
        m_dxCluster = nullptr;
        m_rbnClient = nullptr;
        m_spotCollectorClient = nullptr;
        m_potaClient = nullptr;
#ifdef HAVE_WEBSOCKETS
        m_mw.m_freedvClient = nullptr;
#endif
        m_spotThread->quit();
        m_spotThread->wait(3000);
    }
}

void SpotController::submitDigitalReceptionReport(const AetherSDR::ReceptionReport& report)
{
    if (!m_digitalSpotClient && !m_pskReporterUploader)
        return;

    ReceptionReport adjusted = report;
    adjusted.mode = reporterModeName(report.mode);
    adjusted.lifetimeSec = digitalSpotLifetimeSeconds();

    auto& settings = AppSettings::instance();
    const bool spotEnabled =
        settings.value(spotEnabledSettingKey(adjusted.mode), QStringLiteral("False")).toString()
        == QStringLiteral("True");
    const bool reporterEnabled =
        settings.value(reporterEnabledSettingKey(adjusted.mode), QStringLiteral("False")).toString()
        == QStringLiteral("True");

    if (spotEnabled && m_digitalSpotClient)
        m_digitalSpotClient->submitReport(adjusted);
    if (reporterEnabled && m_pskReporterUploader)
        m_pskReporterUploader->submitReport(adjusted);
}

DxClusterDialog* SpotController::showSpotHubDialog(const QString& initialTab)
{
    if (m_spotHubDialog) {
        m_spotHubDialog->setTotalSpots(m_mw.m_radioModel.spotModel().spots().size());
        m_spotHubDialog->updateStatus();
        if (!initialTab.isEmpty())
            m_spotHubDialog->selectTab(initialTab);
        m_spotHubDialog->raise();
        m_spotHubDialog->activateWindow();
        return m_spotHubDialog;
    }

    auto* dlg = new DxClusterDialog(m_dxCluster, m_rbnClient, m_digitalSpotClient,
                        m_spotCollectorClient, m_potaClient,
#ifdef HAVE_WEBSOCKETS
                        m_mw.m_freedvClient,
#endif
                        &m_mw.m_radioModel, &m_mw.m_dxccProvider, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_spotHubDialog = dlg;
    dlg->setTotalSpots(m_mw.m_radioModel.spotModel().spots().size());

    // Live preview: refresh spots on every display settings change.
    auto refreshSpots = [this]() {
        auto& s = AppSettings::instance();
        bool on       = s.value("IsSpotsEnabled", "True").toString() == "True";
        int fontSize  = s.value("SpotFontSize", "16").toInt();
        int levels    = s.value("SpotsMaxLevel", "3").toInt();
        int position  = s.value("SpotsStartingHeightPercentage", "50").toInt();
        bool override = s.value("IsSpotsOverrideColorsEnabled", "False").toString() == "True";
        QColor spotColor(s.value("SpotsOverrideColor", "#FFFF00").toString());
        QColor bgColor(s.value("SpotsOverrideBgColor", "#000000").toString());
        int bgOpacity = s.value("SpotsBackgroundOpacity", 48).toInt();
        for (auto* a : m_mw.m_panStack->allApplets()) {
            auto* sw = a->spectrumWidget();
            sw->setShowSpots(on);
            sw->setSpotFontSize(fontSize);
            sw->setSpotMaxLevels(levels);
            sw->setSpotStartPct(position);
            sw->setSpotOverrideColors(override);
            sw->setSpotOverrideBg(
                s.value("IsSpotsOverrideBackgroundColorsEnabled", "True").toString() == "True");
            sw->setSpotColor(spotColor);
            sw->setSpotBgColor(bgColor);
            sw->setSpotBgOpacity(bgOpacity);
        }
        // Rebuild markers so source-level visibility changes, such as the
        // Memories feed toggle, apply immediately without mutating the cache.
        m_mw.m_radioModel.spotModel().refresh();
    };
    connect(dlg, &DxClusterDialog::settingsChanged, this, refreshSpots);
    connect(dlg, &DxClusterDialog::connectRequested,
            this, [this](const QString& host, quint16 port, const QString& call) {
        QMetaObject::invokeMethod(m_dxCluster, [this, host, port, call] {
            m_dxCluster->connectToCluster(host, port, call);
        });
    });
    connect(dlg, &DxClusterDialog::disconnectRequested,
            this, [this] {
        QMetaObject::invokeMethod(m_dxCluster, [this] { m_dxCluster->disconnect(); });
    });
    connect(dlg, &DxClusterDialog::rbnConnectRequested,
            this, [this](const QString& host, quint16 port, const QString& call) {
        QMetaObject::invokeMethod(m_rbnClient, [this, host, port, call] {
            m_rbnClient->connectToCluster(host, port, call);
        });
    });
    connect(dlg, &DxClusterDialog::rbnDisconnectRequested,
            this, [this] {
        QMetaObject::invokeMethod(m_rbnClient, [this] { m_rbnClient->disconnect(); });
    });
    connect(dlg, &DxClusterDialog::spotCollectorStartRequested,
            this, [this](quint16 port) {
        QMetaObject::invokeMethod(m_spotCollectorClient, [this, port] {
            m_spotCollectorClient->startListening(port);
        });
    });
    connect(dlg, &DxClusterDialog::spotCollectorStopRequested,
            this, [this] {
        QMetaObject::invokeMethod(m_spotCollectorClient, [this] {
            m_spotCollectorClient->stopListening();
        });
    });
    connect(dlg, &DxClusterDialog::potaStartRequested,
            this, [this](int interval) {
        QMetaObject::invokeMethod(m_potaClient, [this, interval] {
            m_potaClient->startPolling(interval);
        });
    });
    connect(dlg, &DxClusterDialog::potaStopRequested,
            this, [this] {
        QMetaObject::invokeMethod(m_potaClient, [this] { m_potaClient->stopPolling(); });
    });
#ifdef HAVE_WEBSOCKETS
    connect(dlg, &DxClusterDialog::freedvStartRequested,
            this, [this] {
        QMetaObject::invokeMethod(m_mw.m_freedvClient, [this] { m_mw.m_freedvClient->startConnection(); });
    });
    connect(dlg, &DxClusterDialog::freedvStopRequested,
            this, [this] {
        QMetaObject::invokeMethod(m_mw.m_freedvClient, [this] { m_mw.m_freedvClient->stopConnection(); });
    });
#endif
    connect(dlg, &DxClusterDialog::spotsClearedAll,
            this, [this] { m_spotDedup.clear(); });
    connect(dlg, &DxClusterDialog::tuneRequested,
            this, [this](double freqMhz) {
        if (auto* sl = m_mw.activeSlice())
            sl->tuneAndRecenter(freqMhz);
    });
    connect(dlg, &QDialog::finished, this, refreshSpots);

    dlg->updateStatus();
    if (!initialTab.isEmpty())
        dlg->selectTab(initialTab);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

#ifdef HAVE_WEBSOCKETS
void SpotController::setFreeDvReporterEnabled(bool on)
{
    if (!m_mw.m_freedvClient)
        return;
    if (on) {
        QMetaObject::invokeMethod(m_mw.m_freedvClient, [this] {
            m_mw.m_freedvClient->startConnection();
        });
    } else {
        QMetaObject::invokeMethod(m_mw.m_freedvClient, [this] {
            m_mw.m_freedvClient->stopConnection();
        });
    }
}
#endif

void SpotController::autoStartSpotSources()
{
    auto& cs = AppSettings::instance();
    if (cs.value("DxClusterAutoConnect", "False").toString() == "True") {
        QString host = cs.value("DxClusterHost", "dxc.nc7j.com").toString();
        quint16 cPort = static_cast<quint16>(cs.value("DxClusterPort", 7300).toInt());
        QString call = cs.value("DxClusterCallsign").toString();
        if (!call.isEmpty() && !m_dxCluster->isConnected())
            QMetaObject::invokeMethod(m_dxCluster, [=] { m_dxCluster->connectToCluster(host, cPort, call); });
    }
    // Auto-connect RBN if enabled
    if (cs.value("RbnAutoConnect", "False").toString() == "True") {
        QString host = cs.value("RbnHost", "telnet.reversebeacon.net").toString();
        quint16 rPort = static_cast<quint16>(cs.value("RbnPort", 7000).toInt());
        QString call = cs.value("RbnCallsign").toString();
        if (call.isEmpty())
            call = cs.value("DxClusterCallsign").toString();
        if (!call.isEmpty() && !m_rbnClient->isConnected())
            QMetaObject::invokeMethod(m_rbnClient, [=] { m_rbnClient->connectToCluster(host, rPort, call); });
    }
    // Auto-start SpotCollector listener if enabled
    if (cs.value("SpotCollectorAutoStart", "False").toString() == "True") {
        quint16 scPort = static_cast<quint16>(cs.value("SpotCollectorPort", 9999).toInt());
        if (!m_spotCollectorClient->isListening())
            QMetaObject::invokeMethod(m_spotCollectorClient, [=] { m_spotCollectorClient->startListening(scPort); });
    }
    // Auto-start POTA polling if enabled
    if (cs.value("PotaAutoStart", "False").toString() == "True") {
        int pInterval = cs.value("PotaPollInterval", 30).toInt();
        if (!m_potaClient->isPolling())
            QMetaObject::invokeMethod(m_potaClient, [=] { m_potaClient->startPolling(pInterval); });
    }
#ifdef HAVE_WEBSOCKETS
    // Auto-start FreeDV Reporter if enabled
    if (cs.value("FreeDvAutoStart", "False").toString() == "True") {
        if (!m_mw.m_freedvClient->isConnected())
            QMetaObject::invokeMethod(m_mw.m_freedvClient, [this] { m_mw.m_freedvClient->startConnection(); });
    }
#endif
}

void SpotController::stopSpotSources()
{
    QMetaObject::invokeMethod(m_dxCluster, [=] { m_dxCluster->disconnect(); });
    QMetaObject::invokeMethod(m_rbnClient, [=] { m_rbnClient->disconnect(); });
    QMetaObject::invokeMethod(m_spotCollectorClient, [=] { m_spotCollectorClient->stopListening(); });
    QMetaObject::invokeMethod(m_potaClient, [=] { m_potaClient->stopPolling(); });
#ifdef HAVE_WEBSOCKETS
    QMetaObject::invokeMethod(m_mw.m_freedvClient, [this] { m_mw.m_freedvClient->stopConnection(); });
#endif
}

void SpotController::addManualSpot(double freqMhz, const QString& callsign, const QString& comment, int lifetimeSec, bool forwardToCluster)
{
    auto& as = AppSettings::instance();
    QString call = QString(callsign).replace(' ', QChar(0x7f));
    QString freq = QString::number(freqMhz, 'f', 6);
    QString myCall = as.value("DxClusterCallsign").toString();
    if (myCall.isEmpty()) myCall = "AetherSDR";
    QString cmd = "spot add callsign=" + call + " rx_freq=" + freq
                 + " tx_freq=" + freq
                 + " source=Manual"
                 + " spotter_callsign=" + myCall
                 + " lifetime_seconds=" + QString::number(lifetimeSec);
    if (!comment.isEmpty())
        cmd += " comment=" + QString(comment).replace(' ', QChar(0x7f));
    QString spotColor = as.value("ManualSpotColor", "#00FF00").toString();
    if (spotColor.length() == 7) spotColor = "#FF" + spotColor.mid(1);
    cmd += " color=" + spotColor;
    m_mw.m_radioModel.sendCommand(cmd);

    // Forward to DX cluster if requested
    if (forwardToCluster && m_dxCluster && m_dxCluster->isConnected()) {
        QString dxCmd = QString("DX %1 %2 %3")
            .arg(freqMhz * 1000.0, 0, 'f', 1)
            .arg(callsign)
            .arg(comment);
        QMetaObject::invokeMethod(m_dxCluster, [this, dxCmd] {
            m_dxCluster->sendCommand(dxCmd);
        });
    }
}

} // namespace AetherSDR
