#include "gui/mainwindow/MenuBuilder.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/AudioDspController.h"
#include "gui/mainwindow/CatServersController.h"
#include "gui/mainwindow/AudioRoutingController.h"
#include "gui/mainwindow/StationToolsController.h"
#include "gui/mainwindow/ConnectionController.h"
#include "gui/mainwindow/DaxController.h"
#include "gui/mainwindow/DigitalModesController.h"
#include "gui/mainwindow/ExternalControlsController.h"
#include "gui/mainwindow/MemoryController.h"
#include "gui/mainwindow/ShortcutsController.h"
#include "gui/mainwindow/SpotController.h"
#include "gui/TitleBar.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "gui/SpectrumWidget.h"
#include "gui/AppletPanel.h"
#include "gui/ProfileManagerDialog.h"
#include "gui/ProfileImportExportDialog.h"
#include "gui/SupportDialog.h"
#include "gui/SliceTroubleshootingDialog.h"
#include "gui/ShortcutDialog.h"
#include "gui/MultiFlexDialog.h"
#include "gui/HelpDialog.h"
#include "gui/WhatsNewDialog.h"
#include "models/BandPlanManager.h"
#include "models/TransmitModel.h"
#include <algorithm>
#include <QApplication>
#include <QTimer>
#include <QPixmap>
#include <QWidgetAction>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QMenuBar>
#include <QDialog>
#include <QGridLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QAction>
#include <QActionGroup>
#include <QLabel>
#include <QScrollArea>
#include <QFrame>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include "core/AppSettings.h"
#include <QPushButton>

