#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// Mode menu and the mutually exclusive digital-mode tools (CW decode panel, RTTY, PSK, HF-Fax, SSTV, FT8/WSPR, time stations, 2G ALE).
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class DigitalModesController : public QObject {
public:
    explicit DigitalModesController(MainWindow& mw);

    HfFaxDialog* showHfFaxDialog();
    PskDialog* showPskDialog();
    RttyDialog* showRttyDialog();
    SstvDialog* showSstvDialog();
    WeakSignalDialog* showWeakSignalDialog();
    WwvDialog* showWwvDialog();
    Ale2gDialog* showAle2gDialog();
    void closeExclusiveDigitalMode(QAction* action);
    void closeExclusiveDigitalModesExcept(QAction* keepAction);
    void openExclusiveDigitalMode(QAction* action);
    void setExclusiveDigitalModeChecked(QAction* action, bool checked);
    void closeAllTools();
    void buildModeMenu();
    void setActiveSliceOnTools(SliceModel* s);

private:
    MainWindow& m_mw;

    QPointer<HfFaxDialog> m_hfFaxDialog;
    QPointer<PskDialog> m_pskDialog;
    QPointer<RttyDialog> m_rttyDialog;
    QPointer<SstvDialog> m_sstvDialog;
    QPointer<WeakSignalDialog> m_weakSignalDialog;
    QPointer<WwvDialog> m_wwvDialog;
    QPointer<Ale2gDialog> m_ale2gDialog;
    QActionGroup*    m_exclusiveDigitalModeGroup{nullptr};
    QAction*         m_modeRttyAction{nullptr};
    QAction*         m_modePskAction{nullptr};
    QAction*         m_modeHfFaxAction{nullptr};
    QAction*         m_modeSstvAction{nullptr};
    QAction*         m_modeWeakSignalAction{nullptr};
    QAction*         m_modeWwvAction{nullptr};
    QAction*         m_modeAle2gAction{nullptr};
};

} // namespace AetherSDR
