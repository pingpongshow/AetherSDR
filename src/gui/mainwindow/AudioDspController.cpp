#include "gui/mainwindow/AudioDspController.h"

#include "gui/MainWindow.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "gui/SpectrumWidget.h"
#include "gui/SpectrumOverlayMenu.h"
#include "gui/VfoWidget.h"
#include "gui/AppletPanel.h"
#include "gui/RxApplet.h"
#include "gui/ClientEqApplet.h"
#include "gui/ClientEqEditor.h"
#include "gui/ClientCompApplet.h"
#include "gui/ClientCompEditor.h"
#include "gui/ClientGateApplet.h"
#include "gui/ClientGateEditor.h"
#include "gui/ClientDeEssApplet.h"
#include "gui/ClientDeEssEditor.h"
#include "gui/ClientTubeApplet.h"
#include "gui/ClientTubeEditor.h"
#include "gui/ClientPuduApplet.h"
#include "gui/ClientPuduEditor.h"
#include "gui/ClientReverbApplet.h"
#include "gui/ClientReverbEditor.h"
#include "gui/ClientChainApplet.h"
#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientDeEss.h"
#include "core/ClientTube.h"
#include "core/ClientPudu.h"
#include "core/ClientReverb.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "gui/AetherDspDialog.h"
#include "gui/DspParamPopup.h"
#include <QTimer>
#include <QPropertyAnimation>
#include <QProgressDialog>
#include <QThread>
#include "core/AppSettings.h"

namespace AetherSDR {

AudioDspController::AudioDspController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void AudioDspController::wireNrButtonSync()
{
    // ── NR2/RN2 feedback: AudioEngine → all VFO + overlay buttons ──────
    // Iterate all panadapter spectrums to find VFO widgets and overlay menus,
    // since spectrum()/vfoWidget() lookups can return null depending on
    // pan activation state.
    auto syncNr2 = [this](bool on) {
        // Iterate PanadapterStack's actual applets — avoids RadioModel→PanStack
        // ID mismatch during connect when rekey hasn't happened yet.
        if (m_mw.m_panStack) {
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                if (auto* vfo = sw->vfoWidget(m_mw.m_activeSliceId)) {
                    QSignalBlocker sb(vfo->nr2Button());
                    vfo->nr2Button()->setChecked(on);
                }
                if (auto* btn = sw->overlayMenu()->dspNr2Button())
                    { QSignalBlocker sb(btn); btn->setChecked(on); }
            }
        }
    };
    auto syncRn2 = [this](bool on) {
        if (m_mw.m_panStack) {
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                if (auto* vfo = sw->vfoWidget(m_mw.m_activeSliceId)) {
                    QSignalBlocker sb(vfo->rn2Button());
                    vfo->rn2Button()->setChecked(on);
                }
                if (auto* btn = sw->overlayMenu()->dspRn2Button())
                    { QSignalBlocker sb(btn); btn->setChecked(on); }
            }
        }
    };
    auto syncBnr = [this](bool on) {
        if (m_mw.m_panStack) {
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                if (auto* vfo = sw->vfoWidget(m_mw.m_activeSliceId)) {
                    QSignalBlocker sb(vfo->bnrButton());
                    vfo->bnrButton()->setChecked(on);
                }
                if (auto* btn = sw->overlayMenu()->dspBnrButton())
                    { QSignalBlocker sb(btn); btn->setChecked(on); }
            }
        }
    };
    auto syncNr4 = [this](bool on) {
        if (m_mw.m_panStack) {
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                if (auto* vfo = sw->vfoWidget(m_mw.m_activeSliceId)) {
                    QSignalBlocker sb(vfo->nr4Button());
                    vfo->nr4Button()->setChecked(on);
                }
                if (auto* btn = sw->overlayMenu()->dspNr4Button())
                    { QSignalBlocker sb(btn); btn->setChecked(on); }
            }
        }
    };
    auto syncMnr = [this](bool on) {
        if (m_mw.m_panStack) {
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                if (auto* vfo = sw->vfoWidget(m_mw.m_activeSliceId)) {
                    QSignalBlocker sb(vfo->mnrButton());
                    vfo->mnrButton()->setChecked(on);
                }
                if (auto* btn = sw->overlayMenu()->dspMnrButton())
                    { QSignalBlocker sb(btn); btn->setChecked(on); }
            }
        }
    };
    auto syncDfnr = [this](bool on) {
        if (m_mw.m_panStack) {
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                auto* sw = applet->spectrumWidget();
                if (auto* vfo = sw->vfoWidget(m_mw.m_activeSliceId)) {
                    QSignalBlocker sb(vfo->dfnrButton());
                    vfo->dfnrButton()->setChecked(on);
                }
                if (auto* btn = sw->overlayMenu()->dspDfnrButton())
                    { QSignalBlocker sb(btn); btn->setChecked(on); }
            }
        }
    };
    connect(m_mw.m_audio, &AudioEngine::nr2EnabledChanged, this, syncNr2);
    connect(m_mw.m_audio, &AudioEngine::rn2EnabledChanged, this, syncRn2);
    connect(m_mw.m_audio, &AudioEngine::bnrEnabledChanged, this, syncBnr);
    connect(m_mw.m_audio, &AudioEngine::nr4EnabledChanged, this, syncNr4);
    connect(m_mw.m_audio, &AudioEngine::mnrEnabledChanged, this, syncMnr);
    connect(m_mw.m_audio, &AudioEngine::dfnrEnabledChanged, this, syncDfnr);
    // NR2/RN2/BNR/NR4/DFNR DSP controls now only in VFO DSP tab and spectrum overlay.
    // RxApplet DSP buttons removed — no sync wiring needed.
}

