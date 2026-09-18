#include "gui/mainwindow/DigitalVoiceController.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/DaxController.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "gui/SpectrumWidget.h"
#include "gui/VfoWidget.h"
#include "gui/RxApplet.h"
#include "models/SliceModel.h"
#include "models/BandDefs.h"
#include <memory>
#include <atomic>
#include <cmath>
#include <QTimer>
#include <QMessageBox>
#include <QThread>
#include "core/AppSettings.h"
#ifdef HAVE_RADE
#include "core/RADEEngine.h"
#endif
#ifdef HAVE_DIGITAL_VOICE
#include "core/FreeDvEngine.h"
#endif
#include "core/FreeDvReporterProtocol.h"
#include "gui/DigitalVoiceDialog.h"

namespace AetherSDR {

DigitalVoiceController::DigitalVoiceController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

#ifdef HAVE_WEBSOCKETS
#ifdef HAVE_DIGITAL_VOICE
void DigitalVoiceController::onFreeDvReporterStopped()
{
    // Connection closed elsewhere (menu/Spot Hub): publishing stops too.
    if (m_dvReporting) {
        m_dvReporterOpenedConnection = false;
        setDigitalVoiceReporting(false);
        if (m_dvDialog) {
            m_dvDialog->setReportingState(false);
        }
    }
}
#endif
#endif

#ifdef HAVE_DIGITAL_VOICE
void DigitalVoiceController::updateForMox(bool tx)
{
    if (digitalVoiceActive()) {
        // Only a DV slice that IS the TX slice routes mic → engine (same
        // rule as updateDaxTxMode); otherwise MOX on another slice's
        // voice mode would be hijacked into the modem.
        const auto* dvSlice = m_mw.m_radioModel.slice(m_dvSliceId);
        const bool dvTx = tx && dvSlice && dvSlice->isTxSlice();
        // setRadeMode() clears TX buffers owned by the audio thread.
        // (Name kept for history: it gates mic → digital voice engine for
        // every digital voice mode, not only RADE.)
        QMetaObject::invokeMethod(m_mw.m_audio, [this, dvTx]() { m_mw.m_audio->setRadeMode(dvTx); });
        if (!tx) {
            // Engine lives on its worker thread — queue, never call directly.
            QMetaObject::invokeMethod(m_dvEngine, &DigitalVoiceEngine::resetTx,
                                      Qt::QueuedConnection);
        }
#ifdef HAVE_WEBSOCKETS
        if (m_dvReporting && m_mw.m_freedvClient) {
            const QString modeName = digitalVoiceReporterModeName(m_dvMode);
            QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, modeName, dvTx] {
                m_mw.m_freedvClient->reportTransmit(modeName, dvTx);
            });
        }
#endif
    }
}

bool DigitalVoiceController::deferUnkeyForEndOfOver(std::function<void()> unkey)
{
    const auto* dvSlice = m_mw.m_radioModel.slice(m_dvSliceId);
    if (!digitalVoiceActive() || m_dvMode != DigitalVoiceMode::RadeV1
        || !dvSlice || !dvSlice->isTxSlice()) {
        return false;
    }
    // EOO frame = rade_n_tx_eoo_out() samples at 8 kHz (1152 → 144 ms) plus
    // RADEEngine::kEooSilenceTailMs; allow 50 ms for the DAX TX UDP path.
    constexpr int kEooPlayoutMs = 144 + RADEEngine::kEooSilenceTailMs + 50;
    auto done = std::make_shared<bool>(false);
    auto fire = [done, unkey] {
        if (!*done) {
            *done = true;
            unkey();
        }
    };
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_dvEngine, &DigitalVoiceEngine::endOfOverSent, this, [conn, fire] {
        disconnect(*conn);
        QTimer::singleShot(kEooPlayoutMs, fire);
    }, Qt::QueuedConnection);
    // Safety net: never hold the transmitter if the engine does not answer.
    QTimer::singleShot(1000, this, fire);
    QMetaObject::invokeMethod(m_dvEngine, &DigitalVoiceEngine::requestEndOfOver,
                              Qt::QueuedConnection);
    return true;
}

void DigitalVoiceController::shutdownDigitalVoice()
{
    if (m_dvSliceId >= 0 || m_dvEngine) {
        deactivateDigitalVoice();
    }
}

