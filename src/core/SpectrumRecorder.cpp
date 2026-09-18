#include "SpectrumRecorder.h"
#include "LogManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTimerEvent>

namespace AetherSDR {

namespace {
constexpr qint64 kPerFrameOverheadBytes = 96;  // QVector header + event + record header
}

// ─── SpectrumRecorder (main-thread facade) ───────────────────────────────────

SpectrumRecorder::SpectrumRecorder(QObject* parent)
    : QObject(parent)
{
    m_thread.setObjectName(QStringLiteral("SpectrumRecorder"));
    m_worker = new SpectrumRecorderWorker(this);  // no parent: moved to m_thread
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &SpectrumRecorderWorker::workerStopped,
            this, &SpectrumRecorder::onWorkerStopped);
    connect(m_worker, &SpectrumRecorderWorker::workerFileOpened,
            this, &SpectrumRecorder::fileOpened);
    connect(m_worker, &SpectrumRecorderWorker::workerStats, this,
            [this](qint64 bytes, quint64 dropped) {
                emit statsUpdated(bytes, elapsedMs(), dropped);
            });
    // The writer thread is started lazily by start(): idle recorders (e.g. the
    // unused slots of a SpectrumRecorderBank) cost no thread.
}

SpectrumRecorder::~SpectrumRecorder()
{
    stop();
    if (m_thread.isRunning()) {
        m_thread.quit();
        m_thread.wait();   // finished → m_worker->deleteLater() runs on exit
    } else {
        delete m_worker;   // thread never ran: nothing else will delete it
    }
    m_worker = nullptr;
}

void SpectrumRecorder::setStreamIds(quint32 panStreamId, quint32 wfStreamId)
{
    m_panStreamId.store(panStreamId, std::memory_order_relaxed);
    m_wfStreamId.store(wfStreamId, std::memory_order_relaxed);
}

QString SpectrumRecorder::defaultBaseName(const QDateTime& utc, double centerMhz, const QString& panId)
{
    // Mirrors the SDR# IQ naming style (…_<yyyyMMdd>_<HHmmss>Z_<Hz>Hz_…) so the
    // spectrum and IQ recordings of one session sort next to each other.
    QString name = QStringLiteral("AetherSDR_%1Z_%2Hz_Spectrum")
        .arg(utc.toUTC().toString(QStringLiteral("yyyyMMdd_HHmmss")))
        .arg(qRound64(centerMhz * 1e6));
    if (!panId.isEmpty()) {
        name += QStringLiteral("_Pan") + QString(panId).remove(QStringLiteral("0x"));
    }
    return name;
}

qint64 SpectrumRecorder::nowUs() const
{
    const qint64 o = m_clockOverrideUs.load(std::memory_order_relaxed);
    if (o != 0) { return o; }
    return m_startUtcUs + m_clock.nsecsElapsed() / 1000;
}

qint64 SpectrumRecorder::elapsedMs() const
{
    return isRecording() ? m_clock.elapsed() : 0;
}

