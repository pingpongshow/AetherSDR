#include "gui/mainwindow/SliceController.h"

#include "gui/ScannerPanel.h"
#include "gui/mainwindow/StationToolsController.h"
#include "gui/mainwindow/MemoryController.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/AudioDspController.h"
#include "gui/mainwindow/DaxController.h"
#include "gui/mainwindow/DigitalModesController.h"
#include "gui/mainwindow/DigitalVoiceController.h"
#include "gui/mainwindow/ExternalControlsController.h"
#include "gui/mainwindow/MemoryController.h"
#include "gui/mainwindow/PanadapterController.h"
#include "gui/mainwindow/StatusBarController.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "gui/SpectrumWidget.h"
#include "gui/SpectrumOverlayMenu.h"
#include "gui/VfoWidget.h"
#include "gui/AppletPanel.h"
#include "gui/RxApplet.h"
#include "models/SliceModel.h"
#include "models/MeterModel.h"
#include "models/BandDefs.h"
#include "models/TransmitModel.h"
#include <algorithm>
#include <cmath>
#include <QTimer>
#include "core/AppSettings.h"

namespace AetherSDR {

SliceController::SliceController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void SliceController::wireSliceApplets()
{
    // ── AF gain from applet panel → radio per-slice audio_level ─────────
    connect(m_mw.m_appletPanel->rxApplet(), &RxApplet::afGainChanged, this, [this](int v) {
        if (auto* s = m_mw.activeSlice()) s->setAudioGain(v);
    });

    // ── Slice tab toggle: click A/B/C/D → switch active slice (#1278) ──
    connect(m_mw.m_appletPanel->rxApplet(), &RxApplet::sliceActivationRequested,
            this, &SliceController::setActiveSlice);
    // Initialize slice tab buttons once the radio reports its actual capacity
    connect(&m_mw.m_radioModel, &RadioModel::infoChanged, this, [this]() {
        m_mw.m_appletPanel->setMaxSlices(m_mw.m_radioModel.maxSlices());
    });

    // Propagate late-arriving SmartSDR+ subscription + dual-SCU diversity
    // eligibility to all existing VFOs. Both depend on radio info (license
    // subscription / model) that can arrive after a slice is created — in
    // which case onSliceAdded reads empty strings and the VFO gets stuck in
    // the wrong state (#1356 for SmartSDR+, #1503 for DIV).
    connect(&m_mw.m_radioModel, &RadioModel::infoChanged, this, [this]() {
        const bool hasPlus = m_mw.m_radioModel.licenseSubscription().contains("SmartSDR+");
        // Radio-reported diversity_allowed (fw 4.2.18+), model-name fallback otherwise
        const bool divAllowed = m_mw.m_radioModel.diversityAllowed();
        if (m_mw.m_panStack) {
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                for (auto* vfo : sw->findChildren<VfoWidget*>()) {
                    vfo->setSmartSdrPlus(hasPlus);
                    vfo->setDiversityAllowed(divAllowed);
                }
            }
        }
    });
}

void SliceController::wireStepSize()
{
    // ── Tuning step size → AppSettings + radio command ─────────────────────
    // Per-pan SpectrumWidget::setStepSize connections are made in wirePanadapter()
    // so all pans (including new ones added at runtime) stay in sync.
    connect(m_mw.m_appletPanel->rxApplet(), &RxApplet::stepSizeChanged,
            this, [this](int step) {
        // Send step to radio for the active slice
        if (auto* s = m_mw.m_radioModel.slice(m_mw.m_activeSliceId))
            m_mw.m_radioModel.sendCommand(QString("slice set %1 step=%2").arg(s->sliceId()).arg(step));
        // Also save to AppSettings for SpectrumWidget scroll-to-tune
        auto& settings = AppSettings::instance();
        settings.setValue("TuningStepSize", QString::number(step));
        settings.save();
    });
    // Radio-reported step (mode change, other client): keep the saved
    // scroll-to-tune step in sync but never send it back to the radio.
    connect(m_mw.m_appletPanel->rxApplet(), &RxApplet::stepSizeSyncedFromRadio,
            this, [](int step) {
        auto& settings = AppSettings::instance();
        settings.setValue("TuningStepSize", QString::number(step));
        settings.save();
    });
    int savedStep = AppSettings::instance().value("TuningStepSize", "100").toInt();
    for (auto* a : m_mw.m_panStack->allApplets()) a->spectrumWidget()->setStepSize(savedStep);
    m_mw.m_appletPanel->rxApplet()->setInitialStepSize(savedStep);
}

// Creates and fully sets up a slice's VFO flag on a spectrum. Used when a slice
// is added and when it moves to another pan — the move path previously only
// called wireVfoWidget(), leaving the recreated VFO without S-meter/ESC
// meter feeds, antenna list updates, SmartSDR+ filters or the DIV button.
VfoWidget* SliceController::createVfoForSlice(SpectrumWidget* sw, SliceModel* s)
{
    auto* vfo = sw->addVfoWidget(s->sliceId());

    // Set SmartSDR+ flag before wireVfoWidget so rebuildFilterButtons
    // sees the correct value when setSlice() triggers the first build (#1356)
    vfo->setSmartSdrPlus(m_mw.m_radioModel.licenseSubscription().contains("SmartSDR+"));

    wireVfoWidget(vfo, s);

    // Show DIV button on dual-SCU radios
    {
        // Radio-reported diversity_allowed (fw 4.2.18+), model-name fallback otherwise
        const bool divAllowed = m_mw.m_radioModel.diversityAllowed();
        vfo->setDiversityAllowed(divAllowed);
    }

    // Feed S-meter per-slice — only this VFO's slice level
    const int sid = s->sliceId();
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::sLevelChanged,
            vfo, [vfo, sid](int sliceIndex, float dbm) {
        if (sliceIndex == sid)
            vfo->setSignalLevel(dbm);
    });
    // Feed ESC meter per-slice — signal strength after ESC processing
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::escLevelChanged,
            vfo, [vfo, sid](int sliceIndex, float dbm) {
        if (sliceIndex == sid)
            vfo->setEscLevel(dbm);
    });
    connect(&m_mw.m_radioModel, &RadioModel::antListChanged,
            vfo, &VfoWidget::setAntennaList);
#ifdef HAVE_DIGITAL_VOICE
    m_mw.m_digitalVoiceCtl->applyVfoIndicator(vfo, s);
#endif
    return vfo;
}

