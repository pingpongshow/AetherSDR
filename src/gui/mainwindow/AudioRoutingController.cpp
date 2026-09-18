#include "gui/mainwindow/AudioRoutingController.h"

#include <QTimer>

#include "gui/MainWindow.h"
#include "gui/TitleBar.h"
#include "core/LogManager.h"
#include "gui/AppletPanel.h"
#include "gui/PhoneCwApplet.h"
#include "gui/ClientChainApplet.h"
#include "models/TransmitModel.h"
#include <algorithm>
#include <cmath>
#include "core/AppSettings.h"
#include "core/SliceAudioRouting.h"
#include "gui/AudioRoutingDialog.h"
#include "gui/mainwindow/DaxController.h"
#if defined(HAVE_DAX_BRIDGE)
#include "core/VirtualAudioBridge.h"
#endif
#include "models/SliceModel.h"

namespace AetherSDR {

AudioRoutingController::AudioRoutingController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void AudioRoutingController::wireTxAudioStreams()
{
    // ── TX audio stream: set stream ID for DAX TX path ──────────────────
    // DAX TX audio is sent via PanadapterStream::sendToRadio() (the
    // registered VITA-49 socket).  We do NOT start a separate mic TX
    // stream — that would open a QAudioSource and an unregistered UDP
    // socket, wasting resources and corrupting the shared packet counter.
    // Route TX VITA-49 packets through the registered UDP socket
    connect(m_mw.m_audio, &AudioEngine::txPacketReady,
            m_mw.m_radioModel.panStream(), &PanadapterStream::sendToRadio);

    // RADE mode asks for its preferred DAX TX routing on the radio side.
    // Emitted from AudioEngine::setRadeMode() so toggling the RADE
    // button does the whole routing consolidation in one place.
    connect(m_mw.m_audio, &AudioEngine::daxRouteRequested, this,
            [this](int daxValue) {
        m_mw.m_radioModel.sendCommand(
            QString("transmit set dax=%1").arg(daxValue));
    });

    connect(&m_mw.m_radioModel, &RadioModel::txAudioStreamReady,
            this, [this](quint32 streamId) {
        m_mw.m_audio->setTxStreamId(streamId);
        // TX audio on remote_audio_tx always requires Opus (radio enforces compression=OPUS)
        m_mw.m_audio->setOpusTxEnabled(true);
        qCDebug(lcGui) << "MainWindow: DAX TX stream ID set to" << Qt::hex << streamId;
        // Start PC audio TX if mic_selection is PC
        if (m_mw.m_radioModel.transmitModel().micSelection() == "PC") {
            m_mw.audioStartTx(m_mw.m_radioModel.radioAddress(), 4991);
        }
    });
    connect(&m_mw.m_radioModel, &RadioModel::remoteTxStreamReady,
            this, [this](quint32 streamId) {
        m_mw.m_audio->setRemoteTxStreamId(streamId);
        // Radio always forces Opus for remote_audio_tx (confirmed v1.4.0.0)
        m_mw.m_audio->setOpusTxEnabled(true);
        // Only the PC mic path needs local audio capture. For radio-side mic
        // selections, remote_audio_tx still exists for VOX/met_in_rx, but the
        // radio owns the actual input path. Starting QAudioSource here on
        // macOS pins Bluetooth output in telephony mode.
        if (m_mw.m_radioModel.transmitModel().micSelection() == "PC") {
            // Restore PC mic gain from client-side settings (radio has no
            // hardware gain stage for PC input — client-authoritative)
            int gain = AppSettings::instance().value("PcMicGain", 100).toInt();
            m_mw.m_audio->setPcMicGain(gain);
            if (!m_mw.m_audio->isTxStreaming()) {
                m_mw.audioStartTx(m_mw.m_radioModel.radioAddress(), 4991);
            }
        } else if (m_mw.m_audio->isTxStreaming()) {
            m_mw.audioStopTx();
        }
        qCDebug(lcGui) << "MainWindow: remote audio TX stream ID set to" << Qt::hex << streamId;
    });
    // Start/stop PC audio TX when mic_selection changes
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::micStateChanged,
            this, [this]() {
#ifdef Q_OS_MAC
        const bool allowBluetoothTelephonyOutput =
            m_mw.m_radioModel.transmitModel().micSelection() == "PC";
        QMetaObject::invokeMethod(m_mw.m_audio, [this, allowBluetoothTelephonyOutput]() {
            m_mw.m_audio->setAllowBluetoothTelephonyOutput(allowBluetoothTelephonyOutput);
        }, Qt::QueuedConnection);
#endif
        if (m_mw.m_radioModel.transmitModel().micSelection() == "PC") {
            // Restore PC mic gain from client-side settings
            int gain = AppSettings::instance().value("PcMicGain", 100).toInt();
            m_mw.m_audio->setPcMicGain(gain);
            // Only start if TX stream ID is already assigned (avoid streamId=0)
            if (!m_mw.m_audio->isTxStreaming() && m_mw.m_audio->txStreamId() != 0) {
                m_mw.audioStartTx(m_mw.m_radioModel.radioAddress(), 4991);
            }
        } else {
            // Reset to full gain — radio handles hardware mic gain
            m_mw.m_audio->setPcMicGain(100);
            m_mw.audioStopTx();
        }
        // PooDoo Audio readiness indicator — turn the chain widget's
        // MIC endpoint green when the TX input is actually flowing
        // through the client DSP chain: mic source = PC AND radio
        // DAX TX is off.  Any other combination routes audio around
        // our DSP, so the green cue would mislead.  The TX pulse
        // is gated on the same readiness so it only fires when
        // PooDoo is actually doing something during transmit.
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet()) {
            const auto& tx = m_mw.m_radioModel.transmitModel();
            const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
            m_mw.m_appletPanel->clientChainApplet()->setMicInputReady(ready);
            m_mw.m_appletPanel->clientChainApplet()->setTxActive(
                ready && tx.isTransmitting());

            // If the user pulls the plug on readiness mid-recording
            // (mic source away from PC, or DAX back on), stop the
            // recording — auto-play kicks in via recordingStopped.
            if (!ready && m_mw.m_puduMonitor && m_mw.m_puduMonitor->isRecording()) {
                m_mw.m_puduMonitor->stopRecording();
            }
        }
    });