void DigitalVoiceController::applyVfoIndicator(VfoWidget* vfo, SliceModel* s)
{
    // Slice moved to another pan while running digital voice: the recreated
    // VFO must show the DV indicator (and later be cleared on deactivation).
    if (digitalVoiceActive() && s->sliceId() == m_dvSliceId) {
        vfo->setDigitalVoiceTag(digitalVoiceModeTag(m_dvMode));
        vfo->setRadeActive(true);
        vfo->setRadeSynced(false);
    }
}
#endif

#if defined(HAVE_DAX_BRIDGE)
#ifdef HAVE_DIGITAL_VOICE
void DigitalVoiceController::onTxSliceChanged(int txSliceId)
{
    // Digital voice should only route mic→engine when the TX slice IS
    // the digital voice slice.  Otherwise a DV slice running on a non-TX
    // slice would hijack voice TX on the actual TX slice.
    if (digitalVoiceActive()) {
        const bool rade = (txSliceId == m_dvSliceId);
        QMetaObject::invokeMethod(m_mw.m_audio, [this, rade]() { m_mw.m_audio->setRadeMode(rade); });
    }
}
#endif
#endif

#ifdef HAVE_DIGITAL_VOICE
void DigitalVoiceController::handleSliceRemoved(int id)
{
    // If the digital voice slice was closed, deactivate the engine — even
    // during a layout rebuild, otherwise the engine keeps running on a slice
    // that no longer exists.
    if (id == m_dvSliceId) {
        deactivateDigitalVoice();
    }
}
#endif

#ifdef HAVE_DIGITAL_VOICE
void DigitalVoiceController::onRadeSelection(bool on, int sliceId)
{
    if (on) {
        activateDigitalVoice(sliceId, DigitalVoiceMode::RadeV1);
        return;
    }
    // Mode combos emit radeActivated(false, id) on every non-RADE selection;
    // only tear down when that slice is the one running digital voice.
    if (m_dvSliceId >= 0 && (sliceId < 0 || sliceId == m_dvSliceId)) {
        deactivateDigitalVoice();
    }
}

QString DigitalVoiceController::digitalVoiceTxCallsign() const
{
    QString cs = AppSettings::instance().value("FreeDvTxCallsign", QString()).toString().trimmed();
    if (cs.isEmpty()) {
        cs = m_mw.m_radioModel.callsign().trimmed();
    }
    return cs.toUpper();
}