bool SpectrumRecorder::start(const Options& opts, const SpecRec::PanState& initial, QString* err)
{
    if (isRecording()) {
        if (err) { *err = tr("Already recording"); }
        return false;
    }
    QDir dir(opts.directory);
    if (opts.directory.isEmpty() || !dir.mkpath(QStringLiteral("."))) {
        if (err) { *err = tr("Cannot create recording directory %1").arg(opts.directory); }
        return false;
    }
    const QStorageInfo storage(dir.absolutePath());
    if (storage.isValid() && storage.bytesAvailable() < opts.minFreeBytes) {
        if (err) { *err = tr("Not enough free disk space (%1 MB available)")
                              .arg(storage.bytesAvailable() >> 20); }
        return false;
    }

    const QDateTime utc = QDateTime::currentDateTimeUtc();
    m_startUtcUs = utc.toMSecsSinceEpoch() * 1000;
    m_clock.start();

    QString base = opts.baseName;
    if (base.isEmpty()) {
        base = defaultBaseName(utc, initial.centerMhz,
                               opts.metadata.value(QStringLiteral("panId")).toString());
    }
    m_firstPath = dir.absoluteFilePath(base + QLatin1Char('.') + QLatin1String(SpecRec::kFileSuffix));

    if (!m_thread.isRunning()) {
        m_thread.start(QThread::LowPriority);
    }
    m_maxPendingBytes.store(std::max<qint64>(opts.maxPendingBytes, 1 << 16));
    m_losslessFft.store(opts.losslessFft);
    m_pendingBytes.store(0);
    m_bytesWritten.store(0);
    m_dropped.store(0);

    bool ok = false;
    QString werr;
    const qint64 t = nowUs();
    QMetaObject::invokeMethod(m_worker, [&] {
        ok = m_worker->open(opts, m_firstPath, initial, t, &werr);
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        if (err) { *err = werr; }
        return false;
    }
    m_recording.store(true, std::memory_order_release);
    qCInfo(lcDsp) << "SpectrumRecorder: recording to" << m_firstPath;
    return true;
}

void SpectrumRecorder::stop()
{
    if (!m_recording.exchange(false, std::memory_order_acq_rel)) { return; }
    QMetaObject::invokeMethod(m_worker, [this] { m_worker->finish(QStringLiteral("user")); },
                              Qt::BlockingQueuedConnection);
}

void SpectrumRecorder::onWorkerStopped(const QString& reason)
{
    m_recording.store(false, std::memory_order_release);
    if (reason != QLatin1String("user")) {
        qCWarning(lcDsp) << "SpectrumRecorder: stopped:" << reason;
    }
    emit stopped(reason, m_firstPath);
}

bool SpectrumRecorder::reserve(qint64 bytes)
{
    const qint64 prev = m_pendingBytes.fetch_add(bytes, std::memory_order_relaxed);
    if (prev + bytes > m_maxPendingBytes.load(std::memory_order_relaxed)) {
        m_pendingBytes.fetch_sub(bytes, std::memory_order_relaxed);
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void SpectrumRecorder::submitSpectrum(quint32 streamId, const QVector<float>& binsDbm)
{
    if (!m_recording.load(std::memory_order_acquire)) { return; }
    const quint32 want = m_panStreamId.load(std::memory_order_relaxed);
    if (want != 0 && want != streamId) { return; }
    const qint64 cost = binsDbm.size() * static_cast<qint64>(sizeof(float)) + kPerFrameOverheadBytes;
    if (!reserve(cost)) { return; }
    SpecRec::Record r;
    r.type     = SpecRec::RecordType::FftFrame;
    r.encoding = m_losslessFft.load(std::memory_order_relaxed)
        ? SpecRec::BinEncoding::Float32 : SpecRec::BinEncoding::Int16CentiDb;
    r.tUs      = nowUs();
    r.bins     = binsDbm;  // implicit share, no deep copy on this thread
    QMetaObject::invokeMethod(m_worker, [w = m_worker, r = std::move(r), cost]() mutable {
        w->append(std::move(r), cost);
    }, Qt::QueuedConnection);
}

void SpectrumRecorder::submitWaterfallRow(quint32 streamId, const QVector<float>& bins,
                                          double lowFreqMhz, double highFreqMhz, quint32 timecode)
{
    if (!m_recording.load(std::memory_order_acquire)) { return; }
    const quint32 want = m_wfStreamId.load(std::memory_order_relaxed);
    if (want != 0 && want != streamId) { return; }
    const qint64 cost = bins.size() * static_cast<qint64>(sizeof(float)) + kPerFrameOverheadBytes;
    if (!reserve(cost)) { return; }
    SpecRec::Record r;
    r.type     = SpecRec::RecordType::WaterfallRow;
    r.tUs      = nowUs();
    r.bins     = bins;
    r.lowMhz   = lowFreqMhz;
    r.highMhz  = highFreqMhz;
    r.timecode = timecode;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, r = std::move(r), cost]() mutable {
        w->append(std::move(r), cost);
    }, Qt::QueuedConnection);
}

void SpectrumRecorder::submitPanState(const SpecRec::PanState& state)
{
    if (!isRecording()) { return; }
    SpecRec::Record r;
    r.type = SpecRec::RecordType::PanState;
    r.tUs  = nowUs();
    r.pan  = state;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, r = std::move(r)]() mutable {
        w->append(std::move(r), 0);
    }, Qt::QueuedConnection);
}

