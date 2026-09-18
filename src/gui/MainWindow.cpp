#include "MainWindow.h"
#include "mainwindow/AudioDspController.h"
#include "mainwindow/AudioRoutingController.h"
#include "mainwindow/CatServersController.h"
#include "mainwindow/ConnectionController.h"
#include "mainwindow/DaxController.h"
#include "mainwindow/DigitalModesController.h"
#include "mainwindow/DigitalVoiceController.h"
#include "mainwindow/ExternalControlsController.h"
#include "mainwindow/MemoryController.h"
#include "mainwindow/MenuBuilder.h"
#include "mainwindow/PanadapterController.h"
#include "mainwindow/RadioAppletsController.h"
#include "mainwindow/StationToolsController.h"
#include "mainwindow/ShortcutsController.h"
#include "mainwindow/SliceController.h"
#include "mainwindow/SpotController.h"
#include "mainwindow/StatusBarController.h"
#include "ConnectionPanel.h"
#include "TitleBar.h"
#include "PanadapterApplet.h"
#include "PanadapterStack.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "SpectrumWidget.h"
#include "AppletPanel.h"
#include "RxApplet.h"
#include "PhoneApplet.h"
#include "AntennaGeniusApplet.h"
#include "RadioSetupDialog.h"
#include "NetworkDiagnosticsDialog.h"
#include "PropDashboardDialog.h"
#include "CwxPanel.h"
#include "DvkPanel.h"
#include "core/DvkWavTransfer.h"
#include "MultiFlexDialog.h"
#include "models/SliceModel.h"
#include "models/BandPlanManager.h"
#include "models/TransmitModel.h"
#include <algorithm>
#include <QApplication>
#include <QProcess>
#include <QTimer>
#include <QIcon>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QSplitter>
#include <QMenuBar>
#include <QDialog>
#include <QMenu>
#include <QAction>
#include <QCloseEvent>
#include <QMessageBox>
#include <QShortcut>
#include <QComboBox>
#include <QThread>
#include "core/AppSettings.h"
#include <QFontDatabase>
#include <QPushButton>

namespace AetherSDR {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // Feature controllers (src/gui/mainwindow/). Created before anything else
    // so every constructor step below can route through them; Qt parent
    // ownership (this window) controls their lifetime.
    m_connectionCtl = new ConnectionController(*this);
    m_spotCtl = new SpotController(*this);
    m_memoryCtl = new MemoryController(*this);
    m_digitalModesCtl = new DigitalModesController(*this);
    m_digitalVoiceCtl = new DigitalVoiceController(*this);
    m_daxCtl = new DaxController(*this);
    m_catCtl = new CatServersController(*this);
    m_extCtl = new ExternalControlsController(*this);
    m_panCtl = new PanadapterController(*this);
    m_sliceCtl = new SliceController(*this);
    m_menuBuilder = new MenuBuilder(*this);
    m_statusCtl = new StatusBarController(*this);
    m_shortcutCtl = new ShortcutsController(*this);
    m_dspCtl = new AudioDspController(*this);
    m_audioCtl = new AudioRoutingController(*this);
    m_appletsCtl = new RadioAppletsController(*this);
    m_toolsCtl = new StationToolsController(*this);

    setWindowTitle(QString("AetherSDR v%1").arg(QCoreApplication::applicationVersion()));
    setWindowIcon(QIcon(":/icon.png"));
    setMinimumSize(1024, 600);
    resize(1400, 800);

