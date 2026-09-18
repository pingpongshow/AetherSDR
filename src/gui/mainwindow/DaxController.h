#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// DAX audio bridge (macOS virtual driver / PipeWire): start/stop, DAX RX/IQ stream routing and DAX TX mode tracking.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class DaxController : public QObject {
public:
    explicit DaxController(MainWindow& mw);

#if defined(HAVE_DAX_BRIDGE)
    bool startDax();
    void stopDax();
    void setBridgeTransmitting(bool tx);
    void wireDaxApplet();
    void setDaxAutoStart(bool on);
    void scheduleDaxAutoStart();
    void wireTxModeForSlice(SliceModel* s);
    bool isDaxBridgeRunning() const;
    DaxBridge* daxBridge() const { return m_daxBridge; }   // per-slice routing feeds
    void applyDaxDeviceSettings(bool backendChanged);
    void showDaxDeviceMappingDialog();
#ifdef Q_OS_MAC
    bool ensureMacDaxDriver();
#endif
#endif

private:
    MainWindow& m_mw;

#if defined(HAVE_DAX_BRIDGE)
    DaxBridge* m_daxBridge{nullptr};
    QString m_savedMicSelection;  // restore on stopDax
#ifdef Q_OS_MAC
    class MacDaxDriver* m_macDaxDriver{nullptr};
    bool m_daxDriverUpdateDeclined{false};
#endif
#endif
};

} // namespace AetherSDR
