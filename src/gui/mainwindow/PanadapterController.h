#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// Panadapter lifecycle and per-pan wiring: VITA-49 FFT/waterfall routing, pan add/remove, layouts, overlay menu and spectrum signals.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class PanadapterController : public QObject {
public:
    explicit PanadapterController(MainWindow& mw);

    void wirePanadapter(PanadapterApplet* applet);
    void setActivePanApplet(PanadapterApplet* applet);
    void routeCwDecoderOutput();  // wire CW decoder to the pan owning the active slice
    void applyPanLayout(const QString& layoutId);
    void createPansSequentially(const QString& layoutId, int total,
                                std::shared_ptr<QStringList> panIds, int created);
    // All PanadapterModel → widget connections; safe to call repeatedly.
    void wirePanadapterModel(PanadapterApplet* applet, PanadapterModel* pan);
    void wireStreamsAndLifecycle();
    void wireActivePanTracking();

private:
    MainWindow& m_mw;

    QTimer* m_layoutRestoreTimer{nullptr}; // debounced layout rearrange after pans added on connect
};

} // namespace AetherSDR