void SliceController::onSliceAdded(SliceModel* s)
{
    // During layout transition, spectrums are being destroyed/recreated — skip
    if (m_mw.m_applyingLayout) return;

    qCDebug(lcGui) << "MainWindow: slice added" << s->sliceId();
    const bool firstSlice = (m_mw.m_activeSliceId < 0);

    // First slice — wire everything up
    if (firstSlice) {
        setActiveSlice(s->sliceId());

        // Detect initial band from radio's frequency
        if (m_mw.m_bandSettings.currentBand().isEmpty())
            m_mw.m_bandSettings.setCurrentBand(BandSettings::bandForFrequency(s->frequency()));

        // Re-create audio stream if it was invalidated by a profile load.
        // Only create if PC Audio is enabled — if the user is listening
        // through the radio's hardware outputs, don't switch to PC. (#336)
        if (m_needAudioStream) {
            m_needAudioStream = false;
            if (AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True")
                m_mw.m_radioModel.createRxAudioStream();
        }

        // Restore client-side DSP (NR2/RN2) from last session.
        // Deferred so the VFO widget exists for button sync.
        QTimer::singleShot(500, this, [this]() {
            auto& settings = AppSettings::instance();
            if (settings.value("ClientNr2Enabled", "False").toString() == "True")
                m_mw.m_dspCtl->enableNr2WithWisdom();
            else if (settings.value("ClientRn2Enabled", "False").toString() == "True")
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setRn2Enabled(true); });
            else if (settings.value("ClientNr4Enabled", "False").toString() == "True")
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr4Enabled(true); });
            else if (settings.value("ClientDfnrEnabled", "False").toString() == "True")
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setDfnrEnabled(true); });
            else if (settings.value("ClientMnrEnabled", "False").toString() == "True")
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setMnrEnabled(true); });
            // BNR not auto-restored — requires manual enable each session

            // Restore DEXP (downward expander) — radio does not persist across sessions
            bool dexpSaved = settings.value("DexpEnabled", "False").toString() == "True";
            int dexpLevel = settings.value("DexpLevel", "0").toInt();
            if (dexpSaved) {
                m_mw.m_radioModel.transmitModel().setDexp(true);
                if (dexpLevel > 0) {
                    m_mw.m_radioModel.transmitModel().setDexpLevel(dexpLevel);
                }
            }

            // Deferred CW decoder restart after profile load (#305).
            // Mode status arrives asynchronously — by the time setActiveSlice
            // runs, the slice may still have its default mode (not CW).
            // Re-check after status has settled.
            auto* sl = m_mw.activeSlice();
            if (sl) {
                bool isCw = (sl->mode() == "CW" || sl->mode() == "CWL");
                bool decodeOn = settings.value("CwDecodeOverlay", "True").toString() == "True";
                if (m_mw.m_cwDecoderApplet) m_mw.m_cwDecoderApplet->setCwPanelVisible(isCw && decodeOn);
                if (isCw && !m_mw.m_cwDecoder.isRunning())
                    m_mw.m_cwDecoder.start();
            }
        });
    }

    // Restore per-slice DAX channel from last session (#1221).
    // Deferred so the radio's initial slice status has arrived first.
    {
        const int sliceIdx = m_mw.m_radioModel.slices().indexOf(s);
        if (sliceIdx >= 0) {
            const QString key = QString("DaxChannel_Slice%1").arg(QChar('A' + sliceIdx));
            int savedDax = AppSettings::instance().value(key, "0").toInt();
            if (savedDax > 0) {
                QTimer::singleShot(300, this, [s, savedDax]() {
                    if (s) { s->setDaxChannel(savedDax); }
                });
            }
        }
    }

    // Re-claim TX assignment after profile load or slice recreation (#145).
    // The radio sets tx=1 on the slice but tx_client_handle may be 0x00000000
    // if the slice was destroyed and recreated (e.g. by profile global load).
    if (s->isTxSlice())
        m_mw.m_radioModel.sendCommand(QString("slice set %1 tx=1").arg(s->sliceId()));

#if defined(HAVE_DAX_BRIDGE)
    m_mw.m_daxCtl->wireTxModeForSlice(s);
#endif

    // Push overlay for this slice to the spectrum widget
    pushSliceOverlay(s);

    // Set the panadapter applet's slice label (e.g. "Slice B") based on
    // which pan this slice belongs to
    if (m_mw.m_panStack && !s->panId().isEmpty()) {
        if (auto* applet = m_mw.m_panStack->panadapter(s->panId()))
            applet->setSliceId(s->sliceId());
    }

    // Set initial hasTxSlice for waterfall freeze logic
    if (s->isTxSlice()) {
        if (auto* sw = m_mw.spectrumForSlice(s))
            sw->setHasTxSlice(true);
    }

    // Sync show-TX-in-waterfall on first slice
    if (auto* sw = m_mw.spectrumForSlice(s))
        sw->setShowTxInWaterfall(
            m_mw.m_radioModel.transmitModel().showTxInWaterfall());

    // Connect slice state changes → spectrum overlay updates
    connect(s, &SliceModel::frequencyChanged, this, [this, s](double mhz) {
        // Don't snap overlay back to stale radio-confirmed freq during active
        // encoder tuning — the optimistic VFO position is already ahead (#1524)
        bool activeTuning = m_mw.m_extCtl->isEncoderTuningActive();
        if (activeTuning && s->sliceId() == m_mw.m_activeSliceId)
            return;

        m_updatingFromModel = true;
        if (auto* sw = m_mw.spectrumForSlice(s))
            sw->setSliceOverlay(s->sliceId(), mhz,
                s->filterLow(), s->filterHigh(), s->isTxSlice(),
                s->sliceId() == m_mw.m_activeSliceId,
                s->mode(), s->rttyMark(), s->rttyShift(),
                s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq());
        m_updatingFromModel = false;

        // Feed frequency to Antenna Genius for band→antenna recall
        if (s->sliceId() == m_mw.m_activeSliceId)
            m_mw.m_antennaGenius.setRadioFrequency(mhz);
    });

    // Feed current frequency immediately (AG may connect later and reprocess).
    if (s->sliceId() == m_mw.m_activeSliceId && s->frequency() > 0.0)
        m_mw.m_antennaGenius.setRadioFrequency(s->frequency());

    connect(s, &SliceModel::filterChanged, this, [this, s](int lo, int hi) {
        auto* sw = m_mw.spectrumForSlice(s);
        if (!sw) return;
        // Skip overlay update while user is dragging a filter edge — the radio's
        // status echo would overwrite the drag position, causing snap-to-zero (#764)
        if (sw->isDraggingFilter()) return;
        sw->setSliceOverlay(s->sliceId(), s->frequency(),
            lo, hi, s->isTxSlice(), s->sliceId() == m_mw.m_activeSliceId,
            s->mode(), s->rttyMark(), s->rttyShift(),
            s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq());
    });
    connect(s, &SliceModel::txSliceChanged, this, [this, s](bool tx) {
        // Update hasTxSlice on all spectrums for waterfall freeze logic
        if (tx) {
            for (auto* pan : m_mw.m_radioModel.panadapters()) {
                if (auto* sw = m_mw.m_panStack ? m_mw.m_panStack->spectrum(pan->panId()) : nullptr)
                    sw->setHasTxSlice(sw == m_mw.spectrumForSlice(s));
            }
            if (!m_mw.m_panStack && m_mw.m_panApplet)
                m_mw.m_panApplet->spectrumWidget()->setHasTxSlice(true);
        }
        if (auto* sw = m_mw.spectrumForSlice(s))
            sw->setSliceOverlay(s->sliceId(), s->frequency(),
                s->filterLow(), s->filterHigh(), tx,
                s->sliceId() == m_mw.m_activeSliceId,
                s->mode(), s->rttyMark(), s->rttyShift(),
                s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq());
        updateSplitState();

        // Active follows TX slice (#1351) — switch the displayed/active slice
        // when an external program (e.g. WSJT-X) moves the TX flag
        if (tx && s->sliceId() != m_mw.m_activeSliceId
            && AppSettings::instance().value("ActiveFollowsTxSlice", "False").toString() == "True") {
            setActiveSlice(s->sliceId());
        }
    });

    // When the radio notifies us that this slice became active, switch to it
    connect(s, &SliceModel::activeChanged, this, [this, s](bool active) {
        if (!active) return;
        // Accept radio's active echo — update client state but use
        // m_updatingFromModel to prevent sending active=1 back (feedback loop).
        m_updatingFromModel = true;
        setActiveSlice(s->sliceId());
        m_updatingFromModel = false;
    });

    // Update filter limits when the active slice's mode changes
    connect(s, &SliceModel::modeChanged, this, [this, s](const QString& mode) {
        if (s->sliceId() == m_mw.m_activeSliceId)
            updateFilterLimitsForMode(mode);

        // Update spectrum overlay with new mode (for RTTY mark/space lines)
        pushSliceOverlay(s);

        // Show/hide CW decode panel and start/stop decoder
        if (s->sliceId() == m_mw.m_activeSliceId) {
            bool isCw = (mode == "CW" || mode == "CWL");
            bool decodeOn = AppSettings::instance().value("CwDecodeOverlay", "True").toString() == "True";
            if (m_mw.m_cwDecoderApplet) m_mw.m_cwDecoderApplet->setCwPanelVisible(isCw && decodeOn);
            if (isCw && !m_mw.m_cwDecoder.isRunning())
                m_mw.m_cwDecoder.start();
            else if (!isCw && m_mw.m_cwDecoder.isRunning())
                m_mw.m_cwDecoder.stop();

            // Update CWX/DVK indicator availability for new mode
            m_mw.m_statusCtl->updateKeyerAvailability(mode);

            // Disable client-side DSP in digital and CW modes — NR2/RN2/BNR
            // corrupt digital data (#534) and suppress CW tones (#784)
            bool disableDsp = (mode == "DIGU" || mode == "DIGL" || mode == "RTTY"
                            || mode == "CW"   || mode == "CWL");
            if (disableDsp) {
                if (m_mw.m_audio->nr2Enabled())
                    QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(false); });
                if (m_mw.m_audio->rn2Enabled())
                    QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setRn2Enabled(false); });