    // Apply frameless flag before first show() so the window is created
    // without chrome from the start — avoids the flash + re-create that
    // setWindowFlags() after show would cause (#framleess via View menu).
    if (AppSettings::instance().value("FramelessWindow", "False").toString() == "True")
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);

    applyDarkTheme();

    // Audio worker thread (#502) — AudioEngine runs on its own thread so
    // audio processing never competes with paintEvent for main thread CPU.
    m_audioThread = new QThread(this);
    m_audioThread->setObjectName("AudioEngine");
    m_audio = new AudioEngine;  // no parent — will be moved to thread
    m_audio->setRxBoost(
        AppSettings::instance().value("AudioBoost", "False").toString() == "True");
    m_audio->setRxBufferCapMs(
        AppSettings::instance().value("AudioBufferMs", "200").toInt());
    m_audio->moveToThread(m_audioThread);
    m_audioThread->start();

    // QSO audio recorder (#1297) — lives on main thread, audio feeds are thread-safe
    m_qsoRecorder = new QsoRecorder(this);
    connect(m_qsoRecorder, &QsoRecorder::playbackAudio,
            m_audio, &AudioEngine::feedDecodedSpeech);
    // During playback, block live RX audio from entering the buffer
    connect(m_qsoRecorder, &QsoRecorder::recordingError, this, [this](const QString& err) {
        statusBar()->showMessage(QStringLiteral("Recording failed: %1").arg(err), 5000);
    });
    // Recording filenames include the station callsign — take it from the radio.
    connect(&m_radioModel, &RadioModel::infoChanged, this, [this]() {
        m_qsoRecorder->setCallsign(m_radioModel.callsign());
    });
    connect(m_qsoRecorder, &QsoRecorder::muteRxRequested, this, [this](bool mute) {
        if (mute) {
            disconnect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
                       m_audio, &AudioEngine::feedAudioData);
        } else {
            connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
                    m_audio, &AudioEngine::feedAudioData);
        }
    });

    // PUDU TX monitor — captures post-PooDoo TX int16 audio, plays
    // back through the RX sink so the user can hear what their chain
    // is producing without keying the radio.  Registered with
    // AudioEngine so its tap hook picks it up on the audio thread.
    m_puduMonitor = new ClientPuduMonitor(this);
    m_audio->setTxPostDspMonitor(m_puduMonitor);
    // Monitor owns a dedicated QAudioSink in pull mode — no
    // feedDecodedSpeech routing, no timer pacing.  Keeps playback
    // glitch-free on macOS/Windows where QTimer jitter was starving
    // the shared RX sink.
    // Disconnect live RX audio while the monitor is recording or
    // playing so the user hears ONLY the captured PooDoo audio.
    // Mirrors QsoRecorder's muteRxRequested handling — merely setting
    // the sink volume to 0 would mute our playback too.
    connect(m_puduMonitor, &ClientPuduMonitor::muteRxRequested,
            this, [this](bool mute) {
        if (mute) {
            disconnect(m_radioModel.panStream(),
                       &PanadapterStream::audioDataReady,
                       m_audio, &AudioEngine::feedAudioData);
        } else {
            connect(m_radioModel.panStream(),
                    &PanadapterStream::audioDataReady,
                    m_audio, &AudioEngine::feedAudioData);
        }
    });

    // Band plan manager — must be created before buildMenuBar() which references it
    m_bandPlanMgr = new BandPlanManager(this);
    m_bandPlanMgr->loadPlans();

    m_menuBuilder->buildMenuBar();
    buildUI();
    m_shortcutCtl->registerShortcutActions();
    m_statusCtl->loadPaTempUnit();

    m_spotCtl->initDxccColoring();

    // Install event filter on the application to intercept Space PTT
    // before child widgets (buttons, combos) consume the key event.
    qApp->installEventFilter(this);

    // Ctrl+M toggle — must be a QShortcut on MainWindow, not a menu action,
    // because the menu bar is hidden in minimal mode.
    auto* minimalShortcut = new QShortcut(QKeySequence("Ctrl+M"), this);
    connect(minimalShortcut, &QShortcut::activated, this, [this]() {
        bool next = !m_minimalMode;
        // Sync the menu action (with blocker to avoid double-toggle)
        if (m_minimalModeAction) {
            QSignalBlocker b(m_minimalModeAction);
            m_minimalModeAction->setChecked(next);
        }
        toggleMinimalMode(next);
    });

    // Restore minimal mode if it was active on last exit
    if (AppSettings::instance().value("MinimalModeEnabled", "False").toString() == "True")
        toggleMinimalMode(true);

    m_connectionCtl->wireConnectionSignals();

    m_spotCtl->createSpotClients();

    // ── HF Propagation Forecast ────────────────────────────────────────────
    m_propForecast = new PropForecastClient(this);
    connect(m_propForecast, &PropForecastClient::forecastUpdated,
            this, [this](const PropForecast& fc) {
        for (PanadapterApplet* applet : m_panStack->allApplets()) {
            applet->spectrumWidget()->setPropForecast(fc.kIndex, fc.aIndex, fc.sfi);
        }
    });
    // Restore persisted setting — timer only arms if enabled
    if (AppSettings::instance().value("PropForecastEnabled", "False").toString() == "True") {
        m_propForecast->setEnabled(true);
    }

    m_spotCtl->wireSpotForwarding();

    // ── Wire up radio model ────────────────────────────────────────────────
    connect(&m_radioModel, &RadioModel::connectionStateChanged,
            m_connectionCtl, &ConnectionController::onConnectionStateChanged);
    connect(&m_radioModel, &RadioModel::connectionError,
            m_connectionCtl, &ConnectionController::onConnectionError);
    // CAT (rigctl) / TCI split requests
    connect(&m_radioModel, &RadioModel::splitRequested,
            this, [this](int rxSliceId, bool on, double txFreqMhz) {
        if (on) {
            m_sliceCtl->enableSplit(rxSliceId, txFreqMhz);
        } else {
            m_sliceCtl->disableSplit();
        }
    });
    connect(&m_radioModel, &RadioModel::sliceAdded,
            m_sliceCtl, &SliceController::onSliceAdded);
    connect(&m_radioModel, &RadioModel::sliceRemoved,
            m_sliceCtl, &SliceController::onSliceRemoved);
    connect(&m_radioModel, &RadioModel::memoryChanged,
            m_memoryCtl, &MemoryController::syncMemorySpot);
    connect(&m_radioModel, &RadioModel::memoryRemoved,
            m_memoryCtl, &MemoryController::removeMemorySpot);
    connect(&m_radioModel, &RadioModel::memoriesCleared,
            m_memoryCtl, &MemoryController::clearMemorySpotFeed);
    connect(&m_radioModel, &RadioModel::memoryChanged,
            this, [this](int) { m_memoryCtl->refreshMemoryBrowsePanel(); });
    connect(&m_radioModel, &RadioModel::memoryRemoved,
            this, [this](int) { m_memoryCtl->refreshMemoryBrowsePanel(); });
    connect(&m_radioModel, &RadioModel::memoriesCleared,
            this, [this]() { m_memoryCtl->refreshMemoryBrowsePanel(); });
    // Keep the MEM button target-slice badge in sync with slice topology
    // changes so the displayed letter always matches which slice a
    // save/recall will route to (#1781).  Active-slice changes are
    // handled inside setActiveSlice() itself since RadioModel has no
    // activeSliceChanged signal.
    connect(&m_radioModel, &RadioModel::sliceAdded,
            this, [this](SliceModel*) { m_memoryCtl->refreshMemoryBrowsePanel(); });
    connect(&m_radioModel, &RadioModel::sliceRemoved,
            this, [this](int) { m_memoryCtl->refreshMemoryBrowsePanel(); });
    connect(&m_radioModel, &RadioModel::panadapterLimitReached,
            this, [this](int limit, const QString& model) {
        statusBar()->showMessage(
            QString("%1 supports a maximum of %2 panadapters")
                .arg(model).arg(limit), 4000);
    });
    connect(&m_radioModel, &RadioModel::sliceCreateFailed,
            this, [this](int limit, const QString& model) {
        statusBar()->showMessage(
            QString("%1 supports a maximum of %2 slices across all connected clients")
                .arg(model).arg(limit), 4000);
    });
    connect(&m_radioModel.spotModel(), &SpotModel::spotsCleared,
            m_memoryCtl, &MemoryController::rebuildMemorySpotFeed);

    m_connectionCtl->wireReconnectProgress();

    m_audioCtl->wireTxAudioStreams();

    m_appletsCtl->wireTransmitState();

    m_panCtl->wireStreamsAndLifecycle();

    // ── Per-panadapter signal wiring (extracted for multi-pan support) ──────
    m_panCtl->wirePanadapter(m_panApplet);

    // Display overlay connections are now per-pan in wirePanadapter().

    m_audioCtl->wireRxAudioTaps();
    m_audioCtl->wireSliceRoutingTriggers();

    // ── BNR container autostart ─────────────────────────────────────────
