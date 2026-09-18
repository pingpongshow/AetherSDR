#pragma once

#include "core/SsdvAfskDecoder.h"
#include "core/SsdvAfskModulator.h"
#include "core/SstvDecoder.h"
#include "core/SstvModulator.h"

#include <QDialog>
#include <QMetaObject>
#include <QPointer>

class QByteArray;
class QTimer;

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
QT_END_NAMESPACE

class QHideEvent;

namespace AetherSDR {

class AudioEngine;
class RadioModel;
class SliceModel;
class SstvImageView;

class SstvDialog : public QDialog {
    Q_OBJECT

public:
    explicit SstvDialog(RadioModel* radioModel, AudioEngine* audio, QWidget* parent = nullptr);
    ~SstvDialog() override;

    void setActiveSlice(SliceModel* slice);

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);

signals:
    void txAudioReady(const QByteArray& pcm24kStereo);

private slots:
    void applyRxModeSelection(int index);
    void clearAllRxState();
    void clearDisplayedImage();
    void loadImageOrSsdv();
    void logContact();
    void onAnalogStatusChanged(const QString& text);
    void onDetectedModeChanged(const QString& modeName);
    void onImageUpdated(const QImage& image);
    void onLineProgressChanged(int line, int totalLines);
    void onRadioTransmittingChanged(bool on);
    void onSsdvImageUpdated(const QImage& image);
    void onSsdvMetricsUpdated(float markLevelDb, float spaceLevelDb, float confidence, bool locked);
    void onSsdvPacketStatsChanged(int packetCount, int imageId, int lastPacketId, const QString& callsign);
    void onSsdvStatusChanged(const QString& text);
    void onSyncStateChanged(bool locked, float quality);
    void onTxModeChanged(int index);
    void onTxToggled(bool on);
    void pumpTxAudio();
    void rollImageDown();
    void rollImageUp();
    void saveCurrentImage();
    void shiftImageLeft();
    void shiftImageRight();
    void syncControlsFromSlice();
    void updateStatus(const QString& text);

protected:
    // Closing the window must release PTT this dialog requested.
    void hideEvent(QHideEvent* event) override;

private:
    enum class DisplaySource {
        None,
        Rx,
        Tx
    };

    enum class TxTransport {
        None,
        AnalogSstv,
        SsdvAfsk
    };

    void applySliceTuning();
    void buildUi();
    QString csvEscape(const QString& text) const;
    QString currentImageSummary() const;
    const SstvModeDefinition* currentRxSelection() const;
    const SstvModeDefinition* currentTxSelection() const;
    QImage imageForTransmit() const;
    bool isSsdvModeName(const QString& text) const;
    bool isSsdvRxSelected() const;
    bool isSsdvTxSelected() const;
    QString currentTxModeName() const;
    QString logFilePath() const;
    void loadSettings();
    void saveSettings() const;
    void setDisplayedImage(const QImage& image, DisplaySource source);
    void stopTransmissionNow();
    bool startTransmission();
    void storeCanvasBackToSource();
    void updateSliceConnections(SliceModel* slice);
    void updateSliceSummary();
    void updateSourceLabel();
    void updateTxControls();

    RadioModel* m_radioModel{nullptr};
    AudioEngine* m_audio{nullptr};
    SstvDecoder m_decoder;
    SstvModulator m_modulator;
    SsdvAfskDecoder m_ssdvDecoder;
    SsdvAfskModulator m_ssdvModulator;
    QPointer<SliceModel> m_activeSlice;

    QMetaObject::Connection m_sliceFrequencyConnection;
    QMetaObject::Connection m_sliceModeConnection;
    QMetaObject::Connection m_sliceOffsetConnection;

    QComboBox* m_rxModeCombo{nullptr};
    QLabel* m_detectedModeLabel{nullptr};
    QLabel* m_syncLabel{nullptr};
    QLabel* m_progressLabel{nullptr};
    QLabel* m_sourceLabel{nullptr};
    QLabel* m_sliceSummaryLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    SstvImageView* m_canvas{nullptr};

    QPushButton* m_txButton{nullptr};
    QComboBox* m_txModeCombo{nullptr};
    QLineEdit* m_myIdEdit{nullptr};
    QLineEdit* m_hisIdEdit{nullptr};
    QPushButton* m_logButton{nullptr};
    QPushButton* m_loadButton{nullptr};
    QLabel* m_pendingLabel{nullptr};
    QTimer* m_txTimer{nullptr};

    QImage m_rxImage;
    QImage m_txImage;
    DisplaySource m_displaySource{DisplaySource::None};

    QString m_lastLoadDirectory;
    QString m_lastSaveDirectory;
    int m_ssdvImageId{0};

    bool m_syncingControls{false};
    bool m_txStartPending{false};
    bool m_txKeyRequested{false};   // this dialog sent xmit 1 and has not released it
    int m_drainGuardChunks{0};
    TxTransport m_activeTxTransport{TxTransport::None};
};

} // namespace AetherSDR