#ifdef HAVE_BNR
                if (m_mw.m_audio->bnrEnabled())
                    QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setBnrEnabled(false); });
#endif
                if (m_mw.m_audio->nr4Enabled())
                    QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr4Enabled(false); });
                if (m_mw.m_audio->dfnrEnabled())
                    QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setDfnrEnabled(false); });
            }
        }
    });

    // Update RTTY mark/space lines on spectrum when mark/shift changes
    connect(s, &SliceModel::rttyMarkChanged, this, [this, s](int) { pushSliceOverlay(s); });
    connect(s, &SliceModel::rttyShiftChanged, this, [this, s](int) { pushSliceOverlay(s); });
    connect(s, &SliceModel::ritChanged, this, [this, s](bool, int) { pushSliceOverlay(s); });
    connect(s, &SliceModel::xitChanged, this, [this, s](bool, int) { pushSliceOverlay(s); });

    // Handle slice migration between panadapters
    connect(s, &SliceModel::panIdChanged, this, [this, s](const QString&) {
        // Remove overlay/VFO from all spectrums
        if (m_mw.m_panStack) {
            for (auto* pan : m_mw.m_radioModel.panadapters()) {
                if (auto* sw = m_mw.m_panStack->spectrum(pan->panId())) {
                    sw->removeSliceOverlay(s->sliceId());
                    sw->removeVfoWidget(s->sliceId());
                }
            }
        }
        // Re-add on the new pan (full setup: meters, antennas, license flags)
        auto* sw = m_mw.spectrumForSlice(s);
        if (!sw) return;
        createVfoForSlice(sw, s);
        pushSliceOverlay(s);
    });

    // Create a VfoWidget for this slice on the correct panadapter
    auto* swForVfo = m_mw.spectrumForSlice(s);
    if (!swForVfo) return;
    createVfoForSlice(swForVfo, s);

    // Direct freq label update for runtime changes
    connect(s, &SliceModel::frequencyChanged, this, [this, s]() {
        auto* sw2 = m_mw.spectrumForSlice(s);
        if (!sw2) return;
        auto* v = sw2->vfoWidget(s->sliceId());
        if (!v) return;
        long long hz = static_cast<long long>(std::round(s->frequency() * 1e6));
        int mhzPart = static_cast<int>(hz / 1000000);
        int khzPart = static_cast<int>((hz / 1000) % 1000);
        int hzPart  = static_cast<int>(hz % 1000);
        v->freqLabel()->setText(QString("%1.%2.%3")
            .arg(mhzPart)
            .arg(khzPart, 3, 10, QChar('0'))
            .arg(hzPart, 3, 10, QChar('0')));

        // Diversity: client-side sync — immediately update child VFO display
        // to avoid rubber-banding from the radio round-trip delay.
        // Only for diversity parent→child, NOT split RX→TX.
        if (s->isDiversityParent()) {
            for (auto* other : m_mw.m_radioModel.slices()) {
                if (other->isDiversityChild() && other->sliceId() != s->sliceId()) {
                    auto* csw = m_mw.spectrumForSlice(other);
                    if (!csw) continue;
                    auto* cv = csw->vfoWidget(other->sliceId());
                    if (!cv) continue;
                    cv->freqLabel()->setText(QString("%1.%2.%3")
                        .arg(mhzPart)
                        .arg(khzPart, 3, 10, QChar('0'))
                        .arg(hzPart, 3, 10, QChar('0')));
                }
            }
        }
    });

    // If split is pending, this new slice is the TX slice
    if (m_splitActive && m_splitTxSliceId < 0 && s->sliceId() != m_splitRxSliceId) {
        m_splitTxSliceId = s->sliceId();
        s->setTxSlice(true);
        s->setAudioMute(true);  // TX slice in split has no audio output
        // TX slice frequency is already set by the slice create command
        // (with mode-dependent offset), so do NOT override it here (#789) —
        // unless a CAT client asked for a TX frequency while it was pending.
        if (m_pendingSplitTxFreqMhz > 0.0) {
            s->setFrequency(m_pendingSplitTxFreqMhz);
            m_pendingSplitTxFreqMhz = 0.0;
        }
        if (auto* sw = m_mw.spectrum()) sw->setSplitPair(m_splitRxSliceId, m_splitTxSliceId);
        updateSplitState();
        // Auto-focus the TX VFO so the user can immediately tune the TX offset
        setActiveSlice(s->sliceId());
    }

    // Refresh slice tab buttons (#1278)
    m_mw.m_appletPanel->updateSliceButtons(m_mw.m_radioModel.slices(), m_mw.m_activeSliceId);
}

