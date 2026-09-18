#pragma once

// DAX IQ → WAV/RF64 recorder.
//
// File format (interoperable with SDR#, HDSDR, SDRuno, SDR Console, SDR++,
// Linrad's WAV import and most "IQ WAV" tooling):
//   • 2-channel WAV, I = left, Q = right (same as DAX IQ / every SDR app).
//   • float32 (WAVE_FORMAT_IEEE_FLOAT, fmt size 18) or int16 PCM (fmt size 16).
//   • Chunk order: RIFF/WAVE, JUNK(28) [→ ds64 when promoted to RF64], fmt,
//     auxi(164), data. The 28-byte JUNK placeholder is the EBU Tech 3306
//     technique: if the file passes 4 GiB it is rewritten in place as 'RF64'
//     with a 'ds64' chunk, otherwise it stays a plain RIFF file every reader
//     understands.
//   • 'auxi' chunk: the 164-byte Winrad/HDSDR layout also read by SDR#,
//     SDRuno and SDR Console:
//         SYSTEMTIME StartTime   (8 × u16: yr, mon, dow, day, hr, min, sec, ms) UTC
//         SYSTEMTIME StopTime
//         u32 CenterFreq         Hz  (LO / centre of the IQ band)
//         u32 ADFrequency        sample rate, Hz
//         u32 IFFrequency        0
//         u32 Bandwidth          sample rate (complex → full band)
//         u32 IQOffset           0
//         u32 Unused2..Unused5   0
//         char nextfilename[96]  next file of a split recording ("" if last)
//   • File name: "AetherSDR_<yyyyMMdd>_<HHmmss>Z_<centerHz>Hz_IQ.wav" — the
//     SDR# convention ("SDRSharp_20240520_181030Z_97300000Hz_IQ.wav"), which
//     SDR#, SDR++, HDSDR-style tools and iq_tool parse for centre frequency
//     when no auxi chunk is understood.
//
// Crash safety: the header's size fields are refreshed every ~2 s; after a
// crash IqWav::repairHeader() recomputes them from the file length.

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <complex>
#include <memory>
#include <vector>

namespace AetherSDR {

enum class IqSampleFormat { Float32, Int16 };

struct IqWavInfo {
    bool      rf64{false};
    int       formatTag{0};       // 1 = PCM, 3 = IEEE float (0xFFFE resolved to sub-format)
    int       channels{0};
    int       sampleRate{0};
    int       bitsPerSample{0};
    int       blockAlign{0};
    qint64    dataOffset{0};
    qint64    dataBytes{0};
    qint64    frames{0};
    bool      hasAuxi{false};
    QDateTime startUtc;
    QDateTime stopUtc;
    qint64    centerHz{0};        // from auxi, else parsed from file name
    QString   nextFileName;
    bool      headerSizeMismatch{false}; // header disagreed with file length (crash)
};

namespace IqWav {
QString makeFileName(const QDateTime& utc, qint64 centerHz);
// Parse "…_<N>Hz_…" (SDR#/AetherSDR/SDR++) or "…_<N>kHz_…" (HDSDR/SDRuno).
qint64  centerFromFileName(const QString& fileName);
bool    parse(const QString& path, IqWavInfo& info, QString* err = nullptr);
bool    repairHeader(const QString& path, QString* err = nullptr);
// Read `count` complex frames starting at `startFrame` (float, full scale ±1).
bool    readFrames(QFile& f, const IqWavInfo& info, qint64 startFrame, int count,
                   std::vector<std::complex<float>>& out);
}

// Synchronous WAV/RF64 writer. Not thread-safe; owned by one thread.
class IqWavWriter {
public:
    struct Params {
        int            sampleRate{48000};
        qint64         centerHz{0};
        IqSampleFormat format{IqSampleFormat::Float32};
        QDateTime      startUtc;
    };

    ~IqWavWriter();
    bool open(const QString& path, const Params& p, QString* err = nullptr);
    // Interleaved I,Q float frames (native endian).
    bool writeFrames(const float* iq, qint64 frames);
    bool updateHeader();                       // refresh sizes (crash safety)
    bool close(const QString& nextFileName = {}); // stamps StopTime, final sizes
    bool isOpen() const { return m_file.isOpen(); }

    qint64  framesWritten() const { return m_frames; }
    qint64  fileBytes() const { return m_dataOffset + m_dataBytes; }
    QString path() const { return m_file.fileName(); }
    const Params& params() const { return m_params; }

