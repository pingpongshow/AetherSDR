#pragma once

#include "core/HfFaxDecoder.h"

#include <QDialog>
#include <QMetaObject>
#include <QPointer>

class QByteArray;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QScrollArea;
class QSlider;
class QSpinBox;
QT_END_NAMESPACE

namespace AetherSDR {

class SliceModel;
class HfFaxImageView;
class HfFaxToneScaleWidget;

class HfFaxDialog : public QDialog {
    Q_OBJECT

public:
    explicit HfFaxDialog(QWidget* parent = nullptr);
    ~HfFaxDialog() override;

    void setActiveSlice(SliceModel* slice);

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);

private slots:
    void appendRawLine(const QByteArray& grayLine);
    void applySliceTuning();
    void clearImage();
    void handleTransmissionStarted();
    void handleTransmissionEnded();
    void onFrequencyEdited(double khz);
    void onStationSelected(int index);
    void saveImage();
    void updateStatusText(const QString& text);
    void updateSyncState(bool locked, float quality);
    void updateTone(float toneHz);

private:
    void buildUi();
    void connectDecoder();
    QString currentStationLabel() const;
    QString defaultSavePath(bool automatic) const;
    int findStationIndex(double khz) const;
    void loadSettings();
    void maybeAutoSave();
    void refreshSliceSummary();
    void renderSettingsFromControls();
    void saveSettings() const;
    void syncControlsFromSlice();
    void updateSliceConnections(SliceModel* slice);

    HfFaxDecoder m_decoder;
    QPointer<SliceModel> m_activeSlice;

    QMetaObject::Connection m_sliceFrequencyConnection;
    QMetaObject::Connection m_sliceModeConnection;
    QMetaObject::Connection m_sliceDiglConnection;
    QMetaObject::Connection m_sliceDiguConnection;

    HfFaxImageView* m_imageView{nullptr};
    HfFaxToneScaleWidget* m_toneScale{nullptr};
    QScrollArea* m_scrollArea{nullptr};
    QComboBox* m_stationCombo{nullptr};
    QDoubleSpinBox* m_frequencySpin{nullptr};
    QSpinBox* m_rxOffsetSpin{nullptr};
    QCheckBox* m_lsbCheck{nullptr};
    QCheckBox* m_autoStartCheck{nullptr};
    QCheckBox* m_pauseCheck{nullptr};
    QCheckBox* m_invertedCheck{nullptr};
    QCheckBox* m_autoSaveCheck{nullptr};
    QSlider* m_blackLevelSlider{nullptr};
    QSlider* m_contrastSlider{nullptr};
    QSpinBox* m_clockAdjustSpin{nullptr};
    QLabel* m_syncLabel{nullptr};
    QLabel* m_toneLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_linesLabel{nullptr};
    QLabel* m_sliceSummaryLabel{nullptr};

    bool m_syncingControls{false};
    bool m_receivingTransmission{false};
    QString m_lastSaveDirectory;
};

} // namespace AetherSDR