#ifdef HAVE_BNR
    if (AppSettings::instance().value("BnrAutostart", "False").toString() == "True") {
        QString container = AppSettings::instance().value("BnrContainerName", "maxine-bnr").toString();
        qCDebug(lcGui) << "BNR: autostarting container" << container;
        QProcess::startDetached("docker", {"start", container});
    }
#endif

    // ── CW decoder: feed audio ──────────────────────────────────────────
    // Audio feed is global (same audio for all pans).
    // Text/stats output is routed to the pan owning the active slice
    // via routeCwDecoderOutput(), which re-wires on active slice change (#864).
    connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            &m_cwDecoder, &CwDecoder::feedAudio);

    m_sliceCtl->wireSliceApplets();

    m_dspCtl->wireNrButtonSync();

#ifdef HAVE_RADE
    connect(m_appletPanel->rxApplet(), &RxApplet::radeActivated,
            m_digitalVoiceCtl, &DigitalVoiceController::onRadeSelection);
#endif

    m_sliceCtl->wireStepSize();

    // ── Antenna list from radio → applet panel ─────────────────────────────
    connect(&m_radioModel, &RadioModel::antListChanged,
            m_appletPanel, &AppletPanel::setAntennaList);
    // Overlay-menu antenna wiring is now per-pan in wirePanadapter() (#1260).
    // Antenna list and S-meter are now wired per-widget in onSliceAdded.

    m_audioCtl->wireTitleBarAudio();

    m_appletsCtl->wireMetersAndAmplifiers();

    m_extCtl->createExternalControllers();

    m_appletsCtl->wireTxApplets();

    m_dspCtl->wireClientDspChain();

    // ── Antenna Genius applet: external 4O3A antenna switch ──────────────────
    m_appletPanel->agApplet()->setModel(&m_antennaGenius);
    connect(&m_antennaGenius, &AntennaGeniusModel::presenceChanged,
            m_appletPanel, &AppletPanel::setAgVisible);

    m_catCtl->createCatServers();
    m_toolsCtl->createRotator();
    m_toolsCtl->createCwSkimmer();
    m_toolsCtl->createLogbook();

#if defined(HAVE_DAX_BRIDGE)
    m_daxCtl->wireDaxApplet();
#endif

    m_statusCtl->wireTelemetry();

    m_connectionCtl->startDiscovery();

    // Restore saved geometry from XML settings
    auto& s = AppSettings::instance();
    const QString geomB64 = s.value("MainWindowGeometry").toString();
    if (!geomB64.isEmpty())
        restoreGeometry(QByteArray::fromBase64(geomB64.toLatin1()));
    const QString stateB64 = s.value("MainWindowState").toString();
    if (!stateB64.isEmpty())
        restoreState(QByteArray::fromBase64(stateB64.toLatin1()));
    // Clear stale splitter state — layout has changed across versions.
    s.remove("SplitterState");
    // Force 4-pane sizing: CWX=0, DVK=0 (hidden), center=stretch, applet=260px
    QTimer::singleShot(0, this, [this]() {
        m_splitter->setSizes({0, 0, width() - 260, 260});
    });

    // Auto-popup connection dialog if no saved radio
    QString lastSerial = s.value("LastConnectedRadioSerial", "").toString();
    if (lastSerial.isEmpty()) {
        QTimer::singleShot(500, this, [this]() { m_connectionCtl->toggleConnectionDialog(); });
    }

    // Restore the Memory dialog if it was open when the app last exited.
    QTimer::singleShot(0, this, [this]() {
        if (AppSettings::instance().value("MemoryDialogOpen", "False").toString() == "True")
            m_memoryCtl->showMemoryDialog();
    });

    // Track last-seen version (used by Help → What's New)
    {
        auto& settings = AppSettings::instance();
        QString current = QCoreApplication::applicationVersion();
        if (settings.value("LastSeenVersion").toString() != current) {
            settings.setValue("LastSeenVersion", current);
            settings.save();
        }
    }
}