namespace AetherSDR {

MenuBuilder::MenuBuilder(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void MenuBuilder::buildMenuBar()
{
    buildFileMenu();

    buildSettingsMenu();

    m_mw.m_digitalModesCtl->buildModeMenu();

    buildProfilesMenu();

    buildViewMenu();

    buildLogMenu();

    buildHelpMenu();
}

void MenuBuilder::buildLogMenu()
{
    auto* logMenu = m_mw.menuBar()->addMenu("&Log");
    auto* logbookAction = logMenu->addAction("Logbook...");
    logbookAction->setMenuRole(QAction::NoRole);
    connect(logbookAction, &QAction::triggered, this, [this] { m_mw.m_toolsCtl->showLogbook(); });
    auto* quickAction = logMenu->addAction("Quick Log Entry");
    connect(quickAction, &QAction::triggered, this, [this] { m_mw.m_toolsCtl->focusLogQuickEntry(); });
}

void MenuBuilder::buildFileMenu()
{
    auto* fileMenu = m_mw.menuBar()->addMenu("&File");
    auto* quitAct = fileMenu->addAction("&Quit");
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);
}

void MenuBuilder::buildSettingsMenu()
{
    // ── Settings menu ──────────────────────────────────────────────────────
    auto* settingsMenu = m_mw.menuBar()->addMenu("&Settings");

    auto* radioSetup = settingsMenu->addAction("Radio Setup...");
    radioSetup->setMenuRole(QAction::PreferencesRole);  // macOS: appears in app menu as Preferences (#883, #1013)
    connect(radioSetup, &QAction::triggered, this, [this] {
        m_mw.showRadioSetupDialog();
    });

    auto* chooseRadio = settingsMenu->addAction("Connect to Radio...");
    chooseRadio->setMenuRole(QAction::NoRole);      // prevent macOS auto-reparenting (#883)
    connect(chooseRadio, &QAction::triggered, this, [this] {
        m_mw.m_connectionCtl->toggleConnectionDialog();
    });

#ifdef HAVE_SERIALPORT
    auto* flexControlAction = settingsMenu->addAction("FlexControl...");
    connect(flexControlAction, &QAction::triggered, this, [this] {
        m_mw.showRadioSetupDialog(QStringLiteral("Serial"));
    });
#endif
    auto* networkAction = settingsMenu->addAction("Network...");
    connect(networkAction, &QAction::triggered, this, [this] {
        m_mw.showNetworkDiagnosticsDialog();
    });
    auto* memoryAction = settingsMenu->addAction("Memory...");
    connect(memoryAction, &QAction::triggered, this, [this] {
        m_mw.m_memoryCtl->showMemoryDialog();
    });
    auto* scannerAction = settingsMenu->addAction("Scanner...");
    connect(scannerAction, &QAction::triggered, this, [this] {
        m_mw.m_memoryCtl->showScannerPanel();
    });
    auto* recordingsAction = settingsMenu->addAction("Recordings (Waterfall / IQ)...");
    connect(recordingsAction, &QAction::triggered, this, [this] {
        m_mw.m_toolsCtl->showRecordingPanel();
    });
    auto* skimmerAction = settingsMenu->addAction("CW Skimmer...");
    connect(skimmerAction, &QAction::triggered, this, [this] {
        m_mw.m_toolsCtl->showCwSkimmerPanel();
    });
    auto* usbCablesAction = settingsMenu->addAction("USB Cables...");
    connect(usbCablesAction, &QAction::triggered, this, [this] {
        m_mw.showRadioSetupDialog(QStringLiteral("USB Cables"));
    });
#ifdef HAVE_MIDI
    auto* midiAction = settingsMenu->addAction("MIDI Mapping...");
    connect(midiAction, &QAction::triggered, this, [this] {
        m_mw.m_extCtl->showMidiMappingDialog();
    });
#endif
#ifdef HAVE_HIDAPI
#endif
    auto* spotsAction = settingsMenu->addAction("SpotHub...");
    connect(spotsAction, &QAction::triggered, this, [this] {
        m_mw.m_spotCtl->showSpotHubDialog();
    });
    auto* wsjtxAction = settingsMenu->addAction("WSJT-X / JTDX Link...");
    connect(wsjtxAction, &QAction::triggered, this, [this] {
        m_mw.m_spotCtl->showWsjtxPanel();
    });
    auto* multiFlexAction = settingsMenu->addAction("multiFLEX...");
    auto openMultiFlex = [this] {
        MultiFlexDialog dlg(&m_mw.m_radioModel, &m_mw);
        dlg.exec();
    };
    connect(multiFlexAction, &QAction::triggered, this, openMultiFlex);
    // m_titleBar connect deferred — see after TitleBar creation (~line 2530)
    m_mw.m_txBandAction = settingsMenu->addAction("TX Band Settings...");
    m_mw.m_txBandAction->setMenuRole(QAction::NoRole);   // prevent macOS auto-reparenting (#883)
    auto* txBandAct = m_mw.m_txBandAction;
    connect(txBandAct, &QAction::triggered, this, [this] {
        if (!m_mw.m_radioModel.isConnected()) {
            m_mw.statusBar()->showMessage("Not connected to radio", 3000);
            return;
        }
        auto* dlg = new QDialog(&m_mw);
        dlg->setWindowTitle(QString("TX Band Settings (Current TX Profile: %1)")
            .arg(m_mw.m_radioModel.transmitModel().activeProfile()));
        dlg->setMinimumSize(700, 450);
        dlg->setStyleSheet("QDialog { background: #0f0f1a; }");
        dlg->setAttribute(Qt::WA_DeleteOnClose);

        auto* vb = new QVBoxLayout(dlg);
        auto* gridContainer = new QWidget;
        gridContainer->setStyleSheet("background: #506070;");
        auto* headerGrid = new QGridLayout(gridContainer);
        headerGrid->setContentsMargins(1, 1, 1, 1);
        headerGrid->setSpacing(1);
        const QStringList headers = {"Band", "RF PWR(%)", "Tune PWR(%)", "PTT Inhibit",
                                      "ACC TX", "RCA TX Req", "ACC TX Req",
                                      "RCA TX1", "RCA TX2", "RCA TX3", "HWALC"};
        for (int c = 0; c < headers.size(); ++c) {
            auto* lbl = new QLabel(headers[c]);
            lbl->setStyleSheet("QLabel { color: #8aa8c0; font-size: 10px; "
                                "font-weight: bold; background: #1a2a3a; "
                                "padding: 2px 4px; }");
            lbl->setAlignment(Qt::AlignCenter);
            headerGrid->addWidget(lbl, 0, c);
        }

        const auto& bands = m_mw.m_radioModel.txBandSettings();
        QList<int> sortedIds = bands.keys();
        std::sort(sortedIds.begin(), sortedIds.end());

        static const QString kEditStyle =
            "QLineEdit { background: #0a0a18; color: #c8d8e8; border: 1px solid #304050; "
            "padding: 2px; font-size: 11px; }";
        static const QString kCbStyle =
            "QCheckBox { spacing: 0px; }"
            "QCheckBox::indicator { width: 14px; height: 14px; }";

        int row = 1;
        for (int id : sortedIds) {
            const auto& b = bands[id];
            int col = 0;

            auto* nameLbl = new QLabel(b.bandName);
            nameLbl->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; "
                                    "font-weight: bold; background: #0f0f1a; "
                                    "border: 1px solid #203040; padding: 2px 4px; }");
            headerGrid->addWidget(nameLbl, row, col++);

            auto* rfEdit = new QLineEdit(QString::number(b.rfPower));
            rfEdit->setStyleSheet(kEditStyle);
            rfEdit->setFixedWidth(50);
            rfEdit->setAlignment(Qt::AlignCenter);
            int bandId = id;
            connect(rfEdit, &QLineEdit::editingFinished, dlg, [this, rfEdit, bandId] {
                m_mw.m_radioModel.sendCommand(
                    QString("transmit bandset %1 rfpower=%2").arg(bandId).arg(rfEdit->text()));
            });
            headerGrid->addWidget(rfEdit, row, col++);

            auto* tuneEdit = new QLineEdit(QString::number(b.tunePower));
            tuneEdit->setStyleSheet(kEditStyle);
            tuneEdit->setFixedWidth(50);
            tuneEdit->setAlignment(Qt::AlignCenter);
            connect(tuneEdit, &QLineEdit::editingFinished, dlg, [this, tuneEdit, bandId] {
                m_mw.m_radioModel.sendCommand(
                    QString("transmit bandset %1 tunepower=%2").arg(bandId).arg(tuneEdit->text()));
            });
            headerGrid->addWidget(tuneEdit, row, col++);

            struct CbDef { bool val; const char* txCmd; const char* ilCmd; };
            CbDef cbs[] = {
                {b.inhibit, "inhibit", nullptr},
                {b.accTx,   nullptr, "acc_tx_enabled"},
                {b.rcaTxReq,nullptr, "rca_txreq_enable"},
                {b.accTxReq,nullptr, "acc_txreq_enable"},
                {b.tx1,     nullptr, "tx1_enabled"},
                {b.tx2,     nullptr, "tx2_enabled"},
                {b.tx3,     nullptr, "tx3_enabled"},
                {b.hwAlc,   "hwalc_enabled", nullptr},
            };

            for (const auto& cb : cbs) {
                auto* chk = new QCheckBox;
                chk->setChecked(cb.val);
                chk->setStyleSheet(kCbStyle);
                auto* w = new QWidget;
                w->setStyleSheet("background: #0f0f1a;");
                auto* hb = new QHBoxLayout(w);
                hb->setContentsMargins(0, 0, 0, 0);
                hb->setAlignment(Qt::AlignCenter);
                hb->addWidget(chk);
                const char* txC = cb.txCmd;
                const char* ilC = cb.ilCmd;
                connect(chk, &QCheckBox::toggled, dlg, [this, bandId, txC, ilC](bool on) {
                    if (txC)
                        m_mw.m_radioModel.sendCommand(
                            QString("transmit bandset %1 %2=%3").arg(bandId).arg(txC).arg(on ? 1 : 0));
                    if (ilC)
                        m_mw.m_radioModel.sendCommand(
                            QString("interlock bandset %1 %2=%3").arg(bandId).arg(ilC).arg(on ? 1 : 0));
                });
                headerGrid->addWidget(w, row, col++);
            }
            ++row;
        }

        vb->addWidget(gridContainer);
        vb->addStretch();
        dlg->show();
    });

