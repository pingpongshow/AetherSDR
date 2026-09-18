#include "IqRecorder.h"
#include "LogManager.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr int    kAuxiBytes          = 164;
constexpr int    kDs64Bytes          = 28;
constexpr quint16 kFormatPcm         = 1;
constexpr quint16 kFormatFloat       = 3;
constexpr quint16 kFormatExtensible  = 0xFFFE;
constexpr qint64 kHeaderRefreshSecs  = 2;

void put16(QByteArray& b, quint16 v) { char t[2]; qToLittleEndian(v, t); b.append(t, 2); }
void put32(QByteArray& b, quint32 v) { char t[4]; qToLittleEndian(v, t); b.append(t, 4); }

void putSystemTime(QByteArray& b, const QDateTime& dtIn)
{
    if (!dtIn.isValid()) {
        b.append(QByteArray(16, '\0'));
        return;
    }
    const QDateTime dt = dtIn.toUTC();
    const QDate d = dt.date();
    const QTime t = dt.time();
    put16(b, static_cast<quint16>(d.year()));
    put16(b, static_cast<quint16>(d.month()));
    put16(b, static_cast<quint16>(d.dayOfWeek() % 7));  // SYSTEMTIME: 0 = Sunday
    put16(b, static_cast<quint16>(d.day()));
    put16(b, static_cast<quint16>(t.hour()));
    put16(b, static_cast<quint16>(t.minute()));
    put16(b, static_cast<quint16>(t.second()));
    put16(b, static_cast<quint16>(t.msec()));
}

QDateTime getSystemTime(const char* p)
{
    const int y  = qFromLittleEndian<quint16>(p);
    const int mo = qFromLittleEndian<quint16>(p + 2);
    const int d  = qFromLittleEndian<quint16>(p + 6);
    const int h  = qFromLittleEndian<quint16>(p + 8);
    const int mi = qFromLittleEndian<quint16>(p + 10);
    const int s  = qFromLittleEndian<quint16>(p + 12);
    const int ms = qFromLittleEndian<quint16>(p + 14);
    const QDate date(y, mo, d);
    const QTime time(h, mi, s, ms);
    if (!date.isValid() || !time.isValid()) { return {}; }
    return QDateTime(date, time, QTimeZone::UTC);
}

QByteArray auxiBody(const IqWavWriter::Params& p, const QDateTime& stop, const QString& next)
{
    QByteArray b;
    b.reserve(kAuxiBytes);
    putSystemTime(b, p.startUtc);
    putSystemTime(b, stop);
    put32(b, static_cast<quint32>(std::clamp<qint64>(p.centerHz, 0, 0xFFFFFFFFLL)));
    put32(b, static_cast<quint32>(p.sampleRate));  // ADFrequency
    put32(b, 0);                                   // IFFrequency
    put32(b, static_cast<quint32>(p.sampleRate));  // Bandwidth
    put32(b, 0);                                   // IQOffset
    for (int i = 0; i < 4; ++i) { put32(b, 0); }   // Unused2..Unused5
    QByteArray name = next.toLatin1().left(95);
    name.append(QByteArray(96 - name.size(), '\0'));
    b.append(name);
    return b;
}

} // namespace

// ─── IqWav helpers ───────────────────────────────────────────────────────────

QString IqWav::makeFileName(const QDateTime& utc, qint64 centerHz)
{
    return QStringLiteral("AetherSDR_%1Z_%2Hz_IQ.wav")
        .arg(utc.toUTC().toString(QStringLiteral("yyyyMMdd_HHmmss")))
        .arg(centerHz);
}