#ifdef Q_OS_MAC
    const bool allowBluetoothTelephonyOutput =
        m_mw.m_radioModel.transmitModel().micSelection() == "PC";
    QMetaObject::invokeMethod(m_mw.m_audio, [this, allowBluetoothTelephonyOutput]() {
        m_mw.m_audio->setAllowBluetoothTelephonyOutput(allowBluetoothTelephonyOutput);
    }, Qt::QueuedConnection);
#endif
    // Sync PC mic gain directly from slider (radio ignores mic_level for PC input)
    connect(m_mw.m_appletPanel->phoneCwApplet(), &PhoneCwApplet::micLevelChanged,
            this, [this](int level) {
        if (m_mw.m_radioModel.transmitModel().micSelection() == "PC") {
            m_mw.m_audio->setPcMicGain(level);
            auto& s = AppSettings::instance();
            s.setValue("PcMicGain", level);
            s.save();
        }
    });
}

void AudioRoutingController::wireRxAudioTaps()
{
    // ── Panadapter stream → audio engine ──────────────────────────────────
    // All VITA-49 traffic arrives on the single client udpport socket owned
    // by PanadapterStream. It strips the header from IF-Data packets and emits
    // audioDataReady(); we feed that directly to the QAudioSink.
    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            m_mw.m_audio, &AudioEngine::feedAudioData);

    // ── QSO recorder: tap RX + TX audio, trigger on MOX (#1297) ───────
    // RX: 24 kHz stereo float32 from the radio. TX: AudioEngine::txRawPcmReady
    // (24 kHz stereo float32 mic audio). While MOX is on and TX audio is
    // arriving, QsoRecorder suppresses RX writes so the timeline doesn't
    // double. Both feeds queue to the recorder's (main) thread.
    // txRawPcmReady covers RADE; txProcessedPcmReady covers voice modes
    // (post client TX DSP, int16). DAX TX audio comes from external apps
    // and is not recorded.
    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            m_mw.m_qsoRecorder, &QsoRecorder::feedRxAudio);
    connect(m_mw.m_audio, &AudioEngine::txRawPcmReady,
            m_mw.m_qsoRecorder, &QsoRecorder::feedTxAudio, Qt::QueuedConnection);
    connect(m_mw.m_audio, &AudioEngine::txProcessedPcmReady, m_mw.m_qsoRecorder,
            [this](const QByteArray& pcm) { m_mw.m_qsoRecorder->feedTxPcm(pcm, 24000, 2, false); },
            Qt::QueuedConnection);
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_mw.m_qsoRecorder, &QsoRecorder::onMoxChanged);
}