MainWindow::~MainWindow()
{
#ifdef HAVE_DIGITAL_VOICE
    m_digitalVoiceCtl->shutdownDigitalVoice();
#endif
    // Stop audio processing on the worker thread before destruction (#502).
    // Use BlockingQueuedConnection to ensure completion before we proceed.
    if (m_audio && m_audioThread->isRunning()) {
        QMetaObject::invokeMethod(m_audio, [this]() {
            m_audio->setNr2Enabled(false);
            m_audio->setRn2Enabled(false);
            m_audio->setBnrEnabled(false);
            m_audio->stopRxStream();
            m_audio->stopTxStream();
        }, Qt::BlockingQueuedConnection);
    }
    // Destroy the engine on the audio thread as it exits (deferred deletes
    // run in QThread's finish), so its child timers are stopped by their
    // owning thread instead of warning "Timers cannot be stopped from
    // another thread" when deleted here.
    if (m_audio && m_audioThread->isRunning()) {
        m_audio->deleteLater();
        m_audio = nullptr;
    }
    m_audioThread->quit();
    m_audioThread->wait(3000);
    delete m_audio;   // only if the thread was not running
    m_audio = nullptr;

    m_extCtl->shutdownExternalControls();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_shuttingDown = true;

    m_digitalModesCtl->closeAllTools();
    if (m_radioModel.transmitModel().isMox()) {
        m_radioModel.setTransmit(false);
    }

    m_panStack->prepareShutdown();
    auto& s = AppSettings::instance();
    s.setValue("MainWindowGeometry", saveGeometry().toBase64());
    s.setValue("MainWindowState",   saveState().toBase64());

    // Close the applet-panel pop-out window if it's floating.  We
    // must do this explicitly because the window has parent=nullptr
    // (Qt::Window, top-level) so Qt won't auto-close it when the
    // main window exits.  m_shuttingDown gates the eventFilter so
    // AppletPanelFloating stays True for restart persistence.
    if (m_appletPanelFloatWindow) {
        m_appletPanelFloatWindow->close();
    }
    // SplitterState no longer saved (2-pane layout uses stretch factors)
    // ConnPanelCollapsed removed — panel is now a popup dialog

    s.setValue("MemoryDialogOpen",
        (m_memoryDialog && m_memoryDialog->isVisible()) ? "True" : "False");

    // Save active slice frequency/mode so the next empty-radio reconnect can
    // recreate a default slice at a sensible place (see RadioModel slice-list
    // handling). NOT used for panadapter centering — the radio is
    // authoritative for that (#1493).
    auto* sl = activeSlice();
    if (sl) {
        s.setValue("LastFrequency", QString::number(sl->frequency(), 'f', 6));
        s.setValue("LastMode", sl->mode());
    }

    // Save per-slice DAX channel assignments for restore on next launch.
    // Keyed by slice index (A=0, B=1, ...) since radio-assigned IDs change.
    {
        const QList<SliceModel*> slices = m_radioModel.slices();
        for (int i = 0; i < slices.size(); ++i) {
            const QString key = QString("DaxChannel_Slice%1").arg(QChar('A' + i));
            if (slices[i]->daxChannel() > 0) {
                s.setValue(key, QString::number(slices[i]->daxChannel()));
            } else {
                s.remove(key);
            }
        }
    }

    // DAX IQ channel is radio-authoritative — no client-side persistence needed.
    // The radio echoes daxiq_channel in pan status on reconnect.

    // Save client-side DSP state before destructor disables them
    s.setValue("ClientNr2Enabled", m_audio->nr2Enabled() ? "True" : "False");
    s.setValue("ClientRn2Enabled", m_audio->rn2Enabled() ? "True" : "False");
    s.setValue("ClientNr4Enabled", m_audio->nr4Enabled() ? "True" : "False");
    s.setValue("ClientDfnrEnabled", m_audio->dfnrEnabled() ? "True" : "False");
    s.setValue("ClientMnrEnabled", m_audio->mnrEnabled() ? "True" : "False");
    // BNR not persisted — requires manual enable each session
    // DEXP saved on-change in PhoneApplet — do NOT overwrite here, because
    // the radio may have reset DEXP to off (model reflects radio state, not
    // the user's preference).

    s.save();

    // Suppress reconnect dialog during shutdown (#527)
    m_userDisconnected = true;
    if (m_reconnectDlg) {
        m_reconnectDlg->close();
        delete m_reconnectDlg;
        m_reconnectDlg = nullptr;
    }

    m_audioCtl->restoreSliceRoutingMutes();
    m_toolsCtl->shutdownRecording();
    m_toolsCtl->shutdownCwSkimmer();
    m_discovery.stopListening();
    m_radioModel.disconnectFromRadio();
    audioStopRx();
    m_spotCtl->shutdownSpotClients();

    QMainWindow::closeEvent(event);
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::showNetworkDiagnosticsDialog()
{
    if (!m_networkDiagnosticsDialog) {
        auto* dlg = new NetworkDiagnosticsDialog(&m_radioModel, m_audio, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->setWindowModality(Qt::NonModal);
        m_networkDiagnosticsDialog = dlg;
    }

    m_networkDiagnosticsDialog->show();
    m_networkDiagnosticsDialog->raise();
    m_networkDiagnosticsDialog->activateWindow();
}

void MainWindow::showPropDashboard()
{
    if (!m_propDashboardDialog) {
        auto* dlg = new PropDashboardDialog(m_propForecast, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->setWindowModality(Qt::NonModal);
        m_propDashboardDialog = dlg;
    }

    m_propDashboardDialog->show();
    m_propDashboardDialog->raise();
    m_propDashboardDialog->activateWindow();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    // Applet-panel floating window — save geometry on move/resize, and
    // dock back on close so the menu action stays in sync.
    if (obj == m_appletPanelFloatWindow) {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            AppSettings::instance().setValue(
                "AppletPanelFloatGeometry",
                m_appletPanelFloatWindow->saveGeometry().toBase64());
        } else if (event->type() == QEvent::Close) {
            // Distinguish user-initiated close from app shutdown.
            // During shutdown, leave AppletPanelFloating=True so the
            // next launch re-opens the panel floating — the user
            // didn't "dock it back", the whole app is exiting.
            if (m_shuttingDown) {
                AppSettings::instance().setValue(
                    "AppletPanelFloatGeometry",
                    m_appletPanelFloatWindow->saveGeometry().toBase64());
                // Fall through — let Qt close the window normally.
            } else {
                // User clicked the X on the floating window.  Flip
                // the menu action; its toggled handler docks the
                // panel back and persists AppletPanelFloating=False.
                QTimer::singleShot(0, this, [this]() {
                    if (m_popOutSidebarAction)
                        m_popOutSidebarAction->setChecked(false);
                });
            }
        }
    }

    // Space PTT: intercept at application level so it works regardless of
    // which widget has focus (buttons, combos, etc. won't steal Space).
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Space && !ke->isAutoRepeat()
            && !ShortcutsController::isTextInputFocused()
            && m_radioModel.isConnected()) {
            if (m_keyboardShortcutsEnabled) {
                if (event->type() == QEvent::KeyPress && !m_spacePttActive) {
                    m_spacePttActive = true;
                    m_radioModel.setTransmit(true);
                } else if (event->type() == QEvent::KeyRelease && m_spacePttActive) {
                    m_spacePttActive = false;
                    m_radioModel.setTransmit(false);
                }
            }
            return true;  // always consume Space to prevent button activation
        }

        // Arrow keys on focused sliders: intercept here (before QShortcut processing)
        // so the slider's singleStep/pageStep movement isn't stolen by frequency-tune
        // shortcuts. The application event filter runs before Qt's shortcut map.
        if (event->type() == QEvent::KeyPress) {
            auto* slider = qobject_cast<QAbstractSlider*>(QApplication::focusWidget());
            if (slider) {
                int k = ke->key();
                if (k == Qt::Key_Left || k == Qt::Key_Right
                    || k == Qt::Key_Up || k == Qt::Key_Down) {
                    bool increase = (k == Qt::Key_Right || k == Qt::Key_Up);
                    int step = (ke->modifiers() & Qt::ControlModifier)
                                   ? slider->pageStep() : slider->singleStep();
                    slider->setValue(slider->value() + (increase ? step : -step));
                    return true;
                }
            }
        }
    }

    if (m_statusCtl->handleIndicatorEvent(obj, event)) {
        return true;
    }
    return QMainWindow::eventFilter(obj, event);
}

