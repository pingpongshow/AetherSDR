#pragma once

#include "core/WwvTimecodeDecoder.h"

#include <QDialog>
#include <QMetaObject>
#include <QPointer>

class QByteArray;

QT_BEGIN_NAMESPACE
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
QT_END_NAMESPACE

namespace AetherSDR {

class RadioModel;
class SliceModel;

class WwvDialog : public QDialog {
    Q_OBJECT

public:
    explicit WwvDialog(RadioModel* radioModel, QWidget* parent = nullptr);
    ~WwvDialog() override;

    void setActiveSlice(SliceModel* slice);

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);

private slots:
    void applyFrequencyPreset(int index);
    void clearHistory();
    void onCurrentTimeUpdated(const QString& utcText);
    void onFrameDecoded(const QString& utcText,
                        const QString& frameText,
                        const QString& flagsText,
                        const QString& bitsText);
    void onFrequencyEdited(double khz);
    void onStationProfileChanged(int index);
    void onMetricsUpdated(const QString& station,
                          float tickLevelDb,
                          float codeLevelDb,
                          bool locked,
                          int secondOfMinute,
                          bool minuteMarker,
                          bool hourMarker,
                          const QString& pulseClass);
    void onStatusChanged(const QString& text);
    void syncControlsFromSlice();

private:
    WwvTimecodeDecoder::Profile currentProfile() const;
    void applySliceTuning(bool recenter);
    void buildUi();
    void loadSettings();
    void repopulateFrequencyPresets();
    void saveSettings() const;
    void syncFrequencyPresetSelection();
    void updateSliceSummary();

    RadioModel* m_radioModel{nullptr};
    WwvTimecodeDecoder m_decoder;
    QPointer<SliceModel> m_activeSlice;

    QMetaObject::Connection m_sliceFrequencyConnection;
    QMetaObject::Connection m_sliceModeConnection;

    QComboBox* m_stationCombo{nullptr};
    QComboBox* m_frequencyCombo{nullptr};
    QDoubleSpinBox* m_frequencySpin{nullptr};
    QLabel* m_stationLabel{nullptr};
    QLabel* m_lockLabel{nullptr};
    QLabel* m_currentUtcLabel{nullptr};
    QLabel* m_frameLabel{nullptr};
    QLabel* m_flagsLabel{nullptr};
    QLabel* m_metricsLabel{nullptr};
    QLabel* m_bitsLabel{nullptr};
    QLabel* m_sliceSummaryLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QPlainTextEdit* m_historyEdit{nullptr};

    bool m_syncingControls{false};
};

} // namespace AetherSDR
