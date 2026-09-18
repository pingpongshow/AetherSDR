#pragma once

#include "core/ClientChainStages.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientGate;
class ClientCompKnob;    // reused — generic rotary knob
class ClientGateLevelView;

// Floating editor for the client-side TX gate / expander — layout
// modelled on Ableton Live's Gate device.  One instance lives on
// MainWindow; calling showForTx() raises the window and binds it to
// AudioEngine::clientGateTx().  Geometry persists via AppSettings
// (`ClientGateEditorGeometry` key).
//
// Layout:
//   ┌─ bypass ──────────────────── × ┐
//   │ ┌──────┐  ┌──────────────────┐ │
//   │ │ THR  │  │                  │ │
//   │ │      │  │   level view     │ │
//   │ │ RET  │  │   (Ableton-style)│ │
//   │ │      │  │                  │ │
//   │ │ Flip │  │                  │ │
//   │ │ Look │  │                  │ │
//   │ └──────┘  └──────────────────┘ │
//   │   [ATK] [HLD] [REL] [FLR]      │
//   └────────────────────────────────┘
class ClientGateEditor : public QWidget {
    Q_OBJECT

public:
    explicit ClientGateEditor(AudioEngine* engine, QWidget* parent = nullptr);
    ~ClientGateEditor() override;

    void showForTx();
    // Rebinds the editor to the RX-chain instance and raises it.
    void showForRx();
    void showForPath(ClientChainPath path);
    ClientChainPath path() const { return m_path; }

signals:
    // Fired when bypass toggles.  Docked applet subscribes to keep
    // its Enable button in sync.
    void bypassToggled(bool bypassed);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

private:
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();
    void syncControlsFromEngine();

    // Parameter commit — all control signals land here; each writes
    // the engine + persists via AppSettings.
    void applyThreshold(float db);
    void applyReturn(float db);
    void applyRatio(float ratio);
    void applyAttack(float ms);
    void applyHold(float ms);
    void applyRelease(float ms);
    void applyFloor(float db);
    void applyLookahead(float ms);
    void applyMode(int modeIdx);   // 0=Expander, 1=Gate

    // DSP instance for the currently bound chain path (TX or RX).
    ClientGate* dsp() const;

    AudioEngine*          m_audio{nullptr};
    ClientChainPath m_path{ClientChainPath::Tx};
    QString         m_baseTitle;
    ClientGateLevelView*  m_levelView{nullptr};
    ClientCompKnob*       m_threshold{nullptr};
    ClientCompKnob*       m_returnKnob{nullptr};
    ClientCompKnob*       m_ratio{nullptr};
    ClientCompKnob*       m_attack{nullptr};
    ClientCompKnob*       m_hold{nullptr};
    ClientCompKnob*       m_release{nullptr};
    ClientCompKnob*       m_floor{nullptr};
    QPushButton*          m_flip{nullptr};
    QComboBox*            m_lookahead{nullptr};
    QPushButton*          m_bypass{nullptr};
    QTimer*               m_syncTimer{nullptr};   // mirror engine → knobs
    bool                  m_restoring{false};
};

} // namespace AetherSDR
