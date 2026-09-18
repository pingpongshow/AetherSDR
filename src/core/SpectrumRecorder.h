#pragma once

#include "SpectrumRecordingFormat.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>
#include <memory>

namespace AetherSDR {

class SpectrumRecorderWorker;

// Records one panadapter's FFT frames + waterfall rows + pan/slice metadata to
// an .aesr file (see SpectrumRecordingFormat.h).
//
// Threading:
//   • submitSpectrum()/submitWaterfallRow() are thread-safe and non-blocking.
//     They are designed to be connected with Qt::DirectConnection to
//     PanadapterStream (network thread): they filter by stream ID (atomics),
//     apply a bounded-memory budget (drop + count when the writer falls behind)
//     and post the frame to the recorder's own writer thread. No mutex, no I/O
//     on the caller's thread.
//   • Compression (qCompress/zlib) and file I/O happen only on the writer
//     thread, owned by this object (RAII: joined in the destructor).
//   • start()/stop()/submitPanState()/submitSliceMarkers() are main-thread API.
class SpectrumRecorder : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString     directory;                          // required
        QString     baseName;                           // default: AetherSDR_<UTC>Z_<Hz>Hz_Pan<id>
        qint64      maxSegmentBytes{512LL << 20};       // rotate to _partNNN after this
        qint64      maxTotalBytes{0};                   // 0 = unlimited; stop when reached
        qint64      maxDurationMs{0};                   // 0 = unlimited; stop when reached
        qint64      minFreeBytes{256LL << 20};          // disk-space guard; stop below this
        bool        losslessFft{false};                 // Float32 vs Int16CentiDb (±0.005 dB)
        int         flushIntervalMs{1000};              // chunk cadence (crash-loss bound)
        int         maxChunkRawBytes{2 << 20};          // chunk size cap (uncompressed)
        qint64      maxPendingBytes{32LL << 20};        // queued-but-unwritten budget
        int         compressionLevel{6};
        QJsonObject metadata;                           // merged into the file header JSON
    };

    explicit SpectrumRecorder(QObject* parent = nullptr);
    ~SpectrumRecorder() override;

    // Stream IDs to accept (pan FFT stream = pan ID 0x40xx, waterfall 0x42xx).
    // 0 = accept any stream (useful for tests / single-pan setups).
    void setStreamIds(quint32 panStreamId, quint32 wfStreamId);

    // Initial pan state is written as the first keyframe. Returns false (and
    // sets *err) if the directory cannot be created or disk space is short.
    bool start(const Options& opts, const SpecRec::PanState& initial, QString* err = nullptr);
    // Flushes and closes synchronously (blocks until the writer thread has
    // written the final chunk). Safe to call when not recording.
    void stop();
    bool isRecording() const { return m_recording.load(std::memory_order_acquire); }

    // ── Data entry points (thread-safe, non-blocking) ───────────────────────
    void submitSpectrum(quint32 streamId, const QVector<float>& binsDbm);
    void submitWaterfallRow(quint32 streamId, const QVector<float>& bins,
                            double lowFreqMhz, double highFreqMhz, quint32 timecode);

    // ── Metadata (main thread) ──────────────────────────────────────────────
    void submitPanState(const SpecRec::PanState& state);
    void submitSliceMarkers(const QVector<SpecRec::SliceMarker>& markers);
    void submitNote(const QString& text);

    // Statistics (any thread).
    qint64  bytesWritten() const { return m_bytesWritten.load(std::memory_order_relaxed); }
    qint64  elapsedMs() const;
    quint64 droppedFrames() const { return m_dropped.load(std::memory_order_relaxed); }
    QString firstFilePath() const { return m_firstPath; }

    static QString defaultBaseName(const QDateTime& utc, double centerMhz, const QString& panId);

    // Test hook: µs clock source override (0 = real clock).
    void setClockOverrideUs(qint64 us) { m_clockOverrideUs.store(us, std::memory_order_relaxed); }

