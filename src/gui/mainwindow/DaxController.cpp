#include "gui/mainwindow/DaxController.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/DigitalVoiceController.h"
#include "core/LogManager.h"
#include "gui/AppletPanel.h"
#include "gui/DaxApplet.h"
#include "gui/DaxIqApplet.h"
#include "models/SliceModel.h"
#ifdef Q_OS_MAC
#include "core/MacDaxDriver.h"
#endif
#include "models/TransmitModel.h"
#include <cmath>
#include <QTimer>
#include <QMessageBox>
#include "core/AppSettings.h"
#include <QPushButton>
#include "core/VirtualAudioBridge.h"
#include "core/DeviceMappedDax.h"
#include "gui/DaxDeviceMappingDialog.h"
#include "gui/AppletPanel.h"
#include "gui/DaxApplet.h"
#include <QFileInfo>
#include <QStatusBar>

namespace AetherSDR {

DaxController::DaxController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

#if defined(HAVE_DAX_BRIDGE)
void DaxController::setBridgeTransmitting(bool tx)
{
    if (m_daxBridge)
        m_daxBridge->setTransmitting(tx);
}

void DaxController::wireDaxApplet()
{
    // DAX enable button in DaxApplet → start/stop DAX bridge
    connect(m_mw.m_appletPanel->daxApplet(), &DaxApplet::daxToggled,
            this, [this](bool on) {
        if (on) {
            if (!startDax() && m_mw.m_appletPanel && m_mw.m_appletPanel->daxApplet())
                m_mw.m_appletPanel->daxApplet()->setDaxEnabled(false);
        } else {
            stopDax();
        }
    });
}

void DaxController::setDaxAutoStart(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue("AutoStartDAX", on ? "True" : "False");
    s.save();
    if (m_mw.m_radioModel.isConnected()) {
        if (on) {
            if (startDax() && m_mw.m_appletPanel && m_mw.m_appletPanel->daxApplet())
                m_mw.m_appletPanel->daxApplet()->setDaxEnabled(true);
        } else {
            stopDax();
            if (m_mw.m_appletPanel && m_mw.m_appletPanel->daxApplet())
                m_mw.m_appletPanel->daxApplet()->setDaxEnabled(false);
        }
    }
}

void DaxController::scheduleDaxAutoStart()
{
    // Delay DAX bridge start until RadioModel's SmartConnect sequence
    // is fully complete (streams created, UDP bound, slices discovered).
    // Auto-start DAX bridge if enabled in settings.
    // Starting too early causes our mic_selection=PC and dax=1 to be
    // overridden by RadioModel's own setup, and DAX stream IDs won't
    // be registered in PanadapterStream yet.
    if (AppSettings::instance().value("AutoStartDAX", "False").toString() == "True") {
        QTimer::singleShot(3000, this, [this]() {
            if (startDax() && m_mw.m_appletPanel && m_mw.m_appletPanel->daxApplet())
                m_mw.m_appletPanel->daxApplet()->setDaxEnabled(true);
        });
    }
}

void DaxController::wireTxModeForSlice(SliceModel* s)
{
    // Update m_daxTxMode when TX slice or its mode changes.
    // Digital modes (DIGU/DIGL/RTTY) use DAX bridge; voice modes use mic.
    auto updateDaxTxMode = [this]() {
        bool isDigital = false;
        int txSliceId = -1;
        for (auto* sl : m_mw.m_radioModel.slices()) {
            if (sl->isTxSlice()) {
                txSliceId = sl->sliceId();
                const QString& m = sl->mode();
                isDigital = (m == "DIGU" || m == "DIGL" || m == "RTTY"
                          || m == "DFM"  || m == "NFM");
                break;
            }
        }
        m_mw.m_audio->setDaxTxMode(isDigital);

        // Auto-toggle radio-side DAX flag on mode change (#534).
        // Digital modes need dax=1 for TX audio routing through DAX.
        m_mw.m_radioModel.transmitModel().setDax(isDigital);

#ifdef HAVE_DIGITAL_VOICE
        m_mw.m_digitalVoiceCtl->onTxSliceChanged(txSliceId);
#endif
    };
    connect(s, &SliceModel::modeChanged, this, updateDaxTxMode);
    connect(s, &SliceModel::txSliceChanged, this, updateDaxTxMode);
    updateDaxTxMode();  // set initial state from current TX slice mode
}

bool DaxController::isDaxBridgeRunning() const
{
    return m_daxBridge != nullptr;
}
#endif

#ifdef Q_OS_MAC
// Returns true when the installed driver can be used right now. Otherwise it
// offers to install (or update) the bundled driver; after a successful
// install DAX is started automatically, so callers just return false.
bool DaxController::ensureMacDaxDriver()
{
    using State = MacDaxDriver::State;
    const State st = MacDaxDriver::state();
    if (st == State::Current || st == State::InstalledOnly) {
        return true;
    }
    if (st == State::NotBundled) {
        qCWarning(lcGui) << "MainWindow: DAX HAL plugin not installed and not bundled";
        QMessageBox::warning(&m_mw, "DAX Audio Driver Missing",
            "The AetherSDR DAX audio driver is not installed on this Mac, and this "
            "build does not include it.\n\nInstall the DAX Virtual Audio Driver from the "
            "AetherSDR DMG package, then enable DAX again.");
        return false;
    }
    if (m_macDaxDriver && m_macDaxDriver->isBusy()) {
        return false;
    }

    const bool outdated = (st == State::Outdated);
    if (outdated && m_daxDriverUpdateDeclined) {
        return true;  // keep using the installed driver this session
    }
    QMessageBox box(QMessageBox::Question,
                    outdated ? "Update DAX Audio Driver" : "Install DAX Audio Driver",
                    outdated
                        ? "A newer AetherSDR DAX audio driver is included with this version."
                        : "DAX needs the AetherSDR virtual audio driver.",
                    QMessageBox::NoButton, &m_mw);
    box.setInformativeText(
        "It adds \"AetherSDR DAX\" audio devices for WSJT-X, fldigi, VARA and other "
        "digital-mode apps.\n\nmacOS will ask for an administrator password. System audio "
        "restarts for a moment while the driver loads.");
    QPushButton* installBtn = box.addButton(outdated ? "Update Driver" : "Install Driver",
                                            QMessageBox::AcceptRole);
    box.addButton(outdated ? "Not Now" : QStringLiteral("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(installBtn);
    box.exec();
    if (box.clickedButton() != installBtn) {
        if (outdated) {
            m_daxDriverUpdateDeclined = true;
            return true;
        }
        return false;
    }

    if (!m_macDaxDriver) {
        m_macDaxDriver = new MacDaxDriver(&m_mw);
        connect(m_macDaxDriver, &MacDaxDriver::installFinished,
                this, [this](bool ok, const QString& message) {
            m_mw.statusBar()->showMessage(message, 6000);
            if (!ok) {
                if (!message.contains("cancelled")) {
                    QMessageBox::warning(&m_mw, "DAX Audio Driver", message);
                }
                return;
            }
            // coreaudiod needs a moment to reload HAL plug-ins.
            QTimer::singleShot(2500, this, [this]() {
                if (startDax() && m_mw.m_appletPanel && m_mw.m_appletPanel->daxApplet()) {
                    m_mw.m_appletPanel->daxApplet()->setDaxEnabled(true);
                }
            });
        });
    }
    m_mw.statusBar()->showMessage("Installing DAX audio driver…");
    m_macDaxDriver->install();
    return false;
}
#endif

#if defined(HAVE_DAX_BRIDGE)
bool DaxController::startDax()
{
    if (m_daxBridge) return true;

    VirtualAudioBridge::Backend backend = VirtualAudioBridge::configuredBackend();
    if (backend == VirtualAudioBridge::Backend::DeviceMapped && !DeviceMappedDax::hasAnyMapping()) {
        // Device-mapped DAX (virtual cables) needs at least one mapping first.
        DaxDeviceMappingDialog dlg(&m_mw);
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        backend = VirtualAudioBridge::configuredBackend();
        if (backend == VirtualAudioBridge::Backend::DeviceMapped && !DeviceMappedDax::hasAnyMapping()) {
            return false;
        }
    }

#ifdef Q_OS_MAC
    // DAX is built in: the virtual audio driver ships inside the app bundle
    // (like SmartSDR's DAX) and is installed/updated here on first use.
    if (backend == VirtualAudioBridge::Backend::Native && !ensureMacDaxDriver()) {
        return false;
    }
#endif

    m_daxBridge = new DaxBridge(&m_mw);
    if (!m_daxBridge->open()) {
        qCWarning(lcGui) << "MainWindow: failed to open DAX audio bridge" << m_daxBridge->lastError();
        const QString why = m_daxBridge->lastError();
        QMessageBox::warning(&m_mw, "DAX Audio Bridge Error",
            m_daxBridge->backend() == VirtualAudioBridge::Backend::DeviceMapped
                ? QString("AetherSDR could not open the DAX audio devices.\n\n%1\n\n"
                          "Check Settings → DAX Audio Devices…").arg(why)
                : QString("AetherSDR could not open the DAX audio bridge.\n\n"
                          "If the DAX driver was just installed, quit and relaunch AetherSDR and try again."));
        delete m_daxBridge;
        m_daxBridge = nullptr;
        return false;
    }
    if (!m_daxBridge->lastError().isEmpty()) {
        m_mw.statusBar()->showMessage(m_daxBridge->lastError(), 8000);   // partial open
    }

    // Listen for DAX stream status messages to register them in PanadapterStream.
    // The radio sends "stream 0xNNNNNNNN type=dax_rx dax_channel=N" status lines
    // in response to our stream create commands.
    connect(&m_mw.m_radioModel, &RadioModel::statusReceived,
            m_daxBridge, [this](const QString& obj, const QMap<QString,QString>& kvs) {
        if (!obj.startsWith("stream ")) return;
        QString type = kvs.value("type");
        if (type == "dax_rx") {
            quint32 streamId = obj.mid(7).toUInt(nullptr, 16);
            int ch = kvs.value("dax_channel").toInt();
            if (streamId && ch >= 1 && ch <= 4) {
                m_mw.m_radioModel.panStream()->registerDaxStream(streamId, ch);
                qCDebug(lcGui) << "MainWindow: registered DAX RX ch" << ch
                         << "stream" << Qt::hex << streamId;
            }
        }
    });

    // Create DAX RX streams only for channels with slices assigned.
    // FlexLib creates streams on demand, not all 4 unconditionally.
    // Creating unused streams causes the radio to round-robin audio
    // across all of them, starving the active channels.
    for (auto* s : m_mw.m_radioModel.slices()) {
        int ch = s->daxChannel();
        if (ch >= 1 && ch <= 4) {
            m_mw.m_radioModel.sendCommand(
                QString("stream create type=dax_rx dax_channel=%1").arg(ch));
        }
    }

    // Wire DAX RX: PanadapterStream routes registered DAX streams here
    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
            m_daxBridge, &DaxBridge::feedDaxAudio);

    // ── DAX IQ stream status + VITA routing ─────────────────────────────
    connect(&m_mw.m_radioModel, &RadioModel::statusReceived,
            this, [this](const QString& obj, const QMap<QString,QString>& kvs) {
        if (!obj.startsWith("stream ")) return;
        QString type = kvs.value("type");
        if (type == "dax_iq") {
            qCDebug(lcGui) << "DAX IQ STREAM STATUS:" << obj << "keys=" << kvs.keys()
                     << "ch=" << kvs.value("daxiq_channel") << "ip=" << kvs.value("ip");
            quint32 streamId = obj.mid(7).toUInt(nullptr, 0);
            if (kvs.contains("removed")) {
                m_mw.m_radioModel.panStream()->unregisterIqStream(streamId);
                m_mw.m_radioModel.daxIqModel().handleStreamRemoved(streamId);
            } else {
                m_mw.m_radioModel.daxIqModel().applyStreamStatus(streamId, kvs);
                int ch = kvs.value("daxiq_channel").toInt();
                if (streamId && ch >= 1 && ch <= 4)
                    m_mw.m_radioModel.panStream()->registerIqStream(streamId, ch);
            }
        }
    });

    // Route IQ VITA-49 packets to DaxIqModel worker thread
    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::iqDataReady,
            &m_mw.m_radioModel.daxIqModel(), &DaxIqModel::feedRawIqPacket);

    // Wire DAX IQ level meters to DAX IQ applet
    connect(&m_mw.m_radioModel.daxIqModel(), &DaxIqModel::iqLevelReady,
            m_mw.m_appletPanel->daxIqApplet(), &DaxIqApplet::setDaxIqLevel);

    // Wire DAX IQ enable/disable/rate from DAX IQ applet to DaxIqModel
    connect(m_mw.m_appletPanel->daxIqApplet(), &DaxIqApplet::iqEnableRequested,
            &m_mw.m_radioModel.daxIqModel(), &DaxIqModel::createStream);
    connect(m_mw.m_appletPanel->daxIqApplet(), &DaxIqApplet::iqDisableRequested,
            &m_mw.m_radioModel.daxIqModel(), &DaxIqModel::removeStream);
    connect(m_mw.m_appletPanel->daxIqApplet(), &DaxIqApplet::iqRateChanged,
            &m_mw.m_radioModel.daxIqModel(), &DaxIqModel::setSampleRate);

    // Wire DAX level meters
    connect(m_daxBridge, &DaxBridge::daxRxLevel,
            m_mw.m_appletPanel->daxApplet(), &DaxApplet::setDaxRxLevel);
    connect(m_daxBridge, &DaxBridge::daxTxLevel,
            m_mw.m_appletPanel->daxApplet(), &DaxApplet::setDaxTxLevel);

    // Wire DAX gain sliders
    connect(m_mw.m_appletPanel->daxApplet(), &DaxApplet::daxRxGainChanged,
            m_daxBridge, &DaxBridge::setChannelGain);
    connect(m_mw.m_appletPanel->daxApplet(), &DaxApplet::daxTxGainChanged,
            m_daxBridge, &DaxBridge::setTxGain);

    // Apply saved gains to the bridge
    auto& ss = AppSettings::instance();
    for (int i = 1; i <= 4; ++i)
        m_daxBridge->setChannelGain(i, ss.value(QStringLiteral("DaxRxGain%1").arg(i), "0.5").toString().toFloat());
    m_daxBridge->setTxGain(ss.value("DaxTxGain", "0.5").toString().toFloat());

    // Wire DAX TX: apps → bridge → AudioEngine → VITA-49.
    // AudioEngine chooses packet format/routing based on DaxTxLowLatency.
    connect(m_daxBridge, &DaxBridge::txAudioReady,
            this, [this](const QByteArray& pcm) {
        if (m_mw.m_audio->isRadeMode()) return;
        if (!m_mw.m_audio->isDaxTxMode()) return;
        QMetaObject::invokeMethod(m_mw.m_audio, [this, pcm]() { m_mw.m_audio->feedDaxTxAudio(pcm); });
    });

    // Save current mic selection before forcing PC audio source.
    m_savedMicSelection = m_mw.m_radioModel.transmitModel().micSelection();

    // Default to the radio-native DAX route (dax=1, int16 mono).  RADE
    // mode overrides this to the low-latency route via setRadeMode()
    // when the user enters RADE — see AudioEngine::setRadeMode().
    m_mw.m_audio->setDaxTxUseRadioRoute(true);
    m_mw.m_radioModel.sendCommand("transmit set mic_selection=PC");
    // Don't force dax=1 here — radio-side DAX flag follows mode changes
    // via updateDaxTxMode(). Bridge up ≠ DAX TX active. (#534)

    qCInfo(lcGui) << "MainWindow: starting DAX audio bridge";
    return true;
}

void DaxController::stopDax()
{
    if (!m_daxBridge) return;

    m_mw.m_audio->setDaxTxMode(false);
    m_mw.m_audio->clearTxAccumulators();

    disconnect(m_mw.m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
               m_daxBridge, nullptr);
    disconnect(m_daxBridge, &DaxBridge::txAudioReady,
               this, nullptr);

    // Remove DAX RX streams from radio and unregister from PanadapterStream
    const auto daxIds = m_mw.m_radioModel.panStream()->daxStreamIds();
    for (quint32 id : daxIds) {
        m_mw.m_radioModel.sendCommand(QString("stream remove 0x%1").arg(id, 0, 16));
        m_mw.m_radioModel.panStream()->unregisterDaxStream(id);
    }

    // Restore original mic selection
    if (!m_savedMicSelection.isEmpty() && m_savedMicSelection != "PC")
        m_mw.m_radioModel.sendCommand(QString("transmit set mic_selection=%1").arg(m_savedMicSelection));

    m_daxBridge->close();
    delete m_daxBridge;
    m_daxBridge = nullptr;
    qCInfo(lcGui) << "MainWindow: stopping DAX audio bridge";
}
#endif

#if defined(HAVE_DAX_BRIDGE)
void DaxController::applyDaxDeviceSettings(bool backendChanged)
{
    if (!m_daxBridge) {
        return;   // applies on next DAX start
    }
    if (backendChanged) {
        stopDax();
        if (!startDax() && m_mw.m_appletPanel && m_mw.m_appletPanel->daxApplet()) {
            m_mw.m_appletPanel->daxApplet()->setDaxEnabled(false);
        }
    } else {
        m_daxBridge->reloadDeviceMapping();
    }
}

void DaxController::showDaxDeviceMappingDialog()
{
    DaxDeviceMappingDialog dlg(&m_mw);
    if (dlg.exec() == QDialog::Accepted) {
        applyDaxDeviceSettings(dlg.backendChanged());
    }
}
#endif

} // namespace AetherSDR