void DigitalVoiceController::activateDigitalVoice(int sliceId, DigitalVoiceMode mode)
{
    // Guard against duplicate activation (VfoWidget + RxApplet both selecting RADE)
    if (m_dvSliceId == sliceId && m_dvMode == mode && digitalVoiceActive()) {
        return;
    }

    auto* s = m_mw.m_radioModel.slice(sliceId);
    if (!s) {
        return;
    }
    if (!availableDigitalVoiceModes().contains(mode)) {
        qCWarning(lcGui) << "MainWindow: digital voice mode not available in this build:"
                         << digitalVoiceModeName(mode);
        return;
    }

    // One engine at a time: tear down whatever is running (other slice or mode)
    if (m_dvSliceId >= 0 || m_dvEngine) {
        deactivateDigitalVoice();
    }

    DigitalVoiceEngine* engine = nullptr;
    if (mode == DigitalVoiceMode::RadeV1) {
#ifdef HAVE_RADE
        auto* rade = new RADEEngine;  // no parent — moved to worker thread
        rade->setTxText(digitalVoiceTxCallsign());  // sent in the EOO frame
        engine = rade;
#endif
    } else if (isCodec2Mode(mode) && FreeDvEngine::isSupported()) {
        auto* fdv = new FreeDvEngine(mode);  // no parent — moved to worker thread
        auto& as = AppSettings::instance();
        fdv->setTxText(digitalVoiceTxCallsign());
        fdv->setSquelch(as.value("FreeDvSquelchEnabled", "True").toString() == "True",
                        as.value("FreeDvSquelchSnrDb", "0.0").toString().toFloat());
        engine = fdv;
    }
    if (!engine) {
        qCWarning(lcGui) << "MainWindow: digital voice mode not available in this build:"
                         << digitalVoiceModeName(mode);
        return;
    }

    // Digital voice needs to be the TX slice so it can transmit modem audio.
    // Move TX badge to the DV slice automatically.
    if (!s->isTxSlice()) {
        s->setTxSlice(true);
    }

    // Set radio mode to DIGU/DIGL (passthrough for the modem).
    // RADE: band convention from BandDefs picks the sideband — 60m is USB
    // despite being below 10 MHz (#875).  Unchanged from the original
    // activateRADE().  Codec2 FreeDV modes are USB on every band by FreeDV
    // convention (freedv-gui), so always DIGU.
    QString radioMode = "DIGU";
    if (mode == DigitalVoiceMode::RadeV1) {
        double freqMhz = s->frequency();
        for (const auto& band : AetherSDR::kBands) {
            if (freqMhz >= band.lowMhz && freqMhz <= band.highMhz) {
                radioMode = (QString(band.defaultMode) == "LSB") ? "DIGL" : "DIGU";
                break;
            }
        }
    }
    // Save what activation changes so deactivation restores the slice exactly.
    m_dvPrevMode = s->mode();
    m_dvPrevFilterLo = s->filterLow();
    m_dvPrevFilterHi = s->filterHigh();
    m_dvPrevDaxChannel = s->daxChannel();

    s->setMode(radioMode);
    if (radioMode == "DIGU" || radioMode == "DIGL") {
        if (radioMode == "DIGL") {
            s->setFilterWidth(-3500, 0);
        } else {
            s->setFilterWidth(0, 3500);
        }
    }

    // The engine is fed from DAX RX audio. Previously this only worked if the
    // DAX bridge was already running with a stream for the slice's channel;
    // make activation self-sufficient.
    ensureDigitalVoiceDaxStream(s);

    // Remember which slice and its previous mute state
    m_dvSliceId = sliceId;
    m_dvMode = mode;
    m_dvPrevMute = s->audioMute();
    m_dvLastReportedCall.clear();
    m_dvLastReportedFreqHz = 0;
    s->setAudioMute(true);

    // Engine on a worker thread for multi-core utilization
    m_dvEngine = engine;
    m_dvThread = new QThread(&m_mw);
    m_dvThread->setObjectName(mode == DigitalVoiceMode::RadeV1 ? "RADEEngine" : "FreeDvEngine");
    m_dvEngine->moveToThread(m_dvThread);
    connect(m_dvThread, &QThread::finished, m_dvEngine, &QObject::deleteLater);
    m_dvThread->start();

    // start() must be invoked on the worker thread
    bool ok = false;
    QMetaObject::invokeMethod(m_dvEngine, [this, &ok]() {
        ok = m_dvEngine->start();
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        qCWarning(lcGui) << "MainWindow: failed to start digital voice engine"
                         << digitalVoiceModeName(mode);
        deactivateDigitalVoice();
        return;
    }

    // Only route mic→engine when the DV slice IS the TX slice.
    // If another slice is TX (e.g. USB voice), leave its audio path alone.
    {
        const bool rade = s->isTxSlice();
        QMetaObject::invokeMethod(m_mw.m_audio, [this, rade]() { m_mw.m_audio->setRadeMode(rade); });
    }

    // TX path: mic -> engine (worker) -> sendModemTxAudio (audio thread)
    connect(m_mw.m_audio, &AudioEngine::txRawPcmReady,
            m_dvEngine, &DigitalVoiceEngine::feedTxAudio, Qt::QueuedConnection);
    connect(m_dvEngine, &DigitalVoiceEngine::txModemReady,
            m_mw.m_audio, &AudioEngine::sendModemTxAudio, Qt::QueuedConnection);

    // RX path: DAX RX audio -> engine (worker) -> decoded speech -> speaker.
    // Filter by the DV slice's DAX channel so other slices' DAX audio is
    // ignored.  The channel is mirrored into an atomic on the main thread so
    // the worker never touches SliceModel.
    auto daxCh = std::make_shared<std::atomic<int>>(s->daxChannel());
    connect(s, &SliceModel::daxChannelChanged, m_dvEngine, [daxCh](int ch) {
        daxCh->store(ch);
    }, Qt::DirectConnection);
    DigitalVoiceEngine* eng = m_dvEngine;
    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
            m_dvEngine, [eng, daxCh](int channel, const QByteArray& pcm) {
        if (channel == daxCh->load()) {
            eng->feedRxAudio(channel, pcm);
        }
    }, Qt::QueuedConnection);
    connect(m_dvEngine, &DigitalVoiceEngine::rxSpeechReady,
            m_mw.m_audio, &AudioEngine::feedDecodedSpeech, Qt::QueuedConnection);

    // Start mic capture if not already running
    if (!m_mw.m_audio->isTxStreaming()) {
        m_mw.audioStartTx(m_mw.m_radioModel.radioAddress(), 4991);
    }

    // Status fan-out (queued from worker): VFO indicator, dialog, reporter.
    // Use vfoWidget(sliceId) — the no-arg alias (m_vfoWidget) may be null
    // if setActiveVfoWidget() hasn't been called yet for this slice.
    // spectrumForSlice(): the DV slice may live on a pan other than the
    // active one (multi-pan).
    if (auto* sw = m_mw.spectrumForSlice(s)) {
        if (auto* vfo = sw->vfoWidget(sliceId)) {
            vfo->setDigitalVoiceTag(digitalVoiceModeTag(mode));
            vfo->setRadeActive(true);
            // Show initial unsynchronised state immediately — syncChanged only fires
            // once DAX audio is flowing.
            vfo->setRadeSynced(false);
        }
    }
    const auto vfoForDv = [this]() -> VfoWidget* {
        auto* dvs = m_mw.m_radioModel.slice(m_dvSliceId);
        auto* sw = dvs ? m_mw.spectrumForSlice(dvs) : nullptr;
        return sw ? sw->vfoWidget(m_dvSliceId) : nullptr;
    };
    connect(m_dvEngine, &DigitalVoiceEngine::syncChanged, this, [this, vfoForDv](bool synced) {
        if (auto* vfo = vfoForDv()) {
            vfo->setRadeSynced(synced);
        }
        if (m_dvDialog) {
            m_dvDialog->setSynced(synced);
        }
#ifdef HAVE_WEBSOCKETS
        if (!synced && m_mw.m_freedvClient) {
            const QString modeName = digitalVoiceReporterModeName(m_dvMode);
            QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, modeName] {
                m_mw.m_freedvClient->setRxSyncState(false, modeName, 0);
            });
        }
#endif
    });
    connect(m_dvEngine, &DigitalVoiceEngine::snrChanged, this, [this, vfoForDv](float snr) {
        m_dvLastSnr = snr;
        if (auto* vfo = vfoForDv()) {
            vfo->setRadeSnr(snr);
        }
#ifdef HAVE_WEBSOCKETS
        // Engines emit snrChanged only while synced.
        if (m_dvReporting && m_mw.m_freedvClient) {
            const QString modeName = digitalVoiceReporterModeName(m_dvMode);
            const int snrInt = static_cast<int>(std::lround(snr));
            QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, modeName, snrInt] {
                m_mw.m_freedvClient->setRxSyncState(true, modeName, snrInt);
            });
        }