void AudioRoutingController::wireTitleBarAudio()
{
    // ── Title bar: PC Audio, master volume, headphone volume ────────────────
    // The remote_audio_rx stream controls the radio's audio routing:
    // stream exists → audio to PC; stream removed → audio to radio speakers.
    // Keep the stream alive when TCI clients need it (#1014).
    connect(m_mw.m_titleBar, &TitleBar::pcAudioToggled, this, [this](bool on) {
        if (on) {
            // Restart the local audio sink to recover from stale WASAPI sessions
            // (e.g. after Teams/Zoom reconfigures the audio endpoint). (#1569)
            QMetaObject::invokeMethod(m_mw.m_audio, [this]() {
                m_mw.m_audio->stopRxStream();
                m_mw.m_audio->startRxStream();
            });
            m_mw.m_radioModel.createRxAudioStream();
        } else {
            // Stop the local sink so audio isn't played locally even when
            // the remote_audio_rx stream is still alive for a TCI client
            // (#1571 follow-up).
            QMetaObject::invokeMethod(m_mw.m_audio, [this]() {
                m_mw.m_audio->stopRxStream();
            });
            m_mw.m_radioModel.removeRxAudioStream();
        }
        // Master slider controls a different target in each mode.
        syncMasterVolumeControl();
    });
    connect(m_mw.m_titleBar, &TitleBar::masterVolumeChanged,
            this, &AudioRoutingController::applyMasterVolume);
    connect(m_mw.m_titleBar, &TitleBar::headphoneVolumeChanged,
            &m_mw.m_radioModel, &RadioModel::setHeadphoneGain);
    connect(m_mw.m_titleBar, &TitleBar::lineoutMuteChanged, this, [this](bool muted) {
        m_mw.m_audio->setMuted(muted);
        m_mw.m_radioModel.sendCommand(QString("mixer lineout mute %1").arg(muted ? 1 : 0));
        auto& s = AppSettings::instance();
        s.setValue("PcAudioMuted", muted ? "True" : "False");
        s.save();
    });
    connect(m_mw.m_titleBar, &TitleBar::headphoneMuteChanged, this, [this](bool muted) {
        m_mw.m_radioModel.sendCommand(QString("mixer headphone mute %1").arg(muted ? 1 : 0));
    });
    connect(&m_mw.m_radioModel, &RadioModel::audioOutputChanged, this, [this]() {
        m_mw.m_titleBar->setHeadphoneVolume(m_mw.m_radioModel.headphoneGain());
        syncMasterVolumeControl();  // lineout_gain may have changed (PC Audio off)
    });

    // Multi-Flex: show when another client is transmitting
    connect(&m_mw.m_radioModel, &RadioModel::txOwnerChanged,
            m_mw.m_titleBar, &TitleBar::setOtherClientTx);

    // Multi-Flex: title bar indicator when other clients are connected
    connect(&m_mw.m_radioModel, &RadioModel::otherClientsChanged,
            m_mw.m_titleBar, &TitleBar::setMultiFlexStatus);

    // Apply saved master volume
    int savedMasterVol = AppSettings::instance().value("MasterVolume", "100").toInt();
    m_mw.m_audio->setRxVolume(savedMasterVol / 100.0f);
    syncMasterVolumeControl();

    // Restore saved mute state (#1571)
    bool savedMute = AppSettings::instance().value("PcAudioMuted", "False").toString() == "True";
    if (savedMute) {
        m_mw.m_audio->setMuted(true);
        m_mw.m_titleBar->setLineoutMuted(true);
    }
}

// ── Master volume routing (title bar + MIDI) ────────────────────────────────
// PC Audio on  → client-side AudioEngine RX volume (persisted as MasterVolume)
// PC Audio off → radio "mixer lineout gain" (radio-authoritative, not persisted)
static bool pcAudioEnabledSetting()
{
    return AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
}

void AudioRoutingController::applyMasterVolume(int pct)
{
    pct = std::clamp(pct, 0, 100);
    if (pcAudioEnabledSetting()) {
        m_mw.m_audio->setRxVolume(pct / 100.0f);
        auto& s = AppSettings::instance();
        s.setValue("MasterVolume", QString::number(pct));
        s.save();
    } else {
        m_mw.m_radioModel.setLineoutGain(pct);
    }
}