void AudioDspController::wireClientDspChain()
{
    // ── Client EQ applet: client-side parametric EQ for RX/TX paths ──────────
    m_mw.m_appletPanel->clientEqApplet()->setAudioEngine(m_mw.m_audio);
    // Edit… requests land here — the floating editor window lands in the
    // next PR in this series. For now we surface a concise toast so users
    // understand the feature is arriving incrementally.
    connect(m_mw.m_appletPanel->clientEqApplet(), &ClientEqApplet::editRequested,
            this, [this](ClientEqApplet::Path path) {
        if (!m_clientEqEditor) {
            m_clientEqEditor = new ClientEqEditor(m_mw.m_audio, &m_mw);
            // Editor-side bypass updates the applet's Enable toggle and
            // drives CEQ tile visibility for TX-path changes (the tile
            // follows TX EQ bypass state in sync with the CHAIN widget).
            connect(m_clientEqEditor, &ClientEqEditor::bypassToggled,
                    this, [this](ClientEqApplet::Path bypassPath, bool bypassed) {
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientEqApplet())
                    m_mw.m_appletPanel->clientEqApplet()->refreshEnableFromEngine();
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                    m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                if (bypassPath == ClientEqApplet::Path::Tx && m_mw.m_appletPanel)
                    m_mw.m_appletPanel->setAppletVisible("CEQ", !bypassed);
            });
        }
        m_clientEqEditor->showForPath(path);
    });

    // ── Client Compressor applet: client-side TX dynamics processor ──────────
    m_mw.m_appletPanel->clientCompApplet()->setAudioEngine(m_mw.m_audio);
    connect(m_mw.m_appletPanel->clientCompApplet(), &ClientCompApplet::editRequested,
            this, [this]() {
        if (!m_clientCompEditor) {
            m_clientCompEditor = new ClientCompEditor(m_mw.m_audio, &m_mw);
            connect(m_clientCompEditor, &ClientCompEditor::bypassToggled,
                    this, [this](bool bypassed) {
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientCompApplet())
                    m_mw.m_appletPanel->clientCompApplet()->refreshEnableFromEngine();
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                    m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                if (m_mw.m_appletPanel)
                    m_mw.m_appletPanel->setAppletVisible("CMP", !bypassed);
            });
        }
        m_clientCompEditor->showForTx();
    });

    // ── Client Gate applet: TX downward expander / noise gate (#1661 Phase 2) ─
    m_mw.m_appletPanel->clientGateApplet()->setAudioEngine(m_mw.m_audio);
    connect(m_mw.m_appletPanel->clientGateApplet(), &ClientGateApplet::editRequested,
            this, [this]() {
        if (!m_clientGateEditor) {
            m_clientGateEditor = new ClientGateEditor(m_mw.m_audio, &m_mw);
            connect(m_clientGateEditor, &ClientGateEditor::bypassToggled,
                    this, [this](bool bypassed) {
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientGateApplet())
                    m_mw.m_appletPanel->clientGateApplet()->refreshEnableFromEngine();
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                    m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                if (m_mw.m_appletPanel)
                    m_mw.m_appletPanel->setAppletVisible("GATE", !bypassed);
            });
        }
        m_clientGateEditor->showForTx();
    });

    // ── Client De-esser applet: TX sidechain-filtered dynamics (#1661 Phase 3) ─
    m_mw.m_appletPanel->clientDeEssApplet()->setAudioEngine(m_mw.m_audio);
    connect(m_mw.m_appletPanel->clientDeEssApplet(), &ClientDeEssApplet::editRequested,
            this, [this]() {
        if (!m_clientDeEssEditor) {
            m_clientDeEssEditor = new ClientDeEssEditor(m_mw.m_audio, &m_mw);
            connect(m_clientDeEssEditor, &ClientDeEssEditor::bypassToggled,
                    this, [this](bool bypassed) {
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientDeEssApplet())
                    m_mw.m_appletPanel->clientDeEssApplet()->refreshEnableFromEngine();
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                    m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                if (m_mw.m_appletPanel)
                    m_mw.m_appletPanel->setAppletVisible("DESS", !bypassed);
            });
        }
        m_clientDeEssEditor->showForTx();
    });

    // ── Client Tube applet: TX dynamic tube saturator (#1661 Phase 4) ────────
    m_mw.m_appletPanel->clientTubeApplet()->setAudioEngine(m_mw.m_audio);
    connect(m_mw.m_appletPanel->clientTubeApplet(), &ClientTubeApplet::editRequested,
            this, [this]() {
        if (!m_clientTubeEditor) {
            m_clientTubeEditor = new ClientTubeEditor(m_mw.m_audio, &m_mw);
            connect(m_clientTubeEditor, &ClientTubeEditor::bypassToggled,
                    this, [this](bool bypassed) {
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientTubeApplet())
                    m_mw.m_appletPanel->clientTubeApplet()->refreshEnableFromEngine();
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                    m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                if (m_mw.m_appletPanel)
                    m_mw.m_appletPanel->setAppletVisible("TUBE", !bypassed);
            });
        }
        m_clientTubeEditor->showForTx();
    });

    // ── Client Reverb applet: TX reverb (Freeverb) ─
    m_mw.m_appletPanel->clientReverbApplet()->setAudioEngine(m_mw.m_audio);

    // ── Client PUDU applet: TX exciter — PooDoo™ centrepiece (#1661 Phase 5) ─
    m_mw.m_appletPanel->clientPuduApplet()->setAudioEngine(m_mw.m_audio);
    connect(m_mw.m_appletPanel->clientPuduApplet(), &ClientPuduApplet::editRequested,
            this, [this]() {
        if (!m_clientPuduEditor) {
            m_clientPuduEditor = new ClientPuduEditor(m_mw.m_audio, &m_mw);
            connect(m_clientPuduEditor, &ClientPuduEditor::bypassToggled,
                    this, [this](bool bypassed) {
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientPuduApplet())
                    m_mw.m_appletPanel->clientPuduApplet()->refreshEnableFromEngine();
                if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                    m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                if (m_mw.m_appletPanel)
                    m_mw.m_appletPanel->setAppletVisible("PUDU", !bypassed);
            });
        }
        m_clientPuduEditor->showForTx();
    });

    // ── TX signal-chain applet (#1661) ──────────────────────────────────────
    // Visual strip showing MIC → stages → TX with per-stage bypass +
    // drag-drop reorder.  Clicking a stage opens its floating editor
    // (only EQ and COMP have editors today; Gate / DeEss / Tube / Enh
    // are placeholders awaiting their DSP).
    //
    // CEQ and CMP tile visibility is driven by DSP bypass state — the
    // chain widget right-click menu, and the Bypass buttons inside
    // each floating editor, all push the new state through
    // stageEnabledChanged → setAppletVisible here.  Initial state is
    // seeded from the engine below so settings persist across launches.
    m_mw.m_appletPanel->clientChainApplet()->setAudioEngine(m_mw.m_audio);
    // Seed the PooDoo MIC-ready + TX-pulse indicators from current
    // state — subsequent changes flow through the TransmitModel
    // signal connections above (micStateChanged + moxChanged).
    {
        const auto& tx = m_mw.m_radioModel.transmitModel();
        const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
        m_mw.m_appletPanel->clientChainApplet()->setMicInputReady(ready);
        m_mw.m_appletPanel->clientChainApplet()->setTxActive(
            ready && tx.isTransmitting());
    }
    // Pulse the TX endpoint red when we're transmitting AND PooDoo
    // is actually in the signal path (MIC=PC and DAX off).  Otherwise
    // the pulse would lie about what's being processed.
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool txActive) {
        if (!m_mw.m_appletPanel || !m_mw.m_appletPanel->clientChainApplet()) return;
        const auto& tx = m_mw.m_radioModel.transmitModel();
        const bool ready = (tx.micSelection() == "PC") && !tx.daxOn();
        m_mw.m_appletPanel->clientChainApplet()->setTxActive(ready && txActive);
    });

    // ── PUDU monitor wiring ─────────────────────────────────────
    auto* chainApplet = m_mw.m_appletPanel->clientChainApplet();
    chainApplet->setMonitorHasRecording(m_mw.m_puduMonitor->hasRecording());

    // User-click → start/stop based on current monitor state.  The
    // monitor's own signals drive the button visuals back.
    connect(chainApplet, &ClientChainApplet::monitorRecordClicked,
            this, [this]() {
        if (m_mw.m_puduMonitor->isRecording()) {
            m_mw.m_puduMonitor->stopRecording();
        } else {
            // Don't record while playing — button shouldn't be
            // enabled in that state, but guard anyway.
            if (m_mw.m_puduMonitor->isPlaying()) m_mw.m_puduMonitor->stopPlayback();
            m_mw.m_puduMonitor->startRecording();
        }
    });
    connect(chainApplet, &ClientChainApplet::monitorPlayClicked,
            this, [this]() {
        if (m_mw.m_puduMonitor->isPlaying()) {
            m_mw.m_puduMonitor->stopPlayback();
        } else {
            m_mw.m_puduMonitor->startPlayback();
        }
    });

    // Monitor state → UI updates.  RX audio gating is handled
    // separately via the muteRxRequested wiring above.
    connect(m_mw.m_puduMonitor, &ClientPuduMonitor::recordingStarted,
            this, [this]() {
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
            m_mw.m_appletPanel->clientChainApplet()->setMonitorRecording(true);
    });
    connect(m_mw.m_puduMonitor, &ClientPuduMonitor::recordingStopped,
            this, [this](int /*durationMs*/) {
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet()) {
            auto* a = m_mw.m_appletPanel->clientChainApplet();
            a->setMonitorRecording(false);
            a->setMonitorHasRecording(true);
        }
        // Auto-start playback — the mute stays installed across the
        // transition because the monitor only emits muteRxRequested
        // (false) at stopPlayback().
        m_mw.m_puduMonitor->startPlayback();
    });
    connect(m_mw.m_puduMonitor, &ClientPuduMonitor::playbackStarted,
            this, [this]() {
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
            m_mw.m_appletPanel->clientChainApplet()->setMonitorPlaying(true);
    });
    connect(m_mw.m_puduMonitor, &ClientPuduMonitor::playbackStopped,
            this, [this]() {
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
            m_mw.m_appletPanel->clientChainApplet()->setMonitorPlaying(false);
    });
    if (m_mw.m_audio && m_mw.m_audio->clientEqTx()) {
        m_mw.m_appletPanel->setAppletVisible(
            "CEQ", m_mw.m_audio->clientEqTx()->isEnabled());
    }
    if (m_mw.m_audio && m_mw.m_audio->clientCompTx()) {
        m_mw.m_appletPanel->setAppletVisible(
            "CMP", m_mw.m_audio->clientCompTx()->isEnabled());
    }
    if (m_mw.m_audio && m_mw.m_audio->clientGateTx()) {
        m_mw.m_appletPanel->setAppletVisible(
            "GATE", m_mw.m_audio->clientGateTx()->isEnabled());
    }
    if (m_mw.m_audio && m_mw.m_audio->clientDeEssTx()) {
        m_mw.m_appletPanel->setAppletVisible(
            "DESS", m_mw.m_audio->clientDeEssTx()->isEnabled());
    }
    if (m_mw.m_audio && m_mw.m_audio->clientTubeTx()) {
        m_mw.m_appletPanel->setAppletVisible(
            "TUBE", m_mw.m_audio->clientTubeTx()->isEnabled());
    }
    if (m_mw.m_audio && m_mw.m_audio->clientPuduTx()) {
        m_mw.m_appletPanel->setAppletVisible(
            "PUDU", m_mw.m_audio->clientPuduTx()->isEnabled());
    }
    if (m_mw.m_audio && m_mw.m_audio->clientReverbTx()) {
        m_mw.m_appletPanel->setAppletVisible(
            "REVERB", m_mw.m_audio->clientReverbTx()->isEnabled());
    }

    // Initial applet-stack order mirrors the persisted chain order, and
    // stays in sync on every subsequent drag-reorder.
    if (m_mw.m_audio) {
        m_mw.m_appletPanel->setTxDspChainOrder(m_mw.m_audio->txChainStages());
    }
    connect(m_mw.m_appletPanel->clientChainApplet(),
            &ClientChainApplet::chainReordered,
            this, [this]() {
        if (m_mw.m_appletPanel && m_mw.m_audio)
            m_mw.m_appletPanel->setTxDspChainOrder(m_mw.m_audio->txChainStages());
    });

    connect(m_mw.m_appletPanel->clientChainApplet(),
            &ClientChainApplet::stageEnabledChanged,
            this, [this](AudioEngine::TxChainStage stage, bool enabled) {
        if (stage == AudioEngine::TxChainStage::Eq) {
            m_mw.m_appletPanel->setAppletVisible("CEQ", enabled);
            if (m_mw.m_appletPanel->clientEqApplet())
                m_mw.m_appletPanel->clientEqApplet()->refreshEnableFromEngine();
        } else if (stage == AudioEngine::TxChainStage::Comp) {
            m_mw.m_appletPanel->setAppletVisible("CMP", enabled);
            if (m_mw.m_appletPanel->clientCompApplet())
                m_mw.m_appletPanel->clientCompApplet()->refreshEnableFromEngine();
        } else if (stage == AudioEngine::TxChainStage::Gate) {
            m_mw.m_appletPanel->setAppletVisible("GATE", enabled);
            if (m_mw.m_appletPanel->clientGateApplet())
                m_mw.m_appletPanel->clientGateApplet()->refreshEnableFromEngine();
        } else if (stage == AudioEngine::TxChainStage::DeEss) {
            m_mw.m_appletPanel->setAppletVisible("DESS", enabled);
            if (m_mw.m_appletPanel->clientDeEssApplet())
                m_mw.m_appletPanel->clientDeEssApplet()->refreshEnableFromEngine();
        } else if (stage == AudioEngine::TxChainStage::Tube) {
            m_mw.m_appletPanel->setAppletVisible("TUBE", enabled);
            if (m_mw.m_appletPanel->clientTubeApplet())
                m_mw.m_appletPanel->clientTubeApplet()->refreshEnableFromEngine();
        } else if (stage == AudioEngine::TxChainStage::Enh) {
            // Enh slot hosts the PUDU exciter.
            m_mw.m_appletPanel->setAppletVisible("PUDU", enabled);
            if (m_mw.m_appletPanel->clientPuduApplet())
                m_mw.m_appletPanel->clientPuduApplet()->refreshEnableFromEngine();
        } else if (stage == AudioEngine::TxChainStage::Reverb) {
            m_mw.m_appletPanel->setAppletVisible("REVERB", enabled);
            if (m_mw.m_appletPanel->clientReverbApplet())
                m_mw.m_appletPanel->clientReverbApplet()->refreshEnableFromEngine();
        }
    });
    connect(m_mw.m_appletPanel->clientChainApplet(),
            &ClientChainApplet::editRequested,
            this, [this](AudioEngine::TxChainStage stage) {
        switch (stage) {
            case AudioEngine::TxChainStage::Eq:
                if (!m_clientEqEditor) {
                    m_clientEqEditor = new ClientEqEditor(m_mw.m_audio, &m_mw);
                    connect(m_clientEqEditor, &ClientEqEditor::bypassToggled,
                            this, [this](ClientEqApplet::Path path, bool bypassed) {
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientEqApplet())
                            m_mw.m_appletPanel->clientEqApplet()->refreshEnableFromEngine();
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                            m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                        // TX bypass drives CEQ tile visibility to stay in
                        // sync with the chain widget.  RX bypass is not
                        // represented in the TX chain, so no tile change.
                        if (path == ClientEqApplet::Path::Tx && m_mw.m_appletPanel)
                            m_mw.m_appletPanel->setAppletVisible("CEQ", !bypassed);
                    });
                }
                m_clientEqEditor->showForPath(ClientEqApplet::Path::Tx);
                break;
            case AudioEngine::TxChainStage::Comp:
                if (!m_clientCompEditor) {
                    m_clientCompEditor = new ClientCompEditor(m_mw.m_audio, &m_mw);
                    connect(m_clientCompEditor, &ClientCompEditor::bypassToggled,
                            this, [this](bool bypassed) {
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientCompApplet())
                            m_mw.m_appletPanel->clientCompApplet()->refreshEnableFromEngine();
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                            m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                        if (m_mw.m_appletPanel)
                            m_mw.m_appletPanel->setAppletVisible("CMP", !bypassed);
                    });
                }
                m_clientCompEditor->showForTx();
                break;
            case AudioEngine::TxChainStage::Gate:
                if (!m_clientGateEditor) {
                    m_clientGateEditor = new ClientGateEditor(m_mw.m_audio, &m_mw);
                    connect(m_clientGateEditor, &ClientGateEditor::bypassToggled,
                            this, [this](bool bypassed) {
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientGateApplet())
                            m_mw.m_appletPanel->clientGateApplet()->refreshEnableFromEngine();
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                            m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                        if (m_mw.m_appletPanel)
                            m_mw.m_appletPanel->setAppletVisible("GATE", !bypassed);
                    });
                }
                m_clientGateEditor->showForTx();
                break;
            case AudioEngine::TxChainStage::DeEss:
                if (!m_clientDeEssEditor) {
                    m_clientDeEssEditor = new ClientDeEssEditor(m_mw.m_audio, &m_mw);
                    connect(m_clientDeEssEditor, &ClientDeEssEditor::bypassToggled,
                            this, [this](bool bypassed) {
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientDeEssApplet())
                            m_mw.m_appletPanel->clientDeEssApplet()->refreshEnableFromEngine();
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                            m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                        if (m_mw.m_appletPanel)
                            m_mw.m_appletPanel->setAppletVisible("DESS", !bypassed);
                    });
                }
                m_clientDeEssEditor->showForTx();
                break;
            case AudioEngine::TxChainStage::Tube:
                if (!m_clientTubeEditor) {
                    m_clientTubeEditor = new ClientTubeEditor(m_mw.m_audio, &m_mw);
                    connect(m_clientTubeEditor, &ClientTubeEditor::bypassToggled,
                            this, [this](bool bypassed) {
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientTubeApplet())
                            m_mw.m_appletPanel->clientTubeApplet()->refreshEnableFromEngine();
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                            m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                        if (m_mw.m_appletPanel)
                            m_mw.m_appletPanel->setAppletVisible("TUBE", !bypassed);
                    });
                }
                m_clientTubeEditor->showForTx();
                break;
            case AudioEngine::TxChainStage::Enh:
                // Enh slot hosts PUDU.
                if (!m_clientPuduEditor) {
                    m_clientPuduEditor = new ClientPuduEditor(m_mw.m_audio, &m_mw);
                    connect(m_clientPuduEditor, &ClientPuduEditor::bypassToggled,
                            this, [this](bool bypassed) {
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientPuduApplet())
                            m_mw.m_appletPanel->clientPuduApplet()->refreshEnableFromEngine();
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                            m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                        if (m_mw.m_appletPanel)
                            m_mw.m_appletPanel->setAppletVisible("PUDU", !bypassed);
                    });
                }
                m_clientPuduEditor->showForTx();
                break;
            case AudioEngine::TxChainStage::Reverb:
                if (!m_clientReverbEditor) {
                    m_clientReverbEditor = new ClientReverbEditor(m_mw.m_audio, &m_mw);
                    connect(m_clientReverbEditor, &ClientReverbEditor::bypassToggled,
                            this, [this](bool bypassed) {
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientReverbApplet())
                            m_mw.m_appletPanel->clientReverbApplet()->refreshEnableFromEngine();
                        if (m_mw.m_appletPanel && m_mw.m_appletPanel->clientChainApplet())
                            m_mw.m_appletPanel->clientChainApplet()->refreshFromEngine();
                        if (m_mw.m_appletPanel)
                            m_mw.m_appletPanel->setAppletVisible("REVERB", !bypassed);
                    });
                }
                m_clientReverbEditor->showForTx();
                break;
            default:
                break;
        }
    });

    // ── RX client DSP chain: open editors bound to the RX instances ──
    connect(m_mw.m_appletPanel->clientChainApplet(),
            &ClientChainApplet::rxEditRequested,
            this, [this](AudioEngine::TxChainStage stage) {
        switch (stage) {
            case AudioEngine::TxChainStage::Eq:
                if (!m_clientEqEditor) { m_clientEqEditor = new ClientEqEditor(m_mw.m_audio, &m_mw); }
                m_clientEqEditor->showForPath(ClientEqApplet::Path::Rx);
                break;
            case AudioEngine::TxChainStage::Comp:
                if (!m_clientCompEditor) { m_clientCompEditor = new ClientCompEditor(m_mw.m_audio, &m_mw); }
                m_clientCompEditor->showForRx();
                break;
            case AudioEngine::TxChainStage::Gate:
                if (!m_clientGateEditor) { m_clientGateEditor = new ClientGateEditor(m_mw.m_audio, &m_mw); }
                m_clientGateEditor->showForRx();
                break;
            case AudioEngine::TxChainStage::DeEss:
                if (!m_clientDeEssEditor) { m_clientDeEssEditor = new ClientDeEssEditor(m_mw.m_audio, &m_mw); }
                m_clientDeEssEditor->showForRx();
                break;
            case AudioEngine::TxChainStage::Tube:
                if (!m_clientTubeEditor) { m_clientTubeEditor = new ClientTubeEditor(m_mw.m_audio, &m_mw); }
                m_clientTubeEditor->showForRx();
                break;
            case AudioEngine::TxChainStage::Enh:
                if (!m_clientPuduEditor) { m_clientPuduEditor = new ClientPuduEditor(m_mw.m_audio, &m_mw); }
                m_clientPuduEditor->showForRx();
                break;
            case AudioEngine::TxChainStage::Reverb:
                if (!m_clientReverbEditor) { m_clientReverbEditor = new ClientReverbEditor(m_mw.m_audio, &m_mw); }
                m_clientReverbEditor->showForRx();
                break;
            default:
                break;
        }
    });
    connect(m_mw.m_appletPanel->clientChainApplet(),
            &ClientChainApplet::rxStageEnabledChanged,
            this, [this](AudioEngine::TxChainStage stage, bool) {
        // The CEQ tile has an RX tab — keep its Enable toggle in sync.
        if (stage == AudioEngine::TxChainStage::Eq && m_mw.m_appletPanel->clientEqApplet()) {
            m_mw.m_appletPanel->clientEqApplet()->refreshEnableFromEngine();
        }
    });
}

