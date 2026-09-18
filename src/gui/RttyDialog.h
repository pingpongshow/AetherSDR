#pragma once

#include "core/DigitalModeReporting.h"
#include "core/RttyDecoder.h"
#include "core/RttyModulator.h"

#include <QDialog>
#include <QMetaObject>
#include <QPointer>

class QByteArray;
class QTimer;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
QT_END_NAMESPACE

class QHideEvent;

namespace AetherSDR {

class AudioEngine;
class RadioModel;
class SliceModel;

class RttyDialog : public QDialog {
    Q_OBJECT

public:
    explicit RttyDialog(RadioModel* radioModel, AudioEngine* audio, QWidget* parent = nullptr);
    ~RttyDialog() override;

    void setActiveSlice(SliceModel* slice);

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);

signals:
    void receptionReportReady(const AetherSDR::ReceptionReport& report);
    void txAudioReady(const QByteArray& pcm24kStereo);

private slots:
    void appendDecodedText(const QString& text);
    void applyDecoderSettings();
    void applyFrequencyPreset(int index);
    void applySliceTuning();
    void applyTonePreset(int index);
    void clearRxText();
    void logContact();
    void onFrequencyEdited(double khz);
    void onRadioTransmittingChanged(bool on);
    void onSendClicked();
    void onTxToggled(bool on);
    void pumpTxAudio();
    void syncControlsFromSlice();
    void updateMetrics(float markLevelDb, float spaceLevelDb, float confidence, bool locked);

protected:
    // Closing the window must release PTT this dialog requested.
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    QString csvEscape(const QString& text) const;
    void loadSettings();
    QString logFilePath() const;
    void maybeEmitReceptionReport(const QString& text);
    void queueEditorText();
    QString receiverGrid() const;
    void refreshReportingControls();
    void saveSettings() const;
    void startTransmission();
    void stopTransmissionNow();
    void syncPresetSelections();
    void updateSliceConnections(SliceModel* slice);
    void updateSliceSummary();
    void updateStatus(const QString& text);
    void updateTxControls();

    RadioModel* m_radioModel{nullptr};
    AudioEngine* m_audio{nullptr};
    RttyDecoder m_decoder;
    RttyModulator m_modulator;
    QPointer<SliceModel> m_activeSlice;

    QMetaObject::Connection m_sliceFrequencyConnection;
    QMetaObject::Connection m_sliceModeConnection;
    QMetaObject::Connection m_sliceMarkConnection;
    QMetaObject::Connection m_sliceShiftConnection;

    QComboBox* m_frequencyPresetCombo{nullptr};
    QDoubleSpinBox* m_frequencySpin{nullptr};
    QComboBox* m_tonePresetCombo{nullptr};
    QDoubleSpinBox* m_baudSpin{nullptr};
    QSpinBox* m_markSpin{nullptr};
    QSpinBox* m_shiftSpin{nullptr};
    QCheckBox* m_invertCheck{nullptr};
    QCheckBox* m_unshiftOnSpaceCheck{nullptr};
    QSlider* m_sqlSlider{nullptr};
    QProgressBar* m_confidenceBar{nullptr};
    QLabel* m_metricsLabel{nullptr};
    QLabel* m_sliceSummaryLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_pendingLabel{nullptr};
    QPlainTextEdit* m_rxText{nullptr};
    QPlainTextEdit* m_txEdit{nullptr};
    QPushButton* m_sendButton{nullptr};
    QPushButton* m_txButton{nullptr};
    QCheckBox* m_autoTxCheck{nullptr};
    QCheckBox* m_spotCheck{nullptr};
    QCheckBox* m_pskReporterCheck{nullptr};
    QPushButton* m_eraseButton{nullptr};
    QLineEdit* m_dxEdit{nullptr};
    QLineEdit* m_rstEdit{nullptr};
    QPushButton* m_logButton{nullptr};
    QTimer* m_txTimer{nullptr};

    bool m_syncingControls{false};
    bool m_txStartPending{false};
    bool m_txKeyRequested{false};   // this dialog sent xmit 1 and has not released it
    bool m_manualTxHold{false};
    bool m_stopAfterDrain{false};
    int m_drainGuardChunks{0};
    QString m_recentDecodedText;
};

} // namespace AetherSDR