    // Test hook: promote to RF64 at this data size instead of 4 GiB.
    void setRf64ThresholdForTest(qint64 bytes) { m_rf64Threshold = bytes; }

private:
    bool writeHeader(const QDateTime& stopUtc, const QString& nextFileName);

    QFile   m_file;
    Params  m_params;
    qint64  m_dataOffset{0};
    qint64  m_dataBytes{0};
    qint64  m_frames{0};
    qint64  m_rf64Threshold{0xFFFFFFFFLL - 1024};
    int     m_blockAlign{8};
    QByteArray m_convert;
};

class IqRecorderWorker;

// Records one DAX IQ channel to WAV. submitIq() is thread-safe and
// non-blocking (connect with Qt::DirectConnection to
// PanadapterStream::iqDataReady); conversion and file I/O run on the
// recorder's own writer thread with a bounded pending-bytes budget.
class IqRecorder : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString        directory;
        int            channel{1};                 // DAX IQ channel 1-4
        qint64         centerHz{0};
        IqSampleFormat format{IqSampleFormat::Float32};
        qint64         splitBytes{0};              // 0 = single file (RF64 past 4 GiB)
        qint64         maxTotalBytes{0};           // 0 = unlimited
        qint64         maxDurationMs{0};           // 0 = unlimited
        qint64         minFreeBytes{512LL << 20};  // disk-space guard
        qint64         maxPendingBytes{64LL << 20};
    };

    explicit IqRecorder(QObject* parent = nullptr);
    ~IqRecorder() override;

    bool start(const Options& opts, QString* err = nullptr);
    void stop();
    bool isRecording() const { return m_recording.load(std::memory_order_acquire); }
    int  channel() const { return m_channel.load(std::memory_order_relaxed); }

    // Thread-safe. rawPayload = VITA-49 payload: big-endian float32 I/Q pairs.
    void submitIq(int channel, const QByteArray& rawPayload, int sampleRate);

    // The pan centre moved: WAV metadata can hold one centre frequency, so
    // the recorder closes the current file and continues in a new one.
    void setCenterFrequency(qint64 hz);

    qint64  bytesWritten() const { return m_bytesWritten.load(std::memory_order_relaxed); }
    qint64  framesWritten() const { return m_framesWritten.load(std::memory_order_relaxed); }
    quint64 droppedPackets() const { return m_dropped.load(std::memory_order_relaxed); }

signals:
    void fileOpened(const QString& path);
    void fileClosed(const QString& path);
    void statsUpdated(qint64 bytesWritten, qint64 durationMs, quint64 droppedPackets);
    // reason: "user", "size-limit", "duration-limit", "disk-full", "io-error"
    void stopped(const QString& reason);
    void errorOccurred(const QString& message);

private:
    QThread               m_thread;
    IqRecorderWorker*     m_worker{nullptr};
    std::atomic<bool>     m_recording{false};
    std::atomic<int>      m_channel{0};
    std::atomic<qint64>   m_pendingBytes{0};
    std::atomic<qint64>   m_maxPendingBytes{64LL << 20};
    std::atomic<qint64>   m_bytesWritten{0};
    std::atomic<qint64>   m_framesWritten{0};
    std::atomic<quint64>  m_dropped{0};

    friend class IqRecorderWorker;
};

class IqRecorderWorker : public QObject {
    Q_OBJECT
public:
    explicit IqRecorderWorker(IqRecorder* owner) : m_owner(owner) {}
    void begin(const IqRecorder::Options& opts);
    void packet(const QByteArray& payloadBE, int sampleRate, qint64 reserved);
    void setCenter(qint64 hz);
    void finish(const QString& reason);

signals:
    void workerFileOpened(const QString& path);
    void workerFileClosed(const QString& path);
    void workerStats(qint64 bytes, qint64 durationMs, quint64 dropped);
    void workerStopped(const QString& reason);

private:
    bool openFile(int sampleRate);
    void closeFile(const QString& nextName = {});

    IqRecorder*                  m_owner;
    IqRecorder::Options          m_opts;
    std::unique_ptr<IqWavWriter> m_writer;
    std::vector<float>           m_scratch;
    qint64                       m_totalBytes{0};   // closed files
    qint64                       m_totalFrames{0};  // all files, at 1 rate each
    double                       m_totalSeconds{0.0};
    qint64                       m_lastHeaderUpdateFrames{0};
    qint64                       m_lastStatsFrames{0};
    bool                         m_active{false};
};

} // namespace AetherSDR
