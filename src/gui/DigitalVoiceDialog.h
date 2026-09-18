#pragma once

#include "core/DigitalVoiceEngine.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace AetherSDR {

class RadioModel;

// Mode → "FreeDV / Digital Voice" panel.  Pure view: MainWindow owns the
// engine and drives activation through the request signals, then pushes
// state back via the setters so VFO buttons and this dialog stay in sync.
class DigitalVoiceDialog : public QDialog {
    Q_OBJECT

public:
    explicit DigitalVoiceDialog(RadioModel* model, QWidget* parent = nullptr);

    // Settings-backed values (AppSettings)
    QString txCallsign() const;
    bool squelchEnabled() const;
    float squelchThresholdDb() const;
    bool reportingEnabled() const;

    // Reporting toggle is unavailable without Qt WebSockets.
    void setReportingAvailable(bool available);

public slots:
    void setActiveState(int sliceId, AetherSDR::DigitalVoiceMode mode, bool active);
    void setSynced(bool synced);
    void setSnr(float snrDb);
    void setFreqOffset(float hz);
    void setReceivedText(const QString& text);
    void setReportingState(bool enabled);
    void refreshSlices();

signals:
    void activateRequested(int sliceId, AetherSDR::DigitalVoiceMode mode);
    void deactivateRequested();
    void txCallsignChanged(const QString& callsign);
    void squelchChanged(bool enabled, float thresholdDb);
    void reportingToggled(bool enabled);

protected:
    void showEvent(QShowEvent* ev) override;

private:
    void updateControls();
    void resetStatus();
    int selectedSliceId() const;
    AetherSDR::DigitalVoiceMode selectedMode() const;

    RadioModel*     m_model;
    QComboBox*      m_sliceCombo{nullptr};
    QComboBox*      m_modeCombo{nullptr};
    QPushButton*    m_activateBtn{nullptr};
    QLabel*         m_activeLabel{nullptr};
    QLabel*         m_syncLabel{nullptr};
    QLabel*         m_snrLabel{nullptr};
    QLabel*         m_foffLabel{nullptr};
    QLabel*         m_rxTextLabel{nullptr};
    QLabel*         m_rxTextCaption{nullptr};
    QLineEdit*      m_callsignEdit{nullptr};
    QCheckBox*      m_squelchCheck{nullptr};
    QDoubleSpinBox* m_squelchSpin{nullptr};
    QCheckBox*      m_reportCheck{nullptr};
    QLabel*         m_reportNote{nullptr};

    int  m_activeSliceId{-1};
    AetherSDR::DigitalVoiceMode m_activeMode{AetherSDR::DigitalVoiceMode::None};
    bool m_active{false};
};

} // namespace AetherSDR
