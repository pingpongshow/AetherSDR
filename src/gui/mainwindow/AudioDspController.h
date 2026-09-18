#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// Client-side DSP UI: NR2/RN2/BNR/NR4/MNR/DFNR button sync + popups, AetherDSP dialog and the TX/RX client DSP chain editors.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class AudioDspController : public QObject {
public:
    explicit AudioDspController(MainWindow& mw);

    void enableNr2WithWisdom();  // Wisdom-gated NR2 enable (shared by VFO + overlay)
    void updateNr2Availability(); // Disable NR2 when Opus is active (#1597)
    void showNr2ParamPopup(const QPoint& globalPos);
    void showNr4ParamPopup(const QPoint& globalPos);
    void showDfnrParamPopup(const QPoint& globalPos);
    void showMnrSettings();
    AetherDspDialog* createAetherDspDialog();
    void wireNrButtonSync();
    void wireClientDspChain();
    void showAetherDspSettings();

private:
    MainWindow& m_mw;

    QPointer<QDialog> m_dspDialog;
    class ClientEqEditor* m_clientEqEditor{nullptr}; // lazy — created on first Edit… click
    class ClientCompEditor* m_clientCompEditor{nullptr}; // lazy — created on first Edit… click
    class ClientGateEditor* m_clientGateEditor{nullptr}; // lazy — created on first Edit… click
    class ClientDeEssEditor* m_clientDeEssEditor{nullptr}; // lazy — created on first Edit… click
    class ClientTubeEditor* m_clientTubeEditor{nullptr}; // lazy — created on first Edit… click
    class ClientPuduEditor* m_clientPuduEditor{nullptr}; // lazy — created on first Edit… click
    class ClientReverbEditor* m_clientReverbEditor{nullptr}; // lazy — created on first Edit… click
};

} // namespace AetherSDR
