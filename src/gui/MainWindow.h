#pragma once

#include "models/RadioModel.h"
#include "models/BandSettings.h"
#include "models/AntennaGeniusModel.h"
#include "core/RadioDiscovery.h"
#include "core/AudioEngine.h"
#include "core/RigctlServer.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif
#include "core/RigctlPty.h"
#include "core/SmartLinkClient.h"
#include "core/WanConnection.h"
#include "core/CwDecoder.h"
#include "core/QsoRecorder.h"
#include "core/ClientPuduMonitor.h"
#include "core/DigitalModeReporting.h"
#include "core/DxClusterClient.h"
#ifdef HAVE_MQTT
#include "core/MqttClient.h"
#endif
#include "core/SpotCollectorClient.h"
#include "core/PotaClient.h"
#include "core/PropForecastClient.h"
#ifdef HAVE_WEBSOCKETS
#include "core/FreeDvClient.h"
#endif
#include "core/DigitalVoiceEngine.h"
#include <QPointer>
#include <QThread>
#ifdef HAVE_SERIALPORT
#include "core/SerialPortController.h"
#include "core/FlexControlManager.h"
#endif
#ifdef HAVE_MIDI
#include "core/MidiControlManager.h"
#endif
#ifdef HAVE_HIDAPI
#include "core/HidEncoderManager.h"
#endif
#include "core/ShortcutManager.h"
#include "core/TgxlConnection.h"
#include "core/PgxlConnection.h"
#include "core/DxccColorProvider.h"

#include <QMainWindow>
#include <QSplitter>
#include <QPointer>
#include <QLabel>
#include <QMenu>
#include <QStatusBar>

QT_BEGIN_NAMESPACE
class QActionGroup;
QT_END_NAMESPACE

namespace AetherSDR {

class ConnectionPanel;
class TitleBar;
class SpectrumWidget;
class PanadapterApplet;
class PanadapterStack;
class AppletPanel;
class BandPlanManager;
class WhatsNewDialog;
class CwxPanel;
class DvkPanel;
class DxClusterDialog;
class CommandPalette;
class HfFaxDialog;
class PskDialog;
class RttyDialog;
class SstvDialog;
class WeakSignalDialog;
class WeakSignalNativeDecoder;
class WwvDialog;
class Ale2gDialog;
class DigitalVoiceDialog;
class VirtualAudioBridge;
using DaxBridge = VirtualAudioBridge;   // native or device-mapped backend
class VfoWidget;
class AetherDspDialog;
class AudioDspController;
class AudioRoutingController;
class CatServersController;
class ConnectionController;
class DaxController;
class DigitalModesController;
class DigitalVoiceController;
class ExternalControlsController;
class MemoryController;
class MenuBuilder;
class PanadapterController;
class RadioAppletsController;
class ShortcutsController;
class SliceController;
class SpotController;
class StatusBarController;
class StationToolsController;

// Wheel mode for FlexControl: determines what the encoder knob adjusts
enum class FlexWheelMode { Frequency, Volume, Power };

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    // Feature controllers (src/gui/mainwindow/) are QObject children of this
    // window; they operate on the shared window state below.
    friend class ConnectionController;
    friend class SpotController;
    friend class MemoryController;
    friend class DigitalModesController;
    friend class DigitalVoiceController;
    friend class DaxController;
    friend class CatServersController;
    friend class ExternalControlsController;
    friend class PanadapterController;
    friend class SliceController;
    friend class MenuBuilder;
    friend class StatusBarController;
    friend class ShortcutsController;
    friend class AudioDspController;
    friend class AudioRoutingController;
    friend class RadioAppletsController;
    friend class StationToolsController;

    ConnectionController*       m_connectionCtl{nullptr};
    SpotController*             m_spotCtl{nullptr};
    MemoryController*           m_memoryCtl{nullptr};
    DigitalModesController*     m_digitalModesCtl{nullptr};
    DigitalVoiceController*     m_digitalVoiceCtl{nullptr};
    DaxController*              m_daxCtl{nullptr};
    CatServersController*       m_catCtl{nullptr};
    ExternalControlsController* m_extCtl{nullptr};
    PanadapterController*       m_panCtl{nullptr};
    SliceController*            m_sliceCtl{nullptr};
    MenuBuilder*                m_menuBuilder{nullptr};
    StatusBarController*        m_statusCtl{nullptr};
    ShortcutsController*        m_shortcutCtl{nullptr};
    AudioDspController*         m_dspCtl{nullptr};
    AudioRoutingController*     m_audioCtl{nullptr};
    RadioAppletsController*     m_appletsCtl{nullptr};
    StationToolsController*     m_toolsCtl{nullptr};

    // Drop every connection from sender whose receiver/context is this window
    // or one of its controllers (the pre-split sender->disconnect(this)).
    void disconnectFromWindow(QObject* sender);

    void buildUI();
    void applyDarkTheme();