void SliceController::onSliceRemoved(int id)
{
#ifdef HAVE_DIGITAL_VOICE
    m_mw.m_digitalVoiceCtl->handleSliceRemoved(id);
#endif

    if (m_mw.m_applyingLayout) return;

    qCDebug(lcGui) << "MainWindow: slice removed" << id;

    // If either half of the split pair was closed, disable split
    if (m_splitActive && (id == m_splitTxSliceId || id == m_splitRxSliceId)) {
        m_splitActive = false;
        m_splitRxSliceId = -1;
        m_splitTxSliceId = -1;
        if (auto* sw = m_mw.spectrum()) sw->setSplitPair(-1, -1);
        if (auto* s = m_mw.activeSlice())
            s->setTxSlice(true);
        updateSplitState();
    }

    // Remove overlay from all panadapter spectrums (slice model is already gone,
    // so we can't look up which pan it was on)
    if (m_mw.m_panStack) {
        for (auto* pan : m_mw.m_radioModel.panadapters()) {
            if (auto* sw = m_mw.m_panStack->spectrum(pan->panId())) {
                sw->removeSliceOverlay(id);
                sw->removeVfoWidget(id);
            }
        }
    }
    // Clean slice overlay and VFO widget from all spectrums
    for (auto* a : m_mw.m_panStack->allApplets()) {
        a->spectrumWidget()->removeSliceOverlay(id);
        a->spectrumWidget()->removeVfoWidget(id);
    }

    // Update pan title bars — show the first remaining slice on each pan,
    // or clear the title if the pan has no slices left.
    if (m_mw.m_panStack) {
        for (auto* applet : m_mw.m_panStack->allApplets()) {
            bool found = false;
            for (auto* sl : m_mw.m_radioModel.slices()) {
                if (sl->panId() == applet->panId()) {
                    applet->setSliceId(sl->sliceId());
                    found = true;
                    break;
                }
            }
            if (!found)
                applet->clearSliceTitle();
        }
    }

    // Reset panadapter state so display settings re-sync after profile load
    m_mw.m_radioModel.resetPanState();
    m_needAudioStream = true;

    // If the removed slice was active, switch to the first remaining slice
    if (id == m_mw.m_activeSliceId) {
        m_mw.m_appletPanel->setSlice(nullptr);
        if (auto* sw = m_mw.spectrum()) sw->overlayMenu()->setSlice(nullptr);

        const auto& slices = m_mw.m_radioModel.slices();
        if (!slices.isEmpty())
            setActiveSlice(slices.first()->sliceId());
        else
            m_mw.m_activeSliceId = -1;
    }

    // Refresh slice tab buttons (#1278)
    m_mw.m_appletPanel->updateSliceButtons(m_mw.m_radioModel.slices(), m_mw.m_activeSliceId);
}

void SliceController::setActiveSlice(int sliceId)
{
    auto* s = m_mw.m_radioModel.slice(sliceId);
    if (!s) return;

    // Auto-activate the panadapter that owns this slice
    if (m_mw.m_panStack && !s->panId().isEmpty())
        m_mw.m_panStack->setActivePan(s->panId());

    const int prevId = m_mw.m_activeSliceId;
    m_mw.m_activeSliceId = sliceId;
    if (ScannerPanel* scanner = m_mw.m_memoryCtl->scannerPanel()) {
        scanner->setActiveSliceId(sliceId);
    }
    m_mw.m_toolsCtl->setLogbookActiveSlice(m_mw.m_radioModel.slice(sliceId));

    // Send "slice set N active=1" only when switching to a different slice
    // (matches SmartSDR pcap — sent on VFO flag click, not on every tune).
    // Guard: don't send if triggered by the radio's own activeChanged echo
    // (m_updatingFromModel is set in the activeChanged handler).
    if (sliceId != prevId && !m_updatingFromModel)
        s->setActive(true);

    // Update all overlay isActive flags on each slice's correct spectrum
    for (auto* sl : m_mw.m_radioModel.slices()) {
        const bool isActive = (sl->sliceId() == sliceId);
        if (auto* sw = m_mw.spectrumForSlice(sl))
            sw->setSliceOverlay(sl->sliceId(), sl->frequency(),
                sl->filterLow(), sl->filterHigh(), sl->isTxSlice(), isActive,
                sl->mode(), sl->rttyMark(), sl->rttyShift(),
                sl->ritOn(), sl->ritFreq(), sl->xitOn(), sl->xitFreq());
    }

    // QSO recorder: track active slice for frequency/mode metadata (#1297)
    m_mw.m_qsoRecorder->setSlice(s);

    // Re-wire applet panel, overlay menu to the new active slice
    if (m_mw.m_panStack) {
        if (auto* applet = m_mw.m_panStack->panadapter(s->panId()))
            applet->setSliceId(sliceId);
        else if (m_mw.m_panStack->activeApplet())
            m_mw.m_panStack->activeApplet()->setSliceId(sliceId);
    }
    m_mw.m_appletPanel->setSlice(s);
    m_mw.m_appletPanel->updateSliceButtons(m_mw.m_radioModel.slices(), sliceId);
    auto* sw = m_mw.spectrum();
    if (sw) {
        // Recenter panadapter if the newly active slice is off-screen (#1554)
        double sliceFreq = s->frequency();
        double halfBw = sw->bandwidthMhz() / 2.0;
        double lo = sw->centerMhz() - halfBw;
        double hi = sw->centerMhz() + halfBw;
        if (sliceFreq < lo || sliceFreq > hi) {
            sw->setFrequencyRange(sliceFreq, sw->bandwidthMhz());
            emit sw->centerChangeRequested(sliceFreq);
        }

        sw->overlayMenu()->setSlice(s);

        // Sync step size from the new active slice
        if (s->stepHz() > 0) {
            sw->setStepSize(s->stepHz());
            m_mw.m_appletPanel->rxApplet()->syncStepFromSlice(s->stepHz(), s->stepList());
        }

        // Switch active VFO widget display (NR2/RN2/RADE are wired permanently
        // in wireVfoWidget, no disconnect/reconnect needed)
        sw->setActiveVfoWidget(sliceId);
    } else if (s->stepHz() > 0) {
        m_mw.m_appletPanel->rxApplet()->syncStepFromSlice(s->stepHz(), s->stepList());
    }

    // Update filter limits for the active slice's mode
    updateFilterLimitsForMode(s->mode());

    // Route CW decoder output to the pan owning this slice (#864)
    m_mw.m_panCtl->routeCwDecoderOutput();

    m_mw.m_digitalModesCtl->setActiveSliceOnTools(s);

    // Show/hide CW decode panel for the active slice's current mode
    bool isCw = (s->mode() == "CW" || s->mode() == "CWL");
    bool decodeOn = AppSettings::instance().value("CwDecodeOverlay", "True").toString() == "True";
    if (m_mw.m_cwDecoderApplet) m_mw.m_cwDecoderApplet->setCwPanelVisible(isCw && decodeOn);
    if (isCw && !m_mw.m_cwDecoder.isRunning())
        m_mw.m_cwDecoder.start();
    else if (!isCw && m_mw.m_cwDecoder.isRunning())
        m_mw.m_cwDecoder.stop();

    // Update CWX/DVK indicator availability for this slice's mode
    m_mw.m_statusCtl->updateKeyerAvailability(s->mode());

    // Detect band from frequency
    if (m_mw.m_bandSettings.currentBand().isEmpty())
        m_mw.m_bandSettings.setCurrentBand(BandSettings::bandForFrequency(s->frequency()));

    // NOTE: RADE audio mode is now driven by the TX slice (in updateDaxTxMode),
    // not the active/selected slice. Switching which slice the user is looking at
    // should not change the TX audio routing.

    updateSplitState();

    // TX follows active slice (#441) — auto-assign TX when switching slices
    if (!m_splitActive && sliceId != prevId && !s->isTxSlice()
        && AppSettings::instance().value("TxFollowsActiveSlice", "False").toString() == "True") {
        s->setTxSlice(true);
    }

    // Update MEM button target-slice badge on every overlay (#1781)
    m_mw.m_memoryCtl->refreshMemoryBrowsePanel();

    qCDebug(lcGui) << "MainWindow: active slice set to" << sliceId;
}

