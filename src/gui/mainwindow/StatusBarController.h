#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// SmartSDR-style status bar: indicators, telemetry labels, clock and indicator click handling.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class StatusBarController : public QObject {
public:
    explicit StatusBarController(MainWindow& mw);

    void updateKeyerAvailability(const QString& mode);
    void updatePaTempLabel();
    void setPaTempDisplayUnit(bool useFahrenheit);
    void updateBandStackIndicator();
    void loadPaTempUnit();
    void wireTelemetry();
    bool handleIndicatorEvent(QObject* obj, QEvent* event);
    void buildStatusBar();

private:
    MainWindow& m_mw;

    QLabel* m_addPanLabel{nullptr};
    QLabel* m_cwxIndicator{nullptr};
    QLabel* m_dvkIndicator{nullptr};
    QLabel* m_gpsLabel{nullptr};
    QLabel* m_gpsStatusLabel{nullptr};
    QLabel* m_gridLabel{nullptr};
    QLabel* m_bandStackIndicator{nullptr};
    QLabel* m_cpuLabel{nullptr};
    QLabel* m_memLabel{nullptr};
    QTimer* m_cpuTimer{nullptr};
    QLabel* m_paTempLabel{nullptr};
    QLabel* m_supplyVoltLabel{nullptr};
    QLabel* m_networkLabel{nullptr};
    QLabel* m_gpsDateLabel{nullptr};
    QLabel* m_gpsTimeLabel{nullptr};
    bool m_useSystemClock{true};     // true when no GPS installed
    bool m_paTempUseFahrenheit{true};
    bool m_hasPaTempTelemetry{false};
    float m_lastPaTempC{0.0f};
};

} // namespace AetherSDR
