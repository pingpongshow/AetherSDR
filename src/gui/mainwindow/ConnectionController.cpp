#include "gui/mainwindow/ConnectionController.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/AudioDspController.h"
#include "gui/mainwindow/CatServersController.h"
#include "gui/mainwindow/DaxController.h"
#include "gui/mainwindow/MemoryController.h"
#include "gui/mainwindow/SpotController.h"
#include "gui/mainwindow/StatusBarController.h"
#include "gui/ConnectionPanel.h"
#include "gui/TitleBar.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "gui/SpectrumWidget.h"
#include "gui/SpectrumOverlayMenu.h"
#include "gui/VfoWidget.h"
#include "models/SliceModel.h"
#include "models/BandPlanManager.h"
#include "core/BandStackSettings.h"
#include "gui/BandStackPanel.h"
#include <QApplication>
#include <QScreen>
#include <QTimer>
#include <QDateTime>
#include <QWindow>
#include <QVBoxLayout>
#include <QDialog>
#include <QLabel>
#include "core/AppSettings.h"
#include <QPushButton>

namespace AetherSDR {

ConnectionController::ConnectionController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void ConnectionController::wireConnectionSignals()
{
    // ── Wire up discovery ──────────────────────────────────────────────────
    connect(&m_mw.m_discovery, &RadioDiscovery::radioDiscovered,
            m_mw.m_connPanel, &ConnectionPanel::onRadioDiscovered);
    connect(&m_mw.m_discovery, &RadioDiscovery::radioUpdated,
            m_mw.m_connPanel, &ConnectionPanel::onRadioUpdated);
    connect(&m_mw.m_discovery, &RadioDiscovery::radioLost,
            m_mw.m_connPanel, &ConnectionPanel::onRadioLost);
    connect(m_mw.m_connPanel, &ConnectionPanel::retryDiscoveryRequested, this, [this] {
        m_mw.m_connPanel->setStatusText("Searching your local network…");
        if (m_mw.m_titleBar) m_mw.m_titleBar->setDiscovering(true);
        m_mw.m_discovery.stopListening();
        m_mw.m_discovery.startListening();
    });
    connect(m_mw.m_connPanel, &ConnectionPanel::networkDiagnosticsRequested,
            &m_mw, &MainWindow::showNetworkDiagnosticsDialog);

    // ── Heartbeat indicator + disconnect detection via TCP ping ─────────
    m_heartbeatMissTimer = new QTimer(&m_mw);
    m_heartbeatMissTimer->setInterval(1500);
    connect(m_heartbeatMissTimer, &QTimer::timeout, this, [this]() {
        if (m_mw.m_titleBar) m_mw.m_titleBar->onHeartbeatLost();
    });

    // Ping-based heartbeat — covers local, routed, and SmartLink connections
    connect(&m_mw.m_radioModel, &RadioModel::pingReceived, this, [this]() {
        if (m_mw.m_titleBar) {
            m_mw.m_titleBar->onHeartbeat();
            m_heartbeatMissTimer->start(); // reset miss timer
        }
    });

    connect(m_mw.m_connPanel, &ConnectionPanel::connectRequested,
            this, [this](const RadioInfo& info){
        m_mw.m_connPanel->setStatusText("Connecting…");
        m_mw.m_userDisconnected = false;
        setPanadapterConnectionAnimation(true, "Connecting to radio…");
        m_mw.m_radioModel.connectToRadio(info);
        auto& s = AppSettings::instance();
        s.setValue("LastConnectedRadioSerial", info.serial);
        if (info.isRouted) {
            s.setValue("LastRoutedRadioIp", info.address.toString());
        } else {
            s.remove("LastRoutedRadioIp");
        }
        s.save();
    });

    // Auto-connect: when a radio is discovered, check if it matches the last one
    connect(&m_mw.m_discovery, &RadioDiscovery::radioDiscovered,
            this, [this](const RadioInfo& info) {
        if (m_mw.m_userDisconnected) return;
        const QString lastSerial = AppSettings::instance()
            .value("LastConnectedRadioSerial").toString();
        if (!lastSerial.isEmpty() && info.serial == lastSerial
            && !m_mw.m_radioModel.isConnected()) {
            qCDebug(lcGui) << "Auto-connecting to" << info.displayName();
            m_mw.m_connPanel->setStatusText("Auto-connecting…");
            setPanadapterConnectionAnimation(true, "Connecting to radio…");
            m_mw.m_radioModel.connectToRadio(info);
        }
    });
    connect(m_mw.m_connPanel, &ConnectionPanel::disconnectRequested,
            this, [this]{
        m_mw.m_userDisconnected = true;
        setPanadapterConnectionAnimation(false);
        auto& s = AppSettings::instance();
        s.remove("LastConnectedRadioSerial");
        s.remove("LastRoutedRadioIp");
        s.save();
        m_mw.m_radioModel.disconnectFromRadio();
    });

    // ── SmartLink ──────────────────────────────────────────────────────────
    m_mw.m_connPanel->setSmartLinkClient(&m_mw.m_smartLink);

    connect(m_mw.m_connPanel, &ConnectionPanel::smartLinkLoginRequested,
            this, [this](const QString& email, const QString& pass) {
        m_mw.m_smartLink.login(email, pass);
    });

    // WAN radio connect: ask SmartLink server for a handle, then TLS to radio
    connect(m_mw.m_connPanel, &ConnectionPanel::wanConnectRequested,
            this, [this](const WanRadioInfo& info) {
        m_mw.m_connPanel->setStatusText("Requesting SmartLink connection…");
        setPanadapterConnectionAnimation(true, "Connecting to remote radio…");
        // Store WAN radio info for when connect_ready arrives
        m_pendingWanRadio = info;

        // Pre-bind UDP socket for VITA-49 reception BEFORE requesting
        // connection, so we can pass our port to the SmartLink server.
        // The server tells the radio our public IP:port for UDP streaming.
        quint16 udpPort = m_mw.m_radioModel.panStream()->localPort();
        if (udpPort == 0) {
            // Not yet bound — start WAN early to get a port
            const quint16 radioUdpPort = static_cast<quint16>(
                info.publicUdpPort > 0 ? info.publicUdpPort : 4993);
            auto* ps = m_mw.m_radioModel.panStream();
            QMetaObject::invokeMethod(ps, [ps, info, radioUdpPort]() {
                ps->startWan(QHostAddress(info.publicIp), radioUdpPort);
            }, Qt::BlockingQueuedConnection);
            udpPort = ps->localPort();
        }
        qCDebug(lcGui) << "MainWindow: pre-bound UDP port" << udpPort << "for WAN hole punch";
        m_mw.m_smartLink.requestConnect(info.serial, udpPort);
    });

    // SmartLink server says radio is ready — connect via TLS
    connect(&m_mw.m_smartLink, &SmartLinkClient::connectReady,
            this, [this](const QString& handle, const QString& serial) {
        if (serial != m_pendingWanRadio.serial) return;
        m_mw.m_connPanel->setStatusText("TLS connecting to radio…");
        setPanadapterConnectionAnimation(true, "Connecting to remote radio…");
        m_mw.m_wanConnection.connectToRadio(
            m_pendingWanRadio.publicIp,
            static_cast<quint16>(m_pendingWanRadio.publicTlsPort),
            handle);
    });

    // WAN connection established — wire to RadioModel
    // TODO: RadioModel needs to accept WanConnection as an alternative
    // to RadioConnection. For now, log the event.
    connect(&m_mw.m_wanConnection, &WanConnection::connected, this, [this] {
        qCDebug(lcGui) << "MainWindow: WAN connection established!";
        m_mw.m_connPanel->setStatusText("Connected via SmartLink");
        m_mw.m_connPanel->setConnected(true);

        // Wire WanConnection to RadioModel for full operation
        m_mw.m_radioModel.connectViaWan(&m_mw.m_wanConnection,
            m_pendingWanRadio.publicIp,
            static_cast<quint16>(m_pendingWanRadio.publicUdpPort > 0
                ? m_pendingWanRadio.publicUdpPort : 4993));
    });
    connect(&m_mw.m_wanConnection, &WanConnection::disconnected, this, [this] {
        qCDebug(lcGui) << "MainWindow: WAN connection lost";
        m_mw.m_connPanel->setStatusText("SmartLink disconnected");
        m_mw.m_connPanel->setConnected(false);
        setPanadapterConnectionAnimation(false);
    });
    connect(&m_mw.m_wanConnection, &WanConnection::errorOccurred, this, [this](const QString& err) {
        m_mw.m_connPanel->setStatusText("SmartLink error: " + err);
        if (!m_mw.m_reconnectDlg)
            setPanadapterConnectionAnimation(false);
    });
}

void ConnectionController::wireReconnectProgress()
{
    // ── Auto-reconnect progress ─────────────────────────────────────────
    // Auto-reconnect used to be silent: the UI showed "disconnected" and the
    // model retried in the background forever, so a user could not tell a
    // retrying client from a dead one. Surface the attempt count and a live
    // countdown, and say so explicitly when we give up.
    connect(&m_mw.m_radioModel, &RadioModel::reconnectScheduled, this,
            [this](int attempt, int maxAttempts, int delayMs) {
        m_reconnectDeadline = QDateTime::currentMSecsSinceEpoch() + delayMs;
        m_reconnectAttemptNum = attempt;
        m_reconnectMaxAttempts = maxAttempts;
        if (!m_reconnectTicker) {
            m_reconnectTicker = new QTimer(&m_mw);
            connect(m_reconnectTicker, &QTimer::timeout, this, [this] {
                const qint64 remainMs =
                    m_reconnectDeadline - QDateTime::currentMSecsSinceEpoch();
                if (remainMs <= 0) {
                    m_reconnectTicker->stop();
                    return;
                }
                m_mw.statusBar()->showMessage(
                    QString("Reconnecting to radio — attempt %1 of %2 in %3s…")
                        .arg(m_reconnectAttemptNum)
                        .arg(m_reconnectMaxAttempts)
                        .arg((remainMs + 999) / 1000));
            });
        }
        m_reconnectTicker->start(250);
    });
    connect(&m_mw.m_radioModel, &RadioModel::reconnectAttemptStarted, this,
            [this](int attempt, int maxAttempts) {
        if (m_reconnectTicker) m_reconnectTicker->stop();
        m_mw.statusBar()->showMessage(
            QString("Reconnecting to radio — attempt %1 of %2…")
                .arg(attempt).arg(maxAttempts));
    });
    connect(&m_mw.m_radioModel, &RadioModel::reconnectAbandoned, this,
            [this](int attempts) {
        if (m_reconnectTicker) m_reconnectTicker->stop();
        m_mw.statusBar()->showMessage(
            QString("Could not reconnect after %1 attempts — use Radio ▸ Connect to retry")
                .arg(attempts));
    });
}

void ConnectionController::startDiscovery()
{
    // Start discovery — show amber indicator while waiting for connection
    if (m_mw.m_titleBar) m_mw.m_titleBar->setDiscovering(true);
    m_mw.m_discovery.startListening();

    const QString startupLastSerial =
        AppSettings::instance().value("LastConnectedRadioSerial").toString();
    if (!startupLastSerial.isEmpty()) {
        m_mw.m_connPanel->setStatusText("Looking for your radio…");
        setPanadapterConnectionAnimation(true, "Looking for your radio…");
    }

    // Auto-connect to routed radios (probed, not broadcast-discovered)
    connect(m_mw.m_connPanel, &ConnectionPanel::routedRadioFound,
            this, [this](const RadioInfo& info) {
        if (m_mw.m_userDisconnected || m_mw.m_radioModel.isConnected()) return;
        const QString lastSerial = AppSettings::instance()
            .value("LastConnectedRadioSerial").toString();
        if (!lastSerial.isEmpty() && info.serial == lastSerial) {
            qCDebug(lcGui) << "Auto-connecting to routed radio" << info.address.toString();
            m_mw.m_connPanel->setStatusText("Auto-connecting…");
            setPanadapterConnectionAnimation(true, "Connecting to radio…");
            m_mw.m_radioModel.connectToRadio(info);
        }
    });

    // Probe saved routed radio on startup
    {
        auto& s = AppSettings::instance();
        const QString routedIp = s.value("LastRoutedRadioIp").toString();
        if (!routedIp.isEmpty() && !m_mw.m_userDisconnected) {
            m_mw.m_connPanel->setStatusText("Looking for your radio…");
            setPanadapterConnectionAnimation(true, "Looking for your radio…");
            QTimer::singleShot(500, this, [this, routedIp] {
                m_mw.m_connPanel->probeRadio(routedIp);
            });
        }
    }
}

void ConnectionController::toggleConnectionDialog()
{
    if (m_mw.m_connPanel->isVisible()) {
        m_mw.m_connPanel->hide();
        return;
    }

    // Position above the status bar, centered on the station label, while staying on-screen.
    QPoint statusBarTop = m_mw.statusBar()->mapToGlobal(QPoint(0, 0));
    QPoint labelCenter = m_mw.m_stationNickLabel->mapToGlobal(
        QPoint(m_mw.m_stationNickLabel->width() / 2, 0));
    QScreen* screen = QApplication::screenAt(labelCenter);
    if (!screen && m_mw.windowHandle())
        screen = m_mw.windowHandle()->screen();
    if (!screen)
        screen = QApplication::primaryScreen();

    const QSize dlgSize = m_mw.m_connPanel->size();
    QPoint pos(labelCenter.x() - dlgSize.width() / 2,
               statusBarTop.y() - dlgSize.height() - 8);

    if (screen) {
        const QRect available = screen->availableGeometry();
        const int maxX = available.left() + available.width() - dlgSize.width();
        const int maxY = available.top() + available.height() - dlgSize.height();
        pos.setX(qMax(available.left(), qMin(pos.x(), maxX)));
        pos.setY(qMax(available.top(), qMin(pos.y(), maxY)));
    }

    m_mw.m_connPanel->move(pos);
    m_mw.m_connPanel->show();
    m_mw.m_connPanel->raise();
    m_mw.m_connPanel->activateWindow();
}

void ConnectionController::onConnectionStateChanged(bool connected)
{
    m_mw.m_connPanel->setConnected(connected);
    if (connected) {
        // Clear any reconnect countdown left over from the drop that led here.
        if (m_reconnectTicker) m_reconnectTicker->stop();
        m_mw.statusBar()->clearMessage();
        m_mw.m_radioInfoLabel->setText(m_mw.m_radioModel.model());
        m_mw.m_radioVersionLabel->setText(m_mw.m_radioModel.version());
        m_mw.m_stationLabel->setText(m_mw.m_radioModel.nickname());
        m_mw.m_connStatusLabel->setText("Connected");
        m_mw.m_connPanel->setStatusText("Connected");

        // Slice tab toggle is initialized from infoChanged when radio
        // reports its actual slice capacity (#1278).

        // Show DIV button on dual-SCU radios
        {
            // Radio-reported diversity_allowed (fw 4.2.18+), model-name fallback otherwise
            const bool divAllowed = m_mw.m_radioModel.diversityAllowed();
            // Set diversity allowed on all existing VFO widgets (including Slice A at startup) (#1503)
            if (m_mw.m_panStack) {
                for (auto* pan : m_mw.m_radioModel.panadapters()) {
                    if (auto* sw = m_mw.m_panStack->spectrum(pan->panId())) {
                        for (auto* slice : m_mw.m_radioModel.slices()) {
                            if (auto* vfo = sw->vfoWidget(slice->sliceId())) {
                                vfo->setDiversityAllowed(divAllowed);
                            }
                        }
                    }
                }
            }
        }
        // Only start the local RX audio sink if the user wants audio routed
        // to the PC. When PC Audio is off we may still request a
        // remote_audio_rx stream for TCI clients; the sink should stay
        // silent in that case. The PC Audio toggle handler starts/stops
        // the sink when the user flips it.
        // Always sync the button to the setting here so any divergence
        // (e.g. from a profile load before connect) is corrected (#1536).
        {
            const bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
            m_mw.m_titleBar->setPcAudioEnabled(pcAudio);
            if (pcAudio)
                m_mw.audioStartRx();
        }
        m_mw.m_dspCtl->updateNr2Availability();  // Disable NR2 if connected via SmartLink/Opus (#1597)
        // TX audio stream will start when the radio assigns a stream ID
        // Auto-hide the connection dialog on successful connect
        m_mw.m_connPanel->hide();

        // Close reconnect dialog if it was showing
        if (m_mw.m_reconnectDlg) {
            m_mw.m_reconnectDlg->close();
            m_mw.m_reconnectDlg->deleteLater();
            m_mw.m_reconnectDlg = nullptr;
        }

        // Load band stack bookmarks for this radio
        BandStackSettings::instance().load();
        m_mw.m_panStack->bandStackPanel()->loadBookmarks(
            m_mw.m_radioModel.serial(), m_mw.m_bandPlanMgr);
        m_mw.m_memoryCtl->refreshMemoryBrowsePanel();
        m_mw.m_statusCtl->updateBandStackIndicator();

        m_mw.m_catCtl->autoStartCatServers();
        // Populate XVTR bands after radio status settles, and refresh
        // whenever XVTR config changes (add/remove/rename). (#571)
        auto refreshXvtr = [this]() {
            if (!m_mw.m_radioModel.isConnected()) return;
            QVector<SpectrumOverlayMenu::XvtrBand> xvtrBands;
            for (const auto& x : m_mw.m_radioModel.xvtrList()) {
                if (x.isValid)
                    xvtrBands.append({x.name, x.rfFreq});
            }
            for (auto* applet : m_mw.m_panStack->allApplets())
                applet->spectrumWidget()->overlayMenu()->setXvtrBands(xvtrBands);
        };
        QTimer::singleShot(2000, this, refreshXvtr);
        disconnect(m_xvtrRefreshConn);  // onConnectionStateChanged(true) runs on every reconnect
        m_xvtrRefreshConn = connect(&m_mw.m_radioModel, &RadioModel::infoChanged, this, refreshXvtr);

        // Apply saved display settings after panadapter is created
        m_mw.m_displaySettingsPushed = false;

#if defined(HAVE_DAX_BRIDGE)
        m_mw.m_daxCtl->scheduleDaxAutoStart();
#endif
        // Auto-connect DX cluster if enabled
        {
            auto& cs = AppSettings::instance();
            m_mw.m_spotCtl->autoStartSpotSources();
            // Auto-connect peripherals with manual IPs (#914)
            QString tgxlIp = cs.value("TGXL_ManualIp", "").toString();
            if (!tgxlIp.isEmpty() && !m_mw.m_tgxlConn.isConnected()) {
                quint16 tgxlPort = static_cast<quint16>(cs.value("TGXL_ManualPort", "9010").toInt());
                m_mw.m_tgxlConn.connectToTgxl(tgxlIp, tgxlPort);
            }
            QString pgxlIp = cs.value("PGXL_ManualIp", "").toString();
            if (!pgxlIp.isEmpty() && !m_mw.m_pgxlConn.isConnected()) {
                quint16 pgxlPort = static_cast<quint16>(cs.value("PGXL_ManualPort", "9008").toInt());
                m_mw.m_pgxlConn.connectToPgxl(pgxlIp, pgxlPort);
            }
            QString agIp = cs.value("AG_ManualIp", "").toString();
            if (!agIp.isEmpty() && !m_mw.m_antennaGenius.isConnected()) {
                quint16 agPort = static_cast<quint16>(cs.value("AG_ManualPort", "9007").toInt());
                m_mw.m_antennaGenius.connectToAddress(QHostAddress(agIp), agPort);
            }
        }
    } else {
        m_mw.m_spotCtl->stopSpotSources();
        m_mw.m_connStatusLabel->setText("Disconnected");
        m_mw.m_radioInfoLabel->setText("");
        m_mw.m_radioVersionLabel->setText("");
        m_mw.m_stationLabel->setText("N0CALL");
        m_mw.m_tnfIndicator->setStyleSheet("QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
        m_mw.m_panStack->bandStackPanel()->clear();
        m_mw.m_panStack->setBandStackVisible(false);
        m_mw.m_memoryCtl->refreshMemoryBrowsePanel();
        m_mw.m_statusCtl->updateBandStackIndicator();
        m_mw.m_tgxlIndicator->setVisible(false);
        m_mw.m_tgxlConn.disconnect();
        m_mw.m_pgxlConn.disconnect();
        m_mw.m_pgxlIndicator->setVisible(false);
        m_mw.m_txIndicator->setStyleSheet("QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 21px; }");
        m_mw.m_txIndicator->setText("TX");
        m_mw.m_connPanel->setStatusText("Not connected");
#if defined(HAVE_DAX_BRIDGE)
        m_mw.m_daxCtl->stopDax();
#endif
        m_mw.audioStopRx();
        m_mw.audioStopTx();

        // Clear spectrum/waterfall so the display doesn't look frozen
        for (auto* applet : m_mw.m_panStack->allApplets())
            applet->spectrumWidget()->clearDisplay();

        setPanadapterConnectionAnimation(!m_mw.m_userDisconnected, "Reconnecting to radio…");

        // Show reconnect dialog on unexpected disconnect (only one at a time)
        if (!m_mw.m_userDisconnected && !m_mw.m_reconnectDlg) {
            m_mw.m_reconnectDlg = new QDialog(&m_mw);
            m_mw.m_reconnectDlg->setWindowTitle("Radio Disconnected");
            m_mw.m_reconnectDlg->setModal(false);
            m_mw.m_reconnectDlg->setFixedSize(400, 150);
            m_mw.m_reconnectDlg->setStyleSheet(
                "QDialog { background: #0f0f1a; border: 2px solid #205070; }"
                "QLabel { color: #c8d8e8; font-size: 14px; }"
                "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
                "border-radius: 3px; color: #c8d8e8; font-size: 12px; "
                "font-weight: bold; padding: 6px 20px; }"
                "QPushButton:hover { background: #204060; }");
            auto* layout = new QVBoxLayout(m_mw.m_reconnectDlg);
            layout->setAlignment(Qt::AlignCenter);
            auto* label = new QLabel("Radio disconnected\nWaiting for reconnect...");
            label->setAlignment(Qt::AlignCenter);
            layout->addWidget(label);
            auto* dismissBtn = new QPushButton("Disconnect");
            dismissBtn->setFixedWidth(120);
            layout->addWidget(dismissBtn, 0, Qt::AlignCenter);
            connect(dismissBtn, &QPushButton::clicked, this, [this]() {
                m_mw.m_userDisconnected = true;
                setPanadapterConnectionAnimation(false);
                m_mw.m_reconnectDlg->close();
                m_mw.m_reconnectDlg->deleteLater();
                m_mw.m_reconnectDlg = nullptr;
                auto& s = AppSettings::instance();
                s.remove("LastConnectedRadioSerial");
                s.remove("LastRoutedRadioIp");
                s.save();
                m_mw.m_connPanel->show();
            });
            m_mw.m_reconnectDlg->show();
        }
    }
}

void ConnectionController::onConnectionError(const QString& msg)
{
    m_mw.m_connPanel->setStatusText("Error: " + msg);
    m_mw.m_connStatusLabel->setText("Error");
    m_mw.statusBar()->showMessage("Connection error: " + msg, 5000);
    if (!m_mw.m_reconnectDlg)
        setPanadapterConnectionAnimation(false);
}

void ConnectionController::setPanadapterConnectionAnimation(bool visible, const QString& label)
{
    const QString nextLabel = label.trimmed().isEmpty()
        ? QStringLiteral("Connecting to radio…")
        : label.trimmed();
    m_mw.m_panadapterConnectionAnimationVisible = visible;
    m_waitingForFirstPanadapterFrame = visible;
    m_mw.m_panadapterConnectionAnimationLabel = visible ? nextLabel : QString();

    if (!m_mw.m_panStack)
        return;

    for (auto* applet : m_mw.m_panStack->allApplets()) {
        if (!applet)
            continue;
        if (auto* spectrumWidget = applet->spectrumWidget())
            spectrumWidget->setConnectionAnimationVisible(visible, nextLabel);
    }
}

void ConnectionController::finishPanadapterConnectionAnimation()
{
    if (!m_waitingForFirstPanadapterFrame || !m_mw.m_panadapterConnectionAnimationVisible)
        return;

    setPanadapterConnectionAnimation(false);
}

} // namespace AetherSDR
