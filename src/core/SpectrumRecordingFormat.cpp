#include "SpectrumRecordingFormat.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace AetherSDR::SpecRec {

namespace {

// ── CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — same as zlib's crc32() ──
constexpr std::array<quint32, 256> makeCrcTable()
{
    std::array<quint32, 256> t{};
    for (quint32 i = 0; i < 256; ++i) {
        quint32 c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        t[i] = c;
    }
    return t;
}
constexpr std::array<quint32, 256> kCrcTable = makeCrcTable();

// ── Little-endian byte writer / reader ──────────────────────────────────────
class Writer {
public:
    explicit Writer(QByteArray& b) : m_b(b) {}
    template <typename T> void put(T v)
    {
        char tmp[sizeof(T)];
        qToLittleEndian<T>(v, tmp);
        m_b.append(tmp, sizeof(T));
    }
    void putF32(float f) { quint32 u; std::memcpy(&u, &f, 4); put<quint32>(u); }
    void putF64(double d) { quint64 u; std::memcpy(&u, &d, 8); put<quint64>(u); }
    void putBytes(const QByteArray& a) { m_b.append(a); }
private:
    QByteArray& m_b;
};

class Reader8 {
public:
    Reader8(const char* p, qsizetype n) : m_p(p), m_n(n) {}
    bool has(qsizetype k) const { return m_pos + k <= m_n; }
    template <typename T> bool get(T& v)
    {
        if (!has(sizeof(T))) { return false; }
        v = qFromLittleEndian<T>(m_p + m_pos);
        m_pos += sizeof(T);
        return true;
    }
    bool getF32(float& f) { quint32 u; if (!get(u)) { return false; } std::memcpy(&f, &u, 4); return true; }
    bool getF64(double& d) { quint64 u; if (!get(u)) { return false; } std::memcpy(&d, &u, 8); return true; }
    bool getBytes(qsizetype k, QByteArray& out)
    {
        if (k < 0 || !has(k)) { return false; }
        out = QByteArray(m_p + m_pos, k);
        m_pos += k;
        return true;
    }
    qsizetype pos() const { return m_pos; }
    void skip(qsizetype k) { m_pos += k; }
private:
    const char* m_p;
    qsizetype   m_n;
    qsizetype   m_pos{0};
};

// Byte-plane shuffle: [a0 a1 a2 a3][b0 b1 b2 b3] → [a0 b0][a1 b1][a2 b2][a3 b3].
// Groups the slowly-varying high bytes together, which roughly doubles zlib's
// ratio on float/int16 spectra (same trick as Blosc/HDF5 "shuffle").
QByteArray shuffle(const char* src, int count, int width)
{
    QByteArray out(static_cast<qsizetype>(count) * width, Qt::Uninitialized);
    char* dst = out.data();
    for (int b = 0; b < width; ++b) {
        for (int i = 0; i < count; ++i) {
            dst[static_cast<qsizetype>(b) * count + i] = src[static_cast<qsizetype>(i) * width + b];
        }
    }
    return out;
}

void unshuffle(const char* src, int count, int width, char* dst)
{
    for (int b = 0; b < width; ++b) {
        for (int i = 0; i < count; ++i) {
            dst[static_cast<qsizetype>(i) * width + b] = src[static_cast<qsizetype>(b) * count + i];
        }
    }
}

int encodedWidth(BinEncoding e)
{
    switch (e) {
    case BinEncoding::Float32:      return 4;
    case BinEncoding::Int16CentiDb: return 2;
    case BinEncoding::Int16Q7:      return 2;
    case BinEncoding::None:         return 0;
    }
    return 0;
}

void encodeBins(Writer& w, const QVector<float>& bins, BinEncoding enc)
{
    const int n = static_cast<int>(bins.size());
    w.put<quint32>(static_cast<quint32>(n));
    if (n == 0) { return; }
    if (enc == BinEncoding::Float32) {
        QByteArray le(static_cast<qsizetype>(n) * 4, Qt::Uninitialized);
        for (int i = 0; i < n; ++i) {
            quint32 u;
            std::memcpy(&u, &bins[i], 4);
            qToLittleEndian<quint32>(u, le.data() + static_cast<qsizetype>(i) * 4);
        }
        w.putBytes(shuffle(le.constData(), n, 4));
        return;
    }
    const float scale = (enc == BinEncoding::Int16Q7) ? 128.0f : 100.0f;
    QByteArray le(static_cast<qsizetype>(n) * 2, Qt::Uninitialized);
    qint16 prev = 0;
    for (int i = 0; i < n; ++i) {
        const float  scaled = std::clamp(std::round(bins[i] * scale), -32768.0f, 32767.0f);
        const qint16 v      = static_cast<qint16>(scaled);
        // Horizontal delta (wrapping int16 arithmetic, exactly reversible).
        const quint16 d = static_cast<quint16>(static_cast<quint16>(v) - static_cast<quint16>(prev));
        prev = v;
        qToLittleEndian<quint16>(d, le.data() + static_cast<qsizetype>(i) * 2);
    }
    w.putBytes(shuffle(le.constData(), n, 2));
}

bool decodeBins(Reader8& r, BinEncoding enc, QVector<float>& out)
{
    quint32 n = 0;
    if (!r.get(n)) { return false; }
    const int width = encodedWidth(enc);
    if (width == 0 || n > (1u << 20)) { return false; }
    QByteArray planes;
    if (!r.getBytes(static_cast<qsizetype>(n) * width, planes)) { return false; }
    QByteArray le(planes.size(), Qt::Uninitialized);
    unshuffle(planes.constData(), static_cast<int>(n), width, le.data());
    out.resize(static_cast<qsizetype>(n));
    if (enc == BinEncoding::Float32) {
        for (quint32 i = 0; i < n; ++i) {
            const quint32 u = qFromLittleEndian<quint32>(le.constData() + static_cast<qsizetype>(i) * 4);
            std::memcpy(&out[i], &u, 4);
        }
        return true;
    }
    const float inv = (enc == BinEncoding::Int16Q7) ? (1.0f / 128.0f) : (1.0f / 100.0f);
    quint16 acc = 0;
    for (quint32 i = 0; i < n; ++i) {
        acc = static_cast<quint16>(acc + qFromLittleEndian<quint16>(le.constData() + static_cast<qsizetype>(i) * 2));
        out[i] = static_cast<float>(static_cast<qint16>(acc)) * inv;
    }
    return true;
}

bool readExact(QIODevice& dev, char* dst, qint64 n)
{
    qint64 got = 0;
    while (got < n) {
        const qint64 k = dev.read(dst + got, n - got);
        if (k <= 0) { return false; }
        got += k;
    }
    return true;
}

bool parseChunkHeader(const char* h, ChunkInfo& c)
{
    if (qFromLittleEndian<quint32>(h) != kChunkMagic) { return false; }
    if (qFromLittleEndian<quint32>(h + 36) != crc32(h, 36)) { return false; }
    c.compLen = qFromLittleEndian<quint32>(h + 4);
    c.rawLen  = qFromLittleEndian<quint32>(h + 8);
    c.crc     = qFromLittleEndian<quint32>(h + 12);
    c.t0Us    = qFromLittleEndian<qint64>(h + 16);
    c.t1Us    = qFromLittleEndian<qint64>(h + 24);
    c.count   = qFromLittleEndian<quint32>(h + 32);
    return c.compLen > 0 && c.compLen <= kMaxChunkBytes && c.rawLen <= kMaxChunkBytes;
}

} // namespace