void SliceController::updateFilterLimitsForMode(const QString& mode)
{
    int minHz, maxHz;
    if (mode == "LSB" || mode == "DIGL" || mode == "CWL") {
        minHz = -12000; maxHz = 0;
    } else if (mode == "AM" || mode == "SAM" || mode == "DSB") {
        minHz = -12000; maxHz = 12000;
    } else if (mode == "FM" || mode == "NFM" || mode == "DFM") {
        minHz = -12000; maxHz = 12000;
    } else {
        // USB, DIGU, CW, RTTY, etc.
        minHz = 0; maxHz = 12000;
    }
    if (auto* s = m_mw.spectrum()) {
        s->setFilterLimits(minHz, maxHz);
        s->setMode(mode);
    }
}

void SliceController::pushSliceOverlay(SliceModel* s)
{
    if (m_mw.m_applyingLayout) return;
    auto* sw = m_mw.spectrumForSlice(s);
    if (!sw) return;
    sw->setSliceOverlay(s->sliceId(), s->frequency(),
        s->filterLow(), s->filterHigh(), s->isTxSlice(),
        s->sliceId() == m_mw.m_activeSliceId,
        s->mode(), s->rttyMark(), s->rttyShift(),
        s->ritOn(), s->ritFreq(), s->xitOn(), s->xitFreq());
}

// Step the active slice's pan to the next/previous HF amateur band using the
// radio's band stack (same "display pan set <pan> band=<n>" the overlay band
// buttons send), wrapping 160m <-> 6m.
void SliceController::stepBand(int direction)
{
    static constexpr const char* kHfBands[] = {
        "160", "80", "60", "40", "30", "20", "17", "15", "12", "10", "6"
    };
    static constexpr int kHfCount = static_cast<int>(std::size(kHfBands));

    SliceModel* s = m_mw.activeSlice();
    if (!s || s->panId().isEmpty() || direction == 0) {
        return;
    }
    const double freq = s->frequency();

    // Current band: the one containing the frequency, else the nearest by
    // default frequency (e.g. when parked between bands).
    int current = -1;
    double bestDist = std::numeric_limits<double>::max();
    for (int i = 0; i < kHfCount; ++i) {
        const QString meters = QString::fromLatin1(kHfBands[i]) + QLatin1Char('m');
        for (const BandDef& b : kBands) {
            if (meters != QLatin1String(b.name)) {
                continue;
            }
            if (freq >= b.lowMhz && freq <= b.highMhz) {
                current = i;
                bestDist = 0.0;
            } else if (bestDist > 0.0 && std::abs(freq - b.defaultFreqMhz) < bestDist) {
                bestDist = std::abs(freq - b.defaultFreqMhz);
                current = i;
            }
        }
        if (bestDist == 0.0) {
            break;
        }
    }
    if (current < 0) {
        current = 0;
    }
    const int next = ((current + direction) % kHfCount + kHfCount) % kHfCount;
    const QString band = QString::fromLatin1(kHfBands[next]);
    m_mw.m_bandSettings.setCurrentBand(band);
    m_mw.m_radioModel.sendCommand(QString("display pan set %1 band=%2").arg(s->panId(), band));
}

void SliceController::enableSplit(int rxSliceId, double txFreqMhz)
{
    if (m_splitActive) {
        // Already split (or the TX slice is still being created): only
        // retarget the TX frequency.
        if (txFreqMhz > 0.0) {
            if (auto* tx = m_mw.m_radioModel.slice(m_splitTxSliceId)) {
                tx->setFrequency(txFreqMhz);
            } else {
                m_pendingSplitTxFreqMhz = txFreqMhz;
            }
        }
        return;
    }
    // Entering split: this slice becomes RX, create a new TX slice
    if (m_mw.m_radioModel.slices().size() >= m_mw.m_radioModel.maxSlices())
        return;
    auto* rxSlice = m_mw.m_radioModel.slice(rxSliceId);
    if (!rxSlice) return;

    // Create TX slice on the SAME pan as the RX slice
    QString panId = rxSlice->panId();
    if (panId.isEmpty())
        panId = m_mw.m_panStack ? m_mw.m_panStack->activePanId() : m_mw.m_radioModel.panId();

    // CW split: offset 1 kHz up (convention). Other modes: 5 kHz up.
    const QString mode = rxSlice->mode();
    bool isCw = mode == "CW" || mode == "CWL";
    double offsetMhz = isCw ? 0.001 : 0.005;
    double txFreq = (txFreqMhz > 0.0) ? txFreqMhz : rxSlice->frequency() + offsetMhz;

    m_splitActive = true;
    m_splitRxSliceId = rxSliceId;
    m_pendingSplitTxFreqMhz = 0.0;
    m_mw.m_radioModel.sendCommand(
        QString("slice create pan=%1 freq=%2")
            .arg(panId).arg(txFreq, 0, 'f', 6));
}

void SliceController::disableSplit()
{
    if (!m_splitActive) return;

    m_splitActive = false;

    // Move TX back to the RX slice
    if (auto* rxSlice = m_mw.m_radioModel.slice(m_splitRxSliceId))
        rxSlice->setTxSlice(true);

    // Destroy the split TX slice
    if (m_splitTxSliceId >= 0)
        m_mw.m_radioModel.sendCommand(QString("slice remove %1").arg(m_splitTxSliceId));

    m_splitRxSliceId = -1;
    m_splitTxSliceId = -1;
    if (auto* sw = m_mw.spectrum()) sw->setSplitPair(-1, -1);

    updateSplitState();
}

