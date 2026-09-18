#pragma once

#include "core/AudioEngine.h"

#include <QWidget>

class QLabel;
class QPushButton;

namespace AetherSDR {

class ClientChainWidget;

// Docked chain tile — header with [TX] [RX] [BYPASS] buttons, the
// chain strip, and an interaction hint at the bottom.
//
// TX and RX buttons form an exclusive pair that selects which chain
// the widget displays and edits.  TX is the mic-side chain; RX is the
// receive-audio chain (post-NR, pre-speaker).  Both carry the same
// stage set with independent order, bypass and parameters.
//
// Signals for the TX chain keep their original names so existing
// MainWindow wiring (applet-tile visibility, TX applet order) is not
// disturbed by RX edits; RX interactions emit the rx* variants.
//
// BYPASS is a one-click action that disables every stage in the
// currently-selected chain.  Users re-enable individual stages via
// the chain widget's right-click menu or the per-stage applet tiles.
class ClientChainApplet : public QWidget {
    Q_OBJECT

public:
    enum class ChainMode { Tx, Rx };

    explicit ClientChainApplet(QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    void refreshFromEngine();

    ChainMode mode() const { return m_mode; }

    // Forwarded to the internal chain widget — see ClientChainWidget::
    // setMicInputReady.  MainWindow calls this whenever TransmitModel
    // reports a mic-state change (which covers both mic source and DAX
    // on/off).  Also drives the record button's enable state — no
    // audio to capture when the chain isn't in the signal path.
    void setMicInputReady(bool ready);

    // Forwarded — pulses the TX endpoint red when we're actively
    // transmitting on our own slice.  Driven by TransmitModel::
    // moxChanged from MainWindow.
    void setTxActive(bool active);

    // PUDU monitor state — MainWindow drives these from the monitor's
    // state signals.  Each toggles button appearance, pulse, and the
    // mutual-exclusion enable state of the other button.
    void setMonitorRecording(bool on);
    void setMonitorPlaying(bool on);
    void setMonitorHasRecording(bool has);

signals:
    void editRequested(AudioEngine::TxChainStage stage);
    void stageEnabledChanged(AudioEngine::TxChainStage stage, bool enabled);
    // Emitted after the user drags a stage to a new position.  MainWindow
    // subscribes so the applet-panel sub-container order can be kept
    // in lock-step with the chain order.
    void chainReordered();

    // RX-chain counterparts of the three signals above — emitted only
    // while the RX mode is selected.  Editors should be opened bound to
    // the RX DSP instances (e.g. ClientCompEditor::showForRx()).
    void rxEditRequested(AudioEngine::TxChainStage stage);
    void rxStageEnabledChanged(AudioEngine::TxChainStage stage, bool enabled);
    void rxChainReordered();

    // User clicked the record or play button.  MainWindow decides
    // whether this is a start or a stop based on current monitor
    // state (click on a pulsing button = stop).
    void monitorRecordClicked();
    void monitorPlayClicked();

private:
    void setMode(ChainMode m);
    // Click handler for the BYPASS toggle.  On check: records which
    // stages of the selected chain are enabled, disables them all.  On uncheck:
    // re-enables just the stages that were on before.  Manual changes
    // between clicks are preserved only for stages not in the snapshot.
    void onBypassToggled(bool checked);

    // Applies the per-button stylesheet based on role + current state
    // (idle/active/pulse-bright/pulse-dim/disabled).  Separate method
    // so the pulse timer can call it cheaply at each tick.
    void applyRecordButtonStyle();
    void applyPlayButtonStyle();
    void updateMonitorButtonEnables();

    AudioEngine*       m_audio{nullptr};
    ClientChainWidget* m_chain{nullptr};
    QLabel*            m_hint{nullptr};
    QPushButton*       m_txBtn{nullptr};
    QPushButton*       m_rxBtn{nullptr};
    QPushButton*       m_bypassBtn{nullptr};
    QPushButton*       m_monRecBtn{nullptr};
    QPushButton*       m_monPlayBtn{nullptr};
    class QTimer*      m_monRecPulse{nullptr};
    class QTimer*      m_monPlayPulse{nullptr};
    bool               m_monRecPulseDim{false};
    bool               m_monPlayPulseDim{false};
    bool               m_monRecording{false};
    bool               m_monPlaying{false};
    bool               m_monHasRecording{false};
    bool               m_micReady{false};
    ChainMode          m_mode{ChainMode::Tx};
    // Stages that were on before BYPASS, per chain, plus each chain's
    // BYPASS button state so switching TX/RX restores the right look.
    QVector<AudioEngine::TxChainStage> m_bypassSnapshotTx;
    QVector<AudioEngine::TxChainStage> m_bypassSnapshotRx;
    bool               m_bypassActiveTx{false};
    bool               m_bypassActiveRx{false};
};

} // namespace AetherSDR