void SpectrumRecorder::submitSliceMarkers(const QVector<SpecRec::SliceMarker>& markers)
{
    if (!isRecording()) { return; }
    SpecRec::Record r;
    r.type   = SpecRec::RecordType::SliceMarkers;
    r.tUs    = nowUs();
    r.slices = markers;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, r = std::move(r)]() mutable {
        w->append(std::move(r), 0);
    }, Qt::QueuedConnection);
}

void SpectrumRecorder::submitNote(const QString& text)
{
    if (!isRecording()) { return; }
    SpecRec::Record r;
    r.type = SpecRec::RecordType::Note;
    r.tUs  = nowUs();
    r.text = text;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, r = std::move(r)]() mutable {
        w->append(std::move(r), 0);
    }, Qt::QueuedConnection);
}

// ─── SpectrumRecorderBank ────────────────────────────────────────────────────

SpectrumRecorderBank::SpectrumRecorderBank(QObject* parent)
    : QObject(parent)
{
    for (SpectrumRecorder*& r : m_slots) {
        r = new SpectrumRecorder(this);
    }
}

void SpectrumRecorderBank::onSpectrumReady(quint32 streamId, const QVector<float>& binsDbm)
{
    for (SpectrumRecorder* r : m_slots) {
        r->submitSpectrum(streamId, binsDbm);   // cheap atomic reject when idle
    }
}

void SpectrumRecorderBank::onWaterfallRowReady(quint32 streamId, const QVector<float>& bins,
                                               double lowFreqMhz, double highFreqMhz,
                                               quint32 timecode)
{
    for (SpectrumRecorder* r : m_slots) {
        r->submitWaterfallRow(streamId, bins, lowFreqMhz, highFreqMhz, timecode);
    }
}

// ─── SpectrumRecorderWorker (writer thread) ──────────────────────────────────

bool SpectrumRecorderWorker::open(const SpectrumRecorder::Options& opts, const QString& firstPath,
                                  const SpecRec::PanState& initial, qint64 tUs, QString* err)
{
    m_opts         = opts;
    m_firstPath    = firstPath;
    m_segment      = 0;
    m_totalBytes   = 0;
    m_raw.clear();
    m_rawCount     = 0;
    m_pan          = initial;
    m_slices.clear();
    m_haveSlices   = false;
    m_firstUs      = tUs;
    m_lastFlushUs  = tUs;
    if (!openSegment(0, err)) { return false; }
    m_active = true;
    writeKeyframe(tUs);
    if (m_timerId == 0) {
        m_timerId = startTimer(std::max(50, opts.flushIntervalMs));
    }
    return true;
}

bool SpectrumRecorderWorker::openSegment(int index, QString* err)
{
    if (m_file) {
        m_file->flush();
        m_file->close();
        m_file.reset();
    }
    const QString path = SpecRec::segmentPath(m_firstPath, index);
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) { *err = tr("Cannot open %1: %2").arg(path, file->errorString()); }
        return false;
    }
    SpecRec::FileHeader h;
    h.createdUtcMs = QDateTime::currentMSecsSinceEpoch();
    h.meta = m_opts.metadata;
    h.meta.insert(QStringLiteral("app"), QStringLiteral("AetherSDR"));
    h.meta.insert(QStringLiteral("appVersion"), QCoreApplication::applicationVersion());
    h.meta.insert(QStringLiteral("segment"), index + 1);
    h.meta.insert(QStringLiteral("firstSegment"), QFileInfo(m_firstPath).fileName());
    const QByteArray hdr = SpecRec::encodeFileHeader(h);
    if (file->write(hdr) != hdr.size()) {
        if (err) { *err = file->errorString(); }
        return false;
    }
    file->flush();
    m_segmentBytes = hdr.size();
    m_totalBytes  += hdr.size();
    m_segment      = index;
    m_file         = std::move(file);
    emit workerFileOpened(path);
    return true;
}

void SpectrumRecorderWorker::writeKeyframe(qint64 tUs)
{
    SpecRec::Record k;
    k.type = SpecRec::RecordType::PanState;
    k.tUs  = tUs;
    k.pan  = m_pan;
    SpecRec::appendRecord(m_raw, k);
    ++m_rawCount;
    if (m_haveSlices) {
        SpecRec::Record s;
        s.type   = SpecRec::RecordType::SliceMarkers;
        s.tUs    = tUs;
        s.slices = m_slices;
        SpecRec::appendRecord(m_raw, s);
        ++m_rawCount;
    }
    m_chunkT0 = tUs;
    m_chunkT1 = tUs;
}