qint64 IqWav::centerFromFileName(const QString& fileName)
{
    static const QRegularExpression reHz(QStringLiteral("_(\\d{4,11})Hz(?:_|\\.|$)"),
                                         QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reKHz(QStringLiteral("_(\\d+(?:\\.\\d+)?)kHz(?:_|\\.|$)"),
                                          QRegularExpression::CaseInsensitiveOption);
    const QString base = QFileInfo(fileName).fileName();
    QRegularExpressionMatch m = reHz.match(base);
    if (m.hasMatch()) { return m.captured(1).toLongLong(); }
    m = reKHz.match(base);
    if (m.hasMatch()) { return qRound64(m.captured(1).toDouble() * 1000.0); }
    return 0;
}

bool IqWav::parse(const QString& path, IqWavInfo& info, QString* err)
{
    info = IqWavInfo{};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) { *err = f.errorString(); }
        return false;
    }
    const qint64 fileSize = f.size();
    const QByteArray head = f.read(12);
    if (head.size() < 12 || head.mid(8, 4) != "WAVE"
        || (head.left(4) != "RIFF" && head.left(4) != "RF64")) {
        if (err) { *err = QStringLiteral("not a RIFF/RF64 WAVE file"); }
        return false;
    }
    info.rf64 = head.left(4) == "RF64";
    qint64 ds64Data = -1;
    qint64 pos = 12;
    bool haveFmt = false;
    while (pos + 8 <= fileSize) {
        f.seek(pos);
        const QByteArray ch = f.read(8);
        if (ch.size() < 8) { break; }
        const QByteArray id = ch.left(4);
        qint64 size = qFromLittleEndian<quint32>(ch.constData() + 4);
        const qint64 body = pos + 8;
        if (id == "ds64" && size >= 24) {
            const QByteArray d = f.read(24);
            if (d.size() == 24) { ds64Data = qFromLittleEndian<qint64>(d.constData() + 8); }
        } else if (id == "fmt " && size >= 16) {
            const QByteArray d = f.read(std::min<qint64>(size, 40));
            info.formatTag     = qFromLittleEndian<quint16>(d.constData());
            info.channels      = qFromLittleEndian<quint16>(d.constData() + 2);
            info.sampleRate    = static_cast<int>(qFromLittleEndian<quint32>(d.constData() + 4));
            info.blockAlign    = qFromLittleEndian<quint16>(d.constData() + 12);
            info.bitsPerSample = qFromLittleEndian<quint16>(d.constData() + 14);
            if (info.formatTag == kFormatExtensible && d.size() >= 26) {
                info.formatTag = qFromLittleEndian<quint16>(d.constData() + 24);  // SubFormat GUID Data1
            }
            haveFmt = true;
        } else if (id == "auxi" && size >= 40) {
            const QByteArray d = f.read(std::min<qint64>(size, kAuxiBytes));
            info.hasAuxi  = true;
            info.startUtc = getSystemTime(d.constData());
            info.stopUtc  = getSystemTime(d.constData() + 16);
            info.centerHz = qFromLittleEndian<quint32>(d.constData() + 32);
            if (d.size() >= kAuxiBytes) {
                QByteArray nm = d.mid(68, 96);
                const qsizetype nul = nm.indexOf('\0');
                if (nul >= 0) { nm.truncate(nul); }
                info.nextFileName = QString::fromLatin1(nm);
            }
        } else if (id == "data") {
            info.dataOffset = body;
            if (size == 0xFFFFFFFFLL && ds64Data >= 0) { size = ds64Data; }
            const qint64 avail = fileSize - body;
            if (size <= 0 || size > avail) {
                // Crash-truncated or never-finalised header: trust the file length.
                info.headerSizeMismatch = true;
                size = avail;
            } else if (avail - size > 1) {
                // Trailing bytes after data: either later chunks (other
                // writers) or a stale header from a crash — flag it; the
                // caller may run repairHeader() on AetherSDR files.
                info.headerSizeMismatch = true;
            }
            info.dataBytes = size;
            break;   // data is last in files we write; stop here for others too
        }
        pos = body + size + (size & 1);
    }
    if (!haveFmt || info.dataOffset == 0 || info.blockAlign <= 0) {
        if (err) { *err = QStringLiteral("missing fmt or data chunk"); }
        return false;
    }
    if (info.centerHz == 0) {
        info.centerHz = centerFromFileName(path);
    }
    info.dataBytes -= info.dataBytes % info.blockAlign;
    info.frames = info.dataBytes / info.blockAlign;
    return true;
}