void SliceController::updateSplitState()
{
    auto* sw = m_mw.spectrum();
    if (!sw) return;
    for (auto* s : m_mw.m_radioModel.slices()) {
        if (auto* w = sw->vfoWidget(s->sliceId())) {
            bool isTxSlice = (m_splitActive && s->sliceId() == m_splitTxSliceId);
            bool isRxSplit = (m_splitActive && s->sliceId() == m_splitRxSliceId);
            w->updateSplitBadge(isTxSlice, isRxSplit);
        }
    }
}

void SliceController::panFollowVfo(SliceModel* s, double mhz)
{
    // Pan-follow-VFO is toggleable via View menu (#1476). On by default —
    // matches the historical behavior users expect, but can be disabled
    // for operators who prefer a fixed panadapter window.
    if (AppSettings::instance().value("PanFollowVfo", "True").toString() != "True")
        return;

    // Note: centerActiveSliceInPanadapter() is intentionally not reused here.
    // That helper snaps to dead center and does extra work (pan stack activation,
    // VFO widget update) that is redundant or wrong for incremental step-tuning.
    PanadapterModel* pan = m_mw.m_radioModel.panadapter(s->panId());
    if (!pan) return;
    const double halfBw = pan->bandwidthMhz() / 2.0;
    if (halfBw <= 0.0) return;  // guard: bandwidth not yet received from radio

    // "Fence" behavior (#1476): VFO slides freely inside the middle 90% of
    // the pan window.  When it crosses the 5% boundary on either edge of
    // the *full* window, the pan scrolls by exactly the overshoot so the
    // VFO stays pinned to that boundary.  Continuous tuning past the fence
    // feels like the pan scrolls by the step size — no sudden jumps, the
    // VFO never visibly leaves the window.
    //
    // Express the boundary relative to halfBw: fence is 5% of the full
    // window = 10% of halfBw.
    static constexpr double kMarginFrac = 0.10;
    const double deadZoneHalfBw = halfBw * (1.0 - kMarginFrac);
    // Read the *visible* center from the spectrum widget, not the model —
    // PanadapterModel may lag behind after a manual drag (#1643).
    auto* sw = m_mw.spectrumForSlice(s);
    const double center = sw ? sw->centerMhz() : pan->centerMhz();
    double newCenter;
    if (mhz > center + deadZoneHalfBw) {
        newCenter = mhz - deadZoneHalfBw;   // VFO pinned to right fence
    } else if (mhz < center - deadZoneHalfBw) {
        newCenter = mhz + deadZoneHalfBw;   // VFO pinned to left fence
    } else {
        return;  // VFO inside dead zone — pan stays put
    }
    newCenter = std::max(newCenter, halfBw);

    // Optimistic local update: apply new center immediately so SpectrumWidget
    // repaints without waiting for the radio echo-back round-trip. (#989)
    pan->applyPanStatus({{"center", QString::number(newCenter, 'f', 6)}});
    m_mw.m_radioModel.sendCommand(
        QString("display pan set %1 center=%2")
            .arg(pan->panId()).arg(newCenter, 0, 'f', 6));
}