void AudioDspController::showAetherDspSettings()
{
    if (m_dspDialog) {
        m_dspDialog->raise();
        m_dspDialog->activateWindow();
        return;
    }
    auto* dlg = createAetherDspDialog();
    dlg->show();
}

void AudioDspController::updateNr2Availability()
{
    bool opusActive = (m_mw.m_radioModel.audioCompressionParam() == "opus");
    const QString tooltip = opusActive
        ? "NR2 is not available with compressed (SmartLink) audio"
        : "Client-side spectral noise reduction (Ephraim-Malah MMSE). Right-click for NR2 settings.";

    // If Opus just became active and NR2 is running, disable it
    if (opusActive && m_mw.m_audio->nr2Enabled()) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(false); });
        m_mw.statusBar()->showMessage("NR2 disabled — not available with compressed (SmartLink) audio", 4000);
    }

    // Update all NR2 buttons: VFO widgets and spectrum overlay menus
    if (m_mw.m_panStack) {
        for (auto* applet : m_mw.m_panStack->allApplets()) {
            // Overlay menu NR2 button
            if (auto* menu = applet->spectrumWidget()->overlayMenu()) {
                if (auto* btn = menu->dspNr2Button()) {
                    btn->setEnabled(!opusActive);
                    btn->setToolTip(tooltip);
                }
            }
            // VFO NR2 buttons (one per slice on this pan)
            for (auto* s : m_mw.m_radioModel.slices()) {
                if (auto* vfo = applet->spectrumWidget()->vfoWidget(s->sliceId())) {
                    if (auto* btn = vfo->nr2Button()) {
                        btn->setEnabled(!opusActive);
                        btn->setToolTip(tooltip);
                    }
                }
            }
        }
    }
}