bool IqWav::repairHeader(const QString& path, QString* err)
{
    IqWavInfo info;
    if (!parse(path, info, err)) { return false; }
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) {
        if (err) { *err = f.errorString(); }
        return false;
    }
    const qint64 fileSize = f.size();
    const qint64 dataBytes = std::max<qint64>(0, fileSize - info.dataOffset);
    const qint64 riffSize = fileSize - 8;
    char b4[4];
    char b8[8];
    const bool needRf64 = riffSize > 0xFFFFFFFFLL || dataBytes > 0xFFFFFFFFLL;
    if (needRf64) {
        // Requires the JUNK/ds64 placeholder at offset 12 (all AetherSDR files have it).
        f.seek(12);
        const QByteArray id = f.read(4);
        if (id != "JUNK" && id != "ds64") {
            if (err) { *err = QStringLiteral("no ds64 placeholder; cannot promote to RF64"); }
            return false;
        }
        f.seek(0);  f.write("RF64", 4);
        qToLittleEndian<quint32>(0xFFFFFFFFu, b4); f.write(b4, 4);
        f.seek(12); f.write("ds64", 4);
        qToLittleEndian<quint32>(kDs64Bytes, b4); f.write(b4, 4);
        qToLittleEndian<quint64>(static_cast<quint64>(riffSize), b8); f.write(b8, 8);
        qToLittleEndian<quint64>(static_cast<quint64>(dataBytes), b8); f.write(b8, 8);
        qToLittleEndian<quint64>(static_cast<quint64>(dataBytes / info.blockAlign), b8); f.write(b8, 8);
        qToLittleEndian<quint32>(0, b4); f.write(b4, 4);
        f.seek(info.dataOffset - 4);
        qToLittleEndian<quint32>(0xFFFFFFFFu, b4); f.write(b4, 4);
    } else {
        f.seek(4);
        qToLittleEndian<quint32>(static_cast<quint32>(riffSize), b4); f.write(b4, 4);
        f.seek(info.dataOffset - 4);
        qToLittleEndian<quint32>(static_cast<quint32>(dataBytes), b4); f.write(b4, 4);
    }
    return f.flush();
}

bool IqWav::readFrames(QFile& f, const IqWavInfo& info, qint64 startFrame, int count,
                       std::vector<std::complex<float>>& out)
{
    out.clear();
    if (info.channels != 2 || count <= 0 || startFrame < 0 || startFrame >= info.frames) {
        return false;
    }
    const qint64 n = std::min<qint64>(count, info.frames - startFrame);
    if (!f.seek(info.dataOffset + startFrame * info.blockAlign)) { return false; }
    const QByteArray raw = f.read(n * info.blockAlign);
    const qint64 got = raw.size() / info.blockAlign;
    out.resize(static_cast<size_t>(got));
    const char* p = raw.constData();
    for (qint64 i = 0; i < got; ++i) {
        const char* s = p + i * info.blockAlign;
        float iv = 0.0f, qv = 0.0f;
        if (info.formatTag == kFormatFloat && info.bitsPerSample == 32) {
            const quint32 a = qFromLittleEndian<quint32>(s);
            const quint32 b = qFromLittleEndian<quint32>(s + 4);
            std::memcpy(&iv, &a, 4);
            std::memcpy(&qv, &b, 4);
        } else if (info.formatTag == kFormatPcm && info.bitsPerSample == 16) {
            iv = qFromLittleEndian<qint16>(s) / 32768.0f;
            qv = qFromLittleEndian<qint16>(s + 2) / 32768.0f;
        } else if (info.formatTag == kFormatPcm && info.bitsPerSample == 24) {
            auto s24 = [](const char* q) {
                qint32 v = (static_cast<quint8>(q[0])) | (static_cast<quint8>(q[1]) << 8)
                         | (static_cast<qint8>(q[2]) << 16);
                return v / 8388608.0f;
            };
            iv = s24(s);
            qv = s24(s + 3);
        } else {
            return false;
        }
        out[static_cast<size_t>(i)] = {iv, qv};
    }
    return got > 0;
}

// ─── IqWavWriter ─────────────────────────────────────────────────────────────

IqWavWriter::~IqWavWriter()
{
    if (m_file.isOpen()) { close(); }
}

bool IqWavWriter::open(const QString& path, const Params& p, QString* err)
{
    if (m_file.isOpen()) { close(); }
    m_params = p;
    if (!m_params.startUtc.isValid()) { m_params.startUtc = QDateTime::currentDateTimeUtc(); }
    m_blockAlign = (p.format == IqSampleFormat::Float32) ? 8 : 4;
    m_dataBytes = 0;
    m_frames = 0;
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) { *err = m_file.errorString(); }
        return false;
    }
    if (!writeHeader({}, {})) {
        if (err) { *err = m_file.errorString(); }
        m_file.close();
        return false;
    }
    return true;
}

