#pragma once

#include "gui/MainWindow.h"
#include "core/CwKeyerLogic.h"

#include <QObject>

namespace AetherSDR {

// External hardware controllers on the ExtControllers worker thread: serial PTT/CW, FlexControl, MIDI and HID encoders.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class ExternalControlsController : public QObject {
public:
    explicit ExternalControlsController(MainWindow& mw);

    void createExternalControllers();
    void shutdownExternalControls();
    bool isEncoderTuningActive() const;
    // Paddle input from serial/MIDI: the radio's iambic keyer (default) or the
    // client-side keyer for Ultimatic / Bug / Straight (#416).
    void routeCwPaddles(bool dit, bool dah);
#ifdef HAVE_SERIALPORT
    void                  updateSerialCwPtt();
    void setSerialTransmitting(bool tx);
    void reloadAfterRadioSetup();
#endif
#ifdef HAVE_MIDI
    void registerMidiParams();
    void showMidiMappingDialog();
#endif

private:
    void createCwPaddleKeyer();
    void applyCwKeyerMode(const QString& modeName);

    MainWindow& m_mw;

    CwPaddleKeyer* m_cwPaddleKeyer{nullptr};
    CwKeyerMode    m_cwKeyerMode{CwKeyerMode::Radio};

    // External controllers run on a dedicated worker thread (#502)
    QThread*             m_extCtrlThread{nullptr};
#ifdef HAVE_SERIALPORT
    SerialPortController* m_serialPort{nullptr};
    bool                  m_serialRadioTx{false};   // raw interlock TRANSMITTING
    FlexControlManager*   m_flexControl{nullptr};
    QTimer               m_flexCoalesceTimer;
    double               m_flexTargetMhz{-1.0};
    FlexWheelMode        m_flexWheelMode{FlexWheelMode::Frequency};
#endif
#ifdef HAVE_HIDAPI
    HidEncoderManager*   m_hidEncoder{nullptr};
    QTimer               m_hidCoalesceTimer;
    int                  m_hidPendingSteps{0};
#endif
#ifdef HAVE_MIDI
    MidiControlManager*  m_midiControl{nullptr};
    bool                 m_midiDeviceLost{false};  // show reconnect message after an error
    // MIDI param setters indexed by ID — called on main thread from
    // paramAction signal (worker thread cannot call them directly). (#502)
    QHash<QString, std::function<void(float)>> m_midiSetters;
    QHash<QString, std::function<float()>>     m_midiGetters;
    QPointer<QDialog> m_midiDialog;
#endif
};

} // namespace AetherSDR