void AudioDspController::enableNr2WithWisdom()
{
    if (AudioEngine::needsWisdomGeneration()) {
        auto* dlg = new QProgressDialog(
            "Optimizing FFT plans for NR2...\n\n"
            "This window will automatically close when wisdom generation is complete.",
            QString(), 0, 100, &m_mw);
        dlg->setWindowTitle("AetherSDR — FFTW Wisdom");
        dlg->setWindowModality(Qt::ApplicationModal);
        dlg->setMinimumDuration(0);
        dlg->setAutoClose(false);
        dlg->setAutoReset(false);
        dlg->setCancelButton(nullptr);
        dlg->setMinimumWidth(500);
        dlg->setStyleSheet(
            "QProgressBar { text-align: center; font-size: 13px;"
            " font-weight: bold; color: #c8d8e8;"
            " background: #1a2a3a; border: 1px solid #2e4e6e; border-radius: 3px; }"
            "QProgressBar::chunk { background: #00b4d8; }");
        dlg->show();

        auto* breathe = new QPropertyAnimation(dlg, "windowOpacity", dlg);
        breathe->setDuration(1500);
        breathe->setStartValue(1.0);
        breathe->setKeyValueAt(0.5, 0.55);
        breathe->setEndValue(1.0);
        breathe->setLoopCount(-1);

        auto* thread = QThread::create([this, dlg, breathe]() {
            AudioEngine::generateWisdom([dlg, breathe](int step, int total, const std::string& desc) {
                int pct = total > 0 ? (step * 100 / total) : 0;
                QString d = QString::fromStdString(desc);
                QMetaObject::invokeMethod(dlg, [dlg, breathe, pct, d]() {
                    if (!d.isEmpty()) {
                        dlg->setLabelText(d + "\n\n"
                            "This window will automatically close when wisdom generation is complete.");
                        if (dlg->value() >= 90 && breathe->state() != QAbstractAnimation::Running)
                            breathe->start();
                    } else {
                        dlg->setValue(pct);
                    }
                });
            });
        });
        connect(thread, &QThread::finished, this, [this, dlg, breathe, thread]() {
            breathe->stop();
            dlg->setWindowOpacity(1.0);
            dlg->setValue(100);
            dlg->setLabelText("Wisdom generation complete!");
            QTimer::singleShot(800, this, [this, dlg, thread]() {
                dlg->close();
                dlg->deleteLater();
                thread->deleteLater();
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(true); });
            });
        });
        thread->start();
    } else {
        QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(true); });
    }
}