bool IqWavWriter::writeHeader(const QDateTime& stopUtc, const QString& nextFileName)
{
    const bool isFloat = m_params.format == IqSampleFormat::Float32;
    const quint16 fmtSize = isFloat ? 18 : 16;
    const bool rf64 = m_dataBytes > m_rf64Threshold;

    QByteArray h;
    h.append(rf64 ? "RF64" : "RIFF", 4);
    put32(h, 0);                // RIFF size patched below
    h.append("WAVE", 4);
    h.append(rf64 ? "ds64" : "JUNK", 4);
    put32(h, kDs64Bytes);
    const qsizetype ds64At = h.size();
    h.append(QByteArray(kDs64Bytes, '\0'));
    h.append("fmt ", 4);
    put32(h, fmtSize);
    put16(h, isFloat ? kFormatFloat : kFormatPcm);
    put16(h, 2);
    put32(h, static_cast<quint32>(m_params.sampleRate));
    put32(h, static_cast<quint32>(m_params.sampleRate * m_blockAlign));
    put16(h, static_cast<quint16>(m_blockAlign));
    put16(h, isFloat ? 32 : 16);
    if (isFloat) { put16(h, 0); }  // cbSize
    h.append("auxi", 4);
    put32(h, kAuxiBytes);
    h.append(auxiBody(m_params, stopUtc, nextFileName));
    h.append("data", 4);
    put32(h, 0);
    m_dataOffset = h.size();

    const qint64 riffSize = m_dataOffset + m_dataBytes - 8;
    if (rf64) {
        qToLittleEndian<quint32>(0xFFFFFFFFu, h.data() + 4);
        qToLittleEndian<quint64>(static_cast<quint64>(riffSize), h.data() + ds64At);
        qToLittleEndian<quint64>(static_cast<quint64>(m_dataBytes), h.data() + ds64At + 8);
        qToLittleEndian<quint64>(static_cast<quint64>(m_frames), h.data() + ds64At + 16);
        qToLittleEndian<quint32>(0xFFFFFFFFu, h.data() + m_dataOffset - 4);
    } else {
        qToLittleEndian<quint32>(static_cast<quint32>(riffSize), h.data() + 4);
        qToLittleEndian<quint32>(static_cast<quint32>(m_dataBytes), h.data() + m_dataOffset - 4);
    }

    const qint64 resume = m_file.pos();
    if (!m_file.seek(0) || m_file.write(h) != h.size()) { return false; }
    if (resume > h.size() && !m_file.seek(resume)) { return false; }
    return m_file.flush();
}

bool IqWavWriter::writeFrames(const float* iq, qint64 frames)
{
    if (!m_file.isOpen() || frames <= 0) { return m_file.isOpen(); }
    const qint64 bytes = frames * m_blockAlign;
    m_convert.resize(bytes);
    char* dst = m_convert.data();
    if (m_params.format == IqSampleFormat::Float32) {
        for (qint64 i = 0; i < frames * 2; ++i) {
            quint32 u;
            std::memcpy(&u, &iq[i], 4);
            qToLittleEndian<quint32>(u, dst + i * 4);
        }
    } else {
        for (qint64 i = 0; i < frames * 2; ++i) {
            const float v = std::clamp(iq[i], -1.0f, 1.0f) * 32767.0f;
            qToLittleEndian<qint16>(static_cast<qint16>(std::lrint(v)), dst + i * 2);
        }
    }
    if (m_file.write(m_convert) != bytes) { return false; }
    m_dataBytes += bytes;
    m_frames += frames;
    return true;
}

bool IqWavWriter::updateHeader()
{
    return m_file.isOpen() && writeHeader({}, {});
}

bool IqWavWriter::close(const QString& nextFileName)
{
    if (!m_file.isOpen()) { return true; }
    const QDateTime stop = m_params.startUtc.addMSecs(
        m_params.sampleRate > 0 ? m_frames * 1000 / m_params.sampleRate : 0);
    const bool ok = writeHeader(stop, nextFileName);
    m_file.close();
    return ok;
}

// ─── IqRecorder ──────────────────────────────────────────────────────────────

IqRecorder::IqRecorder(QObject* parent)
    : QObject(parent)
{
    m_thread.setObjectName(QStringLiteral("IqRecorder"));
    m_worker = new IqRecorderWorker(this);  // no parent: moved to m_thread
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &IqRecorderWorker::workerFileOpened, this, &IqRecorder::fileOpened);
    connect(m_worker, &IqRecorderWorker::workerFileClosed, this, &IqRecorder::fileClosed);
    connect(m_worker, &IqRecorderWorker::workerStats, this, &IqRecorder::statsUpdated);
    connect(m_worker, &IqRecorderWorker::workerStopped, this, [this](const QString& reason) {
        m_recording.store(false, std::memory_order_release);
        emit stopped(reason);
    });
    // Writer thread starts lazily in start().
}

