#pragma once

#include <QObject>
#include <QPointer>
#include <QFile>
#include <QTimer>
#include <QDateTime>
#include <QString>
#include <atomic>
#include <memory>
#include <mutex>

namespace AetherSDR {

class Resampler;
class SliceModel;
class TransmitModel;

// Records QSO audio (both RX and TX sides) to WAV files.
//
// Usage:
//   - Connect feedRxAudio() to AudioEngine::feedAudioData() (or post-DSP tap)
//   - Connect feedTxAudio() to AudioEngine::txRawPcmReady()
//   - Connect onMoxChanged() to TransmitModel::moxChanged()
//   - Set the active slice for frequency/mode metadata via setSlice()
//
// Recording triggers:
//   - Auto: starts when MOX goes true (first TX), stops after idle timeout
//   - Manual: startRecording() / stopRecording()
//
// Output: 24 kHz stereo int16 WAV (matches AudioEngine native format).

class QsoRecorder : public QObject {
    Q_OBJECT

public:
    explicit QsoRecorder(QObject* parent = nullptr);
    ~QsoRecorder() override;

    // Configuration
    void setRecordingDir(const QString& path);
    QString recordingDir() const { return m_recordingDir; }

    void setIdleTimeoutSecs(int secs);
    int idleTimeoutSecs() const { return m_idleTimeoutSecs; }

    void setAutoRecordEnabled(bool on);
    bool autoRecordEnabled() const { return m_autoRecord; }

    void setCallsign(const QString& call);
    QString callsign() const { return m_callsign; }

    // Filename component toggles
    void setIncludeDate(bool on);
    void setIncludeTime(bool on);
    void setIncludeFrequency(bool on);
    void setIncludeMode(bool on);

    // Active slice (provides frequency + mode for filename)
    void setSlice(SliceModel* slice);

    bool isRecording() const { return m_recording; }
    bool isPlaying() const { return m_playing; }
    bool hasLastRecording() const { return !m_lastRecordingPath.isEmpty(); }

    // Duration of current recording in seconds (0 if not recording)
    int recordingDurationSecs() const;

public slots:
    // Manual control
    void startRecording();
    void stopRecording();

    // Playback of last recording
    void startPlayback();
    void stopPlayback();

    // Audio feeds — thread-safe, may be called from any thread.
    // RX: 24 kHz stereo float32 (PanadapterStream::audioDataReady).
    void feedRxAudio(const QByteArray& pcm);
    // TX: 24 kHz stereo float32 (AudioEngine::txRawPcmReady format).
    void feedTxAudio(const QByteArray& pcm);
    // TX in an arbitrary PCM format; resampled/converted to the file format.
    // channels: 1 or 2. float32=false means int16 samples.
    void feedTxPcm(const QByteArray& pcm, int sampleRate, int channels, bool float32);

    // TX state tracking (connect to TransmitModel::moxChanged)
    void onMoxChanged(bool mox);

signals:
    void recordingStarted(const QString& filePath);
    void recordingStopped(const QString& filePath, int durationSecs);
    void recordingError(const QString& error);
    void playbackStarted();
    void playbackStopped();
    void playbackAudio(const QByteArray& pcm);  // float32 stereo chunks for AudioEngine
    void muteRxRequested(bool mute);  // mute live RX during playback

private:
    // Re-read all QsoRecording* keys from AppSettings. RadioSetupDialog writes
    // these at runtime, so they are re-read at every entry point that uses them.
    void reloadSettings();
    void startFile();
    void finalizeFile();
    QString buildFilename() const;
    void writeWavHeader();
    void patchWavHeader();
    // Appends int16 PCM and keeps the header sizes current.  Caller holds
    // m_writeMutex.
    void writePcmLocked(const QByteArray& int16pcm);

    // Recording state
    bool        m_recording{false};
    QFile*      m_file{nullptr};
    QDateTime   m_startTime;
    quint32     m_dataBytes{0};    // PCM data bytes written (for WAV header patching)
    quint32     m_dataBytesAtLastPatch{0};

    // Configuration
    QString     m_recordingDir;
    int         m_idleTimeoutSecs{120};  // 2 minutes default
    bool        m_autoRecord{false};
    QString     m_callsign;
    bool        m_includeDate{true};
    bool        m_includeTime{true};
    bool        m_includeFreq{true};
    bool        m_includeMode{true};

    // Slice metadata (captured at recording start)
    QPointer<SliceModel> m_slice;  // slices are deleteLater'd on removal
    double      m_freqMhz{0.0};
    QString     m_mode;

    // Idle timeout
    QTimer*     m_idleTimer{nullptr};

    // Playback
    bool        m_playing{false};
    QString     m_lastRecordingPath;
    QFile*      m_playFile{nullptr};
    QTimer*     m_playTimer{nullptr};

    // Thread safety for audio feed paths
    std::mutex  m_writeMutex;

    // TX/RX timeline: while MOX is active and TX audio is being recorded,
    // RX audio writes are suppressed so the file isn't double-filled.
    std::atomic<bool> m_txActive{false};
    bool        m_txAudioThisOver{false};      // guarded by m_writeMutex
    std::unique_ptr<Resampler> m_txResampler;  // guarded by m_writeMutex
    int         m_txResamplerRate{0};
    int         m_txResamplerChannels{0};

    // WAV format constants (matching AudioEngine native format)
    static constexpr int SAMPLE_RATE = 24000;
    static constexpr int NUM_CHANNELS = 2;
    static constexpr int BITS_PER_SAMPLE = 16;
    static constexpr int WAV_HEADER_SIZE = 44;
    // Rewrite the header sizes every ~5 s of audio so a crash or power loss
    // leaves a playable file instead of one whose data chunk claims 0 bytes.
    static constexpr quint32 kHeaderPatchIntervalBytes =
        SAMPLE_RATE * NUM_CHANNELS * (BITS_PER_SAMPLE / 8) * 5;
    // RIFF sizes are 32-bit: stop appending before the chunk size wraps
    // (~12.4 h at 24 kHz stereo int16).
    static constexpr quint32 kMaxDataBytes = 0xFFFFFFFFu - WAV_HEADER_SIZE;
};

} // namespace AetherSDR