void AudioDspController::showNr2ParamPopup(const QPoint& globalPos)
{
    auto& s = AppSettings::instance();
    auto* popup = new DspParamPopup(&m_mw);

    popup->addSlider("Reduction (dB)", 10, 300,
        static_cast<int>(s.value("NR2GainMax", "1.50").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR2GainMax", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, val]() { m_mw.m_audio->setNr2GainMax(val); });
        });

    popup->addSlider("Smoothing",  50, 98,
        static_cast<int>(s.value("NR2GainSmooth", "0.85").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR2GainSmooth", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, val]() { m_mw.m_audio->setNr2GainSmooth(val); });
        });

    popup->addSlider("Voice Threshold", 1, 50,
        static_cast<int>(s.value("NR2Qspp", "0.20").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR2Qspp", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, val]() { m_mw.m_audio->setNr2Qspp(val); });
        });

    popup->addCheckbox("AE Filter",
        s.value("NR2AeFilter", "True").toString() == "True",
        [this](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("NR2AeFilter", on ? "True" : "False");
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setNr2AeFilter(on); });
        });

    popup->finalize(
        [this]() {
            // Open AetherDSP Settings dialog
            if (m_dspDialog) {
                m_dspDialog->raise();
                m_dspDialog->activateWindow();
                return;
            }
            auto* dlg = createAetherDspDialog();
            dlg->show();
        },
        nullptr  // Reset handled by individual control resetters
    );

    popup->showAt(globalPos);
}