IqRecorder::~IqRecorder()
{
    stop();
    if (m_thread.isRunning()) {
        m_thread.quit();
        m_thread.wait();   // finished → m_worker->deleteLater()
    } else {
        delete m_worker;
    }
    m_worker = nullptr;
}

bool IqRecorder::start(const Options& opts, QString* err)
{
    if (isRecording()) {
        if (err) { *err = tr("Already recording"); }
        return false;
    }
    if (opts.channel < 1 || opts.channel > 4) {
        if (err) { *err = tr("Invalid DAX IQ channel %1").arg(opts.channel); }
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
    if (!m_thread.isRunning()) {
        m_thread.start();
    }
    m_pendingBytes.store(0);
    m_bytesWritten.store(0);
    m_framesWritten.store(0);
    m_dropped.store(0);
    m_maxPendingBytes.store(std::max<qint64>(opts.maxPendingBytes, 1 << 20));
    m_channel.store(opts.channel);
    Options o = opts;
    o.directory = dir.absolutePath();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, o] { w->begin(o); },
                              Qt::BlockingQueuedConnection);
    m_recording.store(true, std::memory_order_release);
    qCInfo(lcDax) << "IqRecorder: recording DAX IQ channel" << opts.channel << "to" << o.directory;
    return true;
}

void IqRecorder::stop()
{
    if (!m_recording.exchange(false, std::memory_order_acq_rel)) { return; }
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->finish(QStringLiteral("user")); },
                              Qt::BlockingQueuedConnection);
}

void IqRecorder::submitIq(int channel, const QByteArray& rawPayload, int sampleRate)
{
    if (!m_recording.load(std::memory_order_acquire)) { return; }
    if (channel != m_channel.load(std::memory_order_relaxed)) { return; }
    const qint64 cost = rawPayload.size() + 64;
    const qint64 prev = m_pendingBytes.fetch_add(cost, std::memory_order_relaxed);
    if (prev + cost > m_maxPendingBytes.load(std::memory_order_relaxed)) {
        m_pendingBytes.fetch_sub(cost, std::memory_order_relaxed);
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    QMetaObject::invokeMethod(m_worker, [w = m_worker, rawPayload, sampleRate, cost] {
        w->packet(rawPayload, sampleRate, cost);
    }, Qt::QueuedConnection);
}

void IqRecorder::setCenterFrequency(qint64 hz)
{
    QMetaObject::invokeMethod(m_worker, [w = m_worker, hz] { w->setCenter(hz); },
                              Qt::QueuedConnection);
}

// ─── IqRecorderWorker ────────────────────────────────────────────────────────

void IqRecorderWorker::begin(const IqRecorder::Options& opts)
{
    m_opts = opts;
    m_writer.reset();
    m_totalBytes = 0;
    m_totalFrames = 0;
    m_totalSeconds = 0.0;
    m_lastHeaderUpdateFrames = 0;
    m_lastStatsFrames = 0;
    m_active = true;
    // The file itself is opened on the first packet, once the rate is known.
}

bool IqRecorderWorker::openFile(int sampleRate)
{
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    const QDir dir(m_opts.directory);
    QString path = dir.absoluteFilePath(IqWav::makeFileName(utc, m_opts.centerHz));
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = dir.absoluteFilePath(IqWav::makeFileName(utc, m_opts.centerHz)
                                    .replace(QStringLiteral("_IQ.wav"),
                                             QStringLiteral("_IQ_%1.wav").arg(n)));
    }
    auto w = std::make_unique<IqWavWriter>();
    IqWavWriter::Params p;
    p.sampleRate = sampleRate;
    p.centerHz   = m_opts.centerHz;
    p.format     = m_opts.format;
    p.startUtc   = utc;
    QString err;
    if (!w->open(path, p, &err)) {
        emit m_owner->errorOccurred(err);
        return false;
    }
    m_writer = std::move(w);
    m_lastHeaderUpdateFrames = 0;
    emit workerFileOpened(path);
    return true;
}

void IqRecorderWorker::closeFile(const QString& nextName)
{
    if (!m_writer) { return; }
    const QString path = m_writer->path();
    m_totalBytes += m_writer->fileBytes();
    m_totalSeconds += static_cast<double>(m_writer->framesWritten())
                      / std::max(1, m_writer->params().sampleRate);
    m_writer->close(nextName);
    m_writer.reset();
    emit workerFileClosed(path);
}

