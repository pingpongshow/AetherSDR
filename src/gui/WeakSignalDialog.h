#pragma once

#include "core/CtyDatParser.h"
#include "core/DigitalModeReporting.h"
#include "core/WeakSignalNativeDecoder.h"

#include <QByteArray>
#include <QDialog>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <array>

QT_BEGIN_NAMESPACE
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

class QHideEvent;

namespace AetherSDR {

class AudioEngine;
class RadioModel;
class SliceModel;
class WeakSignalMapDialog;

class WeakSignalDialog : public QDialog {
    Q_OBJECT

public:
    explicit WeakSignalDialog(RadioModel* radioModel,
                              WeakSignalNativeDecoder* decoder,
                              AudioEngine* audio,
                              QWidget* parent = nullptr);
    ~WeakSignalDialog() override;

    void setActiveSlice(SliceModel* slice);

    enum class Mode {
        FT8 = 0,
        FT4 = 1,
        JT65 = 2,
        WSPR = 3
    };

    struct FrequencyPreset {
        QString label;
        double khz{0.0};
    };

    struct DisplayDecode {
        WeakSignalDecode decode;
        QString country;
        double latitude{0.0};
        double longitude{0.0};
        bool hasLocation{false};
    };

signals:
    void receptionReportReady(const AetherSDR::ReceptionReport& report);
    void txAudioReady(const QByteArray& pcm24kStereo);

private slots:
    void applyFrequencyPreset(int index);
    void focusPanadapter();
    void onCleared(quint8 window);
    void onCqFilterToggled(bool on);
    void onDecodeReceived(const AetherSDR::WeakSignalDecode& decode);
    void onFrequencyEdited(double khz);
    void onListenerStarted();
    void onListenerStopped();
    void onListenerToggled(bool enabled);
    void onModeButtonClicked(int id);
    void onRadioTransmittingChanged(bool on);
    void onReplyClicked();
    void onRowSelectionChanged();
    void onSendTextClicked();
    void pumpTxAudio();
    void openMapDialog();
    void requestHaltTx();
    void requestReplay();
    void syncControlsFromSlice();
    void updateHomeStation(const QString& status, int tracked, int visible,
                           const QString& grid, const QString& altitude,
                           const QString& lat, const QString& lon,
                           const QString& utcTime);
    void wipeDecodes();

protected:
    // Closing the window must release PTT this dialog requested.
    void hideEvent(QHideEvent* event) override;

private:
    void armScheduledTransmission();
    void applySliceTuning(bool recenter);
    bool buildTxWaveform(bool reply, QString* statusText);
    void buildUi();
    bool hasSelectedDecode() const;
    QString bandLabelForFrequency(double khz) const;
    QString currentContextTitle() const;
    Mode currentMode() const;
    QString currentModeLabel() const;
    QString currentHomeGrid() const;
    int currentTxDfHz() const;
    QString defaultReplyMessage() const;
    QString defaultStandardMessage() const;
    QVector<FrequencyPreset> frequencyPresetsForMode(Mode mode) const;
    QVector<DisplayDecode> focusedDecodes() const;
    QString gridForCallsign(const QString& callsign, const QString& decodeGrid) const;
    QString generatedMessageAt(int index) const;
    QString formattedDialDisplay() const;
    void refreshReportingControls();
    void refreshGeneratedMessages();
    bool maidenheadToLatLon(const QString& grid, double* latitude, double* longitude) const;
    void loadSettings();
    void populateDecodeTable(QTableWidget* table,
                             const QVector<DisplayDecode>& rows,
                             bool showCountryColumn) const;
    void refreshListenerSummary();
    bool resolveLocation(const QString& callsign, const QString& grid,
                         QString* country, double* latitude, double* longitude) const;
    void refreshMap();
    void refreshTable();
    void repopulateFrequencyPresets();
    void saveSettings() const;
    void setModeSelection(Mode mode, bool fromUser);
    void setSelectedDecodeDetails(int row);
    void setSelectedDecodeDetailsByKey(const QString& key);
    bool selectedDecode(DisplayDecode* entry) const;
    QString selectedGeneratedMessage() const;
    void stopTransmissionNow(const QString& statusText);
    void syncFrequencyPresetSelection();
    void updateStatusLine(const QString& text);
    void updateTxControls();
    void updateWindowTitle();
    QVector<DisplayDecode> visibleDecodes() const;

    RadioModel* m_radioModel{nullptr};
    WeakSignalNativeDecoder* m_decoder{nullptr};
    AudioEngine* m_audio{nullptr};
    CtyDatParser m_ctyParser;
    QPointer<SliceModel> m_activeSlice;
    QPointer<WeakSignalMapDialog> m_mapDialog;

    QMetaObject::Connection m_sliceFrequencyConnection;
    QMetaObject::Connection m_sliceModeConnection;

    QButtonGroup* m_modeGroup{nullptr};
    QPushButton* m_rxToggleButton{nullptr};
    QPushButton* m_ft8Button{nullptr};
    QPushButton* m_ft4Button{nullptr};
    QPushButton* m_jt65Button{nullptr};
    QPushButton* m_wsprButton{nullptr};
    QComboBox* m_frequencyCombo{nullptr};
    QDoubleSpinBox* m_frequencySpin{nullptr};
    QCheckBox* m_cqOnlyCheck{nullptr};
    QCheckBox* m_spotCheck{nullptr};
    QCheckBox* m_pskReporterCheck{nullptr};
    QLabel* m_listenerLabel{nullptr};
    QLabel* m_sliceSummaryLabel{nullptr};
    QLabel* m_bigDialLabel{nullptr};
    QLabel* m_rxFrequencyLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QTableWidget* m_bandActivityTable{nullptr};
    QTableWidget* m_rxFrequencyTable{nullptr};
    QLineEdit* m_dxGridEdit{nullptr};
    QLineEdit* m_rxDfEdit{nullptr};
    QLineEdit* m_txDfEdit{nullptr};
    QLineEdit* m_dxEdit{nullptr};
    QLineEdit* m_reportEdit{nullptr};
    QLineEdit* m_messageEdit{nullptr};
    QPushButton* m_stopButton{nullptr};
    QPushButton* m_monitorButton{nullptr};
    QPushButton* m_decodeButton{nullptr};
    QPushButton* m_tuneButton{nullptr};
    QPushButton* m_replyButton{nullptr};
    QPushButton* m_sendTextButton{nullptr};
    QPushButton* m_haltButton{nullptr};
    QButtonGroup* m_txMessageGroup{nullptr};
    std::array<QLineEdit*, 6> m_txMessageEdits{{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}};
    std::array<QRadioButton*, 6> m_txMessageButtons{{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}};

    QVector<DisplayDecode> m_allDecodes;
    QHash<QString, QString> m_gridByCallsign;
    QString m_selectedDecodeKey;
    bool m_syncingControls{false};
    QTimer* m_txTimer{nullptr};
    QTimer* m_slotTimer{nullptr};
    QByteArray m_txWaveform;
    QString m_lastTxMessage;
    int m_txWaveformOffset{0};
    int m_txDrainGuardChunks{0};
    qint64 m_txBoundaryMs{0};
    qint64 m_scheduledStartMs{0};
    bool m_txStartPending{false};
    bool m_txKeyRequested{false};   // this dialog sent xmit 1 and has not released it
    bool m_txActive{false};
};

} // namespace AetherSDR
