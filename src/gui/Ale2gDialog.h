#pragma once

#include "core/Ale2gController.h"
#include "core/Ale2gDecoder.h"

#include <QDialog>
#include <QMetaObject>
#include <QPointer>
#include <QVector>

class QByteArray;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

namespace AetherSDR {

class RadioModel;
class SliceModel;

// 2G ALE window: passive RX decoder (network catalog scan) plus the client-side
// ALE link controller (scan, sound, call, AMD, link) for HFLink / PCALE /
// MARS-ALE interoperation.  Transmit is gated by a master switch that is OFF
// every time the dialog opens.
//
// Coordinator wiring (MainWindow):
//   aleTransmitRequested(bool)  -> TransmitModel::setMox(bool)
//   aleTxAudioReady(pcm)        -> AudioEngine::feedDaxTxAudio (audio thread)
//   RadioModel TX state         -> setRadioTransmitting(bool)
class Ale2gDialog : public QDialog {
    Q_OBJECT

public:
    explicit Ale2gDialog(RadioModel* radioModel, QWidget* parent = nullptr);
    ~Ale2gDialog() override;

    void setActiveSlice(SliceModel* slice);
    Ale2gController* controller() { return &m_controller; }

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);
    void setRadioTransmitting(bool transmitting);

signals:
    void aleTransmitRequested(bool on);
    void aleTxAudioReady(const QByteArray& pcm24kStereoFloat32);

private slots:
    void clearOutput();
    void onFrequencySelectionChanged(int index);
    void onFrameDecoded(const QString& utcText, const QString& summary);
    void onFrequencyEdited(double khz);
    void onGroupSelected(int index);
    void onMetricsUpdated(bool locked, int phaseSamples, int boundary, float score, int totalWords);
    void onScanTick();
    void onStatusChanged(const QString& text);
    void onWordDecoded(const QString& utcText,
                       const QString& preamble,
                       const QString& text,
                       int errors,
                       float confidence,
                       const QString& summary);
    void syncControlsFromSlice();

    // ALE controller
    void onAleStateChanged(AetherSDR::Ale2gController::State state, const QString& remote);
    void onAleTuneRequested(qint64 frequencyHz);
    void onAleTransmitRequested(bool on);
    void onAleFrame(const QString& summary);
    void onAleAmd(const QString& from, const QString& text);
    void onAleLog(const QString& text);
    void onAleError(const QString& text);
    void onAleHeardChanged();
    void onTxEnableToggled(bool checked);
    void onStartStopClicked();
    void onCallClicked();
    void onAllCallClicked();
    void onSoundClicked();
    void onSendAmdClicked();
    void onTerminateClicked();
    void onEditChannelsClicked();
    void onChannelPresetSelected(int index);
    void applyAleSettings();

private:
    void buildUi();
    QWidget* buildAlePanel();
    int findFrequencyIndex(const QVector<double>& frequencies, double khz) const;
    bool isGroupSelectionActive() const;
    void loadFrequencyGroups();
    void loadSettings();
    void loadAleSettings();
    void rebuildFrequencySelectionCombo();
    void saveSettings() const;
    void saveAleSettings() const;
    void startScan();
    void stopScan();
    void syncSelectionToCurrentFrequency();
    void tuneToFrequencyKhz(double khz, bool recenter);
    void updateSliceSummary();
    void updateAleButtons();
    void appendAleLog(const QString& text);

    struct FrequencyGroup {
        QString name;
        QVector<double> frequencies;
    };

    RadioModel* m_radioModel{nullptr};
    Ale2gDecoder m_decoder;
    Ale2gController m_controller;
    QPointer<SliceModel> m_activeSlice;

    QMetaObject::Connection m_sliceFrequencyConnection;
    QMetaObject::Connection m_sliceModeConnection;

    QVector<FrequencyGroup> m_frequencyGroups;
    QComboBox* m_groupCombo{nullptr};
    QComboBox* m_frequencyCombo{nullptr};
    QDoubleSpinBox* m_frequencySpin{nullptr};
    QLabel* m_lockLabel{nullptr};
    QLabel* m_metricsLabel{nullptr};
    QLabel* m_sliceSummaryLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QTableWidget* m_table{nullptr};
    QPlainTextEdit* m_historyEdit{nullptr};
    QTimer* m_scanTimer{nullptr};

    // ALE controller UI
    QLineEdit* m_selfEdit{nullptr};
    QCheckBox* m_txEnableCheck{nullptr};
    QLabel* m_aleStateLabel{nullptr};
    QComboBox* m_channelPresetCombo{nullptr};
    QPushButton* m_editChannelsButton{nullptr};
    QLabel* m_channelSummaryLabel{nullptr};
    QSpinBox* m_dwellSpin{nullptr};
    QSpinBox* m_scanCallSpin{nullptr};
    QSpinBox* m_txLevelSpin{nullptr};
    QPushButton* m_startStopButton{nullptr};
    QLineEdit* m_callEdit{nullptr};
    QPushButton* m_callButton{nullptr};
    QPushButton* m_allCallButton{nullptr};
    QPushButton* m_terminateButton{nullptr};
    QLineEdit* m_amdEdit{nullptr};
    QPushButton* m_sendAmdButton{nullptr};
    QPushButton* m_soundButton{nullptr};
    QSpinBox* m_soundIntervalSpin{nullptr};
    QComboBox* m_soundTypeCombo{nullptr};
    QTabWidget* m_tabs{nullptr};
    QTableWidget* m_heardTable{nullptr};
    QPlainTextEdit* m_aleLogEdit{nullptr};
    QVector<Ale2gChannel> m_aleChannels;

    bool m_syncingControls{false};
    bool m_decoderLocked{false};
    bool m_loadingAleSettings{false};
    qint64 m_lastAleActivityMs{0};
    int m_scanPosition{-1};
};

} // namespace AetherSDR