void AudioDspController::showNr4ParamPopup(const QPoint& globalPos)
{
    auto& s = AppSettings::instance();
    auto* popup = new DspParamPopup(&m_mw);

    popup->addSlider("Reduction (dB)", 0, 400,
        static_cast<int>(s.value("NR4ReductionAmount", "10.0").toFloat() * 10),
        [](int v) { return QString::number(v / 10.0f, 'f', 1); },
        [this](int v) {
            float val = v / 10.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR4ReductionAmount", QString::number(val, 'f', 1));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, val]() { m_mw.m_audio->setNr4ReductionAmount(val); });
        });

    popup->addSlider("Smoothing (%)", 0, 100,
        static_cast<int>(s.value("NR4SmoothingFactor", "0.0").toFloat()),
        [](int v) { return QString::number(v); },
        [this](int v) {
            auto& s = AppSettings::instance();
            s.setValue("NR4SmoothingFactor", QString::number(static_cast<float>(v), 'f', 1));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr4SmoothingFactor(static_cast<float>(v)); });
        });

    popup->addSlider("Masking Depth", 0, 100,
        static_cast<int>(s.value("NR4MaskingDepth", "0.50").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR4MaskingDepth", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, val]() { m_mw.m_audio->setNr4MaskingDepth(val); });
        });

    popup->addSlider("Suppression", 0, 100,
        static_cast<int>(s.value("NR4SuppressionStrength", "0.50").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float val = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("NR4SuppressionStrength", QString::number(val, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, val]() { m_mw.m_audio->setNr4SuppressionStrength(val); });
        });

    popup->addCheckbox("Adaptive Noise",
        s.value("NR4AdaptiveNoise", "True").toString() == "True",
        [this](bool on) {
            auto& s = AppSettings::instance();
            s.setValue("NR4AdaptiveNoise", on ? "True" : "False");
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setNr4AdaptiveNoise(on); });
        });

    popup->finalize(
        [this]() {
            // Open AetherDSP Settings dialog (NR4 tab)
            if (m_dspDialog) {
                m_dspDialog->raise();
                m_dspDialog->activateWindow();
                return;
            }
            auto* dlg = createAetherDspDialog();
            dlg->show();
        },
        nullptr  // Reset handled by individual control resetters
    );

    popup->showAt(globalPos);
}