    // Inhibit during TUNE submenu — user selects which TX outputs to suppress.
    // Uses QWidgetAction with QCheckBox so the menu stays open for multi-select.
    auto* tuneInhibitMenu = settingsMenu->addMenu("Inhibit during TUNE");

    auto& settings = AppSettings::instance();
    struct InhibitDef { const char* label; const char* key; };
    static const InhibitDef inhibitDefs[] = {
        {"None",   "TuneInhibitNone"},
        {"ACC TX", "TuneInhibitAccTx"},
        {"TX1",    "TuneInhibitTx1"},
        {"TX2",    "TuneInhibitTx2"},
        {"TX3",    "TuneInhibitTx3"},
    };

    QCheckBox* noneCb = nullptr;
    QVector<QCheckBox*> outputCbs;

    for (const auto& def : inhibitDefs) {
        auto* cb = new QCheckBox(def.label);
        cb->setStyleSheet(
            "QCheckBox { color: #c8d8e8; padding: 4px 12px; }"
            "QCheckBox::indicator { width: 14px; height: 14px; }"
            "QCheckBox::indicator:unchecked { border: 1px solid #506070; background: #1a2a3a; border-radius: 2px; }"
            "QCheckBox::indicator:checked { border: 1px solid #00b4d8; background: #00b4d8; border-radius: 2px; }");
        bool on = settings.value(def.key, "False").toString() == "True";
        cb->setChecked(on);

        auto* wa = new QWidgetAction(tuneInhibitMenu);
        wa->setDefaultWidget(cb);
        tuneInhibitMenu->addAction(wa);

        if (QString(def.label) == "None")
            noneCb = cb;
        else
            outputCbs.append(cb);
    }

    // Migrate old TuneInhibitAmp → TuneInhibitAccTx
    if (settings.value("TuneInhibitAmp", "").toString() == "True"
        && settings.value("TuneInhibitAccTx", "").toString().isEmpty()) {
        settings.setValue("TuneInhibitAccTx", "True");
        settings.setValue("TuneInhibitNone", "False");
        outputCbs[0]->setChecked(true);  // ACC TX
        if (noneCb) noneCb->setChecked(false);
        settings.save();
    }

    // If no outputs selected, check None
    bool anyOutput = false;
    for (auto* cb : outputCbs) anyOutput |= cb->isChecked();
    if (noneCb && !anyOutput) noneCb->setChecked(true);

    auto syncNone = [noneCb, outputCbs]() {
        bool anyOn = false;
        for (auto* cb : outputCbs) anyOn |= cb->isChecked();
        if (noneCb) {
            QSignalBlocker b(noneCb);
            noneCb->setChecked(!anyOn);
        }
    };

    // "None" unchecks all outputs
    connect(noneCb, &QCheckBox::toggled, this, [noneCb, outputCbs, &settings](bool on) {
        if (on) {
            for (auto* cb : outputCbs) {
                QSignalBlocker b(cb);
                cb->setChecked(false);
            }
            settings.setValue("TuneInhibitAccTx", "False");
            settings.setValue("TuneInhibitTx1", "False");
            settings.setValue("TuneInhibitTx2", "False");
            settings.setValue("TuneInhibitTx3", "False");
            settings.setValue("TuneInhibitNone", "True");
            settings.save();
        } else {
            QSignalBlocker b(noneCb);
            bool anyOn = false;
            for (auto* cb : outputCbs) anyOn |= cb->isChecked();
            if (!anyOn) noneCb->setChecked(true);
        }
    });