int AudioRoutingController::masterVolumePercent() const
{
    if (pcAudioEnabledSetting()) {
        return static_cast<int>(std::lround(m_mw.m_audio->rxVolume() * 100.0f));
    }
    return m_mw.m_radioModel.lineoutGain();
}

void AudioRoutingController::syncMasterVolumeControl()
{
    if (m_mw.m_titleBar) {
        // TitleBar::setMasterVolume blocks signals — no feedback loop.
        m_mw.m_titleBar->setMasterVolume(masterVolumePercent());
    }
}

// ── Per-slice RX audio routing ──────────────────────────────────────────────
// Default (no SliceAudioRoute_* keys): nothing is connected and AudioEngine
// has no extra sinks — identical to the pre-routing behaviour.

void AudioRoutingController::showSliceAudioRoutingDialog()
{
    if (!m_sliceRoutingDialog) {
        m_sliceRoutingDialog = new AudioRoutingDialog(&m_mw.m_radioModel, &m_mw);
        connect(m_sliceRoutingDialog, &AudioRoutingDialog::routesChanged,
                this, &AudioRoutingController::applySliceAudioRouting);
    }
    applySliceAudioRouting();   // refresh warnings
    m_sliceRoutingDialog->show();
    m_sliceRoutingDialog->raise();
}

void AudioRoutingController::applySliceAudioRouting()
{
    const QMap<QChar, SliceAudioRoute> routes = AudioRoutingDialog::loadRoutes();
    QVector<SliceAudioInfo> infos;
    for (SliceModel* s : m_mw.m_radioModel.slices()) {
        infos.append({s->sliceId(), QChar(QLatin1Char(static_cast<char>('A' + s->sliceId() % 26))),
                      s->daxChannel()});
    }
#if defined(HAVE_DAX_BRIDGE)
    DaxBridge* bridge = m_mw.m_daxCtl ? m_mw.m_daxCtl->daxBridge() : nullptr;
#else
    void* bridge = nullptr;
#endif
    const SliceAudioPlan plan = planSliceAudioRouting(routes, infos, bridge != nullptr);

    // Radio-side mute for "not on main output" (the mixed stream cannot be
    // un-mixed).  Remember the user's mute so it can be restored; never
    // persisted (audio_mute is radio-authoritative).
    QSet<int> wantMuted;
    for (const SliceAudioPlanEntry& e : plan.entries) {
        if (e.removeFromMain) {
            wantMuted.insert(e.sliceId);
        }
    }
    for (auto it = m_routingMutedSlices.begin(); it != m_routingMutedSlices.end();) {
        if (!wantMuted.contains(it.key())) {
            if (SliceModel* s = m_mw.m_radioModel.slice(it.key())) {
                s->setAudioMute(it.value());
            }
            it = m_routingMutedSlices.erase(it);
        } else {
            ++it;
        }
    }
    for (int id : wantMuted) {
        SliceModel* s = m_mw.m_radioModel.slice(id);
        if (s && !m_routingMutedSlices.contains(id)) {
            m_routingMutedSlices.insert(id, s->audioMute());
            s->setAudioMute(true);
        }
    }

    // Runtime table + client-side mirror of audio_level / audio_pan.
    QVector<SliceRouteRuntime> runtime;
    for (QMetaObject::Connection& c : m_sliceGainConns) {
        disconnect(c);
    }
    m_sliceGainConns.clear();
    for (const SliceAudioPlanEntry& e : plan.entries) {
        SliceModel* s = m_mw.m_radioModel.slice(e.sliceId);
        if (!s) {
            continue;
        }
        auto gainFor = [this, id = e.sliceId](SliceModel* sl) {
            const bool userMute = m_routingMutedSlices.contains(id)
                                      ? m_routingMutedSlices.value(id) : sl->audioMute();
            return sliceOutputGains(sl->audioGain(), sl->audioPan(), userMute);
        };
        runtime.append({e.sliceId, e.sourceDaxChannel, e.outputDevice, e.bridgeChannel, gainFor(s)});
        auto push = [this, s, gainFor, id = e.sliceId] {
            const StereoGain g = gainFor(s);
            QMetaObject::invokeMethod(m_mw.m_audio, [audio = m_mw.m_audio, id, g] {
                audio->setSliceRouteGain(id, g);
            });
        };
        m_sliceGainConns << connect(s, &SliceModel::audioGainChanged, this, push)
                         << connect(s, &SliceModel::audioPanChanged, this, push)
                         << connect(s, &SliceModel::audioMuteChanged, this, push);
        ensureSliceDaxStream(e.sourceDaxChannel);
    }
    QMetaObject::invokeMethod(m_mw.m_audio, [audio = m_mw.m_audio, runtime] {
        audio->setSliceAudioRoutes(runtime);
    });

    // Only connect the DAX tap when something is routed.
    disconnect(m_sliceDaxFeedConn);
    disconnect(m_sliceBridgeConn);
    if (!runtime.isEmpty()) {
        m_sliceDaxFeedConn = connect(m_mw.m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
                                     m_mw.m_audio, &AudioEngine::feedSliceDaxAudio,
                                     Qt::QueuedConnection);
#if defined(HAVE_DAX_BRIDGE)
        m_sliceBridgeConn = connect(m_mw.m_audio, &AudioEngine::sliceDaxFeedReady, this,
                                    [this](int ch, const QByteArray& pcm) {
            if (DaxBridge* b = m_mw.m_daxCtl ? m_mw.m_daxCtl->daxBridge() : nullptr) {
                b->feedDaxAudio(ch, pcm);
            }
        }, Qt::QueuedConnection);
#endif
    }
    if (!plan.warnings.isEmpty()) {
        qCInfo(lcAudio) << "Slice audio routing:" << plan.warnings;
    }
    if (m_sliceRoutingDialog) {
        m_sliceRoutingDialog->setPlanWarnings(plan.warnings);
    }
}

