#pragma once

#include "core/DigitalModeReporting.h"
#include "core/PskDecoder.h"
#include "core/PskModulator.h"

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

class PskDialog : public QDialog {
    Q_OBJECT

public:
    explicit PskDialog(RadioModel* radioModel, AudioEngine* audio, QWidget* parent = nullptr);
    ~PskDialog() override;

    void setActiveSlice(SliceModel* slice);

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);

signals:
    void receptionReportReady(const AetherSDR::ReceptionReport& report);
    void txAudioReady(const QByteArray& pcm24kStereo);

private slots:
    void appendDecodedText(const QString& text);
    void applyBandPreset(int index);
    void applyModePreset(int index);
    void applySettings();
    void clearRxText();
    void copyRxToTxOffset();
    void copyTxToRxOffset();
    void logContact();
    void onFrequencyEdited(double khz);
    void onOffsetChanged(int);
    void onRadioTransmittingChanged(bool on);
    void onSendClicked();
    void onTxToggled(bool on);
    void pumpTxAudio();
    void syncControlsFromSlice();
    void updateMetrics(float signalLevelDb, float confidence, float timingQuality, bool locked);

protected:
    // Closing the window must release PTT this dialog requested.
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    QString csvEscape(const QString& text) const;
    void loadSettings();
    QString logFilePath() const;
    void queueEditorText();
    void saveSettings() const;
    double effectiveBaud() const;
    QString effectiveModeLabel() const;
    QString effectiveReportMode() const;
    bool effectiveQpsk() const;
    void maybeEmitReceptionReport(const QString& text);
    QString receiverGrid() const;
    void refreshReportingControls();
    void startTransmission();
    void stopTransmissionNow();
    void syncPresetSelections();
    void updateModeControls();
    void updateSliceConnections(SliceModel* slice);
    void updateSliceSummary();
    void updateStatus(const QString& text);
    void updateTxControls();

    RadioModel* m_radioModel{nullptr};
    AudioEngine* m_audio{nullptr};
    PskDecoder m_decoder;
    PskModulator m_modulator;
    QPointer<SliceModel> m_activeSlice;

    QMetaObject::Connection m_sliceFrequencyConnection;
    QMetaObject::Connection m_sliceModeConnection;
    QMetaObject::Connection m_sliceOffsetConnection;

    QComboBox* m_bandCombo{nullptr};
    QDoubleSpinBox* m_frequencySpin{nullptr};
    QComboBox* m_modeCombo{nullptr};
    QComboBox* m_manualConstellationCombo{nullptr};
    QDoubleSpinBox* m_manualBaudSpin{nullptr};
    QSlider* m_sqlSlider{nullptr};
    QSpinBox* m_rxOffsetSpin{nullptr};
    QSpinBox* m_txOffsetSpin{nullptr};
    QPushButton* m_copyRxToTxButton{nullptr};
    QPushButton* m_copyTxToRxButton{nullptr};
    QCheckBox* m_lockOffsetsCheck{nullptr};
    QCheckBox* m_spotCheck{nullptr};
    QCheckBox* m_pskReporterCheck{nullptr};
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