#endif
        if (m_dvDialog) {
            m_dvDialog->setSnr(snr);
        }
    });
    connect(m_dvEngine, &DigitalVoiceEngine::freqOffsetChanged, this, [this, vfoForDv](float hz) {
        if (auto* vfo = vfoForDv()) {
            vfo->setRadeFreqOffset(hz);
        }
        if (m_dvDialog) {
            m_dvDialog->setFreqOffset(hz);
        }
    });
    connect(m_dvEngine, &DigitalVoiceEngine::textReceived, this, [this](const QString& text) {
        if (m_dvDialog) {
            m_dvDialog->setReceivedText(text);
        }
#ifdef HAVE_WEBSOCKETS
        // rx_report only for a plausible callsign, de-duplicated against the
        // last report on the same frequency (freedv-gui behaviour).
        const QString call = text.trimmed().toUpper();
        auto* dvs = m_mw.m_radioModel.slice(m_dvSliceId);
        if (m_dvReporting && m_mw.m_freedvClient && dvs
            && FreeDvReporterProtocol::looksLikeCallsign(call)) {
            const auto hz = static_cast<quint64>(std::llround(dvs->frequency() * 1.0e6));
            if (call != m_dvLastReportedCall || hz != m_dvLastReportedFreqHz) {
                m_dvLastReportedCall = call;
                m_dvLastReportedFreqHz = hz;
                const QString modeName = digitalVoiceReporterModeName(m_dvMode);
                const int snr = static_cast<int>(std::lround(m_dvLastSnr));
                QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, call, modeName, snr] {
                    m_mw.m_freedvClient->reportRx(call, modeName, snr);
                });
            }
        }
