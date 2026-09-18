#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// RadioModel -> applet wiring: TX/interlock state, S-meter, tuner/TGXL, PGXL/amplifier, TX and P/CW applets.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class RadioAppletsController : public QObject {
public:
    explicit RadioAppletsController(MainWindow& mw);

    void wireTransmitState();
    void wireMetersAndAmplifiers();
    void wireTxApplets();

private:
    MainWindow& m_mw;
};

} // namespace AetherSDR
