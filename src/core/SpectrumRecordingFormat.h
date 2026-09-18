#pragma once

// AetherSDR Spectrum Recording (.aesr) — on-disk format, v1.
//
// Append-only, crash-recoverable container for panadapter FFT frames,
// waterfall rows and the pan metadata needed to re-render them.
//
//   File  := FileHeader Chunk*
//   FileHeader (little-endian, 32 bytes fixed + JSON):
//     0  char[8]  magic "AETHSPEC"
//     8  u16      format version (1)
//     10 u16      fixed header size (32)
//     12 u32      flags (0)
//     16 i64      created, UTC ms since epoch
//     24 u32      metaLen — length of the UTF-8 JSON metadata that follows
//     28 u32      CRC-32 of the JSON metadata
//     32 u8[metaLen] JSON object (app version, pan id, segment index, ...)
//   Chunk (40-byte header + payload):
//     0  u32  magic "CHNK"
//     4  u32  compLen — payload bytes (qCompress/zlib output)
//     8  u32  rawLen  — uncompressed record bytes
//     12 u32  CRC-32 of the payload
//     16 i64  t0 — first record timestamp (µs since epoch, UTC)
//     24 i64  t1 — last record timestamp
//     32 u32  record count
//     36 u32  CRC-32 of header bytes 0..35 (validates lengths before trust)
//   Record (inside the uncompressed chunk payload):
//     0  u8   type (RecordType)
//     1  u8   bin encoding (BinEncoding; 0 for non-bin records)
//     2  u16  reserved (0)
//     4  u32  bodyLen
//     8  i64  timestamp µs since epoch (UTC)
//     16 body
//
// Every chunk begins with a PanState keyframe (and SliceMarkers if known), so
// any chunk is independently decodable: seeking = pick a chunk, decode it.
// A crash can only leave a partially written *last* chunk, which the scanner
// detects (short read / CRC mismatch) and recoverFile() truncates away.

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QIODevice;

