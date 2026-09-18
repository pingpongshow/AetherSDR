#pragma once

#include "core/Scanner.h"

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace AetherSDR {

class RadioModel;
class SliceModel;

// ScanTarget adapter for a real SliceModel. Receive-only: frequency, mode and
// filter. Tunes with `slice tune autopan=0` while the target stays inside the
// slice's panadapter, and with a recentering tune when it leaves it (band scan).
class SliceScanTarget : public ScanTarget {
public:
    SliceScanTarget(SliceModel* slice, RadioModel* radio);

    bool    isValid() const override;
    int     sliceId() const override;
    bool    isLocked() const override;
    double  frequencyMhz() const override;
    QString mode() const override;
    int     filterLow() const override;
    int     filterHigh() const override;
    void    tune(double mhz) override;
    void    setMode(const QString& mode) override;
    void    setFilter(int low, int high) override;

private:
    QPointer<SliceModel> m_slice;
    QPointer<RadioModel> m_radio;
};

// Memory / band / frequency-list scanner window.
//
// Everything the scanner changes on the radio happens only between the user's
// Start and Stop; Stop restores the slice's original frequency/mode/filter.
// Only client-side preferences (scan list setup, timings, skip list) are
// persisted, in AppSettings under "Scanner*" keys.
class ScannerPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScannerPanel(RadioModel* radio, QWidget* parent = nullptr);
    ~ScannerPanel() override;

    Scanner* scanner() const { return m_scanner; }

public slots:
    // MainWindow keeps the notion of the "active" slice; the panel's
    // "Active slice" choice follows it (only applied when a scan starts).
    void setActiveSliceId(int sliceId);
    // Squelch-open detection input: RX audio RMS (AudioEngine::levelChanged).
    void setRxAudioLevel(float rms);

    // Hooks for keyboard shortcuts / command palette.
    void toggleStartStop();
    void togglePause();
    void skipCurrent();

protected:
    void closeEvent(QCloseEvent* ev) override;

private slots:
    void onStart();
    void onStop();
    void onStateChanged(AetherSDR::Scanner::State state);
    void onChannelChanged(int index, const AetherSDR::ScanChannel& ch, bool priority);
    void onHitAdded(const AetherSDR::ScanHit& hit);
    void onHitPeakUpdated(int hitId, float peakDbm);
    void onLogActivated(int row, int column);
    void refreshMemories();
    void refreshSlices();
    void useCurrentBand();

private:
    void buildUi();
    void loadSettings();
    void saveSettings() const;
    void saveSkipList() const;
    void updateSkipLabel();
    Scanner::Config configFromUi() const;
    QVector<ScanChannel> buildChannels(QString* error) const;
    QVector<ScanChannel> allMemoryChannels() const;
    SliceModel* chosenSlice() const;
    void pushConfig();
    void restorePanView();

    // Panadapter view at scan start, restored on a user stop if a channel
    // outside the displayed span made the scan recenter the pan.
    QString m_scanPanId;
    double  m_scanPanCenterMhz{0.0};
    double  m_scanPanBandwidthMhz{0.0};

    RadioModel* m_radio{nullptr};
    Scanner*    m_scanner{nullptr};
    int         m_activeSliceId{-1};

    // Source
    QComboBox*      m_sliceCombo{nullptr};
    QComboBox*      m_modeCombo{nullptr};
    QStackedWidget* m_stack{nullptr};
    // Memory page
    QComboBox*      m_memSelectCombo{nullptr};
    QComboBox*      m_memGroupCombo{nullptr};
    QListWidget*    m_memList{nullptr};
    // Band page
    QComboBox*      m_bandCombo{nullptr};
    QDoubleSpinBox* m_bandStart{nullptr};
    QDoubleSpinBox* m_bandStop{nullptr};
    QSpinBox*       m_bandStep{nullptr};
    QComboBox*      m_bandMode{nullptr};
    QLabel*         m_bandCount{nullptr};
    // List page
    QPlainTextEdit* m_listEdit{nullptr};
    // Behaviour
    QSpinBox*       m_dwellSpin{nullptr};
    QSpinBox*       m_settleSpin{nullptr};
    QComboBox*      m_detectCombo{nullptr};
    QSpinBox*       m_thresholdSpin{nullptr};
    QComboBox*      m_resumeCombo{nullptr};
    QDoubleSpinBox* m_resumeSpin{nullptr};
    QCheckBox*      m_priorityCheck{nullptr};
    QDoubleSpinBox* m_priorityFreq{nullptr};
    QComboBox*      m_priorityMode{nullptr};
    QSpinBox*       m_priorityInterval{nullptr};
    // Controls / status
    QPushButton*    m_startBtn{nullptr};
    QPushButton*    m_pauseBtn{nullptr};
    QPushButton*    m_skipBtn{nullptr};
    QPushButton*    m_clearSkipBtn{nullptr};
    QLabel*         m_skipLabel{nullptr};
    QLabel*         m_stateLabel{nullptr};
    QLabel*         m_channelLabel{nullptr};
    QLabel*         m_levelLabel{nullptr};
    QTableWidget*   m_log{nullptr};
    QPushButton*    m_clearLogBtn{nullptr};
};

} // namespace AetherSDR
