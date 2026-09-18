#pragma once

#include "core/IqRecorder.h"
#include "core/SpectrumPlayer.h"
#include "core/SpectrumRecorder.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace AetherSDR {

class SpectrumWidget;

// Recording & playback panel:
//   • Spectrum recording per panadapter (SpectrumRecorderBank slot per pan)
//   • DAX IQ recording to WAV/RF64 (channel, rate request, sample format)
//   • Library of recordings in RecordingDirectory: play, rename, delete
//     (to trash), export a waterfall time range as PNG, "Play in waterfall"
//     for IQ files (offline FFT → .aesr → SpectrumPlayer)
//   • Transport: play/pause, stop, 1/2/4/8×, loop, seek slider
//
// The panel never talks to the radio directly. The coordinator (MainWindow)
// feeds pan info/state, wires the recorder bank and IQ recorder to
// PanadapterStream, and handles iqStreamSetupRequested().
//
// Settings (AppSettings): RecordingDirectory, SpectrumRecordingLosslessFft,
// SpectrumRecordingSegmentMB, RecordingMinFreeMB, IqRecordingChannel,
// IqRecordingRate, IqRecordingFormat, IqRecordingSplitMB,
// SpectrumPlaybackSpeed, SpectrumPlaybackLoop.
class RecordingPanel : public QWidget {
    Q_OBJECT

public:
    struct PanInfo {
        QString           panId;          // "0x40000000"
        QString           label;          // "Pan A — 14.225 MHz"
        quint32           panStreamId{0};
        quint32           wfStreamId{0};
        SpecRec::PanState state;
    };

    explicit RecordingPanel(QWidget* parent = nullptr);
    ~RecordingPanel() override;

    // ── Coordinator API ─────────────────────────────────────────────────────
    SpectrumRecorderBank* recorderBank() const { return m_bank; }
    IqRecorder*           iqRecorder() const { return m_iq.get(); }
    SpectrumPlayer*       player() const { return m_player; }

    void setPanadapters(const QVector<PanInfo>& pans);
    void updatePanState(const QString& panId, const SpecRec::PanState& state);
    void updateSliceMarkers(const QString& panId, const QVector<SpecRec::SliceMarker>& markers);
    // Centre frequency of the pan a DAX IQ channel is attached to (Hz).
    void setIqChannelCenter(int channel, qint64 centerHz);
    // Resolve panId → SpectrumWidget for playback (MainWindow: m_panStack->spectrum(id)).
    void setSpectrumResolver(std::function<SpectrumWidget*(const QString& panId)> fn);

    static QString recordingDirectory();
    static QString defaultRecordingDirectory();

    // PNG export (also used by tests/tools). Rows from [fromMs, toMs] of the
    // recording; waterfall rows if present, else FFT frames.
    static bool exportWaterfallPng(const QString& aesrPath, const QString& pngPath,
                                   qint64 fromMs, qint64 toMs, int colorScheme = 0,
                                   QString* err = nullptr);

signals:
    // Ask the coordinator to ensure a DAX IQ stream exists on `channel` at
    // `sampleRate` (DaxIqModel::createStream + setSampleRate).
    void iqStreamSetupRequested(int channel, int sampleRate);

private:
    void buildUi();
    void refreshLibrary();
    void refreshDiskSpace();
    void onSpectrumRecordClicked();
    void onIqRecordClicked();
    void onPlayClicked();
    void onRenameClicked();
    void onDeleteClicked();
    void onExportClicked();
    void onChooseFolder();
    void updateSpectrumStatus();
    void updateTransport();
    bool startPlayback(const QString& aesrPath);
    void stopPlayback();
    void playIqInWaterfall(const QString& wavPath);
    int  slotForPan(const QString& panId) const;
    QString selectedPath() const;
    QString selectedPanId() const;
    qint64 minFreeBytes() const;

    SpectrumRecorderBank*       m_bank{nullptr};
    std::unique_ptr<IqRecorder> m_iq;
    SpectrumPlayer*             m_player{nullptr};
    std::function<SpectrumWidget*(const QString&)> m_resolver;

    QVector<PanInfo>            m_pans;
    QHash<QString, int>         m_panSlot;        // panId → bank slot
    qint64                      m_iqCenter[5]{};  // index 1-4

    // Spectrum recording UI
    QComboBox*   m_panCombo{nullptr};
    QPushButton* m_specRecBtn{nullptr};
    QLabel*      m_specStatus{nullptr};
    // IQ recording UI
    QComboBox*   m_iqChannelCombo{nullptr};
    QComboBox*   m_iqRateCombo{nullptr};
    QComboBox*   m_iqFormatCombo{nullptr};
    QPushButton* m_iqRecBtn{nullptr};
    QLabel*      m_iqStatus{nullptr};
    QString      m_iqCurrentFile;
    // Library
    QLabel*      m_dirLabel{nullptr};
    QLabel*      m_diskLabel{nullptr};
    QTreeWidget* m_library{nullptr};
    // Transport
    QPushButton* m_playBtn{nullptr};
    QPushButton* m_stopBtn{nullptr};
    QComboBox*   m_speedCombo{nullptr};
    QCheckBox*   m_loopCheck{nullptr};
    QSlider*     m_seekSlider{nullptr};
    QLabel*      m_timeLabel{nullptr};
    bool         m_sliderDragging{false};

    QTimer*      m_statusTimer{nullptr};
    QString      m_tempAesr;   // IQ→spectrum conversion output, removed on stop
};

} // namespace AetherSDR