// ─── Audio thread helpers (#502) ─────────────────────────────────────────────
// These invoke AudioEngine methods on the audio worker thread.

void MainWindow::audioStartRx()
{
    QMetaObject::invokeMethod(m_audio, &AudioEngine::startRxStream);
}

void MainWindow::audioStopRx()
{
    QMetaObject::invokeMethod(m_audio, &AudioEngine::stopRxStream);
}

void MainWindow::audioStartTx(const QHostAddress& addr, quint16 port)
{
    QMetaObject::invokeMethod(m_audio, [this, addr, port]() {
        m_audio->startTxStream(addr, port);
    });
}

void MainWindow::audioStopTx()
{
    QMetaObject::invokeMethod(m_audio, &AudioEngine::stopTxStream);
}

// ─── UI Construction ──────────────────────────────────────────────────────────

void MainWindow::buildUI()
{
    // ── Title bar + central splitter ─────────────────────────────────────────
    m_titleBar = new TitleBar(this);
    // Embed the menu bar into the title bar (left side)
    m_titleBar->setMenuBar(menuBar());
    connect(m_titleBar, &TitleBar::multiFlexClicked, this, [this] {
        MultiFlexDialog dlg(&m_radioModel, this);
        dlg.exec();
    });
    connect(m_titleBar, &TitleBar::minimalModeRequested, this, [this]() {
        toggleMinimalMode(!m_minimalMode);
        if (m_minimalModeAction) {
            QSignalBlocker b(m_minimalModeAction);
            m_minimalModeAction->setChecked(m_minimalMode);
        }
    });

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(0);

    auto* central = new QWidget(this);
    auto* vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(m_titleBar);
    vbox->addWidget(m_splitter, 1);
    setCentralWidget(central);

    auto* splitter = m_splitter;

    // Connection panel — modeless dialog with standard decorations (#560, #574)
    m_connPanel = new ConnectionPanel(this);
    m_connPanel->setWindowFlags(Qt::Dialog);
    m_connPanel->setWindowTitle("Connect to Radio");
    m_connPanel->setMinimumSize(640, 580);
    m_connPanel->resize(760, 660);
    m_connPanel->hide();

    // CWX panel — left of spectrum, hidden by default
    m_cwxPanel = new CwxPanel(&m_radioModel.cwxModel(), splitter);
    // Provide state probes so CWX can guard its F1-F12 / ESC app-wide
    // shortcuts on mode + transmit state (#1552).
    m_cwxPanel->setActiveModeProvider([this]() {
        auto* s = activeSlice();
        return s ? s->mode() : QString();
    });
    m_cwxPanel->setTransmittingProvider([this]() {
        return m_radioModel.transmitModel().isTransmitting();
    });
    splitter->addWidget(m_cwxPanel);
    m_cwxPanel->hide();

    // DVK panel — left of spectrum, hidden by default (mutually exclusive with CWX)
    m_dvkPanel = new DvkPanel(&m_radioModel.dvkModel(), splitter);
    auto* dvkTransfer = new DvkWavTransfer(&m_radioModel, this);
    m_dvkPanel->setWavTransfer(dvkTransfer);
    splitter->addWidget(m_dvkPanel);
    m_dvkPanel->hide();

    // Centre — panadapter stack (one or more FFT + waterfall panes)
    m_panStack = new PanadapterStack(splitter);
    m_panApplet = nullptr;  // ensure setActivePanApplet sees a change
    m_panCtl->setActivePanApplet(m_panStack->addPanadapter("default"));
    splitter->addWidget(m_panStack);

    m_memoryCtl->wireBandStackPanel();
    m_memoryCtl->refreshMemoryBrowsePanel();

    m_panCtl->wireActivePanTracking();
    splitter->setStretchFactor(0, 0);  // CWX panel: fixed width
    splitter->setStretchFactor(1, 0);  // DVK panel: fixed width
    splitter->setStretchFactor(2, 1);  // PanStack: stretch
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);

    // Right — applet panel (includes S-Meter)
    m_appletPanel = new AppletPanel(splitter);
    splitter->addWidget(m_appletPanel);
    splitter->setStretchFactor(3, 0);
    splitter->setCollapsible(3, false);

    // Set initial splitter sizes: CWX=0, DVK=0 (both hidden), center=stretch, right=310
    const int centerWidth = qMax(400, width() - 310);
    splitter->setSizes({0, 0, centerWidth, 310});

    // Restore applet panel visibility
    if (AppSettings::instance().value("AppletPanelVisible", "True").toString() != "True")
        m_appletPanel->hide();

    m_statusCtl->buildStatusBar();
}