void SliceController::wireVfoWidget(VfoWidget* w, SliceModel* s)
{
    const int sliceId = s->sliceId();

    // Note: w->setSlice(s) is called at the end of this method (line ~1895)

    // Per-slice signals — these are specific to the slice this widget represents
    connect(w, &VfoWidget::closeSliceRequested, this, [this, sliceId]() {
        if (m_mw.m_radioModel.slices().size() <= 1) return;
        m_mw.m_radioModel.sendCommand(QString("slice remove %1").arg(sliceId));
    });
    // Pan-follow-VFO (#989): wheel uses setFrequency(autopan=0) so re-center
    // the pan explicitly when the stepped frequency falls outside the window.
    connect(w, &VfoWidget::stepTuned, this, [this, sliceId](double mhz) {
        if (auto* sl = m_mw.m_radioModel.slice(sliceId))
            panFollowVfo(sl, mhz);
    });
    connect(w, &VfoWidget::lockToggled, this, [this, sliceId](bool locked) {
        if (auto* sl = m_mw.m_radioModel.slice(sliceId))
            sl->setLocked(locked);
    });
    connect(w, &VfoWidget::afGainChanged, this, [this, sliceId](int v) {
        if (auto* sl = m_mw.m_radioModel.slice(sliceId))
            sl->setAudioGain(v);
    });
    // Record/playback — route to radio or client-side QsoRecorder (#1297)
    connect(w, &VfoWidget::recordToggled, this, [this, w, sliceId](bool on) {
        bool clientSide = AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
        if (clientSide) {
            // Button state follows recordingStarted/recordingStopped/recordingError
            // below, so a failed start doesn't leave REC pulsing.
            if (on)
                m_mw.m_qsoRecorder->startRecording();
            else
                m_mw.m_qsoRecorder->stopRecording();
            if (on != m_mw.m_qsoRecorder->isRecording()) {
                w->setRecordOn(m_mw.m_qsoRecorder->isRecording());
            }
        } else {
            if (auto* sl = m_mw.m_radioModel.slice(sliceId))
                sl->setRecordOn(on);
        }
    });
    // Client-side recording stopped by idle timeout → update VFO button
    connect(m_mw.m_qsoRecorder, &QsoRecorder::recordingStopped, w, [w]() {
        w->setRecordOn(false);
        w->setPlayEnabled(true);
    });
    // Client-side playback
    connect(w, &VfoWidget::playToggled, this, [this, w, sliceId](bool on) {
        bool clientSide = AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
        if (clientSide) {
            if (on)
                m_mw.m_qsoRecorder->startPlayback();
            else
                m_mw.m_qsoRecorder->stopPlayback();
        } else {
            if (auto* sl = m_mw.m_radioModel.slice(sliceId))
                sl->setPlayOn(on);
        }
    });
    connect(m_mw.m_qsoRecorder, &QsoRecorder::playbackStopped, w, [w]() {
        w->setPlayOn(false);
    });
    // Client-side recorder state → VFO buttons (auto-record on TX starts
    // recording without a button press; errors must reset the pulse).
    connect(m_mw.m_qsoRecorder, &QsoRecorder::recordingStarted, w, [w](const QString&) {
        w->setRecordOn(true);
    });
    connect(m_mw.m_qsoRecorder, &QsoRecorder::recordingError, w, [w](const QString&) {
        w->setRecordOn(false);
    });
    connect(m_mw.m_qsoRecorder, &QsoRecorder::playbackStarted, w, [w]() {
        w->setPlayOn(true);
    });
    connect(s, &SliceModel::recordOnChanged, w, &VfoWidget::setRecordOn);
    connect(s, &SliceModel::playOnChanged, w, &VfoWidget::setPlayOn);
    connect(s, &SliceModel::playEnabledChanged, w, &VfoWidget::setPlayEnabled);
    connect(w, &VfoWidget::autotuneRequested, this, [this, sliceId](bool intermittent) {
        if (m_mw.m_radioModel.slice(sliceId))
            m_mw.m_radioModel.cwAutoTune(sliceId, intermittent);
    });
    connect(w, &VfoWidget::autotuneOnceRequested, this, [this, sliceId]() {
        if (m_mw.m_radioModel.slice(sliceId))
            m_mw.m_radioModel.cwAutoTuneOnce(sliceId);
    });
    connect(w, &VfoWidget::zeroBeatRequested, this, [this]() {
        SliceModel* slice = m_mw.activeSlice();
        if (!slice) return;
        float detected = m_mw.m_cwDecoder.estimatedPitch();
        if (detected <= 0.0f) return;
        int configured = m_mw.m_radioModel.transmitModel().cwPitch();
        double offsetMhz = (detected - configured) / 1.0e6;
        slice->setFrequency(slice->frequency() + offsetMhz);
    });
    connect(w, &VfoWidget::addSpotRequested, this, [this](double freqMhz) {
        // Deferred: this fires inside the VFO's context-menu exec(); a modal
        // dialog nested there would return into a deleted VFO if the slice is
        // removed while the dialog is open.
        QTimer::singleShot(0, this, [this, freqMhz]() {
            if (auto* sw = m_mw.spectrum()) {
                sw->showAddSpotDialog(freqMhz);
            }
        });
    });

    // Clicking an inactive VfoWidget activates that slice
    connect(w, &VfoWidget::sliceActivationRequested, this, [this](int id) {
        if (id != m_mw.m_activeSliceId)
            setActiveSlice(id);
    });

    // SWAP — swap RX and TX frequencies (keep TX/RX assignments)
    connect(w, &VfoWidget::swapRequested, this, [this]() {
        if (!m_splitActive || m_splitRxSliceId < 0 || m_splitTxSliceId < 0) return;
        auto* rx = m_mw.m_radioModel.slice(m_splitRxSliceId);
        auto* tx = m_mw.m_radioModel.slice(m_splitTxSliceId);
        if (!rx || !tx) return;
        double rxFreq = rx->frequency();
        double txFreq = tx->frequency();
        rx->setFrequency(txFreq);
        tx->setFrequency(rxFreq);
    });

    // Split toggle — per-widget, slice-aware (#328)
    connect(w, &VfoWidget::splitToggled, this, [this, sliceId]() {
        if (!m_splitActive) {
            enableSplit(sliceId);
        } else if (sliceId == m_splitRxSliceId) {
            // Clicking SPLIT on the RX VFO again → disable split, destroy TX slice
            disableSplit();
        }
    });

    // NR2 toggle with FFTW wisdom generation — wired once per VFO, never disconnected
    connect(w, &VfoWidget::nr2Toggled, this, [this, w](bool on) {
        if (!on) { QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(false); }); return; }
        // NR2 amplifies Opus codec artifacts → disable when compressed audio is active (#1597)
        if (m_mw.m_radioModel.audioCompressionParam() == "opus") {
            QSignalBlocker blocker(w->nr2Button());
            w->nr2Button()->setChecked(false);
            m_mw.statusBar()->showMessage("NR2 is not available with compressed (SmartLink) audio", 4000);
            return;
        }
        m_mw.m_dspCtl->enableNr2WithWisdom();
    });
    connect(w, &VfoWidget::nr2RightClicked,
            this, [this](const QPoint& pos) { m_mw.m_dspCtl->showNr2ParamPopup(pos); });

    // RN2 toggle
    connect(w, &VfoWidget::rn2Toggled, this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setRn2Enabled(on); });
    });
    connect(w, &VfoWidget::bnrToggled, this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setBnrEnabled(on); });
    });
    connect(w, &VfoWidget::nr4Toggled, this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setNr4Enabled(on); });
    });
    connect(w, &VfoWidget::nr4RightClicked,
            this, [this](const QPoint& pos) { m_mw.m_dspCtl->showNr4ParamPopup(pos); });
    connect(w, &VfoWidget::mnrToggled, this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setMnrEnabled(on); });
    });
    connect(w, &VfoWidget::mnrRightClicked, this, [this](const QPoint&) { m_mw.m_dspCtl->showMnrSettings(); });
    connect(w, &VfoWidget::dfnrToggled, this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setDfnrEnabled(on); });
    });
    connect(w, &VfoWidget::dfnrRightClicked,
            this, [this](const QPoint& pos) { m_mw.m_dspCtl->showDfnrParamPopup(pos); });

#ifdef HAVE_RADE
    connect(w, &VfoWidget::radeActivated, m_mw.m_digitalVoiceCtl, &DigitalVoiceController::onRadeSelection);
#endif

    // Per-slice VFO marker style (#1526): push user's saved line thickness and
    // filter-edge visibility preferences to this slice's overlay whenever they
    // change. Also fires once on setSlice() below to apply loaded defaults.
    connect(w, &VfoWidget::markerStyleChanged, this,
            [this, sliceId](bool thin, bool hideEdges) {
        auto* sl = m_mw.m_radioModel.slice(sliceId);
        if (!sl) return;
        if (auto* sw = m_mw.spectrumForSlice(sl))
            sw->setSliceOverlayMarkerStyle(sliceId, thin, hideEdges);
    });

    // Pan re-apply after NR mono-mix (#1460): keep AudioEngine in sync with the
    // radio-side pan so client NR can restore the balance on its output.
    connect(w, &VfoWidget::rxPanChanged, this, [this](int v) {
        m_mw.m_audio->setRxPan(v);
    });

    // Per-slice AF mute persistence (#1560): save state to AppSettings on each
    // toggle so it survives the session; restore it once the slice is ready
    // (the radio does not persist audio_mute between client connections).
    // Use the user-visible slice letter (A/B/C/D) as the key so the saved state
    // survives sessions where the radio assigns different numeric IDs (#1560).
    const QChar sliceLetter = QChar('A' + sliceId);
    connect(w, &VfoWidget::audioMuteToggled, this, [this, sliceLetter](bool on) {
        AppSettings::instance().setValue(
            QString("SliceAudioMuted_%1").arg(sliceLetter), on ? "True" : "False");
    });
    {
        bool savedMute = AppSettings::instance()
            .value(QString("SliceAudioMuted_%1").arg(sliceLetter), "False")
            .toString() == "True";
        if (savedMute)
            s->setAudioMute(true);  // send audio_mute=1 to radio for this slice
    }

    // Wire slice data into widget
    w->setSlice(s);
    w->setAntennaList(m_mw.m_radioModel.antennaList());
    w->setTransmitModel(&m_mw.m_radioModel.transmitModel());
}

