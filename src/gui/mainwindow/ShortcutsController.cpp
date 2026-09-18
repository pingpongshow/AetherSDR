#include "gui/mainwindow/ShortcutsController.h"

#include "gui/ScannerPanel.h"
#include "core/ContestEngine.h"
#include "gui/mainwindow/StationToolsController.h"
#include "gui/mainwindow/MemoryController.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/AudioDspController.h"
#include "gui/mainwindow/MemoryController.h"
#include "gui/mainwindow/SliceController.h"
#include "gui/CommandPalette.h"
#include "gui/SpectrumWidget.h"
#include "gui/VfoWidget.h"
#include "models/SliceModel.h"
#include "models/BandDefs.h"
#include "models/TransmitModel.h"
#include <algorithm>
#include <functional>
#include <QApplication>
#include <QTimer>
#include <QLineEdit>
#include <QShortcut>
#include <QPointer>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QComboBox>

namespace AetherSDR {

// ─── Shortcut guard (file-scope for use as std::function<bool()>) ───────────

static bool s_keyboardShortcutsEnabled = false;

void ShortcutsController::setShortcutsGuardEnabled(bool on)
{
    s_keyboardShortcutsEnabled = on;
}

bool ShortcutsController::isTextInputFocused()
{
    auto* w = QApplication::focusWidget();
    if (!w) return false;
    return qobject_cast<QLineEdit*>(w) || qobject_cast<QTextEdit*>(w)
        || qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QSpinBox*>(w)
        || qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSlider*>(w);
}

bool ShortcutsController::shortcutGuard()
{
    return s_keyboardShortcutsEnabled && !isTextInputFocused();
}

ShortcutsController::ShortcutsController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void ShortcutsController::registerShortcutActions()
{
    // Helper: nudge active slice frequency by N steps.
    // Uses tuneAndRecenter() so m_frequency is updated immediately (no stale
    // base on rapid presses) and the radio keeps the slice centered in the pan.
    auto nudgeFreq = [this](int steps) {
        if (!m_mw.m_radioModel.isConnected()) return;
        auto* s = m_mw.activeSlice();
        if (!s || s->isLocked()) return;
        int stepHz = m_mw.spectrum() ? m_mw.spectrum()->stepSize() : 100;
        double newMhz = s->frequency() + steps * stepHz / 1e6;
        // Use setFrequency (autopan=0) + panFollowVfo so all step-tuning paths
        // (keyboard, wheel, FlexControl, MIDI) share the same pan-follow logic. (#989)
        s->setFrequency(newMhz);
        m_mw.m_sliceCtl->panFollowVfo(s, newMhz);
    };

    // Step cycle helper
    auto cycleStep = [this](int dir) {
        auto* sw = m_mw.spectrum();
        if (!sw) return;
        static const int steps[] = {10, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
        int cur = sw->stepSize();
        if (dir > 0) {
            for (int i = 0; i < static_cast<int>(std::size(steps)); ++i)
                if (steps[i] > cur) { sw->setStepSize(steps[i]); return; }
        } else {
            for (int i = static_cast<int>(std::size(steps)) - 1; i >= 0; --i)
                if (steps[i] < cur) { sw->setStepSize(steps[i]); return; }
        }
    };

    // ── Frequency ───────────────────────────────────────────────────────
    // autoRepeat=true so holding the key continuously tunes (accessibility).
    m_mw.m_shortcutManager.registerAction("tune_up_1", "Tune Up (1 step)", "Frequency",
        QKeySequence(Qt::Key_Right), [nudgeFreq]() { nudgeFreq(1); }, true);
    m_mw.m_shortcutManager.registerAction("tune_down_1", "Tune Down (1 step)", "Frequency",
        QKeySequence(Qt::Key_Left), [nudgeFreq]() { nudgeFreq(-1); }, true);
    m_mw.m_shortcutManager.registerAction("tune_up_10", "Tune Up (10 steps)", "Frequency",
        QKeySequence(Qt::SHIFT | Qt::Key_Right), [nudgeFreq]() { nudgeFreq(10); }, true);
    m_mw.m_shortcutManager.registerAction("tune_down_10", "Tune Down (10 steps)", "Frequency",
        QKeySequence(Qt::SHIFT | Qt::Key_Left), [nudgeFreq]() { nudgeFreq(-10); }, true);
    m_mw.m_shortcutManager.registerAction("tune_up_1mhz", "Tune Up 1 MHz", "Frequency",
        QKeySequence(), [nudgeFreq]() { nudgeFreq(10000); });
    m_mw.m_shortcutManager.registerAction("tune_down_1mhz", "Tune Down 1 MHz", "Frequency",
        QKeySequence(), [nudgeFreq]() { nudgeFreq(-10000); });
    m_mw.m_shortcutManager.registerAction("go_to_freq", "Go to Frequency", "Frequency",
        QKeySequence(Qt::Key_G), [this]() {
            auto* s = m_mw.activeSlice();
            auto* sw = s ? m_mw.spectrumForSlice(s) : nullptr;
            auto* vfo = (s && sw) ? sw->vfoWidget(s->sliceId()) : nullptr;
            if (!s || !vfo) return;
            m_mw.m_sliceCtl->centerActiveSliceInPanadapter(true);
            QPointer<VfoWidget> vfoGuard = vfo;
            QTimer::singleShot(0, this, [vfoGuard]() {
                if (vfoGuard)
                    vfoGuard->beginDirectEntry();
            });
        });

    // ── Band ────────────────────────────────────────────────────────────
    struct BandDef { const char* id; const char* name; double mhz; };
    static const BandDef bands[] = {
        {"band_160m", "160m", 1.900}, {"band_80m", "80m", 3.800},
        {"band_60m", "60m", 5.357},   {"band_40m", "40m", 7.200},
        {"band_30m", "30m", 10.125},  {"band_20m", "20m", 14.225},
        {"band_17m", "17m", 18.118},  {"band_15m", "15m", 21.300},
        {"band_12m", "12m", 24.940},  {"band_10m", "10m", 28.400},
        {"band_6m",  "6m",  50.125},  {"band_2m",  "2m",  146.000},
    };
    for (const auto& b : bands) {
        double freq = b.mhz;
        m_mw.m_shortcutManager.registerAction(b.id, b.name, "Band",
            QKeySequence(), [this, freq]() {
                if (!m_mw.m_radioModel.isConnected()) return;
                auto* s = m_mw.activeSlice();
                if (s && !s->isLocked()) s->tuneAndRecenter(freq);
            });
    }

    // ── Mode ────────────────────────────────────────────────────────────
    static const char* modes[] = {"USB", "LSB", "CW", "CWL", "AM", "SAM", "FM", "NFM", "DFM", "DIGU", "DIGL", "RTTY"};
    for (const char* mode : modes) {
        QString m = mode;
        m_mw.m_shortcutManager.registerAction(
            QString("mode_%1").arg(m.toLower()), m, "Mode",
            QKeySequence(), [this, m]() {
                if (!m_mw.m_radioModel.isConnected()) return;
                auto* s = m_mw.activeSlice();
                if (s) s->setMode(m);
            });
    }

    // ── TX ──────────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("mox_toggle", "MOX Toggle", "TX",
        QKeySequence(Qt::Key_T), [this]() {
            if (!m_mw.m_radioModel.isConnected()) return;
            m_mw.m_radioModel.setTransmit(!m_mw.m_radioModel.transmitModel().isTransmitting());
        });
    // PTT (Hold) via Space is handled by the app-level event filter
    // because QShortcut has no "released" signal. Register with null
    // handler so the keyboard map shows it as bound.
    m_mw.m_shortcutManager.registerAction("ptt_hold", "PTT (Hold)", "TX",
        QKeySequence(Qt::Key_Space), nullptr);
    m_mw.m_shortcutManager.registerAction("tune_toggle", "TUNE Toggle", "TX",
        QKeySequence(), [this]() {
            if (!m_mw.m_radioModel.isConnected()) return;
            if (m_mw.m_radioModel.transmitModel().isTuning())
                m_mw.m_radioModel.transmitModel().stopTune();
            else
                m_mw.m_radioModel.transmitModel().startTune();
        });

    // ── Audio ───────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("af_gain_up", "AF Gain Up", "Audio",
        QKeySequence(Qt::Key_Up), [this]() {
            auto* s = m_mw.activeSlice();
            if (s) s->setAudioGain(std::min(100.0f, s->audioGain() + 5.0f));
        });
    m_mw.m_shortcutManager.registerAction("af_gain_down", "AF Gain Down", "Audio",
        QKeySequence(Qt::Key_Down), [this]() {
            auto* s = m_mw.activeSlice();
            if (s) s->setAudioGain(std::max(0.0f, s->audioGain() - 5.0f));
        });
    m_mw.m_shortcutManager.registerAction("mute_toggle", "Mute Toggle", "Audio",
        QKeySequence(Qt::Key_M), [this]() {
            auto* s = m_mw.activeSlice();
            if (s) s->setAudioMute(!s->audioMute());
        });