namespace AetherSDR::SpecRec {

inline constexpr char    kFileMagic[8]         = {'A', 'E', 'T', 'H', 'S', 'P', 'E', 'C'};
inline constexpr quint32 kChunkMagic           = 0x4B4E4843u;  // "CHNK" little-endian
inline constexpr quint16 kFormatVersion        = 1;
inline constexpr int     kFileHeaderFixedBytes = 32;
inline constexpr int     kChunkHeaderBytes     = 40;
inline constexpr int     kRecordHeaderBytes    = 16;
inline constexpr quint32 kMaxMetaBytes         = 1u << 20;   // 1 MiB sanity cap
inline constexpr quint32 kMaxChunkBytes        = 64u << 20;  // 64 MiB sanity cap
inline constexpr const char* kFileSuffix       = "aesr";

enum class RecordType : quint8 {
    PanState     = 1,  // f64 centerMhz, f64 bandwidthMhz, f32 minDbm, f32 maxDbm
    FftFrame     = 2,  // u32 n, bins[n]
    WaterfallRow = 3,  // f64 lowMhz, f64 highMhz, u32 timecode, u32 n, bins[n]
    SliceMarkers = 4,  // u32 n, n × {i32 id, f64 freqMhz, i32 lo, i32 hi, u8 flags, u8 0, u16 len, utf8 mode}
    Note         = 5,  // utf8 text
};

enum class BinEncoding : quint8 {
    None         = 0,
    Float32      = 1,  // lossless IEEE float, byte-plane shuffled
    Int16CentiDb = 2,  // round(v*100), horizontal delta, byte-plane shuffled (±0.005 dB)
    Int16Q7      = 3,  // v*128 exactly (waterfall tiles are int16/128 → lossless), delta, shuffled
};

struct PanState {
    double centerMhz{0.0};
    double bandwidthMhz{0.0};
    float  minDbm{-130.0f};
    float  maxDbm{-40.0f};
    bool operator==(const PanState&) const = default;
};

struct SliceMarker {
    int     sliceId{0};
    double  freqMhz{0.0};
    int     filterLowHz{0};
    int     filterHighHz{0};
    bool    active{false};
    bool    tx{false};
    QString mode;
    bool operator==(const SliceMarker&) const = default;
};

struct Record {
    RecordType           type{RecordType::Note};
    BinEncoding          encoding{BinEncoding::None};
    qint64               tUs{0};
    PanState             pan;
    QVector<float>       bins;
    double               lowMhz{0.0};
    double               highMhz{0.0};
    quint32              timecode{0};
    QVector<SliceMarker> slices;
    QString              text;
};

struct FileHeader {
    quint16     version{kFormatVersion};
    qint64      createdUtcMs{0};
    QJsonObject meta;
};

struct ChunkInfo {
    int     segment{0};   // index into Reader::segments()
    qint64  offset{0};    // file offset of the chunk header
    quint32 compLen{0};
    quint32 rawLen{0};
    quint32 crc{0};
    qint64  t0Us{0};
    qint64  t1Us{0};
    quint32 count{0};
};

struct ScanResult {
    FileHeader         header;
    qint64             dataStart{0};
    QVector<ChunkInfo> chunks;
    qint64             validEnd{0};   // offset just past the last valid chunk
    qint64             fileSize{0};
    bool               truncated{false}; // trailing bytes after validEnd
};

// ── Primitives ───────────────────────────────────────────────────────────────
quint32 crc32(const char* data, qsizetype len, quint32 seed = 0);

QByteArray encodeFileHeader(const FileHeader& h);
bool       readFileHeader(QIODevice& dev, FileHeader& h, qint64& dataStart, QString* err = nullptr);

// Serialise one record onto the end of `buf`.
void appendRecord(QByteArray& buf, const Record& r);
// Build a complete chunk (header + compressed payload) from raw record bytes.
QByteArray buildChunk(const QByteArray& raw, qint64 t0Us, qint64 t1Us, quint32 count,
                      int compressionLevel = 6);
// Decode the raw (uncompressed) payload of a chunk.
bool decodeRecords(const QByteArray& raw, QVector<Record>& out);

// Choose the densest *exact* encoding for waterfall intensity bins.
BinEncoding pickWaterfallEncoding(const QVector<float>& bins);

// ── File-level helpers ───────────────────────────────────────────────────────
bool scanFile(const QString& path, ScanResult& out, QString* err = nullptr);
// Truncate a crash-damaged file to its last valid chunk. Returns false only on
// I/O error; *bytesDropped is 0 if the file was already clean.
bool recoverFile(const QString& path, qint64* bytesDropped = nullptr, QString* err = nullptr);
bool readChunk(QIODevice& dev, const ChunkInfo& c, QVector<Record>& out, QString* err = nullptr);

// Segment discovery: "Foo.aesr" + "Foo_part002.aesr" + ... in order.
QStringList segmentPaths(const QString& firstPath);
QString     segmentPath(const QString& firstPath, int segmentIndex); // index 0 = first

// Multi-segment reader with a global chunk index (used by SpectrumPlayer and
// by the PNG exporter). Not thread-safe; use from one thread.
class Reader {
public:
    bool open(const QString& path, QString* err = nullptr);
    void close();
    bool isOpen() const { return !m_segments.isEmpty(); }

    const QStringList&        segments() const { return m_segments; }
    const QVector<ChunkInfo>& chunks() const { return m_chunks; }
    const FileHeader&         header() const { return m_header; }
    qint64 startUs() const;
    qint64 endUs() const;
    qint64 durationMs() const { return (endUs() - startUs()) / 1000; }
    qint64 totalBytes() const { return m_totalBytes; }
    bool   hadTruncation() const { return m_truncated; }

    bool readChunk(int index, QVector<Record>& out, QString* err = nullptr);
    // Index of the last chunk whose t0 <= tUs (0 if tUs precedes the file).
    int  chunkForTime(qint64 tUs) const;

    // Iterate every record in [fromUs, toUs]; callback returns false to stop.
    bool forEach(qint64 fromUs, qint64 toUs, const std::function<bool(const Record&)>& fn);

private:
    QStringList        m_segments;
    QVector<ChunkInfo> m_chunks;
    FileHeader         m_header;
    qint64             m_totalBytes{0};
    bool               m_truncated{false};
};

} // namespace AetherSDR::SpecRec
