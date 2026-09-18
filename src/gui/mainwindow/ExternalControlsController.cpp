#include "gui/mainwindow/ExternalControlsController.h"
#include "gui/PhoneCwApplet.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/AudioRoutingController.h"
#include "gui/mainwindow/SliceController.h"
#include "gui/TitleBar.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "gui/SpectrumWidget.h"
#include "gui/AppletPanel.h"
#include "gui/RxApplet.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "models/EqualizerModel.h"
#ifdef HAVE_MIDI
#include "core/MidiSettings.h"
#endif
#ifdef HAVE_MIDI
#include "gui/MidiMappingDialog.h"
#endif
#include <algorithm>
#include <memory>
#include <cmath>
#include <functional>
#include <QTimer>
#include <QThread>
#include "core/AppSettings.h"

namespace AetherSDR {

ExternalControlsController::ExternalControlsController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

#ifdef HAVE_SERIALPORT
void ExternalControlsController::setSerialTransmitting(bool tx)
{
    QMetaObject::invokeMethod(m_serialPort, [this, tx] { m_serialPort->setTransmitting(tx); });
    updateSerialCwPtt();
}
#endif

void ExternalControlsController::createExternalControllers()
{
    // ── External controllers run on a dedicated worker thread (#502) ────
    // FlexControl, SerialPort, and MIDI controllers are created on the
    // worker thread so their I/O (serial port, RtMidi callbacks, poll timers)
    // never competes with paintEvent. Signals auto-queue to main thread.
    createCwPaddleKeyer();

    m_extCtrlThread = new QThread(&m_mw);
    m_extCtrlThread->setObjectName("ExtControllers");

#ifdef HAVE_SERIALPORT
    m_serialPort = new SerialPortController;  // no parent — moved to thread
    m_serialPort->moveToThread(m_extCtrlThread);
    m_flexControl = new FlexControlManager;
    m_flexControl->moveToThread(m_extCtrlThread);

    // Serial port signals (auto-queued from worker → main)
    connect(m_serialPort, &SerialPortController::externalPttChanged,
            this, [this](bool active) {
        m_mw.m_radioModel.setTransmit(active);
    });
    connect(m_serialPort, &SerialPortController::cwKeyChanged,
            this, [this](bool down) {
        m_mw.m_radioModel.sendCwKey(down);
    });
    connect(m_serialPort, &SerialPortController::cwPaddleChanged,
            this, [this](bool dit, bool dah) {
        routeCwPaddles(dit, dah);
    });
    connect(m_serialPort, &SerialPortController::errorOccurred,
            this, [this](const QString& msg) {
        m_mw.statusBar()->showMessage(QStringLiteral("Serial port error — %1").arg(msg), 8000);
    });

    // DTR/RTS "CW Key" output follows the local straight-key state from any
    // key source (serial input, MIDI, TCI). Iambic paddle elements are timed
    // by the radio's keyer and have no local edge, so they only drive "CW PTT".
    connect(&m_mw.m_radioModel, &RadioModel::localCwKeyChanged, this, [this](bool down) {
        QMetaObject::invokeMethod(m_serialPort, [this, down] { m_serialPort->setCwKeyDown(down); });
    });
    connect(&m_mw.m_radioModel, &RadioModel::localCwActivityChanged, this, [this](bool active) {
        QMetaObject::invokeMethod(m_serialPort, [this, active] { m_serialPort->setCwActivity(active); });
    });
    connect(&m_mw.m_radioModel, &RadioModel::radioTransmittingChanged, this, [this](bool tx) {
        m_serialRadioTx = tx;
        updateSerialCwPtt();
    });

    // FlexControl coalesce timer stays on main thread (accesses activeSlice)
    m_flexCoalesceTimer.setSingleShot(true);
    m_flexCoalesceTimer.setInterval(20);
    connect(&m_flexCoalesceTimer, &QTimer::timeout, this, [this]() {
        if (m_flexTargetMhz < 0.0) return;
        auto* s = m_mw.activeSlice();
        if (!s || s->isLocked()) { m_flexTargetMhz = -1.0; return; }
        double target = m_flexTargetMhz;
        // Use slice tune (not slice m) — doesn't recenter pan, correct for encoder
        s->setFrequency(target);
        m_mw.m_sliceCtl->panFollowVfo(s, target);  // re-center pan if VFO stepped outside visible window (#989)
    });
    // FlexControl signals (auto-queued from worker → main)
    connect(m_flexControl, &FlexControlManager::tuneSteps,
            this, [this](int steps) {
        switch (m_flexWheelMode) {
        case FlexWheelMode::Volume: {
            auto* s = m_mw.activeSlice();
            if (!s) return;
            float gain = s->audioGain() + steps * 2.0f;
            s->setAudioGain(std::clamp(gain, 0.0f, 100.0f));
            return;
        }
        case FlexWheelMode::Power: {
            auto& tx = m_mw.m_radioModel.transmitModel();
            int power = tx.rfPower() + steps;
            tx.setRfPower(std::clamp(power, 0, 100));
            return;
        }
        case FlexWheelMode::Frequency:
        default:
            break;
        }
        // Frequency mode (default)
        auto* s = m_mw.activeSlice();
        if (!s || s->isLocked()) return;
        int stepHz = m_mw.spectrum() ? m_mw.spectrum()->stepSize() : 100;
        // Initialize target from slice on first step or after external QSY (#1098)
        if (m_flexTargetMhz < 0.0 ||
            (!m_flexCoalesceTimer.isActive() &&
             std::abs(m_flexTargetMhz - s->frequency()) > 0.001))
            m_flexTargetMhz = s->frequency();
        m_flexTargetMhz += steps * stepHz / 1e6;
        // Optimistic VFO display update — immediate visual feedback
        if (m_mw.spectrum()) m_mw.spectrum()->setVfoFrequency(m_flexTargetMhz);
        if (!m_flexCoalesceTimer.isActive())
            m_flexCoalesceTimer.start();
    });

    connect(m_flexControl, &FlexControlManager::buttonPressed,
            this, [this](int button, int action) {
        // Knob press while wheel function is active → return to frequency mode (#1354)
        if (button == 4 && action == 0 && m_flexWheelMode != FlexWheelMode::Frequency) {
            m_flexWheelMode = FlexWheelMode::Frequency;
            return;
        }
        QString key = QString("FlexControlBtn%1Action%2").arg(button).arg(action);
        auto& settings = AppSettings::instance();
        static const char* defaults[4][3] = {
            {"StepUp",     "StepDown",     "None"},
            {"ToggleMox",  "ToggleTune",   "None"},
            {"ToggleMute", "ToggleLock",   "None"},
            {"StepUp",     "StepDown",     "None"},  // Knob button
        };
        const char* def = (button >= 1 && button <= 4 && action >= 0 && action <= 2)
                          ? defaults[button-1][action] : "None";
        QString actionName = settings.value(key, def).toString();

        if (actionName == "StepUp") {
            if (auto* rx = m_mw.m_appletPanel->rxApplet()) rx->cycleStepUp();
        } else if (actionName == "StepDown") {
            if (auto* rx = m_mw.m_appletPanel->rxApplet()) rx->cycleStepDown();
        } else if (actionName == "ToggleMox") {
            m_mw.m_radioModel.setTransmit(!m_mw.m_radioModel.transmitModel().isTransmitting());
        } else if (actionName == "ToggleTune") {
            if (m_mw.m_radioModel.transmitModel().isTuning())
                m_mw.m_radioModel.transmitModel().stopTune();
            else
                m_mw.m_radioModel.transmitModel().startTune();
        } else if (actionName == "ToggleMute") {
            m_mw.m_audio->setMuted(!m_mw.m_audio->isMuted());
        } else if (actionName == "ToggleLock") {
            if (auto* s = m_mw.activeSlice()) s->setLocked(!s->isLocked());
        } else if (actionName == "NextSlice") {
            const auto& slices = m_mw.m_radioModel.slices();
            if (slices.size() > 1) {
                int idx = 0;
                for (int i = 0; i < slices.size(); ++i) {
                    if (slices[i]->sliceId() == m_mw.m_activeSliceId) { idx = i; break; }
                }
                m_mw.m_sliceCtl->setActiveSlice(slices[(idx + 1) % slices.size()]->sliceId());
            }
        } else if (actionName == "PrevSlice") {
            const auto& slices = m_mw.m_radioModel.slices();
            if (slices.size() > 1) {
                int idx = 0;
                for (int i = 0; i < slices.size(); ++i) {
                    if (slices[i]->sliceId() == m_mw.m_activeSliceId) { idx = i; break; }
                }
                m_mw.m_sliceCtl->setActiveSlice(slices[(idx - 1 + slices.size()) % slices.size()]->sliceId());
            }
        } else if (actionName == "ToggleAgc") {
            if (auto* s = m_mw.activeSlice()) {
                static const char* modes[] = {"off", "slow", "med", "fast"};
                QString cur = s->agcMode().toLower();
                int idx = 0;
                for (int i = 0; i < 4; ++i) {
                    if (cur == modes[i]) { idx = i; break; }
                }
                s->setAgcMode(modes[(idx + 1) % 4]);
            }
        } else if (actionName == "VolumeUp") {
            if (auto* s = m_mw.activeSlice()) {
                s->setAudioGain(std::min(100.0f, s->audioGain() + 5.0f));
            }
        } else if (actionName == "VolumeDown") {
            if (auto* s = m_mw.activeSlice()) {
                s->setAudioGain(std::max(0.0f, s->audioGain() - 5.0f));
            }
        } else if (actionName == "WheelFrequency") {
            m_flexWheelMode = FlexWheelMode::Frequency;
        } else if (actionName == "WheelVolume") {
            m_flexWheelMode = FlexWheelMode::Volume;
        } else if (actionName == "WheelPower") {
            m_flexWheelMode = FlexWheelMode::Power;
        }
    });
#endif

#ifdef HAVE_MIDI
    m_midiControl = new MidiControlManager;
    m_midiControl->moveToThread(m_extCtrlThread);

    // Register MIDI params — setters/getters stored on MainWindow for
    // main-thread dispatch. Param metadata still registered on the manager.
    registerMidiParams();

    // MIDI port state → status bar (errors, hot-unplug, reconnect)
    connect(m_midiControl, &MidiControlManager::portError,
            this, [this](const QString& err) {
        m_midiDeviceLost = true;
        m_mw.statusBar()->showMessage(QStringLiteral("MIDI: %1").arg(err), 8000);
    });
    connect(m_midiControl, &MidiControlManager::portOpened,
            this, [this](const QString& name) {
        if (m_midiDeviceLost) {
            m_midiDeviceLost = false;
            m_mw.statusBar()->showMessage(QStringLiteral("MIDI: connected to %1").arg(name), 4000);
        }
    });

    // MIDI paramAction signal: dispatches setter on main thread (#502)
    connect(m_midiControl, &MidiControlManager::paramAction,
            this, [this](const QString& paramId, float scaledValue) {
        auto it = m_midiSetters.find(paramId);
        if (it == m_midiSetters.end()) return;
        if (scaledValue == -1.0f) {
            // Toggle sentinel: read getter, flip, call setter
            auto git = m_midiGetters.find(paramId);
            float cur = (git != m_midiGetters.end() && *git) ? (*git)() : 0.0f;
            it.value()(cur > 0.5f ? 0.0f : 1.0f);
        } else {
            it.value()(scaledValue);
        }
    });

    // MIDI relativeAction signal: coalesced step-based tuning with acceleration
    connect(m_midiControl, &MidiControlManager::relativeAction,
            this, [this](const QString& paramId, int steps) {
        if (paramId == "rx.tuneKnob") {
            auto* s = m_mw.activeSlice();
            if (!s || s->isLocked()) return;
            int stepHz = s->stepHz();
            if (stepHz <= 0) return;  // radio hasn't sent step yet
            // Snap to step grid: round current freq to nearest step, then offset
            long long curHz = static_cast<long long>(std::round(s->frequency() * 1e6));
            long long snapped = ((curHz + stepHz / 2) / stepHz) * stepHz;
            double newMhz = (snapped + steps * stepHz) / 1e6;
            s->setFrequency(newMhz);
            m_mw.m_sliceCtl->panFollowVfo(s, newMhz);  // re-center pan if VFO stepped outside visible window (#989)
        }
    });

    MidiSettings::instance().load();
    auto savedBindings = MidiSettings::instance().loadBindings();
    for (const auto& b : savedBindings)
        m_midiControl->addBinding(b);
#endif

#ifdef HAVE_HIDAPI
    m_hidEncoder = new HidEncoderManager;
    m_hidEncoder->moveToThread(m_extCtrlThread);

    // HID encoder coalesce timer — same 20ms pattern as FlexControl
    m_hidCoalesceTimer.setSingleShot(true);
    m_hidCoalesceTimer.setInterval(20);
    connect(&m_hidCoalesceTimer, &QTimer::timeout, this, [this]() {
        if (m_hidPendingSteps == 0) return;
        auto* s = m_mw.activeSlice();
        if (!s || s->isLocked()) { m_hidPendingSteps = 0; return; }
        int stepHz = m_mw.spectrum() ? m_mw.spectrum()->stepSize() : 100;
        double newMhz = s->frequency() + m_hidPendingSteps * stepHz / 1e6;
        m_hidPendingSteps = 0;
        QString panId = m_mw.m_panStack ? m_mw.m_panStack->activePanId() : m_mw.m_radioModel.panId();
        if (!panId.isEmpty())
            m_mw.m_radioModel.sendCommand(
                QString("slice m %1 pan=%2").arg(newMhz, 0, 'f', 6).arg(panId));
        if (m_mw.spectrum()) m_mw.spectrum()->setVfoFrequency(newMhz);
    });

    connect(m_hidEncoder, &HidEncoderManager::tuneSteps,
            this, [this](int steps) {
        m_hidPendingSteps += steps;
        if (!m_hidCoalesceTimer.isActive())
            m_hidCoalesceTimer.start();
    });

    connect(m_hidEncoder, &HidEncoderManager::buttonPressed,
            this, [this](int button, int action) {
        // Reuse same action dispatch as FlexControl
        QString key = QString("HidEncoderBtn%1Action%2").arg(button).arg(action);
        QString actionName = AppSettings::instance().value(key, "None").toString();
        if (actionName == "ToggleMox")
            m_mw.m_radioModel.setTransmit(!m_mw.m_radioModel.transmitModel().isTransmitting());
        else if (actionName == "ToggleTune") {
            if (m_mw.m_radioModel.transmitModel().isTuning())
                m_mw.m_radioModel.transmitModel().stopTune();
            else
                m_mw.m_radioModel.transmitModel().startTune();
        } else if (actionName == "ToggleMute")
            m_mw.m_audio->setMuted(!m_mw.m_audio->isMuted());
        else if (actionName == "ToggleLock") {
            if (auto* s = m_mw.activeSlice()) s->setLocked(!s->isLocked());
        }
    });

    connect(m_hidEncoder, &HidEncoderManager::connectionChanged,
            this, [](bool connected, const QString& name) {
        qCDebug(lcGui) << "HID encoder:" << (connected ? "connected" : "disconnected") << name;
    });

    // StreamDeck native integration removed — use TCI StreamController plugin instead.
#endif

    // Start the external controller thread — objects are already moved
    m_extCtrlThread->start();

    // Init that must happen on the worker thread (serial port open, etc.)
#ifdef HAVE_SERIALPORT
    QMetaObject::invokeMethod(m_serialPort, [this] {
        m_serialPort->loadSettings();
    });
    m_flexControl->setInvertDirection(
        AppSettings::instance().value("FlexControlInvertDir", "False").toString() == "True");
    if (AppSettings::instance().value("FlexControlAutoDetect", "True").toString() == "True") {
        QString fcPort = FlexControlManager::detectPort();
        if (!fcPort.isEmpty()) {
            QMetaObject::invokeMethod(m_flexControl, [this, fcPort] {
                m_flexControl->open(fcPort);
            });
        }
    }
#endif
#ifdef HAVE_MIDI
    if (MidiSettings::instance().autoConnect()) {
        QString dev = MidiSettings::instance().lastDevice();
        if (!dev.isEmpty()) {
            QMetaObject::invokeMethod(m_midiControl, [this, dev] {
                m_midiControl->openPortByName(dev);
            });
        }
    }
#endif

#ifdef HAVE_HIDAPI
    QMetaObject::invokeMethod(m_hidEncoder, [this] {
        m_hidEncoder->loadSettings();
    });
#endif
}

void ExternalControlsController::shutdownExternalControls()
{
    // Stop external controller thread (#502)
    if (m_extCtrlThread && m_extCtrlThread->isRunning()) {
        m_extCtrlThread->quit();
        m_extCtrlThread->wait(3000);
    }
#ifdef HAVE_SERIALPORT
    delete m_serialPort;
    delete m_flexControl;
#endif
#ifdef HAVE_MIDI
    delete m_midiControl;
#endif
#ifdef HAVE_HIDAPI
    delete m_hidEncoder;
#endif
}

#ifdef HAVE_MIDI
void ExternalControlsController::showMidiMappingDialog()
{
    if (m_midiDialog) {
        m_midiDialog->raise();
        m_midiDialog->activateWindow();
        return;
    }
    auto* dlg = new MidiMappingDialog(m_midiControl, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_midiDialog = dlg;
    dlg->show();
}
#endif

bool ExternalControlsController::isEncoderTuningActive() const
{
    bool activeTuning = false;
#ifdef HAVE_SERIALPORT
    activeTuning = m_flexCoalesceTimer.isActive();
#endif
#ifdef HAVE_HIDAPI
    activeTuning = activeTuning || m_hidCoalesceTimer.isActive();
#endif
    return activeTuning;
}

#ifdef HAVE_SERIALPORT
void ExternalControlsController::reloadAfterRadioSetup()
{
    // Re-load serial port settings if changed (on worker thread)
    QMetaObject::invokeMethod(m_serialPort, [this] { m_serialPort->loadSettings(); });
    // Re-check FlexControl open/close state
    auto& fcs = AppSettings::instance();
    bool fcOpen = fcs.value("FlexControlOpen", "False").toString() == "True";
    QString fcPort = fcs.value("FlexControlPort").toString();
    bool fcInvert = fcs.value("FlexControlInvertDir", "False").toString() == "True";
    QMetaObject::invokeMethod(m_flexControl, [this, fcOpen, fcPort, fcInvert] {
        if (fcOpen) {
            if (!m_flexControl->isOpen() && !fcPort.isEmpty())
                m_flexControl->open(fcPort);
        } else {
            if (m_flexControl->isOpen()) m_flexControl->close();
        }
        m_flexControl->setInvertDirection(fcInvert);
    });
}
#endif

#ifdef HAVE_SERIALPORT
// "CW PTT" serial output: asserted while the radio is transmitting on our
// behalf (interlock TRANSMITTING, TX owned by this client, or local MOX)
// with the TX slice in CW/CWL. This covers radio-timed keying (break-in,
// CWX, iambic keyer) where no local key edge exists. Local key activity is
// OR'd in inside SerialPortController::applyCwPtt().
void ExternalControlsController::updateSerialCwPtt()
{
    if (!m_serialPort) {
        return;
    }
    bool cwMode = false;
    for (SliceModel* s : m_mw.m_radioModel.slices()) {
        if (s && s->isTxSlice()) {
            const QString mode = s->mode();
            cwMode = (mode == QLatin1String("CW") || mode == QLatin1String("CWL"));
            break;
        }
    }
    const bool tx = (m_serialRadioTx && m_mw.m_radioModel.isTxOwnedByUs())
        || m_mw.m_radioModel.transmitModel().isTransmitting();
    const bool cwTx = cwMode && tx;
    QMetaObject::invokeMethod(m_serialPort, [this, cwTx] { m_serialPort->setCwTransmitting(cwTx); });
}
#endif

#ifdef HAVE_MIDI
void ExternalControlsController::registerMidiParams()
{
    using P = MidiParamType;
    // Setters/getters stored on MainWindow for main-thread dispatch (#502).
    // MidiControlManager gets metadata only (no lambdas that capture main-thread objects).
    auto reg = [this](const char* id, const char* name, const char* cat,
                      MidiParamType type, float lo, float hi,
                      std::function<void(float)> setter,
                      std::function<float()> getter = {}) {
        m_midiSetters[id] = setter;
        if (getter) m_midiGetters[id] = getter;
        m_midiControl->registerParam({id, name, cat, type, lo, hi, std::move(setter), std::move(getter)});
    };

    // ── RX ──────────────────────────────────────────────────────────────
    reg("rx.afGain", "AF Gain", "RX", P::Slider, 0, 200,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setAudioGain(v); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s ? s->audioGain() : 0; });

    reg("rx.squelch", "Squelch Level", "RX", P::Slider, 0, 100,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setSquelch(s->squelchOn(), static_cast<int>(v)); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s ? s->squelchLevel() : 0; });