void AudioDspController::showDfnrParamPopup(const QPoint& globalPos)
{
    auto& s = AppSettings::instance();
    auto* popup = new DspParamPopup(&m_mw);

    popup->addSlider("Attenuation Limit (dB)", 0, 100,
        static_cast<int>(s.value("DfnrAttenLimit", "100").toFloat()),
        [](int v) { return QString::number(v); },
        [this](int v) {
            float db = static_cast<float>(v);
            auto& s = AppSettings::instance();
            s.setValue("DfnrAttenLimit", QString::number(db, 'f', 0));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, db]() { m_mw.m_audio->setDfnrAttenLimit(db); });
        });

    popup->addSlider("Post-Filter Beta", 0, 30,
        static_cast<int>(s.value("DfnrPostFilterBeta", "0.0").toFloat() * 100),
        [](int v) { return QString::number(v / 100.0f, 'f', 2); },
        [this](int v) {
            float beta = v / 100.0f;
            auto& s = AppSettings::instance();
            s.setValue("DfnrPostFilterBeta", QString::number(beta, 'f', 2));
            s.save();
            QMetaObject::invokeMethod(m_mw.m_audio, [this, beta]() { m_mw.m_audio->setDfnrPostFilterBeta(beta); });
        });

    popup->finalize(
        [this]() {
            // Open AetherDSP Settings dialog (DFNR tab)
            if (m_dspDialog) {
                m_dspDialog->raise();
                m_dspDialog->activateWindow();
                return;
            }
            auto* dlg = createAetherDspDialog();
            dlg->show();
        },
        nullptr
    );

    popup->showAt(globalPos);
}