quint32 crc32(const char* data, qsizetype len, quint32 seed)
{
    quint32 c = seed ^ 0xFFFFFFFFu;
    for (qsizetype i = 0; i < len; ++i) {
        c = kCrcTable[(c ^ static_cast<quint8>(data[i])) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

QByteArray encodeFileHeader(const FileHeader& h)
{
    const QByteArray meta = QJsonDocument(h.meta).toJson(QJsonDocument::Compact);
    QByteArray out;
    out.reserve(kFileHeaderFixedBytes + meta.size());
    out.append(kFileMagic, 8);
    Writer w(out);
    w.put<quint16>(h.version);
    w.put<quint16>(static_cast<quint16>(kFileHeaderFixedBytes));
    w.put<quint32>(0);
    w.put<qint64>(h.createdUtcMs);
    w.put<quint32>(static_cast<quint32>(meta.size()));
    w.put<quint32>(crc32(meta.constData(), meta.size()));
    out.append(meta);
    return out;
}

bool readFileHeader(QIODevice& dev, FileHeader& h, qint64& dataStart, QString* err)
{
    char fixed[kFileHeaderFixedBytes];
    if (!dev.seek(0) || !readExact(dev, fixed, kFileHeaderFixedBytes)) {
        if (err) { *err = QStringLiteral("file too short for header"); }
        return false;
    }
    if (std::memcmp(fixed, kFileMagic, 8) != 0) {
        if (err) { *err = QStringLiteral("not an AetherSDR spectrum recording"); }
        return false;
    }
    h.version = qFromLittleEndian<quint16>(fixed + 8);
    const quint16 fixedSize = qFromLittleEndian<quint16>(fixed + 10);
    if (h.version == 0 || h.version > kFormatVersion || fixedSize < kFileHeaderFixedBytes) {
        if (err) { *err = QStringLiteral("unsupported format version %1").arg(h.version); }
        return false;
    }
    h.createdUtcMs = qFromLittleEndian<qint64>(fixed + 16);
    const quint32 metaLen = qFromLittleEndian<quint32>(fixed + 24);
    const quint32 metaCrc = qFromLittleEndian<quint32>(fixed + 28);
    if (metaLen > kMaxMetaBytes) {
        if (err) { *err = QStringLiteral("corrupt header (metadata length)"); }
        return false;
    }
    QByteArray meta(metaLen, Qt::Uninitialized);
    if (!dev.seek(fixedSize) || !readExact(dev, meta.data(), metaLen)
        || crc32(meta.constData(), meta.size()) != metaCrc) {
        if (err) { *err = QStringLiteral("corrupt header (metadata)"); }
        return false;
    }
    h.meta    = QJsonDocument::fromJson(meta).object();
    dataStart = fixedSize + static_cast<qint64>(metaLen);
    return true;
}

void appendRecord(QByteArray& buf, const Record& r)
{
    QByteArray body;
    Writer bw(body);
    BinEncoding enc = BinEncoding::None;
    switch (r.type) {
    case RecordType::PanState:
        bw.putF64(r.pan.centerMhz);
        bw.putF64(r.pan.bandwidthMhz);
        bw.putF32(r.pan.minDbm);
        bw.putF32(r.pan.maxDbm);
        break;
    case RecordType::FftFrame:
        enc = (r.encoding == BinEncoding::None) ? BinEncoding::Float32 : r.encoding;
        encodeBins(bw, r.bins, enc);
        break;
    case RecordType::WaterfallRow:
        enc = (r.encoding == BinEncoding::None) ? pickWaterfallEncoding(r.bins) : r.encoding;
        bw.putF64(r.lowMhz);
        bw.putF64(r.highMhz);
        bw.put<quint32>(r.timecode);
        encodeBins(bw, r.bins, enc);
        break;
    case RecordType::SliceMarkers:
        bw.put<quint32>(static_cast<quint32>(r.slices.size()));
        for (const SliceMarker& s : r.slices) {
            const QByteArray mode = s.mode.left(32).toUtf8();
            bw.put<qint32>(s.sliceId);
            bw.putF64(s.freqMhz);
            bw.put<qint32>(s.filterLowHz);
            bw.put<qint32>(s.filterHighHz);
            bw.put<quint8>(static_cast<quint8>((s.active ? 1 : 0) | (s.tx ? 2 : 0)));
            bw.put<quint8>(0);
            bw.put<quint16>(static_cast<quint16>(mode.size()));
            bw.putBytes(mode);
        }
        break;
    case RecordType::Note:
        bw.putBytes(r.text.toUtf8());
        break;
    }

    Writer w(buf);
    w.put<quint8>(static_cast<quint8>(r.type));
    w.put<quint8>(static_cast<quint8>(enc));
    w.put<quint16>(0);
    w.put<quint32>(static_cast<quint32>(body.size()));
    w.put<qint64>(r.tUs);
    w.putBytes(body);
}

QByteArray buildChunk(const QByteArray& raw, qint64 t0Us, qint64 t1Us, quint32 count,
                      int compressionLevel)
{
    const QByteArray payload = qCompress(raw, compressionLevel);
    QByteArray out;
    out.reserve(kChunkHeaderBytes + payload.size());
    Writer w(out);
    w.put<quint32>(kChunkMagic);
    w.put<quint32>(static_cast<quint32>(payload.size()));
    w.put<quint32>(static_cast<quint32>(raw.size()));
    w.put<quint32>(crc32(payload.constData(), payload.size()));
    w.put<qint64>(t0Us);
    w.put<qint64>(t1Us);
    w.put<quint32>(count);
    w.put<quint32>(crc32(out.constData(), 36));
    out.append(payload);
    return out;
}

bool decodeRecords(const QByteArray& raw, QVector<Record>& out)
{
    Reader8 r(raw.constData(), raw.size());
    while (r.has(kRecordHeaderBytes)) {
        quint8 type = 0, enc = 0;
        quint16 reserved = 0;
        quint32 bodyLen = 0;
        Record rec;
        r.get(type);
        r.get(enc);
        r.get(reserved);
        r.get(bodyLen);
        r.get(rec.tUs);
        if (!r.has(bodyLen)) { return false; }
        const qsizetype bodyStart = r.pos();
        Reader8 b(raw.constData() + bodyStart, bodyLen);
        rec.type     = static_cast<RecordType>(type);
        rec.encoding = static_cast<BinEncoding>(enc);
        bool ok = true;
        switch (rec.type) {
        case RecordType::PanState:
            ok = b.getF64(rec.pan.centerMhz) && b.getF64(rec.pan.bandwidthMhz)
              && b.getF32(rec.pan.minDbm) && b.getF32(rec.pan.maxDbm);
            break;
        case RecordType::FftFrame:
            ok = decodeBins(b, rec.encoding, rec.bins);
            break;
        case RecordType::WaterfallRow:
            ok = b.getF64(rec.lowMhz) && b.getF64(rec.highMhz) && b.get(rec.timecode)
              && decodeBins(b, rec.encoding, rec.bins);
            break;
        case RecordType::SliceMarkers: {
            quint32 n = 0;
            ok = b.get(n) && n <= 64;
            for (quint32 i = 0; ok && i < n; ++i) {
                SliceMarker s;
                qint32 id = 0, lo = 0, hi = 0;
                quint8 flags = 0, pad = 0;
                quint16 len = 0;
                QByteArray mode;
                ok = b.get(id) && b.getF64(s.freqMhz) && b.get(lo) && b.get(hi)
                  && b.get(flags) && b.get(pad) && b.get(len) && b.getBytes(len, mode);
                s.sliceId = id; s.filterLowHz = lo; s.filterHighHz = hi;
                s.active = (flags & 1) != 0; s.tx = (flags & 2) != 0;
                s.mode = QString::fromUtf8(mode);
                rec.slices.append(s);
            }
            break;
        }
        case RecordType::Note: {
            QByteArray t;
            ok = b.getBytes(bodyLen, t);
            rec.text = QString::fromUtf8(t);
            break;
        }
        default:
            // Unknown record type from a newer writer — skip it (forward compat).
            r.skip(bodyLen);
            continue;
        }
        if (!ok) { return false; }
        r.skip(bodyLen);
        out.append(std::move(rec));
    }
    return r.pos() == raw.size();
}

BinEncoding pickWaterfallEncoding(const QVector<float>& bins)
{
    for (const float v : bins) {
        const float s = v * 128.0f;
        if (!(s >= -32768.0f && s <= 32767.0f) || std::nearbyint(s) != s) {
            return BinEncoding::Float32;
        }
    }
    return BinEncoding::Int16Q7;
}

bool scanFile(const QString& path, ScanResult& out, QString* err)
{
    out = ScanResult{};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) { *err = f.errorString(); }
        return false;
    }
    out.fileSize = f.size();
    if (!readFileHeader(f, out.header, out.dataStart, err)) { return false; }

    qint64 pos = out.dataStart;
    char hdr[kChunkHeaderBytes];
    while (pos + kChunkHeaderBytes <= out.fileSize) {
        if (!f.seek(pos) || !readExact(f, hdr, kChunkHeaderBytes)) { break; }
        ChunkInfo c;
        if (!parseChunkHeader(hdr, c)) { break; }
        if (pos + kChunkHeaderBytes + c.compLen > out.fileSize) { break; }
        c.offset = pos;
        out.chunks.append(c);
        pos += kChunkHeaderBytes + c.compLen;
    }
    // Only the payload CRC of the *last* chunk is verified during scan — it is
    // the only one a crash can have torn. Earlier chunks are verified lazily
    // by readChunk() so opening a multi-GB file stays O(chunks) not O(bytes).
    while (!out.chunks.isEmpty()) {
        const ChunkInfo& last = out.chunks.constLast();
        QByteArray payload(last.compLen, Qt::Uninitialized);
        if (f.seek(last.offset + kChunkHeaderBytes)
            && readExact(f, payload.data(), last.compLen)
            && crc32(payload.constData(), payload.size()) == last.crc) {
            break;
        }
        out.chunks.removeLast();
    }
    out.validEnd = out.chunks.isEmpty()
        ? out.dataStart
        : out.chunks.constLast().offset + kChunkHeaderBytes + out.chunks.constLast().compLen;
    out.truncated = out.validEnd < out.fileSize;
    return true;
}

bool recoverFile(const QString& path, qint64* bytesDropped, QString* err)
{
    ScanResult s;
    if (!scanFile(path, s, err)) { return false; }
    if (bytesDropped) { *bytesDropped = s.fileSize - s.validEnd; }
    if (!s.truncated) { return true; }
    QFile f(path);
    if (!f.resize(s.validEnd)) {
        if (err) { *err = f.errorString(); }
        return false;
    }
    return true;
}

bool readChunk(QIODevice& dev, const ChunkInfo& c, QVector<Record>& out, QString* err)
{
    QByteArray payload(c.compLen, Qt::Uninitialized);
    if (!dev.seek(c.offset + kChunkHeaderBytes) || !readExact(dev, payload.data(), c.compLen)) {
        if (err) { *err = QStringLiteral("short read"); }
        return false;
    }
    if (crc32(payload.constData(), payload.size()) != c.crc) {
        if (err) { *err = QStringLiteral("chunk CRC mismatch"); }
        return false;
    }
    const QByteArray raw = qUncompress(payload);
    if (raw.size() != static_cast<qsizetype>(c.rawLen)) {
        if (err) { *err = QStringLiteral("decompression failed"); }
        return false;
    }
    if (!decodeRecords(raw, out)) {
        if (err) { *err = QStringLiteral("malformed records"); }
        return false;
    }
    return true;
}

QString segmentPath(const QString& firstPath, int segmentIndex)
{
    if (segmentIndex <= 0) { return firstPath; }
    const QFileInfo fi(firstPath);
    return fi.dir().filePath(QStringLiteral("%1_part%2.%3")
        .arg(fi.completeBaseName())
        .arg(segmentIndex + 1, 3, 10, QLatin1Char('0'))
        .arg(fi.suffix()));
}

QStringList segmentPaths(const QString& firstPath)
{
    QStringList out{firstPath};
    for (int i = 1; i < 10000; ++i) {
        const QString p = segmentPath(firstPath, i);
        if (!QFileInfo::exists(p)) { break; }
        out.append(p);
    }
    return out;
}

// ── Reader ───────────────────────────────────────────────────────────────────

bool Reader::open(const QString& path, QString* err)
{
    close();
    const QStringList paths = segmentPaths(path);
    for (int i = 0; i < paths.size(); ++i) {
        ScanResult s;
        if (!scanFile(paths[i], s, err)) {
            if (i == 0) { close(); return false; }
            break;  // a broken later segment just ends the recording there
        }
        if (i == 0) { m_header = s.header; }
        m_segments.append(paths[i]);
        for (ChunkInfo c : s.chunks) {
            c.segment = i;
            m_chunks.append(c);
        }
        m_totalBytes += s.fileSize;
        m_truncated = m_truncated || s.truncated;
    }
    return true;
}

void Reader::close()
{
    m_segments.clear();
    m_chunks.clear();
    m_header = FileHeader{};
    m_totalBytes = 0;
    m_truncated = false;
}

qint64 Reader::startUs() const
{
    return m_chunks.isEmpty() ? 0 : m_chunks.constFirst().t0Us;
}

qint64 Reader::endUs() const
{
    qint64 e = startUs();
    for (const ChunkInfo& c : m_chunks) {
        e = std::max(e, c.t1Us);
    }
    return e;
}

bool Reader::readChunk(int index, QVector<Record>& out, QString* err)
{
    if (index < 0 || index >= m_chunks.size()) { return false; }
    const ChunkInfo& c = m_chunks[index];
    QFile f(m_segments.value(c.segment));
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) { *err = f.errorString(); }
        return false;
    }
    return SpecRec::readChunk(f, c, out, err);
}

int Reader::chunkForTime(qint64 tUs) const
{
    int best = 0;
    for (int i = 0; i < m_chunks.size(); ++i) {
        if (m_chunks[i].t0Us <= tUs) { best = i; }
        else { break; }
    }
    return best;
}

bool Reader::forEach(qint64 fromUs, qint64 toUs, const std::function<bool(const Record&)>& fn)
{
    for (int i = chunkForTime(fromUs); i < m_chunks.size(); ++i) {
        if (m_chunks[i].t0Us > toUs) { break; }
        QVector<Record> recs;
        if (!readChunk(i, recs)) { continue; }
        for (const Record& r : recs) {
            if (r.tUs < fromUs || r.tUs > toUs) { continue; }
            if (!fn(r)) { return true; }
        }
    }
    return true;
}

} // namespace AetherSDR::SpecRec