void AudioRoutingController::ensureSliceDaxStream(int ch)
{
    // Same as DigitalVoiceController::ensureDigitalVoiceDaxStream: the DAX
    // bridge (if running) already streams every assigned channel.
    if (ch < 1 || ch > 4 || m_mw.m_radioModel.panStream()->daxStreamIdForChannel(ch) != 0) {
        return;
    }
    disconnect(m_sliceDaxStatusConn);
    m_sliceDaxStatusConn = connect(&m_mw.m_radioModel, &RadioModel::statusReceived, this,
            [this](const QString& obj, const QMap<QString, QString>& kvs) {
        if (!obj.startsWith("stream ") || kvs.value("type") != "dax_rx") {
            return;
        }
        const int c = kvs.value("dax_channel").toInt();
        const quint32 streamId = obj.mid(7).toUInt(nullptr, 16);
        if (streamId && c >= 1 && c <= 4
            && m_mw.m_radioModel.panStream()->daxStreamIdForChannel(c) == 0) {
            m_mw.m_radioModel.panStream()->registerDaxStream(streamId, c);
        }
    });
    m_mw.m_radioModel.sendCommand(QString("stream create type=dax_rx dax_channel=%1").arg(ch));
}

void AudioRoutingController::restoreSliceRoutingMutes()
{
    for (auto it = m_routingMutedSlices.cbegin(); it != m_routingMutedSlices.cend(); ++it) {
        if (SliceModel* s = m_mw.m_radioModel.slice(it.key())) {
            s->setAudioMute(it.value());
        }
    }
    m_routingMutedSlices.clear();
}

void AudioRoutingController::wireSliceRoutingTriggers()
{
    // Coalesce bursts (slice list on connect) into one apply.
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this] {
        if (m_mw.m_radioModel.isConnected()) {
            applySliceAudioRouting();
        }
    });
    auto schedule = [timer] { timer->start(); };
    connect(&m_mw.m_radioModel, &RadioModel::sliceAdded, this, [this, schedule](SliceModel* s) {
        connect(s, &SliceModel::daxChannelChanged, this, [schedule](int) { schedule(); });
        schedule();
    });
    connect(&m_mw.m_radioModel, &RadioModel::sliceRemoved, this, [schedule](int) { schedule(); });
    connect(&m_mw.m_radioModel, &RadioModel::connectionStateChanged, this,
            [this, schedule](bool connected) {
        if (connected) {
            schedule();
        } else {
            // The radio keeps any routing mute after an unexpected disconnect;
            // forget our bookkeeping so a reconnect re-evaluates from scratch.
            m_routingMutedSlices.clear();
        }
    });
}

} // namespace AetherSDR