// Every entry point (Settings menu, NR2/NR4/DFNR/MNR right-click popups)
// shares one single-instance dialog, so it must always be fully wired —
// the MNR path used to skip the NR4 signals, leaving those sliders dead.
AetherDspDialog* AudioDspController::createAetherDspDialog()
{
    auto* dlg = new AetherDspDialog(m_mw.m_audio, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // Wire NR2 parameter signals to AudioEngine (audio thread safe)
    connect(dlg, &AetherDspDialog::nr2GainMaxChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr2GainMax(v); });
    });
    connect(dlg, &AetherDspDialog::nr2GainSmoothChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr2GainSmooth(v); });
    });
    connect(dlg, &AetherDspDialog::nr2QsppChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr2Qspp(v); });
    });
    connect(dlg, &AetherDspDialog::nr2GainMethodChanged, this, [this](int m) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, m]() { m_mw.m_audio->setNr2GainMethod(m); });
    });
    connect(dlg, &AetherDspDialog::nr2NpeMethodChanged, this, [this](int m) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, m]() { m_mw.m_audio->setNr2NpeMethod(m); });
    });
    connect(dlg, &AetherDspDialog::nr2AeFilterChanged, this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setNr2AeFilter(on); });
    });
    // Wire NR4 parameter signals to AudioEngine
    connect(dlg, &AetherDspDialog::nr4ReductionChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr4ReductionAmount(v); });
    });
    connect(dlg, &AetherDspDialog::nr4SmoothingChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr4SmoothingFactor(v); });
    });
    connect(dlg, &AetherDspDialog::nr4WhiteningChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr4WhiteningFactor(v); });
    });
    connect(dlg, &AetherDspDialog::nr4AdaptiveNoiseChanged, this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setNr4AdaptiveNoise(on); });
    });
    connect(dlg, &AetherDspDialog::nr4NoiseMethodChanged, this, [this](int m) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, m]() { m_mw.m_audio->setNr4NoiseEstimationMethod(m); });
    });
    connect(dlg, &AetherDspDialog::nr4MaskingDepthChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr4MaskingDepth(v); });
    });
    connect(dlg, &AetherDspDialog::nr4SuppressionChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setNr4SuppressionStrength(v); });
    });
    // Wire DFNR parameter signals
    connect(dlg, &AetherDspDialog::dfnrAttenLimitChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setDfnrAttenLimit(v); });
    });
    connect(dlg, &AetherDspDialog::dfnrPostFilterBetaChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setDfnrPostFilterBeta(v); });
    });
    // Wire MNR enable / strength signals
    connect(dlg, &AetherDspDialog::mnrEnabledChanged, this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setMnrEnabled(on); });
    });
    connect(dlg, &AetherDspDialog::mnrStrengthChanged, this, [this](float v) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, v]() { m_mw.m_audio->setMnrStrength(v); });
    });
    m_dspDialog = dlg;
    return dlg;
}

void AudioDspController::showMnrSettings()
{
    if (m_dspDialog) {
        m_dspDialog->raise();
        m_dspDialog->activateWindow();
        if (auto* d = qobject_cast<AetherDspDialog*>(m_dspDialog))
            d->selectTab("MNR");
        return;
    }
    auto* dlg = createAetherDspDialog();
    dlg->selectTab("MNR");
    dlg->show();
}

} // namespace AetherSDR