    // Each output toggle saves and syncs None
    for (int i = 0; i < outputCbs.size(); ++i) {
        connect(outputCbs[i], &QCheckBox::toggled, this,
                [i, syncNone, &settings](bool on) {
            static const char* keys[] = {"TuneInhibitAccTx", "TuneInhibitTx1",
                                         "TuneInhibitTx2", "TuneInhibitTx3"};
            settings.setValue(keys[i], on ? "True" : "False");
            if (on)
                settings.setValue("TuneInhibitNone", "False");
            syncNone();
            settings.save();
        });
    }

    auto* dspAction = settingsMenu->addAction("AetherDSP Settings...");
    dspAction->setMenuRole(QAction::NoRole);        // prevent macOS auto-reparenting (#883)
    connect(dspAction, &QAction::triggered, this, [this] {
        m_mw.m_dspCtl->showAetherDspSettings();
    });

    settingsMenu->addSeparator();

    auto* autoRigctlAction = settingsMenu->addAction("Autostart rigctld with AetherSDR");
    autoRigctlAction->setCheckable(true);
    autoRigctlAction->setChecked(
        AppSettings::instance().value("AutoStartRigctld", "False").toString() == "True");
    connect(autoRigctlAction, &QAction::toggled, this, [this](bool on) {
        m_mw.m_catCtl->setRigctldAutoStart(on);
    });

#ifdef _WIN32
    // CAT virtual serial ports require macOS or Linux. Force the setting
    // off so a saved True (e.g. from a Linux settings import) can't
    // activate anything, and omit the menu entry entirely (#1556).
    {
        auto& s = AppSettings::instance();
        if (s.value("AutoStartCAT", "False").toString() != "False") {
            s.setValue("AutoStartCAT", "False");
            s.save();
        }
    }
#else
    auto* autoCatAction = settingsMenu->addAction("Autostart CAT with AetherSDR");
    autoCatAction->setCheckable(true);
    autoCatAction->setChecked(
        AppSettings::instance().value("AutoStartCAT", "False").toString() == "True");
    connect(autoCatAction, &QAction::toggled, this, [this](bool on) {
        m_mw.m_catCtl->setCatPtyAutoStart(on);
    });
#endif

    auto* kcatTcpAction = settingsMenu->addAction("SmartSDR CAT (Kenwood/ZZ) TCP ports 5002+");
    kcatTcpAction->setCheckable(true);
    kcatTcpAction->setChecked(
        AppSettings::instance().value("CatKenwoodEnabled", "False").toString() == "True");
    connect(kcatTcpAction, &QAction::toggled, this, [this](bool on) {
        m_mw.m_catCtl->setKenwoodCatTcpEnabled(on);
    });
#ifndef _WIN32
    auto* kcatPtyAction = settingsMenu->addAction("SmartSDR CAT (Kenwood/ZZ) serial ports");
    kcatPtyAction->setCheckable(true);
    kcatPtyAction->setChecked(
        AppSettings::instance().value("CatKenwoodPtyEnabled", "False").toString() == "True");
    connect(kcatPtyAction, &QAction::toggled, this, [this](bool on) {
        m_mw.m_catCtl->setKenwoodCatPtyEnabled(on);
    });
#endif

    auto* autoTciAction = settingsMenu->addAction("Autostart TCI with AetherSDR");
    autoTciAction->setCheckable(true);
    autoTciAction->setChecked(
        AppSettings::instance().value("AutoStartTCI", "False").toString() == "True");
    connect(autoTciAction, &QAction::toggled, this, [this](bool on) {
        m_mw.m_catCtl->setTciAutoStart(on);
    });

#if !defined(HAVE_DAX_BRIDGE)
    // DAX audio bridge requires macOS CoreAudio or Linux with PipeWire.
    // Force off and omit the menu entry on platforms without a bridge (#1556).
    {
        auto& s = AppSettings::instance();
        if (s.value("AutoStartDAX", "False").toString() != "False") {
            s.setValue("AutoStartDAX", "False");
            s.save();
        }
    }
#else
    auto* autoDaxAction = settingsMenu->addAction("Autostart DAX with AetherSDR");
    autoDaxAction->setCheckable(true);
    autoDaxAction->setChecked(
        AppSettings::instance().value("AutoStartDAX", "False").toString() == "True");
    connect(autoDaxAction, &QAction::toggled, this, [this](bool on) {
        m_mw.m_daxCtl->setDaxAutoStart(on);
    });
    auto* sliceRoutingAction = settingsMenu->addAction("Slice Audio Routing…");
    connect(sliceRoutingAction, &QAction::triggered, this, [this] {
        m_mw.m_audioCtl->showSliceAudioRoutingDialog();
    });
    auto* daxDevicesAction = settingsMenu->addAction("DAX Audio Devices…");
    connect(daxDevicesAction, &QAction::triggered, this, [this] {
        m_mw.m_daxCtl->showDaxDeviceMappingDialog();
    });
#endif