    reg("rx.agcThreshold", "AGC Threshold", "RX", P::Slider, 0, 100,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setAgcThreshold(static_cast<int>(v)); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s ? s->agcThreshold() : 0; });

    reg("rx.audioPan", "Audio Pan", "RX", P::Slider, 0, 100,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setAudioPan(static_cast<int>(v)); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s ? s->audioPan() : 50; });

    reg("rx.nbEnable", "Noise Blanker", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setNb(v > 0.5f); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s && s->nbOn() ? 1 : 0; });

    reg("rx.nrEnable", "Noise Reduction", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setNr(v > 0.5f); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s && s->nrOn() ? 1 : 0; });

    reg("rx.anfEnable", "Auto Notch", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setAnf(v > 0.5f); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s && s->anfOn() ? 1 : 0; });

    reg("rx.squelchEnable", "Squelch Enable", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setSquelch(v > 0.5f, s->squelchLevel()); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s && s->squelchOn() ? 1 : 0; });

    reg("rx.mute", "Audio Mute", "RX", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_audio->setMuted(v > 0.5f); },
        [this]() -> float { return m_mw.m_audio->isMuted() ? 1 : 0; });

    reg("rx.tuneLock", "Tune Lock", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setLocked(v > 0.5f); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s && s->isLocked() ? 1 : 0; });

    reg("rx.ritEnable", "RIT Enable", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setRit(v > 0.5f, s->ritFreq()); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s && s->ritOn() ? 1 : 0; });

    reg("rx.xitEnable", "XIT Enable", "RX", P::Toggle, 0, 1,
        [this](float v) { if (auto* s = m_mw.activeSlice()) s->setXit(v > 0.5f, s->xitFreq()); },
        [this]() -> float { auto* s = m_mw.activeSlice(); return s && s->xitOn() ? 1 : 0; });

    reg("rx.nr2Enable", "NR2 (Spectral)", "RX", P::Toggle, 0, 1,
        [this](float v) { QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr2Enabled(v > 0.5f); }); },
        [this]() -> float { return m_mw.m_audio->nr2Enabled() ? 1 : 0; });

    reg("rx.rn2Enable", "RN2 (RNNoise)", "RX", P::Toggle, 0, 1,
        [this](float v) { QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setRn2Enabled(v > 0.5f); }); },
        [this]() -> float { return m_mw.m_audio->rn2Enabled() ? 1 : 0; });

    reg("rx.nr4Enable", "NR4 (Spectral Bleach)", "RX", P::Toggle, 0, 1,
        [this](float v) { QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr4Enabled(v > 0.5f); }); },
        [this]() -> float { return m_mw.m_audio->nr4Enabled() ? 1 : 0; });

    reg("rx.dfnrEnable", "DFNR (DeepFilter)", "RX", P::Toggle, 0, 1,
        [this](float v) { QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setDfnrEnabled(v > 0.5f); }); },
        [this]() -> float { return m_mw.m_audio->dfnrEnabled() ? 1 : 0; });

    reg("rx.stepUp", "Step Size Up", "RX", P::Trigger, 0, 1,
        [this](float) { if (auto* rx = m_mw.m_appletPanel->rxApplet()) rx->cycleStepUp(); });

    reg("rx.stepDown", "Step Size Down", "RX", P::Trigger, 0, 1,
        [this](float) { if (auto* rx = m_mw.m_appletPanel->rxApplet()) rx->cycleStepDown(); });

    // rx.tuneKnob: bind a relative MIDI knob for VFO tuning.
    // Set the binding to "relative" mode in MIDI Mapping dialog.
    // Steps are coalesced every 20ms with acceleration:
    //   Slow spin → ½ rate (fine tuning), Fast spin → 4× (band scanning)
    reg("rx.tuneKnob", "VFO Tune Knob", "RX", P::Slider, 0, 127,
        [this](float v) {
            // Absolute fallback (non-relative bindings): center=64
            auto* s = m_mw.activeSlice();
            if (!s || s->isLocked()) return;
            int steps = static_cast<int>(v) - 64;
            if (steps == 0) return;
            int stepHz = m_mw.spectrum() ? m_mw.spectrum()->stepSize() : 100;
            double newMhz = s->frequency() + steps * stepHz / 1e6;
            s->setFrequency(newMhz);
        });

    // ── TX ──────────────────────────────────────────────────────────────
    reg("tx.rfPower", "RF Power", "TX", P::Slider, 0, 100,
        [this](float v) { m_mw.m_radioModel.transmitModel().setRfPower(static_cast<int>(v)); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().rfPower(); });

    reg("tx.tunePower", "Tune Power", "TX", P::Slider, 0, 100,
        [this](float v) { m_mw.m_radioModel.transmitModel().setTunePower(static_cast<int>(v)); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().tunePower(); });

    reg("tx.mox", "MOX", "TX", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.setTransmit(v > 0.5f); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().isMox() ? 1 : 0; });

    reg("tx.tune", "TUNE", "TX", P::Toggle, 0, 1,
        [this](float v) {
            if (v > 0.5f)
                m_mw.m_radioModel.transmitModel().startTune();
            else
                m_mw.m_radioModel.transmitModel().stopTune();
        },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().isTuning() ? 1 : 0; });

    reg("tx.atuStart", "ATU Start", "TX", P::Trigger, 0, 1,
        [this](float) { m_mw.m_radioModel.sendCommand("atu start"); });

    // ── Phone/CW ────────────────────────────────────────────────────────
    reg("phone.micLevel", "Mic Level", "Phone/CW", P::Slider, 0, 100,
        [this](float v) { m_mw.m_radioModel.transmitModel().setMicLevel(static_cast<int>(v)); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().micLevel(); });

    reg("phone.monGain", "Monitor Volume", "Phone/CW", P::Slider, 0, 100,
        [this](float v) { m_mw.m_radioModel.transmitModel().setMonGainSb(static_cast<int>(v)); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().monGainSb(); });

    reg("phone.procEnable", "Speech Processor", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.transmitModel().setSpeechProcessorEnable(v > 0.5f); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().companderOn() ? 1 : 0; });

    reg("phone.daxEnable", "DAX", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.transmitModel().setDax(v > 0.5f); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().daxOn() ? 1 : 0; });

    reg("phone.monEnable", "Monitor", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.transmitModel().setSbMonitor(v > 0.5f); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().sbMonitor() ? 1 : 0; });

    reg("phone.voxEnable", "VOX Enable", "Phone/CW", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.transmitModel().setVoxEnable(v > 0.5f); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().voxEnable() ? 1 : 0; });

    reg("phone.voxLevel", "VOX Level", "Phone/CW", P::Slider, 0, 100,
        [this](float v) { m_mw.m_radioModel.transmitModel().setVoxLevel(static_cast<int>(v)); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().voxLevel(); });

    reg("phone.amCarrier", "AM Carrier", "Phone/CW", P::Slider, 0, 100,
        [this](float v) { m_mw.m_radioModel.transmitModel().setAmCarrierLevel(static_cast<int>(v)); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().amCarrierLevel(); });

    reg("cw.speed", "CW Speed", "Phone/CW", P::Slider, 5, 100,
        [this](float v) { m_mw.m_radioModel.transmitModel().setCwSpeed(static_cast<int>(v)); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().cwSpeed(); });

    reg("cw.key", "CW Key (straight)", "Phone/CW", P::Gate, 0, 1,
        [this](float v) { m_mw.m_radioModel.sendCwKey(v > 0.5f); });

    // Iambic paddle: dit and dah are separate MIDI notes.
    // The radio handles iambic A/B mode and timing — we just send both states.
    {
        auto dit = std::make_shared<bool>(false);
        auto dah = std::make_shared<bool>(false);

        reg("cw.dit", "CW Paddle Dit", "Phone/CW", P::Gate, 0, 1,
            [this, dit, dah](float v) {
                *dit = (v > 0.5f);
                routeCwPaddles(*dit, *dah);
            },
            [dit]() -> float { return *dit ? 1.0f : 0.0f; });

        reg("cw.dah", "CW Paddle Dah", "Phone/CW", P::Gate, 0, 1,
            [this, dit, dah](float v) {
                *dah = (v > 0.5f);
                routeCwPaddles(*dit, *dah);
            },
            [dah]() -> float { return *dah ? 1.0f : 0.0f; });
    }

    reg("cw.ptt", "PTT (hold)", "Phone/CW", P::Gate, 0, 1,
        [this](float v) { m_mw.m_radioModel.setTransmit(v > 0.5f); });

    // ── EQ ──────────────────────────────────────────────────────────────
    reg("eq.txEnable", "TX EQ Enable", "EQ", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.equalizerModel().setTxEnabled(v > 0.5f); },
        [this]() -> float { return m_mw.m_radioModel.equalizerModel().txEnabled() ? 1 : 0; });

    reg("eq.rxEnable", "RX EQ Enable", "EQ", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.equalizerModel().setRxEnabled(v > 0.5f); },
        [this]() -> float { return m_mw.m_radioModel.equalizerModel().rxEnabled() ? 1 : 0; });

    {
        using B = EqualizerModel::Band;
        static const B bands[] = {B::B63, B::B125, B::B250, B::B500, B::B1k, B::B2k, B::B4k, B::B8k};
        static const int freqs[] = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
        static const char* names[] = {"63 Hz", "125 Hz", "250 Hz", "500 Hz",
                                       "1 kHz", "2 kHz", "4 kHz", "8 kHz"};
        for (int i = 0; i < 8; ++i) {
            B band = bands[i];
            QString id = QString("eq.band%1").arg(freqs[i]);
            reg(id.toUtf8().constData(), names[i], "EQ", P::Slider, -10, 10,
                [this, band](float v) { m_mw.m_radioModel.equalizerModel().setTxBand(band, static_cast<int>(v)); },
                [this, band]() -> float { return m_mw.m_radioModel.equalizerModel().txBand(band); });
        }
    }

    // ── Global ──────────────────────────────────────────────────────────
    // Routed exactly like the title-bar Master slider (PC volume when PC
    // Audio is on, radio lineout gain otherwise).
    reg("global.masterVolume", "Master Volume", "Global", P::Slider, 0, 100,
        [this](float v) {
            const int pct = std::clamp(static_cast<int>(std::lround(v)), 0, 100);
            m_mw.m_audioCtl->applyMasterVolume(pct);
            m_mw.m_titleBar->setMasterVolume(pct);
        },
        [this]() -> float { return static_cast<float>(m_mw.m_audioCtl->masterVolumePercent()); });

    reg("global.hpVolume", "Headphone Volume", "Global", P::Slider, 0, 100,
        [this](float v) { m_mw.m_radioModel.sendCommand(QString("mixer headphone gain %1").arg(static_cast<int>(v))); },
        [this]() -> float { return m_mw.m_radioModel.headphoneGain(); });

    reg("global.masterMute", "Master Mute", "Global", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_audio->setMuted(v > 0.5f); },
        [this]() -> float { return m_mw.m_audio->isMuted() ? 1 : 0; });

    reg("global.txButton", "TX Button", "Global", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.setTransmit(v > 0.5f); },
        [this]() -> float { return m_mw.m_radioModel.transmitModel().isMox() ? 1 : 0; });

    reg("global.tnfEnable", "TNF Global", "Global", P::Toggle, 0, 1,
        [this](float v) { m_mw.m_radioModel.sendCommand(QString("radio set tnf_enabled=%1").arg(v > 0.5f ? 1 : 0)); });

    reg("global.bandUp", "Band Up", "Global", P::Trigger, 0, 1,
        [this](float) { m_mw.m_sliceCtl->stepBand(+1); });

    reg("global.bandDown", "Band Down", "Global", P::Trigger, 0, 1,
        [this](float) { m_mw.m_sliceCtl->stepBand(-1); });

    reg("global.nextSlice", "Next Slice", "Global", P::Trigger, 0, 1,
        [this](float) {
            const auto& slices = m_mw.m_radioModel.slices();
            if (slices.size() > 1) {
                int idx = 0;
                for (int i = 0; i < slices.size(); ++i) {
                    if (slices[i]->sliceId() == m_mw.m_activeSliceId) { idx = i; break; }
                }
                m_mw.m_sliceCtl->setActiveSlice(slices[(idx + 1) % slices.size()]->sliceId());
            }
        });

    reg("global.prevSlice", "Previous Slice", "Global", P::Trigger, 0, 1,
        [this](float) {
            const auto& slices = m_mw.m_radioModel.slices();
            if (slices.size() > 1) {
                int idx = 0;
                for (int i = 0; i < slices.size(); ++i) {
                    if (slices[i]->sliceId() == m_mw.m_activeSliceId) { idx = i; break; }
                }
                int prev = (idx - 1 + slices.size()) % slices.size();
                m_mw.m_sliceCtl->setActiveSlice(slices[prev]->sliceId());
            }
        });
}
#endif