// ─── Theme ────────────────────────────────────────────────────────────────────

void MainWindow::applyDarkTheme()
{
    // Only name fonts that are actually installed: an unknown family (e.g.
    // "Inter" on a stock Mac) makes Qt populate every font alias at startup
    // (~90 ms, logged as a warning) before falling back to the system font.
    QStringList families;
    const QStringList installed = QFontDatabase::families();
    for (const QString& f : {QStringLiteral("Inter"), QStringLiteral("Segoe UI")}) {
        if (installed.contains(f)) {
            families << QStringLiteral("\"%1\"").arg(f);
        }
    }
    // The platform UI font rather than the generic "sans-serif", which Qt
    // also treats as an unknown family and resolves via the slow alias scan.
    families << QStringLiteral("\"%1\"").arg(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
    QString sheet = QStringLiteral(R"(
        QWidget {
            background-color: #0f0f1a;
            color: #c8d8e8;
            font-family: @FONT_FAMILY@;
            font-size: 13px;
        }
        QGroupBox {
            border: 1px solid #203040;
            border-radius: 4px;
            margin-top: 8px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 8px;
            color: #00b4d8;
        }
        QPushButton {
            background-color: #1a2a3a;
            border: 1px solid #203040;
            border-radius: 4px;
            padding: 4px 10px;
            color: #c8d8e8;
        }
        QPushButton:hover  { background-color: #203040; }
        QPushButton:pressed { background-color: #00b4d8; color: #000; }
        QComboBox {
            background-color: #1a2a3a;
            border: 1px solid #203040;
            border-radius: 4px;
            padding: 3px 6px;
        }
        QComboBox::drop-down { border: none; }
        QListWidget {
            background-color: #111120;
            border: 1px solid #203040;
            alternate-background-color: #161626;
        }
        QListWidget::item:selected { background-color: #00b4d8; color: #000; }
        QSlider::groove:horizontal {
            height: 4px;
            background: #203040;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 14px; height: 14px;
            margin: -5px 0;
            background: #00b4d8;
            border-radius: 7px;
        }
        QMenuBar { background-color: #0a0a14; }
        QMenuBar::item:selected { background-color: #1a2a3a; }
        QMenu { background-color: #111120; border: 1px solid #203040; }
        QMenu::item:selected { background-color: #00b4d8; color: #000; }
        QStatusBar { background-color: #0a0a14; border-top: 1px solid #203040; }
        QProgressBar {
            background-color: #111120;
            border: 1px solid #203040;
            border-radius: 3px;
        }
        QSplitter::handle { background-color: #203040; width: 2px; }
    )");
    sheet.replace(QStringLiteral("@FONT_FAMILY@"), families.join(QStringLiteral(", ")));
    setStyleSheet(sheet);
}

// ─── Radio/model event handlers ───────────────────────────────────────────────

SliceModel* MainWindow::activeSlice() const
{
    if (m_activeSliceId < 0) return nullptr;
    return m_radioModel.slice(m_activeSliceId);
}

// Single entry point for Radio Setup so every menu/button that opens it gets
// the apply-on-close wiring (serial/FlexControl reload, CW decode overlay,
// audio compression stream rebuild). Previously FlexControl…, USB Cables…
// and the overlay XVTR button opened bare dialogs whose changes never applied.
void MainWindow::showRadioSetupDialog(const QString& tabName)
{
    if (m_radioSetupDialog) {
        if (!tabName.isEmpty()) {
            if (auto* existing = qobject_cast<RadioSetupDialog*>(m_radioSetupDialog.data())) {
                existing->selectTab(tabName);
            }
        }
        m_radioSetupDialog->raise();
        m_radioSetupDialog->activateWindow();
        return;
    }
    // Snapshot compression setting before dialog opens
    QString prevComp = m_radioModel.audioCompressionParam();

    auto* dlg = new RadioSetupDialog(&m_radioModel, m_audio, &m_tgxlConn, &m_pgxlConn, &m_antennaGenius, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_radioSetupDialog = dlg;
    connect(dlg, &RadioSetupDialog::txBandSettingsRequested,
            m_txBandAction, &QAction::trigger);
    connect(dlg, &QDialog::finished, this, [this, prevComp]() {
#ifdef HAVE_SERIALPORT
        m_extCtl->reloadAfterRadioSetup();
#endif
        // Re-evaluate CW decode overlay visibility
        bool decodeOn = AppSettings::instance().value("CwDecodeOverlay", "True").toString() == "True";
        auto* s = activeSlice();
        if (s) {
            bool isCw = (s->mode() == "CW" || s->mode() == "CWL");
            if (m_cwDecoderApplet) m_cwDecoderApplet->setCwPanelVisible(isCw && decodeOn);
        }
        if (m_modeCwDecodePanelAction) {
            QSignalBlocker b(m_modeCwDecodePanelAction);
            m_modeCwDecodePanelAction->setChecked(decodeOn);
        }

        // If audio compression changed, recreate the RX audio stream
        QString newComp = m_radioModel.audioCompressionParam();
        if (newComp != prevComp && m_radioModel.isConnected()) {
            qCDebug(lcGui) << "MainWindow: audio compression changed from" << prevComp
                     << "to" << newComp << "— recreating audio stream";
            m_radioModel.removeRxAudioStream();
            QTimer::singleShot(500, this, [this]() {
                m_radioModel.createRxAudioStream();
            });
            m_dspCtl->updateNr2Availability();  // Disable NR2 if switching to Opus (#1597)
        }
    });
    if (!tabName.isEmpty()) {
        dlg->selectTab(tabName);
    }
    dlg->show();
}

SpectrumWidget* MainWindow::spectrum() const
{
    return m_panStack ? m_panStack->activeSpectrum()
                      : (m_panApplet ? m_panApplet->spectrumWidget() : nullptr);
}

// ── UI Scale helpers ────────────────────────────────────────────────────
static constexpr int kScaleSteps[] = {75, 85, 100, 110, 125, 150, 175, 200};
static constexpr int kScaleStepCount = sizeof(kScaleSteps) / sizeof(kScaleSteps[0]);

void MainWindow::applyUiScale(int pct)
{
    int current = AppSettings::instance().value("UiScalePercent", "100").toInt();
    if (pct == current)
        return;

    AppSettings::instance().setValue("UiScalePercent", QString::number(pct));
    AppSettings::instance().save();

    auto answer = QMessageBox::question(this, "UI Scale",
        QString("UI scale changed to %1%. Restart AetherSDR now to apply?").arg(pct),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QCoreApplication::arguments().mid(1));
        QCoreApplication::quit();
    }
}

void MainWindow::stepUiScale(int direction)
{
    int current = AppSettings::instance().value("UiScalePercent", "100").toInt();
    // Find nearest step in the requested direction
    int best = current;
    if (direction > 0) {
        for (int i = 0; i < kScaleStepCount; ++i) {
            if (kScaleSteps[i] > current) { best = kScaleSteps[i]; break; }
        }
    } else {
        for (int i = kScaleStepCount - 1; i >= 0; --i) {
            if (kScaleSteps[i] < current) { best = kScaleSteps[i]; break; }
        }
    }
    if (best != current)
        applyUiScale(best);
}

void MainWindow::setFramelessWindow(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue("FramelessWindow", on ? "True" : "False");
    s.save();

    // setWindowFlags() re-creates the native window — save and restore
    // geometry so the window stays where the user put it.
    const QRect geom = geometry();
    const bool wasVisible = isVisible();
    Qt::WindowFlags flags = windowFlags();
    if (on)
        flags |= Qt::FramelessWindowHint;
    else
        flags &= ~Qt::FramelessWindowHint;
    setWindowFlags(flags);
    setGeometry(geom);
    if (wasVisible)
        show();
}

void MainWindow::toggleMinimalMode(bool on)
{
    m_minimalMode = on;
    auto& s = AppSettings::instance();

    if (on) {
        // Save full-mode geometry
        s.setValue("FullModeGeometry", saveGeometry().toBase64());

        // Save splitter sizes for restore
        s.setValue("MinimalModeSplitterSizes",
            QString::fromLatin1(m_splitter->saveState().toBase64()));

        // Suspend spectrum rendering to save CPU
        if (m_panStack) {
            for (auto* a : m_panStack->allApplets())
                a->spectrumWidget()->setUpdatesEnabled(false);
        }

        // Hide the splitter (contains spectrum + applet panel) and reparent
        // the applet panel directly into the central layout
        m_splitter->hide();
        m_appletPanel->setParent(centralWidget());
        centralWidget()->layout()->addWidget(m_appletPanel);
        m_appletPanel->show();

        // Strip title bar to heartbeat + logo + restore/feature buttons
        m_titleBar->setMinimalMode(true);
        statusBar()->hide();

        // Force window to applet width
        setMinimumSize(0, 0);
        setFixedWidth(260);

        QByteArray geom = QByteArray::fromBase64(
            s.value("MinimalModeGeometry", "").toByteArray());
        if (!geom.isEmpty())
            restoreGeometry(geom);

    } else {
        // Save minimal geometry
        s.setValue("MinimalModeGeometry", saveGeometry().toBase64());

        // Reparent applet panel back into the splitter and restore layout
        m_splitter->addWidget(m_appletPanel);
        m_appletPanel->show();
        QByteArray splitterState = QByteArray::fromBase64(
            s.value("MinimalModeSplitterSizes", "").toByteArray());
        if (!splitterState.isEmpty())
            m_splitter->restoreState(splitterState);
        m_splitter->show();

        // Resume spectrum rendering
        if (m_panStack) {
            for (auto* a : m_panStack->allApplets())
                a->spectrumWidget()->setUpdatesEnabled(true);
        }

        // Release fixed width and restore minimum size
        setFixedWidth(QWIDGETSIZE_MAX);
        setMinimumSize(1024, 600);

        // Restore title bar and status bar
        m_titleBar->setMinimalMode(false);
        statusBar()->show();

        // Restore full geometry
        QByteArray geom = QByteArray::fromBase64(
            s.value("FullModeGeometry", "").toByteArray());
        if (!geom.isEmpty())
            restoreGeometry(geom);
    }

    s.setValue("MinimalModeEnabled", on ? "True" : "False");
    s.save();
}

SpectrumWidget* MainWindow::spectrumForSlice(SliceModel* s) const
{
    if (s && m_panStack) {
        auto* sw = m_panStack->spectrum(s->panId());
        if (sw) return sw;
    }
    return spectrum();  // fallback to active pan
}

// ─── Pan layout application ───────────────────────────────────────────────────

// ─── Keyboard Shortcuts ───────────────────────────────────────────────────────

// StreamDeck native integration removed — use TCI StreamController plugin instead.

// ─── Applet-panel pop-out (#1713 Phase 6) ───────────────────────────────────
//
// The whole AppletPanel widget (tray buttons + S-Meter + scrollable stack)
// can live either inside m_splitter at the end of the row, or as the sole
// content of its own top-level window.  Reparenting transfers Qt ownership
// cleanly; the splitter pane collapses to zero width when the panel floats,
// and restores to its remembered width when it docks back.

void MainWindow::floatAppletPanel()
{
    if (!m_appletPanel || !m_splitter) return;
    if (m_appletPanelFloatWindow) return;  // already floating

    m_appletPanelFloatWindow = new QWidget(nullptr, Qt::Window);
    m_appletPanelFloatWindow->setWindowTitle("AetherSDR — Applet Panel");
    m_appletPanelFloatWindow->setAttribute(Qt::WA_DeleteOnClose, false);
    auto* layout = new QVBoxLayout(m_appletPanelFloatWindow);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_appletPanel->setParent(m_appletPanelFloatWindow);
    layout->addWidget(m_appletPanel);
    m_appletPanel->show();
    // Qt auto-redistributes remaining splitter slots once the panel
    // is reparented out; we don't need to touch the sizes manually.

    // Restore last-known window geometry or default.
    const QByteArray geom = QByteArray::fromBase64(
        AppSettings::instance()
            .value("AppletPanelFloatGeometry", "").toByteArray());
    if (!geom.isEmpty()) {
        m_appletPanelFloatWindow->restoreGeometry(geom);
    } else {
        m_appletPanelFloatWindow->resize(320, 720);
    }

    // Track geometry changes live so the saved position/size stays
    // current without a per-tick timer.
    m_appletPanelFloatWindow->installEventFilter(this);

    // On close → dock back (unchecks the menu action via its own
    // toggled handler, which re-enters dockAppletPanel).  Using
    // QObject::connect with a lambda so we don't need to subclass.
    connect(m_appletPanelFloatWindow, &QObject::destroyed,
            this, [this]() {
        m_appletPanelFloatWindow = nullptr;
    });
    m_appletPanelFloatWindow->show();
}

void MainWindow::dockAppletPanel()
{
    if (!m_appletPanel || !m_splitter) return;
    if (!m_appletPanelFloatWindow) return;  // already docked

    // Save geometry before tearing the window down.
    AppSettings::instance().setValue(
        "AppletPanelFloatGeometry",
        m_appletPanelFloatWindow->saveGeometry().toBase64());

    // Reparent back to the splitter.  addWidget appends, which is
    // the correct position (last slot) matching the pre-float layout.
    m_appletPanel->setParent(m_splitter);
    m_splitter->addWidget(m_appletPanel);
    m_appletPanel->show();

    // Re-apply the canonical 4-slot layout the app uses at startup:
    // CWX=0, DVK=0, center=stretch, applet=260px.  Using fixed sizes
    // instead of saveState()/restoreState() because the saved state
    // is unreliable in the launch-with-float case — saveState() fires
    // at a QTimer::singleShot(0) turn before the splitter has fully
    // laid out, producing captured sizes that don't match the
    // post-show window width.  The splitter isn't user-draggable for
    // applet width anyway (startup always forces 260), so recomputing
    // is both simpler and deterministic.
    const int centerWidth = std::max(400, m_splitter->width() - 260);
    m_splitter->setSizes({0, 0, centerWidth, 260});

    m_appletPanelFloatWindow->removeEventFilter(this);
    m_appletPanelFloatWindow->deleteLater();
    m_appletPanelFloatWindow = nullptr;
}

void MainWindow::disconnectFromWindow(QObject* sender)
{
    // Before the controller split every one of these connections used the
    // window as receiver/context, so sender->disconnect(this) dropped them all.
    sender->disconnect(this);
    sender->disconnect(m_connectionCtl);
    sender->disconnect(m_spotCtl);
    sender->disconnect(m_memoryCtl);
    sender->disconnect(m_digitalModesCtl);
    sender->disconnect(m_digitalVoiceCtl);
    sender->disconnect(m_daxCtl);
    sender->disconnect(m_catCtl);
    sender->disconnect(m_extCtl);
    sender->disconnect(m_panCtl);
    sender->disconnect(m_sliceCtl);
    sender->disconnect(m_menuBuilder);
    sender->disconnect(m_statusCtl);
    sender->disconnect(m_shortcutCtl);
    sender->disconnect(m_dspCtl);
    sender->disconnect(m_audioCtl);
    sender->disconnect(m_appletsCtl);
    sender->disconnect(m_toolsCtl);
}

} // namespace AetherSDR