#endif
    });

    // Reporter: follow the DV slice frequency
    m_dvFreqConn = connect(s, &SliceModel::frequencyChanged, this, [this](double) {
        if (m_dvFreqReportTimer) {
            m_dvFreqReportTimer->start();
        }
    });
    reportDigitalVoiceFrequency();
#ifdef HAVE_WEBSOCKETS
    if (m_dvReporting && m_mw.m_freedvClient) {
        const QString modeName = digitalVoiceReporterModeName(mode);
        QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, modeName] {
            m_mw.m_freedvClient->reportTransmit(modeName, false);
            m_mw.m_freedvClient->showOurselves();   // modem started: show_self
        });
    }
#endif

    if (m_dvDialog) {
        m_dvDialog->setActiveState(sliceId, mode, true);
    }

    qCInfo(lcGui) << "MainWindow: digital voice" << digitalVoiceModeName(mode)
                  << "activated on slice" << sliceId;
}

void DigitalVoiceController::ensureDigitalVoiceDaxStream(SliceModel* s)
{
    m_dvCreatedDaxStream = false;
    int ch = s->daxChannel();
    if (ch < 1 || ch > 4) {
        // Pick the first DAX channel no other slice uses.
        QSet<int> used;
        for (auto* other : m_mw.m_radioModel.slices()) {
            if (other != s) {
                used.insert(other->daxChannel());
            }
        }
        for (int c = 1; c <= 4; ++c) {
            if (!used.contains(c)) {
                ch = c;
                break;
            }
        }
        if (ch < 1 || ch > 4) {
            m_mw.statusBar()->showMessage("Digital voice: no free DAX channel for this slice", 5000);
            return;
        }
        s->setDaxChannel(ch);
    }
    m_dvDaxChannel = ch;
    if (m_mw.m_radioModel.panStream()->daxStreamIdForChannel(ch) != 0) {
        return;  // DAX bridge / TCI already streams this channel
    }
    // Register the stream from its status line (same as startDax()).
    disconnect(m_dvDaxStatusConn);
    m_dvDaxStatusConn = connect(&m_mw.m_radioModel, &RadioModel::statusReceived, this,
            [this, ch](const QString& obj, const QMap<QString, QString>& kvs) {
        if (!obj.startsWith("stream ") || kvs.value("type") != "dax_rx"
            || kvs.value("dax_channel").toInt() != ch) {
            return;
        }
        const quint32 streamId = obj.mid(7).toUInt(nullptr, 16);
        if (streamId && m_mw.m_radioModel.panStream()->daxStreamIdForChannel(ch) == 0) {
            m_mw.m_radioModel.panStream()->registerDaxStream(streamId, ch);
        }
    });
    m_mw.m_radioModel.sendCommand(QString("stream create type=dax_rx dax_channel=%1").arg(ch));
    m_dvCreatedDaxStream = true;
}