    // "Low-Latency DAX (FreeDV)" menu retired in v0.8.19 — the toggle
    // it used to drive is now applied automatically by RADE mode, since
    // RADE was the only consumer that ever actually wanted that route.
    // See AudioEngine::setRadeMode().
}

void MenuBuilder::buildProfilesMenu()
{
    // ── Profiles menu ──────────────────────────────────────────────────────
    m_profilesMenu = m_mw.menuBar()->addMenu("&Profiles");
    auto* profileMgrAct = m_profilesMenu->addAction("Profile Manager...");
    connect(profileMgrAct, &QAction::triggered, this, [this] {
        ProfileManagerDialog dlg(&m_mw.m_radioModel, &m_mw);
        dlg.exec();
    });
    auto* profileImportExportAct = m_profilesMenu->addAction("Import/Export Profiles...");
    connect(profileImportExportAct, &QAction::triggered, this, [this] {
        auto* dlg = new ProfileImportExportDialog(&m_mw.m_radioModel, &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(true);
        dlg->show();
    });
    m_profilesMenu->addSeparator();

    // Global profile list (populated on connect)
    connect(&m_mw.m_radioModel, &RadioModel::globalProfilesChanged, this, [this] {
        // Remove old profile actions (after the separator)
        const auto actions = m_profilesMenu->actions();
        for (int i = 3; i < actions.size(); ++i)  // skip Manager, Import/Export, separator
            m_profilesMenu->removeAction(actions[i]);

        // Add current global profiles
        const auto profiles = m_mw.m_radioModel.globalProfiles();
        const auto active = m_mw.m_radioModel.activeGlobalProfile();
        for (const auto& name : profiles) {
            auto* act = m_profilesMenu->addAction(name);
            act->setCheckable(true);
            act->setChecked(name == active);
            connect(act, &QAction::triggered, this, [this, name] {
                m_mw.m_radioModel.loadGlobalProfile(name);
            });
        }
    });
}

void MenuBuilder::buildViewMenu()
{
    auto* viewMenu = m_mw.menuBar()->addMenu("&View");

    m_mw.m_panelVisAction = viewMenu->addAction("Applet Panel");
    m_mw.m_panelVisAction->setCheckable(true);
    m_mw.m_panelVisAction->setChecked(
        AppSettings::instance().value("AppletPanelVisible", "True").toString() == "True");
    connect(m_mw.m_panelVisAction, &QAction::toggled, this, [this](bool on) {
        m_mw.m_appletPanel->setVisible(on);
        m_mw.m_panelToggle->setStyleSheet(on
            ? "QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 22px; }"
            : "QLabel { color: #404858; font-weight: bold; font-size: 22px; }");
        AppSettings::instance().setValue("AppletPanelVisible", on ? "True" : "False");
        AppSettings::instance().save();
    });

    // Pop out the whole applet panel into its own window (#1713 Phase 6).
    // Toggles between panel-docked (inside the QSplitter) and floating
    // (separate Qt::Window).  Ctrl+Shift+S is the keyboard shortcut.
    m_mw.m_popOutSidebarAction = viewMenu->addAction("Pop Out Applet Panel");
    m_mw.m_popOutSidebarAction->setCheckable(true);
    m_mw.m_popOutSidebarAction->setShortcut(QKeySequence("Ctrl+Shift+S"));
    m_mw.m_popOutSidebarAction->setChecked(
        AppSettings::instance().value(
            "AppletPanelFloating", "False").toString() == "True");
    connect(m_mw.m_popOutSidebarAction, &QAction::toggled, this, [this](bool floating) {
        if (floating) m_mw.floatAppletPanel();
        else          m_mw.dockAppletPanel();
        AppSettings::instance().setValue(
            "AppletPanelFloating", floating ? "True" : "False");
        AppSettings::instance().save();
    });
    if (m_mw.m_popOutSidebarAction->isChecked()) {
        QTimer::singleShot(0, this, [this]() { m_mw.floatAppletPanel(); });
    }

    viewMenu->addSeparator();

    // Band Plan submenu — Off / Small / Medium / Large / Huge
    auto* bandPlanMenu = viewMenu->addMenu("Band Plan");
    int savedBpSize = AppSettings::instance().value("BandPlanFontSize", "").toInt();
    if (savedBpSize == 0 && AppSettings::instance().value("ShowBandPlan", "True").toString() == "True")
        savedBpSize = 6;  // migrate old boolean setting
    auto* bpGroup = new QActionGroup(bandPlanMenu);
    struct BpOption { const char* label; int pt; };
    for (auto [label, pt] : {BpOption{"Off", 0}, {"Small", 6}, {"Medium", 10}, {"Large", 12}, {"Huge", 16}}) {
        auto* act = bandPlanMenu->addAction(label);
        act->setCheckable(true);
        act->setChecked(pt == savedBpSize);
        bpGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, pt] {
            for (auto* a : m_mw.m_panStack->allApplets())
                a->spectrumWidget()->setBandPlanFontSize(pt);
            AppSettings::instance().setValue("BandPlanFontSize", QString::number(pt));
            AppSettings::instance().save();
        });
    }

    // Band plan region selector (#425)
    bandPlanMenu->addSeparator();
    auto* planGroup = new QActionGroup(bandPlanMenu);
    const QString activePlan = m_mw.m_bandPlanMgr->activePlanName();
    for (const auto& name : m_mw.m_bandPlanMgr->availablePlans()) {
        auto* act = bandPlanMenu->addAction(name);
        act->setCheckable(true);
        act->setChecked(name == activePlan);
        planGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, name] {
            m_mw.m_bandPlanMgr->setActivePlan(name);
        });
    }

    auto* singleClickTuneAct = viewMenu->addAction("Single-Click to Tune");
    singleClickTuneAct->setCheckable(true);
    singleClickTuneAct->setChecked(
        AppSettings::instance().value("SingleClickTune", "False").toString() == "True");
    connect(singleClickTuneAct, &QAction::toggled, this, [this](bool on) {
        for (auto* a : m_mw.m_panStack->allApplets())
            a->spectrumWidget()->setSingleClickTune(on);
        AppSettings::instance().setValue("SingleClickTune", on ? "True" : "False");
        AppSettings::instance().save();
    });

    auto* panFollowVfoAct = viewMenu->addAction("Pan Follows VFO");
    panFollowVfoAct->setCheckable(true);
    panFollowVfoAct->setChecked(
        AppSettings::instance().value("PanFollowVfo", "True").toString() == "True");
    connect(panFollowVfoAct, &QAction::toggled, this, [](bool on) {
        AppSettings::instance().setValue("PanFollowVfo", on ? "True" : "False");
        AppSettings::instance().save();
    });

    // UI Scale submenu — sets QT_SCALE_FACTOR, applies on restart
    auto* scaleMenu = viewMenu->addMenu("UI Scale");
    int savedScale = AppSettings::instance().value("UiScalePercent", "100").toInt();
    auto* scaleGroup = new QActionGroup(scaleMenu);
    for (int pct : {75, 85, 100, 110, 125, 150, 175, 200}) {
        auto* act = scaleMenu->addAction(QString("%1%").arg(pct));
        act->setCheckable(true);
        act->setChecked(pct == savedScale);
        scaleGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, pct] {
            m_mw.applyUiScale(pct);
        });
    }
    scaleMenu->addSeparator();
    auto* zoomInAct = scaleMenu->addAction("Zoom In");
    zoomInAct->setShortcut(QKeySequence("Ctrl+="));
    connect(zoomInAct, &QAction::triggered, this, [this] { m_mw.stepUiScale(+1); });
    auto* zoomOutAct = scaleMenu->addAction("Zoom Out");
    zoomOutAct->setShortcut(QKeySequence("Ctrl+-"));
    connect(zoomOutAct, &QAction::triggered, this, [this] { m_mw.stepUiScale(-1); });
    auto* zoomResetAct = scaleMenu->addAction("Reset (100%)");
    zoomResetAct->setShortcut(QKeySequence("Ctrl+0"));
    connect(zoomResetAct, &QAction::triggered, this, [this] { m_mw.applyUiScale(100); });

    auto* resetOrderAct = viewMenu->addAction("Reset Applet Order");
    connect(resetOrderAct, &QAction::triggered, this, [this] {
        m_mw.m_appletPanel->resetOrder();
    });

    viewMenu->addSeparator();
    m_mw.m_minimalModeAction = viewMenu->addAction("Minimal Mode");
    m_mw.m_minimalModeAction->setCheckable(true);
    m_mw.m_minimalModeAction->setShortcut(QKeySequence("Ctrl+M"));
    m_mw.m_minimalModeAction->setChecked(
        AppSettings::instance().value("MinimalModeEnabled", "False").toString() == "True");
    connect(m_mw.m_minimalModeAction, &QAction::toggled, this, [this](bool on) {
        m_mw.toggleMinimalMode(on);
    });

    auto* framelessAct = viewMenu->addAction("Frameless Window");
    framelessAct->setCheckable(true);
    framelessAct->setShortcut(QKeySequence("Ctrl+Shift+F"));
    framelessAct->setToolTip(
        "Hide the OS title bar to gain screen space.\n"
        "When enabled, move/close via keyboard shortcuts or taskbar.");
    framelessAct->setChecked(
        AppSettings::instance().value("FramelessWindow", "False").toString() == "True");
    connect(framelessAct, &QAction::toggled, this, [this](bool on) {
        m_mw.setFramelessWindow(on);
    });

    auto* propForecastAct = viewMenu->addAction("Propagation Conditions");
    propForecastAct->setCheckable(true);
    propForecastAct->setChecked(
        AppSettings::instance().value("PropForecastEnabled", "False").toString() == "True");
    connect(propForecastAct, &QAction::toggled, this, [this](bool on) {
        AppSettings::instance().setValue("PropForecastEnabled", on ? "True" : "False");
        AppSettings::instance().save();
        // Enable/disable the client (timer only runs when on)
        m_mw.m_propForecast->setEnabled(on);
        // Show/hide the overlay on all panadapters immediately
        for (PanadapterApplet* applet : m_mw.m_panStack->allApplets()) {
            applet->spectrumWidget()->setPropForecastVisible(on);
        }
        // If turning off, clear the stale values so they don't reappear
        if (!on) {
            for (PanadapterApplet* applet : m_mw.m_panStack->allApplets()) {
                applet->spectrumWidget()->setPropForecast(-1, -1, -1);
            }
        }
    });

    m_mw.m_keyboardShortcutsEnabled = AppSettings::instance()
        .value("KeyboardShortcutsEnabled", "False").toString() == "True";
    auto* kbAct = viewMenu->addAction("Keyboard Shortcuts");
    kbAct->setCheckable(true);
    kbAct->setChecked(m_mw.m_keyboardShortcutsEnabled);
    connect(kbAct, &QAction::toggled, this, [this](bool on) {
        m_mw.m_keyboardShortcutsEnabled = on;
        ShortcutsController::setShortcutsGuardEnabled(on);
        AppSettings::instance().setValue("KeyboardShortcutsEnabled", on ? "True" : "False");
        AppSettings::instance().save();
    });
    auto* configShortcutsAct = viewMenu->addAction("Configure Shortcuts...");
    configShortcutsAct->setMenuRole(QAction::NoRole); // prevent macOS auto-reparenting (#883)
    connect(configShortcutsAct, &QAction::triggered, this, [this] {
        ShortcutDialog dlg(&m_mw.m_shortcutManager, &m_mw);
        dlg.exec();
        // Rebuild shortcuts in case bindings changed
        m_mw.m_shortcutManager.rebuildShortcuts(&m_mw, ShortcutsController::shortcutGuard);
    });

    viewMenu->addSeparator();
    auto* heartbeatBlinkAct = viewMenu->addAction("Blink Status Indicator");
    heartbeatBlinkAct->setCheckable(true);
    heartbeatBlinkAct->setChecked(
        AppSettings::instance().value("HeartbeatBlinkEnabled", "True").toString() == "True");
    connect(heartbeatBlinkAct, &QAction::toggled, this, [this](bool on) {
        if (m_mw.m_titleBar) m_mw.m_titleBar->setBlinkEnabled(on);
    });
    // Keep the menu item in sync when the right-click on the indicator changes the setting
    if (m_mw.m_titleBar) {
        connect(m_mw.m_titleBar, &TitleBar::blinkEnabledChanged,
                heartbeatBlinkAct, &QAction::setChecked);
    }
}