void IqRecorderWorker::setCenter(qint64 hz)
{
    if (!m_active || hz == m_opts.centerHz) { return; }
    m_opts.centerHz = hz;
    closeFile();   // next packet opens a file stamped with the new centre
}

void IqRecorderWorker::packet(const QByteArray& payloadBE, int sampleRate, qint64 reserved)
{
    m_owner->m_pendingBytes.fetch_sub(reserved, std::memory_order_relaxed);
    if (!m_active || sampleRate <= 0) { return; }

    if (m_writer && m_writer->params().sampleRate != sampleRate) {
        closeFile();   // rate changed mid-recording → new file
    }
    if (!m_writer && !openFile(sampleRate)) {
        finish(QStringLiteral("io-error"));
        return;
    }

    // DAX IQ payload: big-endian float32, interleaved I,Q (FlexLib DAXIQStream).
    const qint64 floats = payloadBE.size() / 4;
    const qint64 frames = floats / 2;
    m_scratch.resize(static_cast<size_t>(frames * 2));
    const char* src = payloadBE.constData();
    for (qint64 i = 0; i < frames * 2; ++i) {
        const quint32 u = qFromBigEndian<quint32>(src + i * 4);
        std::memcpy(&m_scratch[static_cast<size_t>(i)], &u, 4);
    }
    if (!m_writer->writeFrames(m_scratch.data(), frames)) {
        emit m_owner->errorOccurred(QStringLiteral("IQ write failed"));
        finish(QStringLiteral("io-error"));
        return;
    }
    m_totalFrames += frames;

    const qint64 curFrames = m_writer->framesWritten();
    const qint64 bytesNow = m_totalBytes + m_writer->fileBytes();
    const double secsNow = m_totalSeconds + static_cast<double>(curFrames) / sampleRate;
    m_owner->m_bytesWritten.store(bytesNow, std::memory_order_relaxed);
    m_owner->m_framesWritten.store(m_totalFrames, std::memory_order_relaxed);

    if (curFrames - m_lastHeaderUpdateFrames >= kHeaderRefreshSecs * sampleRate) {
        m_lastHeaderUpdateFrames = curFrames;
        m_writer->updateHeader();
        emit workerStats(bytesNow, static_cast<qint64>(secsNow * 1000.0),
                         m_owner->m_dropped.load(std::memory_order_relaxed));
        const QStorageInfo storage(m_opts.directory);
        if (storage.isValid() && storage.bytesAvailable() < m_opts.minFreeBytes) {
            finish(QStringLiteral("disk-full"));
            return;
        }
    }
    if (m_opts.maxTotalBytes > 0 && bytesNow >= m_opts.maxTotalBytes) {
        finish(QStringLiteral("size-limit"));
        return;
    }
    if (m_opts.maxDurationMs > 0 && secsNow * 1000.0 >= static_cast<double>(m_opts.maxDurationMs)) {
        finish(QStringLiteral("duration-limit"));
        return;
    }
    if (m_opts.splitBytes > 0 && m_writer->fileBytes() >= m_opts.splitBytes) {
        // Chain split files via auxi.nextfilename (HDSDR/SDRuno follow it).
        const QString next = IqWav::makeFileName(
            QDateTime::currentDateTimeUtc().addSecs(1), m_opts.centerHz);
        closeFile(next);
        // Opened lazily on next packet; name may differ if the second ticks,
        // so open it now with the promised name when possible.
        const QString path = QDir(m_opts.directory).absoluteFilePath(next);
        if (!QFileInfo::exists(path)) {
            auto w = std::make_unique<IqWavWriter>();
            IqWavWriter::Params p;
            p.sampleRate = sampleRate;
            p.centerHz   = m_opts.centerHz;
            p.format     = m_opts.format;
            p.startUtc   = QDateTime::currentDateTimeUtc();
            if (w->open(path, p)) {
                m_writer = std::move(w);
                m_lastHeaderUpdateFrames = 0;
                emit workerFileOpened(path);
            }
        }
    }
}

void IqRecorderWorker::finish(const QString& reason)
{
    if (!m_active) { return; }
    m_active = false;
    closeFile();
    m_owner->m_recording.store(false, std::memory_order_release);
    emit workerStats(m_totalBytes, static_cast<qint64>(m_totalSeconds * 1000.0),
                     m_owner->m_dropped.load(std::memory_order_relaxed));
    emit workerStopped(reason);
}

} // namespace AetherSDR