    // Audio thread helpers — invoke AudioEngine methods on the worker thread (#502)
    void audioStartRx();
    void audioStopRx();
    void audioStartTx(const QHostAddress& addr, quint16 port);
    void audioStopTx();
    SliceModel* activeSlice() const;
    SpectrumWidget* spectrum() const;
    SpectrumWidget* spectrumForSlice(SliceModel* s) const;
    void applyUiScale(int pct);
    void stepUiScale(int direction);  // +1 = zoom in, -1 = zoom out
    void toggleMinimalMode(bool on);
    // Toggle OS window-chrome on/off (Qt::FramelessWindowHint).  Persists
    // to AppSettings("FramelessWindow"). When on, users move/close the
    // window via keyboard shortcuts or taskbar.
    void setFramelessWindow(bool on);
    void showNetworkDiagnosticsDialog();
    void showRadioSetupDialog(const QString& tabName = QString());
    void showPropDashboard();

    // Core objects
    RadioDiscovery    m_discovery;
    RadioModel        m_radioModel;
    DxccColorProvider m_dxccProvider;
    AudioEngine*      m_audio{nullptr};
    QThread*          m_audioThread{nullptr};
    QsoRecorder*      m_qsoRecorder{nullptr};
    ClientPuduMonitor* m_puduMonitor{nullptr};
    BandSettings      m_bandSettings;
    SmartLinkClient   m_smartLink;
    WanConnection     m_wanConnection;
    AntennaGeniusModel m_antennaGenius;
    TgxlConnection    m_tgxlConn;        // direct TCP 9010 to TGXL for manual relay control
    PgxlConnection    m_pgxlConn;        // direct TCP 9008 to PGXL for telemetry
    BandPlanManager*  m_bandPlanMgr{nullptr};
    CwDecoder         m_cwDecoder;
    WeakSignalNativeDecoder* m_weakSignalDecoder{nullptr};
    PropForecastClient*  m_propForecast{nullptr};
#ifdef HAVE_WEBSOCKETS
    FreeDvClient*      m_freedvClient{nullptr};
#endif

    // GUI — left sidebar
    ConnectionPanel* m_connPanel{nullptr};

    // GUI — main area
    TitleBar*         m_titleBar{nullptr};
    QSplitter*        m_splitter{nullptr};
    PanadapterStack*  m_panStack{nullptr};
    QPointer<PanadapterApplet> m_panApplet;  // backward compat alias to active applet
    QPointer<PanadapterApplet> m_cwDecoderApplet;  // applet receiving CW decoder output

    // GUI — right applet panel
    AppletPanel*     m_appletPanel{nullptr};

    QPointer<QDialog> m_radioSetupDialog;
    QPointer<QDialog> m_networkDiagnosticsDialog;
    QPointer<QDialog> m_propDashboardDialog;
    QPointer<QDialog> m_memoryDialog;

    QAction*         m_txBandAction{nullptr};
    QAction*         m_modeCwDecodePanelAction{nullptr};
#ifdef HAVE_WEBSOCKETS
    QAction*         m_modeFreeDvReporterAction{nullptr};
#endif

    // Status bar labels (SmartSDR-style)
    QLabel* m_connStatusLabel{nullptr};   // hidden, used for connection state logic
    QLabel* m_panelToggle{nullptr};
    QAction* m_panelVisAction{nullptr};
    QLabel* m_tnfIndicator{nullptr};
    CwxPanel* m_cwxPanel{nullptr};
    DvkPanel* m_dvkPanel{nullptr};
    QLabel* m_fdxIndicator{nullptr};
    QLabel* m_radioInfoLabel{nullptr};
    QLabel* m_radioVersionLabel{nullptr};
    QLabel* m_stationLabel{nullptr};
    QLabel* m_stationNickLabel{nullptr};
    QLabel* m_tgxlIndicator{nullptr};
    QLabel* m_pgxlIndicator{nullptr};
    QLabel* m_txIndicator{nullptr};

    // Active slice tracking for multi-slice support
    int m_activeSliceId{-1};

    bool m_shuttingDown{false};
    bool m_userDisconnected{false};  // true after explicit disconnect, blocks auto-connect
    QDialog* m_reconnectDlg{nullptr}; // shown on unexpected disconnect, dismissed on reconnect

    // Applet-panel pop-out support (#1713 Phase 6).  When floating,
    // the panel lives inside m_appletPanelFloatWindow and its splitter
    // slot is removed; re-dock appends a fresh slot and re-applies the
    // canonical {0, 0, width-260, 260} sizing.
    QWidget*    m_appletPanelFloatWindow{nullptr};
    QAction*    m_popOutSidebarAction{nullptr};
    void floatAppletPanel();
    void dockAppletPanel();
    bool m_displaySettingsPushed{false};  // one-shot: push saved display settings after pan created
    bool m_applyingLayout{false};        // true during layout tear-down/recreate — suppresses panadapterAdded handler
    bool m_keyboardShortcutsEnabled{false}; // global enable for keyboard shortcuts (View menu)
    bool m_spacePttActive{false};          // true while Space is held for PTT
    bool m_minimalMode{false};             // true when spectrum is hidden (#208)
    QAction* m_minimalModeAction{nullptr};
    bool m_panadapterConnectionAnimationVisible{false};
    QString m_panadapterConnectionAnimationLabel;
    ShortcutManager m_shortcutManager;

};

} // namespace AetherSDR