void MenuBuilder::buildHelpMenu()
{
    auto* helpMenu = m_mw.menuBar()->addMenu("&Help");
    helpMenu->addAction("Getting Started...", this, [this]() {
        auto* dlg = new HelpDialog("Getting Started", ":/help/getting-started.md", &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    helpMenu->addAction("AetherSDR Help...", this, [this]() {
        auto* dlg = new HelpDialog("AetherSDR Help", ":/help/aethersdr-help.md", &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    helpMenu->addAction("Understanding Noise Cancellation...", this, [this]() {
        auto* dlg = new HelpDialog("Understanding Noise Cancellation", ":/help/understanding-noise-cancellation.md", &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    auto* controlsHelpAction = helpMenu->addAction("Configuring AetherSDR Controls...", this, [this]() {
        auto* dlg = new HelpDialog("Configuring AetherSDR Controls", ":/help/configuring-aethersdr-controls.md", &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    controlsHelpAction->setMenuRole(QAction::NoRole); // prevent macOS auto-reparenting (#883)
    auto* dataModesAction = helpMenu->addAction("Configuring Data Modes...", this, [this]() {
        auto* dlg = new HelpDialog("Configuring Data Modes", ":/help/understanding-data-modes.md", &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    dataModesAction->setMenuRole(QAction::NoRole); // prevent macOS auto-reparenting (#883)
    helpMenu->addAction("Contributing to AetherSDR...", this, [this]() {
        auto* dlg = new HelpDialog("Contributing to AetherSDR", ":/help/contributing-to-aethersdr.md", &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
    helpMenu->addSeparator();
    helpMenu->addAction("Support...", this, [this]() {
        auto* dlg = new SupportDialog(&m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setRadioModel(&m_mw.m_radioModel);
        dlg->show();
        dlg->raise();
    });
    helpMenu->addAction("Slice Troubleshooting...", this, [this]() {
        SliceTroubleshootingDialog dlg(&m_mw.m_radioModel, m_mw.m_audio, &m_mw);
        dlg.exec();
    });
    helpMenu->addAction("What's New...", this, [this]() {
        if (m_whatsNewDialog) {
            m_whatsNewDialog->raise();
            m_whatsNewDialog->activateWindow();
            return;
        }
        m_whatsNewDialog = WhatsNewDialog::showAll(&m_mw);
    });
    helpMenu->addSeparator();
    helpMenu->addAction("About AetherSDR", this, [this]{
        auto* dlg = new QDialog(&m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle("About AetherSDR");
        dlg->setFixedWidth(380);
        dlg->setStyleSheet("QDialog { background: #0f0f1a; }");

        auto* vbox = new QVBoxLayout(dlg);
        vbox->setSpacing(8);
        vbox->setContentsMargins(16, 16, 16, 16);

        // Icon
        auto* iconLbl = new QLabel;
        iconLbl->setPixmap(QPixmap(":/icon.png").scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        iconLbl->setAlignment(Qt::AlignCenter);
        vbox->addWidget(iconLbl);

        // Header
        auto* header = new QLabel(QString(
            "<div style='text-align:center;'>"
            "<h2 style='margin-bottom:2px; color:#c8d8e8;'>AetherSDR</h2>"
            "<p style='margin-top:0; color:#8aa8c0;'>v%1</p>"
            "<p style='color:#c8d8e8;'>Linux-native SmartSDR-compatible client<br>"
            "for FlexRadio transceivers.</p>"
            "<p style='font-size:11px; color:#6a8090;'>"
            "Built with Qt %2 &middot; C++20<br>"
            "Compiled: %3</p>"
            "</div>")
            .arg(QCoreApplication::applicationVersion(), qVersion(),
                 QStringLiteral(__DATE__)));
        header->setAlignment(Qt::AlignCenter);
        header->setWordWrap(true);
        vbox->addWidget(header);

        // Separator
        auto* sep1 = new QFrame;
        sep1->setFrameShape(QFrame::HLine);
        sep1->setStyleSheet("color: #304050;");
        vbox->addWidget(sep1);

        // Contributors label
        auto* contribTitle = new QLabel("<b style='color:#c8d8e8;'>Contributors</b>");
        contribTitle->setAlignment(Qt::AlignCenter);
        vbox->addWidget(contribTitle);

        // Scrollable contributors list
        auto* contribLabel = new QLabel("Jeremy (KK7GWY)<br>Claude &middot; Anthropic<br>rfoust<br>Ian (M7HNF)<br>VE3NEM<br>jensenpat<br>Dependabot");
        contribLabel->setAlignment(Qt::AlignCenter);
        contribLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");
        contribLabel->setWordWrap(true);

        auto* scroll = new QScrollArea;
        scroll->setWidget(contribLabel);
        scroll->setWidgetResizable(true);
        scroll->setFixedHeight(80);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(
            "QScrollArea { background: #0a0a14; border: 1px solid #203040; border-radius: 4px; }"
            "QScrollBar:vertical { background: #0a0a14; width: 6px; }"
            "QScrollBar::handle:vertical { background: #304050; border-radius: 3px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
        vbox->addWidget(scroll);

        // Separator
        auto* sep2 = new QFrame;
        sep2->setFrameShape(QFrame::HLine);
        sep2->setStyleSheet("color: #304050;");
        vbox->addWidget(sep2);

        // Footer
        auto* footer = new QLabel(
            "<div style='text-align:center;'>"
            "<p style='font-size:11px; color:#8aa8c0;'>"
            "&copy; 2026 AetherSDR Contributors<br>"
            "Licensed under "
            "<a href='https://www.gnu.org/licenses/gpl-3.0.html' style='color:#00b4d8;'>GPLv3</a></p>"
            "<p style='font-size:11px;'>"
            "<a href='https://github.com/ten9876/AetherSDR' style='color:#00b4d8;'>"
            "github.com/ten9876/AetherSDR</a></p>"
            "<p style='font-size:10px; color:#6a8090;'>"
            "SmartSDR protocol &copy; FlexRadio Systems</p>"
            "<p style='font-size:10px; color:#6a8090;'>"
            "HF propagation forecasts provided by "
            "<a href='https://www.hamqsl.com/' style='color:#8aa8c0;'>hamqsl.com</a></p>"
            "</div>");
        footer->setAlignment(Qt::AlignCenter);
        footer->setOpenExternalLinks(true);
        footer->setWordWrap(true);
        vbox->addWidget(footer);

        // OK button
        auto* okBtn = new QPushButton("OK");
        okBtn->setStyleSheet(
            "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
            "border-radius: 4px; padding: 6px 24px; }"
            "QPushButton:hover { background: #00c8f0; }");
        connect(okBtn, &QPushButton::clicked, dlg, &QDialog::close);
        vbox->addWidget(okBtn, 0, Qt::AlignCenter);

        dlg->show();

        // Fetch live contributor list from GitHub API
        auto* nam = new QNetworkAccessManager(dlg);
        auto* reply = nam->get(QNetworkRequest(
            QUrl("https://api.github.com/repos/ten9876/AetherSDR/contributors")));
        connect(reply, &QNetworkReply::finished, dlg, [contribLabel, reply] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;
            auto doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isArray()) return;
            QStringList names;
            names << "Jeremy (KK7GWY)" << "Claude &middot; Anthropic";
            for (const auto& val : doc.array()) {
                auto obj = val.toObject();
                QString login = obj.value("login").toString();
                if (login.isEmpty() || login == "ten9876") continue;
                if (login.contains("[bot]"))
                    login = login.replace("[bot]", "");
                if (!names.contains(login))
                    names << login;
            }
            contribLabel->setText(names.join("<br>"));
        });
    });
}

} // namespace AetherSDR