// wireActiveVfoSignals removed — NR2/RN2/RADE are now wired permanently
// in wireVfoWidget() so connections survive focus switches (#227).

void SliceController::centerActiveSliceInPanadapter(bool forceRadioCenter, double centerMhz)
{
    auto* s = m_mw.activeSlice();
    if (!s || s->panId().isEmpty()) return;

    auto* sw = m_mw.spectrumForSlice(s);
    if (!sw) return;

    auto* pan = m_mw.m_radioModel.panadapter(s->panId());
    const double bandwidthMhz = pan ? pan->bandwidthMhz() : m_mw.m_radioModel.panBandwidthMhz();
    const double targetMhz = (centerMhz > 0.0) ? centerMhz : s->frequency();
    const double clampedCenterMhz = std::max(targetMhz, bandwidthMhz / 2.0);

    if (m_mw.m_panStack) {
        if (auto* applet = m_mw.m_panStack->panadapter(s->panId()))
            m_mw.m_panStack->setActivePan(applet->panId());
    }

    // Keep the local spectrum centered immediately so the active slice marker
    // is visible before the radio's status echo arrives.
    sw->setFrequencyRange(clampedCenterMhz, bandwidthMhz);
    sw->setVfoFrequency(targetMhz);

    if (forceRadioCenter && m_mw.m_radioModel.isConnected()) {
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 center=%2")
                .arg(s->panId()).arg(clampedCenterMhz, 0, 'f', 6));
    }
}

void SliceController::toggleSplitFromShortcut()
{
    if (!m_splitActive) {
        if (m_mw.m_radioModel.slices().size() >= m_mw.m_radioModel.maxSlices()) return;
        auto* s = m_mw.activeSlice();
        if (!s) return;
        QString panId = s->panId();
        if (panId.isEmpty())
            panId = m_mw.m_panStack ? m_mw.m_panStack->activePanId() : m_mw.m_radioModel.panId();
        bool isCw = s->mode() == "CW" || s->mode() == "CWL";
        double txFreq = s->frequency() + (isCw ? 0.001 : 0.005);
        m_splitActive = true;
        m_splitRxSliceId = s->sliceId();
        m_mw.m_radioModel.sendCommand(
            QString("slice create pan=%1 freq=%2").arg(panId).arg(txFreq, 0, 'f', 6));
    } else {
        disableSplit();
    }
}

// ─── Band settings capture / restore ──────────────────────────────────────────

BandSnapshot SliceController::captureCurrentBandState() const
{
    BandSnapshot snap;
    if (auto* s = m_mw.activeSlice()) {
        snap.frequencyMhz  = s->frequency();
        snap.mode          = s->mode();
        snap.rxAntenna     = s->rxAntenna();
        snap.filterLow     = s->filterLow();
        snap.filterHigh    = s->filterHigh();
        snap.agcMode       = s->agcMode();
        snap.agcThreshold  = s->agcThreshold();
    }
    // Center and bandwidth are radio-authoritative — don't capture.
    if (auto* sw = m_mw.spectrum()) {
        snap.minDbm          = sw->refLevel() - sw->dynamicRange();
        snap.maxDbm          = sw->refLevel();
        snap.spectrumFrac    = sw->spectrumFrac();
        snap.rfGain          = sw->rfGainValue();
        snap.wnbOn           = sw->wnbActive();
    }
    return snap;
}

void SliceController::restoreBandState(const BandSnapshot& snap)
{
    m_updatingFromModel = true;
    if (auto* s = m_mw.activeSlice()) {
        s->setMode(snap.mode);
        s->tuneAndRecenter(snap.frequencyMhz);
        if (!snap.rxAntenna.isEmpty())
            s->setRxAntenna(snap.rxAntenna);
        s->setFilterWidth(snap.filterLow, snap.filterHigh);
        if (!snap.agcMode.isEmpty())
            s->setAgcMode(snap.agcMode);
        s->setAgcThreshold(snap.agcThreshold);
    }
    if (auto* pan = m_mw.m_radioModel.activePanadapter()) {
        // Don't push center or bandwidth — slice tune recenters the pan and
        // the radio determines bandwidth. Pushing stale saved values causes
        // FFT/waterfall misalignment during the transition (#279, #291).
        // Only restore dBm scale (client-side display preference).
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 min_dbm=%2 max_dbm=%3").arg(pan->panId())
                .arg(static_cast<double>(snap.minDbm), 0, 'f', 2)
                .arg(static_cast<double>(snap.maxDbm), 0, 'f', 2));
    }
    m_mw.m_radioModel.setPanRfGain(snap.rfGain);
    m_mw.m_radioModel.setPanWnb(snap.wnbOn);
    if (auto* sw = m_mw.spectrum()) {
        sw->setSpectrumFrac(snap.spectrumFrac);
        sw->setRfGain(snap.rfGain);
        sw->setWnbActive(snap.wnbOn);
    }
    m_updatingFromModel = false;
}

// ─── GUI control handlers ─────────────────────────────────────────────────────

void SliceController::onFrequencyChanged(double mhz)
{
    auto* sw = m_mw.spectrum();
    if (!sw) return;  // pan may be absent during band change (#1146)

    // If the slice is locked, snap spectrum back to the current freq.
    if (auto* s = m_mw.activeSlice(); s && s->isLocked()) {
        m_updatingFromModel = true;
        sw->setVfoFrequency(s->frequency());
        m_updatingFromModel = false;
        return;
    }

    sw->setVfoFrequency(mhz);
    if (!m_updatingFromModel) {
        if (auto* s = m_mw.activeSlice()) {
            s->setFrequency(mhz);
            panFollowVfo(s, mhz);  // re-center pan if VFO stepped outside visible window (#989)

            // Diversity: immediately mirror freq to child VFO (no radio round-trip)
            if (s->isDiversityParent()) {
                long long hz = static_cast<long long>(std::round(mhz * 1e6));
                QString freqStr = QString("%1.%2.%3")
                    .arg(static_cast<int>(hz / 1000000))
                    .arg(static_cast<int>((hz / 1000) % 1000), 3, 10, QChar('0'))
                    .arg(static_cast<int>(hz % 1000), 3, 10, QChar('0'));
                for (auto* other : m_mw.m_radioModel.slices()) {
                    if (other->isDiversityChild() && other->sliceId() != s->sliceId()) {
                        auto* csw = m_mw.spectrumForSlice(other);
                        if (!csw) continue;
                        auto* cv = csw->vfoWidget(other->sliceId());
                        if (cv) cv->freqLabel()->setText(freqStr);
                        // Also update the overlay marker position
                        csw->setSliceOverlay(other->sliceId(), mhz,
                            other->filterLow(), other->filterHigh(),
                            other->isTxSlice(), false,
                            other->mode(), other->rttyMark(), other->rttyShift(),
                            other->ritOn(), other->ritFreq(),
                            other->xitOn(), other->xitFreq());
                    }
                }
            }
        }
    }
}

} // namespace AetherSDR