void ExternalControlsController::createCwPaddleKeyer()
{
    m_cwPaddleKeyer = new CwPaddleKeyer(this);
    connect(m_cwPaddleKeyer, &CwPaddleKeyer::keyChanged, this, [this](bool down) {
        m_mw.m_radioModel.sendCwKey(down);
    });
    auto& s = AppSettings::instance();
    applyCwKeyerMode(s.value("CwKeyerMode", "Radio").toString());
    m_cwPaddleKeyer->setWeight(s.value("CwKeyerWeight", "50").toInt());
    m_cwPaddleKeyer->setWpm(m_mw.m_radioModel.transmitModel().cwSpeed());
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::phoneStateChanged, this, [this] {
        m_cwPaddleKeyer->setWpm(m_mw.m_radioModel.transmitModel().cwSpeed());
    });
    if (PhoneCwApplet* pc = m_mw.m_appletPanel ? m_mw.m_appletPanel->phoneCwApplet() : nullptr) {
        connect(pc, &PhoneCwApplet::cwKeyerModeChanged, this,
                [this](const QString& mode) { applyCwKeyerMode(mode); });
    }
    connect(&m_mw.m_radioModel, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (!connected) {
            m_cwPaddleKeyer->abort();
        }
    });
}

void ExternalControlsController::applyCwKeyerMode(const QString& modeName)
{
    const CwKeyerMode mode = settingsDispatchMode(cwKeyerModeFromString(modeName));
    if (m_cwKeyerMode == CwKeyerMode::Radio && mode != CwKeyerMode::Radio) {
        m_mw.m_radioModel.sendCwPaddle(false, false);   // release the radio keyer
    }
    m_cwKeyerMode = mode;
    if (m_cwPaddleKeyer) {
        m_cwPaddleKeyer->setMode(mode);   // aborts any element in progress (key-up)
    }
}

void ExternalControlsController::routeCwPaddles(bool dit, bool dah)
{
    if (m_cwKeyerMode == CwKeyerMode::Radio || !m_cwPaddleKeyer) {
        m_mw.m_radioModel.sendCwPaddle(dit, dah);
    } else {
        m_cwPaddleKeyer->setPaddles(dit, dah);
    }
}

} // namespace AetherSDR