signals:
    void fileOpened(const QString& path);
    void statsUpdated(qint64 bytesWritten, qint64 elapsedMs, quint64 droppedFrames);
    // reason: "user", "size-limit", "duration-limit", "disk-full", "io-error"
    void stopped(const QString& reason, const QString& firstFilePath);
    void errorOccurred(const QString& message);

private:
    qint64 nowUs() const;
    bool   reserve(qint64 bytes);
    void   onWorkerStopped(const QString& reason);

    QThread                                 m_thread;
    SpectrumRecorderWorker*                 m_worker{nullptr};  // lives on m_thread; deleted on finished
    std::atomic<bool>                       m_recording{false};
    std::atomic<quint32>                    m_panStreamId{0};
    std::atomic<quint32>                    m_wfStreamId{0};
    std::atomic<qint64>                     m_pendingBytes{0};
    std::atomic<qint64>                     m_maxPendingBytes{32LL << 20};
    std::atomic<qint64>                     m_bytesWritten{0};
    std::atomic<quint64>                    m_dropped{0};
    std::atomic<qint64>                     m_clockOverrideUs{0};
    std::atomic<bool>                       m_losslessFft{false};
    qint64                                  m_startUtcUs{0};
    QElapsedTimer                           m_clock;
    QString                                 m_firstPath;

    friend class SpectrumRecorderWorker;
};

// Fixed bank of recorders, one slot per panadapter, so the network thread can
// fan frames out without touching any container that the main thread mutates.
// Slots are created up front (their writer threads start lazily on first
// start()); the network-thread entry points only read immutable slot pointers
// and each recorder's atomics.
class SpectrumRecorderBank : public QObject {
    Q_OBJECT
public:
    static constexpr int kSlots = 8;   // FLEX-8600 supports up to 8 panadapters

    explicit SpectrumRecorderBank(QObject* parent = nullptr);

    SpectrumRecorder* slot(int i) const { return (i >= 0 && i < kSlots) ? m_slots[i] : nullptr; }

    // Thread-safe: connect with Qt::DirectConnection to PanadapterStream.
    void onSpectrumReady(quint32 streamId, const QVector<float>& binsDbm);
    void onWaterfallRowReady(quint32 streamId, const QVector<float>& bins,
                             double lowFreqMhz, double highFreqMhz, quint32 timecode);

private:
    SpectrumRecorder* m_slots[kSlots]{};  // children of this; never reassigned
};

// Writer-thread half. Internal, but declared here so AUTOMOC sees it.
class SpectrumRecorderWorker : public QObject {
    Q_OBJECT
public:
    SpectrumRecorderWorker(SpectrumRecorder* owner) : m_owner(owner) {}

    bool open(const SpectrumRecorder::Options& opts, const QString& firstPath,
              const SpecRec::PanState& initial, qint64 tUs, QString* err);
    void append(SpecRec::Record&& rec, qint64 reservedBytes);
    void finish(const QString& reason);

signals:
    void workerStopped(const QString& reason);
    void workerFileOpened(const QString& path);
    void workerStats(qint64 bytes, quint64 dropped);

protected:
    void timerEvent(QTimerEvent* ev) override;

private:
    bool openSegment(int index, QString* err);
    void flushChunk();
    void writeKeyframe(qint64 tUs);
    void checkLimits();

    SpectrumRecorder*           m_owner;
    SpectrumRecorder::Options   m_opts;
    QString                     m_firstPath;
    std::unique_ptr<QFile>      m_file;
    int                         m_segment{0};
    qint64                      m_segmentBytes{0};
    qint64                      m_totalBytes{0};
    QByteArray                  m_raw;
    quint32                     m_rawCount{0};
    qint64                      m_chunkT0{0};
    qint64                      m_chunkT1{0};
    qint64                      m_firstUs{0};
    qint64                      m_lastFlushUs{0};
    SpecRec::PanState           m_pan;
    QVector<SpecRec::SliceMarker> m_slices;
    bool                        m_haveSlices{false};
    bool                        m_active{false};
    int                         m_timerId{0};
};

} // namespace AetherSDR