void DigitalVoiceController::deactivateDigitalVoice()
{
    // Restore audio mute state on the DV slice
    if (m_dvSliceId >= 0) {
        if (auto* s = m_mw.m_radioModel.slice(m_dvSliceId)) {
            s->setAudioMute(m_dvPrevMute);
            // Restore mode/filter/DAX channel changed by activation.
            if (!m_dvPrevMode.isEmpty() && s->mode() != m_dvPrevMode) {
                s->setMode(m_dvPrevMode);
            }
            if (m_dvPrevFilterLo != 0 || m_dvPrevFilterHi != 0) {
                s->setFilterWidth(m_dvPrevFilterLo, m_dvPrevFilterHi);
            }
            if (m_dvPrevDaxChannel >= 0 && s->daxChannel() != m_dvPrevDaxChannel) {
                s->setDaxChannel(m_dvPrevDaxChannel);
            }
        }
        disconnect(m_dvDaxStatusConn);
#if defined(HAVE_DAX_BRIDGE)
        const bool daxBridgeRunning = m_mw.m_daxCtl->isDaxBridgeRunning();
#else
        const bool daxBridgeRunning = false;
#endif
        if (m_dvCreatedDaxStream && !daxBridgeRunning) {
            if (const quint32 id = m_mw.m_radioModel.panStream()->daxStreamIdForChannel(m_dvDaxChannel)) {
                m_mw.m_radioModel.sendCommand(QString("stream remove 0x%1").arg(id, 0, 16));
                m_mw.m_radioModel.panStream()->unregisterDaxStream(id);
            }
        }
        m_dvCreatedDaxStream = false;
        m_dvPrevMode.clear();
        // Clear VFO status label before resetting sliceId — on every pan,
        // since the active pan may have changed since activation.
        if (m_mw.m_panStack) {
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                if (auto* vfo = applet->spectrumWidget()->vfoWidget(m_dvSliceId)) {
                    vfo->setRadeActive(false);
                }
            }
        } else if (auto* sw = m_mw.spectrum()) {
            if (auto* vfo = sw->vfoWidget(m_dvSliceId)) {
                vfo->setRadeActive(false);
            }
        }
        m_dvSliceId = -1;
    }
    if (m_dvFreqConn) {
        disconnect(m_dvFreqConn);
    }
    if (m_dvFreqReportTimer) {
        m_dvFreqReportTimer->stop();
    }

    QMetaObject::invokeMethod(m_mw.m_audio, [this]() {
        m_mw.m_audio->setRadeMode(false);
        m_mw.m_audio->clearTxAccumulators();  // flush stale modem data
    });

    if (m_dvEngine) {
        disconnect(m_mw.m_audio, &AudioEngine::txRawPcmReady, m_dvEngine, nullptr);
        disconnect(m_dvEngine, nullptr, m_mw.m_audio, nullptr);
        disconnect(m_mw.m_radioModel.panStream(), &PanadapterStream::daxAudioReady, m_dvEngine, nullptr);
        disconnect(m_dvEngine, nullptr, this, nullptr);
        for (auto* sl : m_mw.m_radioModel.slices()) {
            disconnect(sl, &SliceModel::daxChannelChanged, m_dvEngine, nullptr);
        }
        // Stop on the worker thread, then shut down the thread
        QMetaObject::invokeMethod(m_dvEngine, &DigitalVoiceEngine::stop,
                                  Qt::BlockingQueuedConnection);
        if (m_dvThread) {
            m_dvThread->quit();
            m_dvThread->wait(2000);
            m_dvThread->deleteLater();
            m_dvThread = nullptr;
        }
        m_dvEngine = nullptr;  // deleteLater (thread finished) handles deletion
    }
    const DigitalVoiceMode oldMode = m_dvMode;
    m_dvMode = DigitalVoiceMode::None;

#ifdef HAVE_WEBSOCKETS
    if (m_dvReporting && m_mw.m_freedvClient && oldMode != DigitalVoiceMode::None) {
        const QString modeName = digitalVoiceReporterModeName(oldMode);
        QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, modeName] {
            m_mw.m_freedvClient->reportTransmit(modeName, false);
            m_mw.m_freedvClient->setRxSyncState(false, modeName, 0);
            // freedv-gui MainFrame::stopRxStream(): hideFromView() → hide_self
            m_mw.m_freedvClient->hideFromView();
        });
    }
#endif

    if (m_dvDialog) {
        m_dvDialog->setActiveState(-1, DigitalVoiceMode::None, false);
    }

    qCInfo(lcGui) << "MainWindow: digital voice deactivated" << digitalVoiceModeName(oldMode);
}

void DigitalVoiceController::reportDigitalVoiceFrequency()
{
#ifdef HAVE_WEBSOCKETS
    if (!m_dvReporting || !m_mw.m_freedvClient || m_dvSliceId < 0) {
        return;
    }
    auto* s = m_mw.m_radioModel.slice(m_dvSliceId);
    if (!s) {
        return;
    }
    const auto hz = static_cast<quint64>(std::llround(s->frequency() * 1.0e6));
    QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, hz] {
        m_mw.m_freedvClient->reportFrequency(hz);
    });
#endif
}