    // ── Slice ───────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("next_slice", "Next Slice", "Slice",
        QKeySequence(), [this]() {
            const auto& slices = m_mw.m_radioModel.slices();
            if (slices.size() <= 1) return;
            int idx = 0;
            for (int i = 0; i < slices.size(); ++i)
                if (slices[i]->sliceId() == m_mw.m_activeSliceId) { idx = i; break; }
            m_mw.m_sliceCtl->setActiveSlice(slices[(idx + 1) % slices.size()]->sliceId());
        });
    m_mw.m_shortcutManager.registerAction("prev_slice", "Prev Slice", "Slice",
        QKeySequence(), [this]() {
            const auto& slices = m_mw.m_radioModel.slices();
            if (slices.size() <= 1) return;
            int idx = 0;
            for (int i = 0; i < slices.size(); ++i)
                if (slices[i]->sliceId() == m_mw.m_activeSliceId) { idx = i; break; }
            m_mw.m_sliceCtl->setActiveSlice(slices[(idx - 1 + slices.size()) % slices.size()]->sliceId());
        });
    m_mw.m_shortcutManager.registerAction("split_toggle", "Split Toggle", "Slice",
        QKeySequence(), [this]() {
            m_mw.m_sliceCtl->toggleSplitFromShortcut();
        });

    // ── Filter ──────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("filter_widen", "Filter Widen", "Filter",
        QKeySequence(), [this]() {
            auto* s = m_mw.activeSlice();
            if (!s) return;
            s->setFilterWidth(s->filterLow(), s->filterHigh() + 100);
        });
    m_mw.m_shortcutManager.registerAction("filter_narrow", "Filter Narrow", "Filter",
        QKeySequence(), [this]() {
            auto* s = m_mw.activeSlice();
            if (!s) return;
            s->setFilterWidth(s->filterLow(), std::max(s->filterLow() + 50, s->filterHigh() - 100));
        });

    // ── Tuning ──────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("step_up", "Step Size Up", "Tuning",
        QKeySequence(Qt::Key_BracketRight), [cycleStep]() { cycleStep(1); });
    m_mw.m_shortcutManager.registerAction("step_down", "Step Size Down", "Tuning",
        QKeySequence(Qt::Key_BracketLeft), [cycleStep]() { cycleStep(-1); });
    m_mw.m_shortcutManager.registerAction("lock_toggle", "Tune Lock Toggle", "Tuning",
        QKeySequence(Qt::Key_L), [this]() {
            auto* s = m_mw.activeSlice();
            if (s) s->setLocked(!s->isLocked());
        });

    static constexpr double kPanZoomFactor = 1.5;
    auto zoomActivePanadapter = [this](double factor) {
        if (!m_mw.m_radioModel.isConnected()) {
            return;
        }

        auto* s = m_mw.activeSlice();
        if (!s || s->panId().isEmpty()) {
            return;
        }

        auto* sw = m_mw.spectrumForSlice(s);
        if (!sw) {
            return;
        }

        const double currentBw = sw->bandwidthMhz();
        // Clamp to limits so the final keypress snaps to exact min/max (#1458).
        const double newBw = std::clamp(currentBw * factor,
                                        m_mw.m_radioModel.minPanBandwidthMhz(),
                                        m_mw.m_radioModel.maxPanBandwidthMhz());
        if (newBw == currentBw) return;  // already at the hard limit

        double newCenter = sw->centerMhz();

        // When zooming in, shift center toward the active slice frequency
        // so it stays visible (mirrors Flex SDR behavior)
        if (factor < 1.0) {
            const double vfoMhz = s->frequency();
            const double halfBw = newBw / 2.0;
            if (vfoMhz > newCenter + halfBw)
                newCenter = vfoMhz - halfBw * 0.8;
            else if (vfoMhz < newCenter - halfBw)
                newCenter = vfoMhz + halfBw * 0.8;
        }
        newCenter = std::max(newCenter, newBw / 2.0);

        sw->setFrequencyRange(newCenter, newBw);
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 bandwidth=%2").arg(s->panId()).arg(newBw, 0, 'f', 6));
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 center=%2").arg(s->panId()).arg(newCenter, 0, 'f', 6));
    };

    // ── DSP ─────────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("nb_toggle", "NB Toggle", "DSP",
        QKeySequence(), [this]() {
            auto* s = m_mw.activeSlice();
            if (s) s->setNb(!s->nbOn());
        });
    m_mw.m_shortcutManager.registerAction("nr_cycle", "NR Cycle (Off/NR/NR2/NR4/DFNR)", "DSP",
        QKeySequence(), [this]() {
            auto* s = m_mw.activeSlice();
            if (!s) return;
            if (m_mw.m_audio->dfnrEnabled()) {
                // DFNR → off
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setDfnrEnabled(false); });
            } else if (m_mw.m_audio->nr4Enabled()) {
                // NR4 → DFNR
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr4Enabled(false); });
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setDfnrEnabled(true); });
            } else if (m_mw.m_audio->nr2Enabled()) {
                // NR2 → NR4
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(false); });
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr4Enabled(true); });
            } else if (s->nrOn()) {
                // NR → NR2
                s->setNr(false);
                m_mw.m_dspCtl->enableNr2WithWisdom();
            } else {
                // off → NR
                s->setNr(true);
            }
        });
    m_mw.m_shortcutManager.registerAction("anf_toggle", "ANF Toggle", "DSP",
        QKeySequence(), [this]() {
            auto* s = m_mw.activeSlice();
            if (s) s->setAnf(!s->anfOn());
        });

    // ── AGC ─────────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("agc_cycle", "AGC Mode Cycle", "AGC",
        QKeySequence(), [this]() {
            auto* s = m_mw.activeSlice();
            if (!s) return;
            static const char* modes[] = {"off", "slow", "med", "fast"};
            QString cur = s->agcMode().toLower();
            int idx = 0;
            for (int i = 0; i < 4; ++i)
                if (cur == modes[i]) { idx = i; break; }
            s->setAgcMode(modes[(idx + 1) % 4]);
        });

    // ── Display ─────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("band_zoom", "Band Zoom", "Display",
        QKeySequence(), [this]() {
            if (!m_mw.m_radioModel.isConnected()) return;
            auto* s = m_mw.activeSlice();
            if (s) m_mw.m_radioModel.sendCommand(
                QString("slice set %1 band_zoom=1").arg(s->sliceId()));
        });
    m_mw.m_shortcutManager.registerAction("segment_zoom", "Segment Zoom", "Display",
        QKeySequence(), [this]() {
            if (!m_mw.m_radioModel.isConnected()) return;
            auto* s = m_mw.activeSlice();
            if (s) m_mw.m_radioModel.sendCommand(
                QString("slice set %1 segment_zoom=1").arg(s->sliceId()));
        });
    m_mw.m_shortcutManager.registerAction("pan_zoom_in", "Panadapter Zoom In", "Display",
        QKeySequence(Qt::Key_Equal), [zoomActivePanadapter]() { zoomActivePanadapter(1.0 / kPanZoomFactor); });
    m_mw.m_shortcutManager.registerAction("pan_zoom_out", "Panadapter Zoom Out", "Display",
        QKeySequence(Qt::Key_Minus), [zoomActivePanadapter]() { zoomActivePanadapter(kPanZoomFactor); });
    m_mw.m_shortcutManager.registerAction("open_memories", "Open Memories Dialog", "Display",
        QKeySequence(Qt::Key_Slash), [this]() { m_mw.m_memoryCtl->showMemoryDialog(); });

    // ── Logging / SO2R (unbound by default; users assign keys) ──────────
    m_mw.m_shortcutManager.registerAction("log_open", "Open Logbook", "Logging",
        QKeySequence(), [this]() { m_mw.m_toolsCtl->showLogbook(); });
    m_mw.m_shortcutManager.registerAction("log_quick_entry", "Log: Quick Entry", "Logging",
        QKeySequence(), [this]() { m_mw.m_toolsCtl->focusLogQuickEntry(); });
    m_mw.m_shortcutManager.registerAction("so2r_swap_tx", "SO2R: Swap TX", "Logging",
        QKeySequence(), [this]() { if (So2rController* s = m_mw.m_toolsCtl->so2r()) { s->swapTx(); } });
    m_mw.m_shortcutManager.registerAction("so2r_swap_focus", "SO2R: Swap Focus", "Logging",
        QKeySequence(), [this]() { if (So2rController* s = m_mw.m_toolsCtl->so2r()) { s->swapFocus(); } });
    m_mw.m_shortcutManager.registerAction("so2r_toggle_role", "SO2R: Run/S&P", "Logging",
        QKeySequence(), [this]() { if (So2rController* s = m_mw.m_toolsCtl->so2r()) { s->toggleRole(); } });

    // ── Scanner (unbound by default; users assign keys in Keyboard Shortcuts) ──
    m_mw.m_shortcutManager.registerAction("scan_open", "Open Scanner", "Scanner",
        QKeySequence(), [this]() { m_mw.m_memoryCtl->showScannerPanel(); });
    m_mw.m_shortcutManager.registerAction("scan_start_stop", "Scan Start/Stop", "Scanner",
        QKeySequence(), [this]() {
            if (ScannerPanel* p = m_mw.m_memoryCtl->scannerPanel()) { p->toggleStartStop(); }
        });
    m_mw.m_shortcutManager.registerAction("scan_pause", "Scan Pause/Resume", "Scanner",
        QKeySequence(), [this]() {
            if (ScannerPanel* p = m_mw.m_memoryCtl->scannerPanel()) { p->togglePause(); }
        });
    m_mw.m_shortcutManager.registerAction("scan_skip", "Scan Skip Channel", "Scanner",
        QKeySequence(), [this]() {
            if (ScannerPanel* p = m_mw.m_memoryCtl->scannerPanel()) { p->skipCurrent(); }
        });

    // ── RIT/XIT ─────────────────────────────────────────────────────────
    m_mw.m_shortcutManager.registerAction("rit_toggle", "RIT Toggle", "RIT/XIT",
        QKeySequence(), [this]() {
            auto* s = m_mw.activeSlice();
            if (s) s->setRit(!s->ritOn(), s->ritFreq());
        });
    m_mw.m_shortcutManager.registerAction("xit_toggle", "XIT Toggle", "RIT/XIT",
        QKeySequence(), [this]() {
            auto* s = m_mw.activeSlice();
            if (s) s->setXit(!s->xitOn(), s->xitFreq());
        });

    // ── Command palette ─────────────────────────────────────────────────
    // Registered like any other action so it shows up in the keyboard
    // shortcuts editor and can be rebound. The palette filters its own entry
    // out of the result list.
    m_mw.m_shortcutManager.registerAction(
        "command_palette", "Command Palette", "View",
        QKeySequence("Ctrl+Shift+P"),
        [this] { showCommandPalette(); });

    // ── Load user bindings and create QShortcuts ────────────────────────
    m_mw.m_shortcutManager.loadBindings();
    s_keyboardShortcutsEnabled = m_mw.m_keyboardShortcutsEnabled;
    m_mw.m_shortcutManager.rebuildShortcuts(&m_mw, shortcutGuard);

    // Disable all QShortcuts while a slider has keyboard focus so arrow keys
    // (and other slider keys) reach the slider instead of being consumed by
    // window-level shortcuts (e.g. frequency tune left/right). Qt processes
    // shortcuts before dispatching key events to focused widgets.
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget* /*old*/, QWidget* now) {
        m_mw.m_shortcutManager.setShortcutsEnabled(
            !qobject_cast<QAbstractSlider*>(now));
    });
}

void ShortcutsController::showCommandPalette()
{
    // Created on first use and kept alive: it is parented to the window, and
    // rebuilding the list is cheap enough that there is no reason to churn the
    // dialog on every invocation.
    if (!m_commandPalette)
        m_commandPalette = new CommandPalette(&m_mw.m_shortcutManager, &m_mw);
    m_commandPalette->showPalette();
}

} // namespace AetherSDR