void SpectrumRecorderWorker::append(SpecRec::Record&& rec, qint64 reservedBytes)
{
    if (reservedBytes > 0) {
        m_owner->m_pendingBytes.fetch_sub(reservedBytes, std::memory_order_relaxed);
    }
    if (!m_active) { return; }

    // State records update the keyframe source; if the chunk is empty the
    // keyframe written below already carries them, so don't duplicate.
    bool isState = false;
    if (rec.type == SpecRec::RecordType::PanState) {
        m_pan = rec.pan;
        isState = true;
    } else if (rec.type == SpecRec::RecordType::SliceMarkers) {
        m_slices = rec.slices;
        m_haveSlices = true;
        isState = true;
    }

    if (m_rawCount == 0) {
        writeKeyframe(rec.tUs);
        if (isState) { return; }
    }
    SpecRec::appendRecord(m_raw, rec);
    ++m_rawCount;
    m_chunkT1 = std::max(m_chunkT1, rec.tUs);

    if (m_raw.size() >= m_opts.maxChunkRawBytes
        || (rec.tUs - m_lastFlushUs) >= static_cast<qint64>(m_opts.flushIntervalMs) * 1000) {
        flushChunk();
    }
}

void SpectrumRecorderWorker::timerEvent(QTimerEvent* ev)
{
    if (ev->timerId() == m_timerId && m_active && m_rawCount > 0) {
        flushChunk();
    }
}

void SpectrumRecorderWorker::flushChunk()
{
    if (!m_file || m_rawCount == 0) { return; }
    const QByteArray chunk = SpecRec::buildChunk(m_raw, m_chunkT0, m_chunkT1, m_rawCount,
                                                 m_opts.compressionLevel);
    const qint64 lastUs = m_chunkT1;
    m_raw.clear();
    m_rawCount = 0;
    m_lastFlushUs = lastUs;

    if (m_file->write(chunk) != chunk.size() || !m_file->flush()) {
        qCWarning(lcDsp) << "SpectrumRecorder: write failed:" << m_file->errorString();
        emit m_owner->errorOccurred(m_file->errorString());
        finish(QStringLiteral("io-error"));
        return;
    }
    m_segmentBytes += chunk.size();
    m_totalBytes   += chunk.size();
    m_owner->m_bytesWritten.store(m_totalBytes, std::memory_order_relaxed);
    emit workerStats(m_totalBytes, m_owner->m_dropped.load(std::memory_order_relaxed));

    // Limits are checked at chunk granularity (≤ flushIntervalMs overshoot).
    if (m_opts.maxTotalBytes > 0 && m_totalBytes >= m_opts.maxTotalBytes) {
        finish(QStringLiteral("size-limit"));
        return;
    }
    if (m_opts.maxDurationMs > 0 && (lastUs - m_firstUs) / 1000 >= m_opts.maxDurationMs) {
        finish(QStringLiteral("duration-limit"));
        return;
    }
    const QStorageInfo storage(QFileInfo(m_firstPath).absolutePath());
    if (storage.isValid() && storage.bytesAvailable() < m_opts.minFreeBytes) {
        finish(QStringLiteral("disk-full"));
        return;
    }
    if (m_opts.maxSegmentBytes > 0 && m_segmentBytes >= m_opts.maxSegmentBytes) {
        QString err;
        if (!openSegment(m_segment + 1, &err)) {
            emit m_owner->errorOccurred(err);
            finish(QStringLiteral("io-error"));
        }
    }
}

void SpectrumRecorderWorker::finish(const QString& reason)
{
    if (!m_active) { return; }
    // Write what we have first (finish() may be re-entered from flushChunk's
    // limit checks; m_active guards against double emission).
    if (m_rawCount > 0 && reason != QLatin1String("io-error")) {
        flushChunk();
        if (!m_active) { return; }
    }
    m_active = false;
    m_owner->m_recording.store(false, std::memory_order_release);
    if (m_timerId != 0) {
        killTimer(m_timerId);
        m_timerId = 0;
    }
    if (m_file) {
        m_file->flush();
        m_file->close();
        m_file.reset();
    }
    emit workerStopped(reason);
}

} // namespace AetherSDR
