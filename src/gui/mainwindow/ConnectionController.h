#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// Radio discovery, SmartLink/WAN login, connect/disconnect/reconnect UX and the connection-state fan-out.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class ConnectionController : public QObject {
public:
    explicit ConnectionController(MainWindow& mw);

    // Radio/connection events
    void onConnectionStateChanged(bool connected);
    void onConnectionError(const QString& msg);
    void setPanadapterConnectionAnimation(bool visible, const QString& label = {});
    void finishPanadapterConnectionAnimation();
    void toggleConnectionDialog();
    void wireConnectionSignals();
    void wireReconnectProgress();
    void startDiscovery();

private:
    MainWindow& m_mw;

    // Pending WAN radio (between requestConnect and connectReady)
    WanRadioInfo     m_pendingWanRadio;
    QMetaObject::Connection m_xvtrRefreshConn;
    QTimer* m_heartbeatMissTimer{nullptr}; // fires every 1.5s to detect missed discovery beats
    bool m_waitingForFirstPanadapterFrame{false};
    // Auto-reconnect status readout (see RadioModel::reconnectScheduled).
    QTimer* m_reconnectTicker{nullptr};   // drives the countdown text
    qint64  m_reconnectDeadline{0};       // ms since epoch of the next attempt
    int     m_reconnectAttemptNum{0};
    int     m_reconnectMaxAttempts{0};
};

} // namespace AetherSDR