void DigitalVoiceController::setDigitalVoiceReporting(bool on)
{
#ifdef HAVE_WEBSOCKETS
    if (!m_mw.m_freedvClient) {
        return;
    }
    const QString callsign = digitalVoiceTxCallsign();
    const QString grid = AppSettings::instance().value("StationGridSquare", QString()).toString().trimmed();
    if (on && (callsign.isEmpty() || grid.isEmpty())) {
        QMessageBox::warning(&m_mw, "FreeDV Reporter",
            "Reporting needs a callsign (TX callsign field) and a grid square "
            "(Radio Setup → station grid square).");
        if (m_dvDialog) {
            m_dvDialog->setReportingState(false);
        }
        return;
    }
    if (!m_dvFreqReportTimer) {
        m_dvFreqReportTimer = new QTimer(&m_mw);
        m_dvFreqReportTimer->setSingleShot(true);
        m_dvFreqReportTimer->setInterval(500);  // debounce tuning bursts
        connect(m_dvFreqReportTimer, &QTimer::timeout, this, &DigitalVoiceController::reportDigitalVoiceFrequency);
    }

    m_dvReporting = on;
    const QString version = QStringLiteral("AetherSDR %1").arg(QCoreApplication::applicationVersion());
    const bool wasConnected = m_mw.m_freedvClient->isConnected();
    const bool modemRunning = m_dvMode != DigitalVoiceMode::None;
    QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, on, callsign, grid, version, modemRunning] {
        // Visibility first so connection_successful sends hide_self or the
        // station state (FreeDVReporter::onConnectionSuccessful_).
        if (modemRunning) {
            m_mw.m_freedvClient->showOurselves();
        } else {
            m_mw.m_freedvClient->hideFromView();
        }
        m_mw.m_freedvClient->setReporting(on, callsign, grid, version);
    });
    if (on) {
        if (!wasConnected) {
            m_dvReporterOpenedConnection = true;
            QMetaObject::invokeMethod(m_mw.m_freedvClient, [this] { m_mw.m_freedvClient->startConnection(); });
        }
        reportDigitalVoiceFrequency();
        if (m_dvMode != DigitalVoiceMode::None) {
            const QString modeName = digitalVoiceReporterModeName(m_dvMode);
            QMetaObject::invokeMethod(m_mw.m_freedvClient, [this, modeName] {
                m_mw.m_freedvClient->reportTransmit(modeName, false);
            });
        }
    } else if (m_dvReporterOpenedConnection) {
        m_dvReporterOpenedConnection = false;
        QMetaObject::invokeMethod(m_mw.m_freedvClient, [this] { m_mw.m_freedvClient->stopConnection(); });
    }
    qCInfo(lcGui) << "MainWindow: FreeDV Reporter publishing" << (on ? "enabled" : "disabled");
#else
    Q_UNUSED(on);
#endif
}
#endif // HAVE_DIGITAL_VOICE

DigitalVoiceDialog* DigitalVoiceController::showDigitalVoiceDialog()
{
    if (!m_dvDialog) {
        m_dvDialog = new DigitalVoiceDialog(&m_mw.m_radioModel, &m_mw);
        m_dvDialog->setAttribute(Qt::WA_DeleteOnClose, false);
#ifdef HAVE_WEBSOCKETS
        m_dvDialog->setReportingAvailable(m_mw.m_freedvClient != nullptr);
#else
        m_dvDialog->setReportingAvailable(false);
#endif
#ifdef HAVE_DIGITAL_VOICE
        m_dvDialog->setReportingState(m_dvReporting);
        m_dvDialog->setActiveState(m_dvSliceId, m_dvMode, digitalVoiceActive());
        connect(m_dvDialog, &DigitalVoiceDialog::activateRequested,
                this, &DigitalVoiceController::activateDigitalVoice);
        connect(m_dvDialog, &DigitalVoiceDialog::deactivateRequested,
                this, &DigitalVoiceController::deactivateDigitalVoice);
        connect(m_dvDialog, &DigitalVoiceDialog::txCallsignChanged, this, [this](const QString& cs) {
            if (m_dvEngine) {
                QMetaObject::invokeMethod(m_dvEngine, [eng = m_dvEngine, cs] { eng->setTxText(cs); });
            }
            if (m_dvReporting) {
                setDigitalVoiceReporting(true);  // re-auth with new callsign
            }
        });
        connect(m_dvDialog, &DigitalVoiceDialog::squelchChanged, this, [this](bool en, float thr) {
            if (m_dvEngine) {
                QMetaObject::invokeMethod(m_dvEngine, [eng = m_dvEngine, en, thr] { eng->setSquelch(en, thr); });
            }
        });
        connect(m_dvDialog, &DigitalVoiceDialog::reportingToggled,
                this, &DigitalVoiceController::setDigitalVoiceReporting);
#else
        m_dvDialog->setActiveState(-1, DigitalVoiceMode::None, false);
#endif
    }
    m_dvDialog->show();
    m_dvDialog->raise();
    m_dvDialog->activateWindow();
    return m_dvDialog;
}

} // namespace AetherSDR
