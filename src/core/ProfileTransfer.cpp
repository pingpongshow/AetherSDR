#include "ProfileTransfer.h"
#include "LogManager.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <array>
#include <cstdint>

namespace AetherSDR {

namespace ProfileDb {

// ── Meta data text ─────────────────────────────────────────────────────────

namespace {

constexpr char kGlobalProfiles[]   = "GLOBAL_PROFILES";
constexpr char kTxProfiles[]       = "HW_PROFILES";
constexpr char kMicProfiles[]      = "MIC_PROFILES";
constexpr char kMemories[]         = "MEMORIES";
constexpr char kBandPersistence[]  = "BAND_PERSISTENCE";
constexpr char kModePersistence[]  = "MODE_PERSISTENCE";
constexpr char kGlobalPersistence[] = "GLOBAL_PERSISTENCE";
constexpr char kPersistByClient[]  = "GLOBAL_PERSIST_BY_CLIENT_ID";
constexpr char kTnfs[]             = "TNFS";
constexpr char kUsbCables[]        = "USB_CABLES";
constexpr char kXvtrs[]            = "XVTRS";

// Tokens must not contain the field / line separators.
QString sanitizeToken(const QString& token)
{
    QString t = token;
    t.remove(QLatin1Char('^'));
    t.remove(QLatin1Char('\n'));
    t.remove(QLatin1Char('\r'));
    return t;
}

void appendListLine(QByteArray& out, const char* tag, const QStringList& items)
{
    QStringList clean;
    for (const QString& item : items) {
        const QString t = sanitizeToken(item);
        if (!t.isEmpty()) {
            clean.append(t);
        }
    }
    if (clean.isEmpty()) {
        return;
    }
    out += tag;
    out += '^';
    for (const QString& t : clean) {
        out += t.toUtf8();
        out += '^';
    }
    out += '\n';
}

void appendFlagLine(QByteArray& out, const char* tag, bool on)
{
    if (on) {
        out += tag;
        out += "^\n";
    }
}

} // namespace

DbMetaData parseMetaData(const QByteArray& raw)
{
    DbMetaData meta;
    const QList<QByteArray> lines = raw.split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();   // tolerate "\r\n"
        if (line.isEmpty()) {
            continue;
        }
        const QStringList parts = QString::fromUtf8(line).split(QLatin1Char('^'));
        const QString tag = parts.value(0);
        QStringList items;
        for (int i = 1; i < parts.size(); ++i) {
            if (!parts[i].isEmpty()) {
                items.append(parts[i]);
            }
        }

        if (tag == QLatin1String(kGlobalProfiles)) {
            meta.globalProfiles = items;
        } else if (tag == QLatin1String(kTxProfiles)) {
            meta.txProfiles = items;
        } else if (tag == QLatin1String(kMicProfiles)) {
            meta.micProfiles = items;
        } else if (tag == QLatin1String(kMemories)) {
            for (const QString& item : items) {
                const qsizetype bar = item.indexOf(QLatin1Char('|'));
                DbMemoryGroup g;
                if (bar < 0) {
                    g.owner = item;
                } else {
                    g.owner = item.left(bar);
                    g.group = item.mid(bar + 1);
                }
                meta.memoryGroups.append(g);
            }
        } else if (tag == QLatin1String(kBandPersistence)) {
            meta.bandPersistence = true;
        } else if (tag == QLatin1String(kModePersistence)) {
            meta.modePersistence = true;
        } else if (tag == QLatin1String(kGlobalPersistence)) {
            meta.globalPersistence = true;
        } else if (tag == QLatin1String(kPersistByClient)) {
            meta.globalPersistByClientId = true;
        } else if (tag == QLatin1String(kTnfs)) {
            meta.tnfs = true;
        } else if (tag == QLatin1String(kUsbCables)) {
            meta.usbCables = true;
        } else if (tag == QLatin1String(kXvtrs)) {
            meta.xvtrs = true;
        } else {
            meta.unknownLines.append(QString::fromUtf8(line));
        }
    }
    return meta;
}

QByteArray buildMetaSubset(const DbMetaData& sel)
{
    // Order matches db_meta_data as emitted by fw 4.1.5.
    QByteArray out;
    appendFlagLine(out, kTnfs, sel.tnfs);

    QStringList mems;
    for (const DbMemoryGroup& g : sel.memoryGroups) {
        QString owner = sanitizeToken(g.owner);
        QString group = sanitizeToken(g.group);
        // fw 4.1.5 encodes spaces inside memory tokens as DEL (0x7F).
        owner.replace(QLatin1Char(' '), QChar(0x7f));
        group.replace(QLatin1Char(' '), QChar(0x7f));
        owner.remove(QLatin1Char('|'));
        if (owner.isEmpty() && group.isEmpty()) {
            continue;
        }
        mems.append(owner + QLatin1Char('|') + group);
    }
    appendListLine(out, kMemories, mems);

    appendFlagLine(out, kBandPersistence, sel.bandPersistence);
    appendFlagLine(out, kModePersistence, sel.modePersistence);
    appendFlagLine(out, kGlobalPersistence, sel.globalPersistence);
    appendFlagLine(out, kPersistByClient, sel.globalPersistByClientId);
    appendListLine(out, kTxProfiles, sel.txProfiles);
    appendListLine(out, kMicProfiles, sel.micProfiles);
    appendListLine(out, kGlobalProfiles, sel.globalProfiles);
    appendFlagLine(out, kUsbCables, sel.usbCables);
    appendFlagLine(out, kXvtrs, sel.xvtrs);
    // Categories newer firmware may add: echo them back verbatim.
    for (const QString& line : sel.unknownLines) {
        QString clean = line;
        clean.remove(QLatin1Char('\n'));
        clean.remove(QLatin1Char('\r'));
        if (!clean.isEmpty()) {
            out += clean.toUtf8();
            out += '\n';
        }
    }
    return out;
}

QString displayToken(const QString& raw)
{
    QString s = raw;
    s.replace(QChar(0x7f), QLatin1Char(' '));
    return s;
}

QString memoryGroupLabel(const DbMemoryGroup& g)
{
    const QString owner = displayToken(g.owner);
    const QString group = displayToken(g.group);
    if (group.isEmpty()) {
        return owner + QStringLiteral(" (no group)");
    }
    return owner + QStringLiteral(" / ") + group;
}

std::optional<quint16> parseTransferPort(const QString& body)
{
    static const QRegularExpression re(QStringLiteral(R"(\b(\d{1,5})\b)"));
    const QRegularExpressionMatch m = re.match(body.trimmed());
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    bool ok = false;
    const int port = m.captured(1).toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        return std::nullopt;
    }
    return static_cast<quint16>(port);
}

// ── CRC32 ──────────────────────────────────────────────────────────────────

quint32 crc32(const QByteArray& data)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    quint32 crc = 0xFFFFFFFFU;
    for (const char ch : data) {
        crc = table[(crc ^ static_cast<quint8>(ch)) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

// ── Raw DEFLATE decoder (RFC 1951) ─────────────────────────────────────────
// Small canonical-Huffman inflater modeled on zlib's contrib/puff. Used only
// to read package metadata, so speed is not a concern.

namespace {

constexpr int kMaxBits = 15;

struct Huffman {
    std::array<quint16, kMaxBits + 1> count{};
    std::array<quint16, 320> symbol{};
};

class Inflater {
public:
    Inflater(const QByteArray& in, qint64 maxOut) : m_in(in), m_maxOut(maxOut) {}

    bool run(QByteArray* out, QString* error)
    {
        int last = 0;
        do {
            last = bits(1);
            const int type = bits(2);
            if (m_err) {
                break;
            }
            bool ok = false;
            if (type == 0) {
                ok = stored();
            } else if (type == 1) {
                ok = fixed();
            } else if (type == 2) {
                ok = dynamic();
            }
            if (!ok) {
                if (m_errText.isEmpty()) {
                    m_errText = QStringLiteral("invalid deflate block");
                }
                m_err = true;
                break;
            }
        } while (!last && !m_err);

        if (m_err) {
            if (error) {
                *error = m_errText.isEmpty() ? QStringLiteral("truncated deflate stream") : m_errText;
            }
            return false;
        }
        *out = m_out;
        return true;
    }

private:
    int bits(int need)
    {
        quint64 val = m_bitBuf;
        while (m_bitCnt < need) {
            if (m_pos >= m_in.size()) {
                m_err = true;
                return 0;
            }
            val |= static_cast<quint64>(static_cast<quint8>(m_in[m_pos++])) << m_bitCnt;
            m_bitCnt += 8;
        }
        m_bitBuf = val >> need;
        m_bitCnt -= need;
        return static_cast<int>(val & ((1ULL << need) - 1ULL));
    }

    bool put(char c)
    {
        if (m_out.size() >= m_maxOut) {
            m_errText = QStringLiteral("inflated data exceeds expected size");
            return false;
        }
        m_out.append(c);
        return true;
    }

    bool stored()
    {
        m_bitBuf = 0;
        m_bitCnt = 0;
        if (m_pos + 4 > m_in.size()) {
            return false;
        }
        const auto b = [this](qsizetype i) { return static_cast<quint8>(m_in[i]); };
        const int len = b(m_pos) | (b(m_pos + 1) << 8);
        const int nlen = b(m_pos + 2) | (b(m_pos + 3) << 8);
        m_pos += 4;
        if (len != (~nlen & 0xFFFF) || m_pos + len > m_in.size()) {
            return false;
        }
        for (int i = 0; i < len; ++i) {
            if (!put(m_in[m_pos++])) {
                return false;
            }
        }
        return true;
    }

    // Returns 0 for a complete code, >0 incomplete, <0 over-subscribed.
    static int construct(Huffman& h, const quint16* lengths, int n)
    {
        h.count.fill(0);
        for (int s = 0; s < n; ++s) {
            h.count[lengths[s]]++;
        }
        if (h.count[0] == n) {
            return 0;
        }
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) {
                return left;
            }
        }
        std::array<quint16, kMaxBits + 1> offs{};
        for (int len = 1; len < kMaxBits; ++len) {
            offs[len + 1] = static_cast<quint16>(offs[len] + h.count[len]);
        }
        for (int s = 0; s < n; ++s) {
            if (lengths[s] != 0) {
                h.symbol[offs[lengths[s]]++] = static_cast<quint16>(s);
            }
        }
        return left;
    }

    int decode(const Huffman& h)
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            if (m_err) {
                return -1;
            }
            const int count = h.count[len];
            if (code - count < first) {
                return h.symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool codes(const Huffman& lencode, const Huffman& distcode)
    {
        static constexpr std::array<quint16, 29> kLenBase = {
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::array<quint8, 29> kLenExtra = {
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::array<quint16, 30> kDistBase = {
            1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
            8193, 12289, 16385, 24577};
        static constexpr std::array<quint8, 30> kDistExtra = {
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
            7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        while (true) {
            int sym = decode(lencode);
            if (sym < 0) {
                return false;
            }
            if (sym < 256) {
                if (!put(static_cast<char>(sym))) {
                    return false;
                }
                continue;
            }
            if (sym == 256) {
                return true;
            }
            sym -= 257;
            if (sym >= 29) {
                return false;
            }
            const int len = kLenBase[sym] + bits(kLenExtra[sym]);
            const int dsym = decode(distcode);
            if (dsym < 0 || dsym >= 30 || m_err) {
                return false;
            }
            const qsizetype dist = kDistBase[dsym] + bits(kDistExtra[dsym]);
            if (m_err || dist > m_out.size()) {
                return false;
            }
            for (int i = 0; i < len; ++i) {
                if (!put(m_out[m_out.size() - dist])) {
                    return false;
                }
            }
        }
    }

    bool fixed()
    {
        std::array<quint16, 320> lengths{};
        int s = 0;
        for (; s < 144; ++s) { lengths[s] = 8; }
        for (; s < 256; ++s) { lengths[s] = 9; }
        for (; s < 280; ++s) { lengths[s] = 7; }
        for (; s < 288; ++s) { lengths[s] = 8; }
        Huffman lencode;
        construct(lencode, lengths.data(), 288);
        for (s = 0; s < 30; ++s) { lengths[s] = 5; }
        Huffman distcode;
        construct(distcode, lengths.data(), 30);
        return codes(lencode, distcode);
    }

    bool dynamic()
    {
        static constexpr std::array<quint8, 19> kOrder = {
            16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257;
        const int ndist = bits(5) + 1;
        const int ncode = bits(4) + 4;
        if (m_err || nlen > 286 || ndist > 30) {
            return false;
        }
        std::array<quint16, 320> lengths{};
        for (int i = 0; i < ncode; ++i) {
            lengths[kOrder[i]] = static_cast<quint16>(bits(3));
        }
        if (m_err) {
            return false;
        }
        Huffman lencode;
        if (construct(lencode, lengths.data(), 19) != 0) {
            return false;
        }
        lengths.fill(0);
        int index = 0;
        while (index < nlen + ndist) {
            int sym = decode(lencode);
            if (sym < 0) {
                return false;
            }
            if (sym < 16) {
                lengths[index++] = static_cast<quint16>(sym);
                continue;
            }
            quint16 len = 0;
            int repeat = 0;
            if (sym == 16) {
                if (index == 0) {
                    return false;
                }
                len = lengths[index - 1];
                repeat = 3 + bits(2);
            } else if (sym == 17) {
                repeat = 3 + bits(3);
            } else {
                repeat = 11 + bits(7);
            }
            if (m_err || index + repeat > nlen + ndist) {
                return false;
            }
            while (repeat-- > 0) {
                lengths[index++] = len;
            }
        }
        if (lengths[256] == 0) {
            return false;
        }
        Huffman litcode;
        int err = construct(litcode, lengths.data(), nlen);
        if (err < 0 || (err > 0 && nlen - litcode.count[0] != 1)) {
            return false;
        }
        Huffman distcode;
        err = construct(distcode, lengths.data() + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
            return false;
        }
        return codes(litcode, distcode);
    }

    const QByteArray& m_in;
    qint64     m_maxOut;
    qsizetype  m_pos{0};
    quint64    m_bitBuf{0};
    int        m_bitCnt{0};
    bool       m_err{false};
    QString    m_errText;
    QByteArray m_out;
};

quint16 le16(const QByteArray& b, qsizetype off)
{
    return static_cast<quint16>(static_cast<quint8>(b[off]) | (static_cast<quint8>(b[off + 1]) << 8));
}

quint32 le32(const QByteArray& b, qsizetype off)
{
    return static_cast<quint32>(le16(b, off)) | (static_cast<quint32>(le16(b, off + 2)) << 16);
}

void put16(QByteArray& b, quint16 v)
{
    b.append(static_cast<char>(v & 0xFF));
    b.append(static_cast<char>((v >> 8) & 0xFF));
}

void put32(QByteArray& b, quint32 v)
{
    put16(b, static_cast<quint16>(v & 0xFFFF));
    put16(b, static_cast<quint16>((v >> 16) & 0xFFFF));
}

void setError(QString* error, const QString& text)
{
    if (error) {
        *error = text;
    }
}

} // namespace

QByteArray inflateRaw(const QByteArray& deflated, qint64 maxOut, QString* error)
{
    Inflater inf(deflated, maxOut);
    QByteArray out;
    if (!inf.run(&out, error)) {
        return {};
    }
    return out;
}

// ── ZIP ────────────────────────────────────────────────────────────────────

QList<ZipEntry> readZip(const QByteArray& zip, QString* error)
{
    constexpr quint32 kEocdSig = 0x06054b50U;
    constexpr quint32 kCentralSig = 0x02014b50U;
    constexpr quint32 kLocalSig = 0x04034b50U;
    constexpr qsizetype kEocdSize = 22;

    QList<ZipEntry> entries;
    if (zip.size() < kEocdSize) {
        setError(error, QStringLiteral("File is too small to be a ZIP archive."));
        return entries;
    }

    qsizetype eocd = -1;
    const qsizetype lowest = qMax<qsizetype>(0, zip.size() - kEocdSize - 0xFFFF);
    for (qsizetype p = zip.size() - kEocdSize; p >= lowest; --p) {
        if (le32(zip, p) == kEocdSig) {
            eocd = p;
            break;
        }
    }
    if (eocd < 0) {
        setError(error, QStringLiteral("Not a ZIP archive (no end-of-directory record)."));
        return entries;
    }

    const int count = le16(zip, eocd + 10);
    const quint32 cdOffset = le32(zip, eocd + 16);
    if (cdOffset == 0xFFFFFFFFU || count == 0xFFFF) {
        setError(error, QStringLiteral("ZIP64 archives are not supported."));
        return entries;
    }

    // Sum of all entries' stored bytes. A well-formed archive never stores
    // more than its own size; a crafted central directory can point thousands
    // of entries at one large local record, and copying each would exhaust
    // memory.
    qsizetype totalStored = 0;
    qsizetype p = cdOffset;
    for (int i = 0; i < count; ++i) {
        if (p + 46 > zip.size() || le32(zip, p) != kCentralSig) {
            setError(error, QStringLiteral("Corrupt ZIP central directory."));
            return {};
        }
        ZipEntry e;
        e.method = le16(zip, p + 10);
        e.crc32 = le32(zip, p + 16);
        const quint32 csize = le32(zip, p + 20);
        e.uncompressedSize = le32(zip, p + 24);
        const int nameLen = le16(zip, p + 28);
        const int extraLen = le16(zip, p + 30);
        const int commentLen = le16(zip, p + 32);
        const quint32 localOff = le32(zip, p + 42);
        if (p + 46 + nameLen > zip.size()) {
            setError(error, QStringLiteral("Corrupt ZIP central directory."));
            return {};
        }
        e.name = QString::fromUtf8(zip.mid(p + 46, nameLen));
        p += 46 + nameLen + extraLen + commentLen;

        const qsizetype lp = localOff;
        if (lp + 30 > zip.size() || le32(zip, lp) != kLocalSig) {
            setError(error, QStringLiteral("Corrupt ZIP local header for %1.").arg(e.name));
            return {};
        }
        const qsizetype dataOff = lp + 30 + le16(zip, lp + 26) + le16(zip, lp + 28);
        if (dataOff + static_cast<qsizetype>(csize) > zip.size()) {
            setError(error, QStringLiteral("Truncated ZIP entry %1.").arg(e.name));
            return {};
        }
        totalStored += static_cast<qsizetype>(csize);
        if (totalStored > zip.size()) {
            setError(error, QStringLiteral("Corrupt ZIP central directory (overlapping entries)."));
            return {};
        }
        e.rawData = zip.mid(dataOff, csize);
        entries.append(e);
    }
    return entries;
}

QByteArray extractZipEntry(const ZipEntry& entry, QString* error)
{
    QByteArray data;
    if (entry.method == 0) {
        data = entry.rawData;
    } else if (entry.method == 8) {
        // uncompressedSize comes from the (untrusted) archive; refuse absurd
        // sizes up front so a zip bomb can't inflate gigabytes into memory.
        constexpr quint32 kMaxInflatedBytes = 64U * 1024U * 1024U;
        if (entry.uncompressedSize > kMaxInflatedBytes) {
            setError(error, QStringLiteral("ZIP entry %1 is too large.").arg(entry.name));
            return {};
        }
        QString err;
        data = inflateRaw(entry.rawData, entry.uncompressedSize, &err);
        if (data.isEmpty() && entry.uncompressedSize != 0) {
            setError(error, QStringLiteral("Cannot decompress %1: %2").arg(entry.name, err));
            return {};
        }
    } else {
        setError(error, QStringLiteral("Unsupported ZIP compression method %1 for %2.")
                            .arg(entry.method).arg(entry.name));
        return {};
    }
    if (static_cast<quint32>(data.size()) != entry.uncompressedSize || crc32(data) != entry.crc32) {
        setError(error, QStringLiteral("CRC mismatch in ZIP entry %1.").arg(entry.name));
        return {};
    }
    return data;
}

ZipEntry makeStoredEntry(const QString& name, const QByteArray& data)
{
    ZipEntry e;
    e.name = name;
    e.method = 0;
    e.crc32 = crc32(data);
    e.uncompressedSize = static_cast<quint32>(data.size());
    e.rawData = data;
    return e;
}

QByteArray writeZip(const QList<ZipEntry>& entries)
{
    const QDateTime now = QDateTime::currentDateTime();
    const QDate d = now.date();
    const QTime t = now.time();
    const quint16 dosTime = static_cast<quint16>((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
    const quint16 dosDate = static_cast<quint16>(((qMax(d.year(), 1980) - 1980) << 9) | (d.month() << 5) | d.day());

    QByteArray out;
    QByteArray central;
    for (const ZipEntry& e : entries) {
        const QByteArray name = e.name.toUtf8();
        const quint32 localOff = static_cast<quint32>(out.size());

        put32(out, 0x04034b50U);
        put16(out, 20);                 // version needed
        put16(out, 0);                  // flags
        put16(out, e.method);
        put16(out, dosTime);
        put16(out, dosDate);
        put32(out, e.crc32);
        put32(out, static_cast<quint32>(e.rawData.size()));
        put32(out, e.uncompressedSize);
        put16(out, static_cast<quint16>(name.size()));
        put16(out, 0);                  // extra length
        out += name;
        out += e.rawData;

        put32(central, 0x02014b50U);
        put16(central, 20);             // version made by
        put16(central, 20);             // version needed
        put16(central, 0);
        put16(central, e.method);
        put16(central, dosTime);
        put16(central, dosDate);
        put32(central, e.crc32);
        put32(central, static_cast<quint32>(e.rawData.size()));
        put32(central, e.uncompressedSize);
        put16(central, static_cast<quint16>(name.size()));
        put16(central, 0);              // extra
        put16(central, 0);              // comment
        put16(central, 0);              // disk
        put16(central, 0);              // internal attrs
        put32(central, 0);              // external attrs
        put32(central, localOff);
        central += name;
    }

    const quint32 cdOffset = static_cast<quint32>(out.size());
    out += central;
    put32(out, 0x06054b50U);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<quint16>(entries.size()));
    put16(out, static_cast<quint16>(entries.size()));
    put32(out, static_cast<quint32>(central.size()));
    put32(out, cdOffset);
    put16(out, 0);
    return out;
}

std::optional<PackageInfo> inspectPackage(const QByteArray& pkg, QString* error)
{
    const QList<ZipEntry> entries = readZip(pkg, error);
    if (entries.isEmpty()) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("The package is empty.");
        }
        return std::nullopt;
    }

    PackageInfo info;
    bool haveMeta = false;
    for (const ZipEntry& e : entries) {
        info.entryNames.append(e.name);
        if (e.name == QLatin1String("flex_payload")) {
            info.hasFlexPayload = !e.rawData.isEmpty();
        } else if (e.name == QLatin1String("meta_data") || e.name == QLatin1String("meta_subset")) {
            if (haveMeta && e.name == QLatin1String("meta_subset")) {
                continue;   // prefer meta_data when both exist
            }
            QString err;
            const QByteArray text = extractZipEntry(e, &err);
            if (!err.isEmpty()) {
                setError(error, err);
                return std::nullopt;
            }
            info.meta = parseMetaData(text);
            haveMeta = true;
        } else if (e.name == QLatin1String("memories.csv")) {
            QString ignored;
            const QByteArray csv = extractZipEntry(e, &ignored);
            info.memoryCsvRows = qMax(0, static_cast<int>(csv.count("\nMEMORY,")));
            if (csv.startsWith("MEMORY,")) {
                ++info.memoryCsvRows;
            }
        }
    }

    if (!info.hasFlexPayload) {
        setError(error, QStringLiteral("The package does not contain flex_payload; it is not a SmartSDR database export."));
        return std::nullopt;
    }
    if (!haveMeta) {
        setError(error, QStringLiteral("The package does not contain meta_data."));
        return std::nullopt;
    }
    return info;
}

QByteArray buildImportPackage(const QByteArray& pkg, const DbMetaData& selection, QString* error)
{
    const QList<ZipEntry> entries = readZip(pkg, error);
    for (const ZipEntry& e : entries) {
        if (e.name != QLatin1String("flex_payload")) {
            continue;
        }
        const QByteArray subset = buildMetaSubset(selection);
        if (subset.isEmpty()) {
            setError(error, QStringLiteral("Nothing selected to import."));
            return {};
        }
        // Layout per FlexLib-compatible clients: meta_subset tells the radio
        // which parts of flex_payload to apply. flex_payload is passed through
        // byte-for-byte (encrypted by the radio; we never decode it).
        return writeZip({makeStoredEntry(QStringLiteral("meta_subset"), subset), e});
    }
    if (error && error->isEmpty()) {
        *error = QStringLiteral("The package does not contain flex_payload.");
    }
    return {};
}

} // namespace ProfileDb

// ── ProfileTransfer ────────────────────────────────────────────────────────

ProfileTransfer::ProfileTransfer(CommandFn sendCommand, AddressFn radioAddress, QObject* parent)
    : QObject(parent)
    , m_sendCommand(std::move(sendCommand))
    , m_radioAddress(std::move(radioAddress))
{
    qRegisterMetaType<AetherSDR::DbMetaData>();
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (m_op == Op::None) {
            return;
        }
        // Upload connect timed out: retry once on the well-known port, like
        // FlexLib does when the radio's reply port can't be reached.
        if (m_step == Step::Upload && m_sent == 0 && !m_triedFallback
            && m_uploadPort != 0 && m_uploadPort != kFallbackPort) {
            connectUpload(kFallbackPort);
            return;
        }
        finish(false, m_timeoutMessage);
    });
}

ProfileTransfer::~ProfileTransfer()
{
    teardownSockets();
}

bool ProfileTransfer::begin(Op op)
{
    if (m_op != Op::None) {
        emit finished(false, QStringLiteral("A profile transfer is already in progress."));
        return false;
    }
    if (!m_sendCommand || !m_radioAddress) {
        emit finished(false, QStringLiteral("Not connected to a radio."));
        return false;
    }
    m_op = op;
    m_step = Step::Idle;
    ++m_generation;
    m_payload.clear();
    m_sent = 0;
    m_received.clear();
    m_triedFallback = false;
    m_uploadPort = 0;
    return true;
}

void ProfileTransfer::finish(bool ok, const QString& message)
{
    if (m_op == Op::None) {
        return;
    }
    m_timer->stop();
    teardownSockets();
    m_op = Op::None;
    m_step = Step::Idle;
    ++m_generation;          // invalidate any late replies / timers
    m_payload.clear();
    m_received.clear();
    if (!ok) {
        qCWarning(lcProtocol) << "ProfileTransfer:" << message;
    }
    emit finished(ok, message);
}

void ProfileTransfer::teardownSockets()
{
    if (m_socket) {
        QTcpSocket* s = m_socket;
        m_socket = nullptr;
        disconnect(s, nullptr, this, nullptr);
        s->abort();
        s->deleteLater();
    }
    if (m_server) {
        QTcpServer* srv = m_server;
        m_server = nullptr;
        disconnect(srv, nullptr, this, nullptr);
        srv->close();
        srv->deleteLater();
    }
}

void ProfileTransfer::cancel()
{
    if (m_op == Op::None) {
        return;
    }
    // Note: an import that has already been fully written to the radio cannot
    // be rolled back by cancelling.
    finish(false, QStringLiteral("Cancelled."));
}

void ProfileTransfer::armTimer(int ms, const QString& message)
{
    m_timeoutMessage = message;
    m_timer->start(ms);
}

// ── Public operations ──────────────────────────────────────────────────────

void ProfileTransfer::fetchMetaData()
{
    if (!begin(Op::FetchMeta)) {
        return;
    }
    emit progressChanged(-1, QStringLiteral("Reading radio database contents..."));
    startDownload(QStringLiteral("db_meta_data"));
}

void ProfileTransfer::exportDatabase(const DbMetaData& selection, const QString& destPath)
{
    const QByteArray subset = ProfileDb::buildMetaSubset(selection);
    if (subset.isEmpty()) {
        emit finished(false, QStringLiteral("Select at least one item to export."));
        return;
    }
    if (destPath.trimmed().isEmpty()) {
        emit finished(false, QStringLiteral("Choose a destination file."));
        return;
    }
    if (!begin(Op::Export)) {
        return;
    }
    m_destPath = destPath;
    qCInfo(lcProtocol) << "ProfileTransfer: export, meta_subset bytes" << subset.size();
    emit progressChanged(0, QStringLiteral("Sending export selection..."));
    startUpload(subset, QStringLiteral("db_meta_subset"));
}

void ProfileTransfer::importDatabase(const QString& srcPath, const DbMetaData& selection)
{
    QFile f(srcPath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit finished(false, QStringLiteral("Cannot open %1: %2").arg(srcPath, f.errorString()));
        return;
    }
    if (f.size() > kMaxImportBytes) {
        emit finished(false, QStringLiteral("The selected file is too large to be a SmartSDR export."));
        return;
    }
    const QByteArray pkg = f.readAll();
    QString error;
    const QByteArray upload = ProfileDb::buildImportPackage(pkg, selection, &error);
    if (upload.isEmpty()) {
        emit finished(false, error);
        return;
    }
    if (!begin(Op::Import)) {
        return;
    }
    qCInfo(lcProtocol) << "ProfileTransfer: import, upload bytes" << upload.size();
    emit progressChanged(0, QStringLiteral("Uploading database to radio..."));
    startUpload(upload, QStringLiteral("db_import"));
}

// ── Upload (client connects to radio) ──────────────────────────────────────

void ProfileTransfer::startUpload(const QByteArray& payload, const QString& kind)
{
    m_step = Step::Upload;
    m_payload = payload;
    m_sent = 0;
    m_uploadKind = kind;
    m_triedFallback = false;
    m_uploadPort = 0;

    const quint64 gen = m_generation;
    QPointer<ProfileTransfer> self(this);
    armTimer(m_timings.commandTimeoutMs,
             QStringLiteral("Radio did not answer the file upload request."));
    m_sendCommand(QStringLiteral("file upload %1 %2").arg(payload.size()).arg(kind),
                  [self, gen](int code, const QString& body) {
                      if (self && self->m_generation == gen) {
                          self->onUploadReply(code, body);
                      }
                  });
}

void ProfileTransfer::onUploadReply(int code, const QString& body)
{
    m_timer->stop();
    if (code != 0) {
        finish(false, QStringLiteral("Radio rejected %1 upload (error 0x%2 %3).")
                          .arg(m_uploadKind).arg(code, 0, 16).arg(body.trimmed()));
        return;
    }
    const quint16 port = ProfileDb::parseTransferPort(body).value_or(m_timings.defaultUploadPort);
    // FlexLib waits 200 ms for the radio's file server to start listening.
    const quint64 gen = m_generation;
    QTimer::singleShot(200, this, [this, gen, port] {
        if (m_generation == gen) {
            connectUpload(port);
        }
    });
}

void ProfileTransfer::connectUpload(quint16 port)
{
    if (m_uploadPort != 0) {
        m_triedFallback = true;   // any second attempt is the fallback
    }
    teardownSockets();
    m_uploadPort = port;
    m_sent = 0;

    auto* sock = new QTcpSocket(this);
    m_socket = sock;
    connect(sock, &QTcpSocket::connected, this, [this, sock] {
        if (m_socket != sock) {
            return;
        }
        armTimer(m_timings.idleTimeoutMs, QStringLiteral("Upload stalled."));
        sendNextChunk();
    });
    connect(sock, &QTcpSocket::bytesWritten, this, [this, sock](qint64 n) {
        if (m_socket != sock) {
            return;
        }
        m_sent += n;
        armTimer(m_timings.idleTimeoutMs, QStringLiteral("Upload stalled."));
        const int pct = m_payload.isEmpty() ? 100 : static_cast<int>(m_sent * 100 / m_payload.size());
        emit progressChanged(m_op == Op::Export ? pct / 10 : pct,
                             QStringLiteral("Uploading %1 (%2 / %3 bytes)...")
                                 .arg(m_uploadKind).arg(m_sent).arg(m_payload.size()));
        if (m_sent >= m_payload.size()) {
            onUploadComplete();
        } else if (sock->bytesToWrite() == 0) {
            sendNextChunk();
        }
    });
    connect(sock, &QTcpSocket::errorOccurred, this, [this, sock](QAbstractSocket::SocketError) {
        if (m_socket != sock) {
            return;
        }
        if (m_sent == 0 && !m_triedFallback && m_uploadPort != kFallbackPort) {
            qCWarning(lcProtocol) << "ProfileTransfer: upload port" << m_uploadPort
                                  << "failed, trying" << kFallbackPort;
            connectUpload(kFallbackPort);
            return;
        }
        finish(false, QStringLiteral("Upload failed: %1").arg(sock->errorString()));
    });

    qCInfo(lcProtocol) << "ProfileTransfer: connecting to radio port" << port;
    emit progressChanged(0, QStringLiteral("Connecting to radio port %1...").arg(port));
    armTimer(m_timings.connectTimeoutMs,
             QStringLiteral("Cannot connect to the radio's file transfer port."));
    sock->connectToHost(m_radioAddress(), port);
}

void ProfileTransfer::sendNextChunk()
{
    if (!m_socket) {
        return;
    }
    const qint64 queued = m_sent + m_socket->bytesToWrite();
    const qint64 remaining = m_payload.size() - queued;
    if (remaining <= 0) {
        return;
    }
    const qint64 n = qMin(kChunkSize, remaining);
    if (m_socket->write(m_payload.constData() + queued, n) < 0) {
        finish(false, QStringLiteral("Upload write failed: %1").arg(m_socket->errorString()));
    }
}

void ProfileTransfer::onUploadComplete()
{
    m_timer->stop();
    if (m_socket) {
        QTcpSocket* s = m_socket;
        m_socket = nullptr;
        disconnect(s, nullptr, this, nullptr);
        s->disconnectFromHost();   // graceful close: radio sees EOF
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        QTimer::singleShot(5000, s, &QObject::deleteLater);
    }
    qCInfo(lcProtocol) << "ProfileTransfer: uploaded" << m_sent << "bytes as" << m_uploadKind;
    m_payload.clear();
    m_step = Step::Settle;

    const quint64 gen = m_generation;
    if (m_op == Op::Export) {
        // fw 4.1.5 needs time to stage the package after receiving the
        // selection; FlexLib sleeps 5 s here.
        emit progressChanged(10, QStringLiteral("Radio is preparing the export package..."));
        QTimer::singleShot(m_timings.subsetSettleMs, this, [this, gen] {
            if (m_generation == gen) {
                startDownload(QStringLiteral("db_package"));
            }
        });
    } else if (m_op == Op::Import) {
        emit progressChanged(100, QStringLiteral("Upload complete; radio is applying the database..."));
        QTimer::singleShot(m_timings.importSettleMs, this, [this, gen] {
            if (m_generation == gen) {
                finish(true, QStringLiteral("Import sent to the radio. Profiles and settings will refresh from the radio."));
            }
        });
    }
}

// ── Download (radio connects to client) ────────────────────────────────────

bool ProfileTransfer::listenOn(quint16 port)
{
    if (m_server && m_server->isListening() && m_server->serverPort() == port) {
        return true;
    }
    if (m_server) {
        QTcpServer* old = m_server;
        m_server = nullptr;
        disconnect(old, nullptr, this, nullptr);
        old->close();
        old->deleteLater();
    }
    auto* srv = new QTcpServer(this);
    if (!srv->listen(QHostAddress::Any, port)) {
        qCWarning(lcProtocol) << "ProfileTransfer: cannot listen on port" << port << srv->errorString();
        srv->deleteLater();
        return false;
    }
    connect(srv, &QTcpServer::newConnection, this, &ProfileTransfer::onDownloadConnection);
    m_server = srv;
    return true;
}

void ProfileTransfer::startDownload(const QString& target)
{
    m_step = Step::Download;
    m_downloadTarget = target;
    m_received.clear();

    // The radio connects back right after replying. Pre-listen on the port
    // fw 4.1.5 always hands out (42607) so a fast connect is never refused;
    // re-bind in onDownloadReply if the radio names a different port.
    listenOn(m_timings.defaultDownloadPort);

    const quint64 gen = m_generation;
    QPointer<ProfileTransfer> self(this);
    armTimer(m_timings.commandTimeoutMs,
             QStringLiteral("Radio did not answer the file download request."));
    m_sendCommand(QStringLiteral("file download %1").arg(target),
                  [self, gen](int code, const QString& body) {
                      if (self && self->m_generation == gen) {
                          self->onDownloadReply(code, body);
                      }
                  });
}

void ProfileTransfer::onDownloadReply(int code, const QString& body)
{
    m_timer->stop();
    if (code != 0) {
        finish(false, QStringLiteral("Radio rejected %1 download (error 0x%2 %3).")
                          .arg(m_downloadTarget).arg(code, 0, 16).arg(body.trimmed()));
        return;
    }
    if (m_socket) {
        // Radio already connected to the pre-listening server.
        armTimer(m_timings.idleTimeoutMs, QStringLiteral("Download stalled."));
        return;
    }
    const quint16 port = ProfileDb::parseTransferPort(body).value_or(m_timings.defaultDownloadPort);
    if (!listenOn(port)) {
        finish(false, QStringLiteral("Cannot listen on port %1 for the radio's download "
                                     "connection (is another transfer running?).").arg(port));
        return;
    }
    qCInfo(lcProtocol) << "ProfileTransfer: waiting for radio on port" << port
                       << "for" << m_downloadTarget;
    emit progressChanged(m_op == Op::Export ? 20 : -1,
                         QStringLiteral("Waiting for radio to connect on port %1...").arg(port));
    armTimer(m_timings.connectTimeoutMs,
             QStringLiteral("The radio never connected back to download %1. Check that no "
                            "firewall, VPN or NAT sits between this computer and the radio.")
                 .arg(m_downloadTarget));
}

void ProfileTransfer::onDownloadConnection()
{
    if (!m_server) {
        return;
    }
    QTcpSocket* sock = m_server->nextPendingConnection();
    if (!sock || m_socket || m_step != Step::Download) {
        if (sock) {
            sock->abort();
            sock->deleteLater();
        }
        return;
    }
    sock->setParent(this);
    m_socket = sock;
    m_server->close();

    armTimer(m_timings.idleTimeoutMs, QStringLiteral("Download stalled."));
    qCInfo(lcProtocol) << "ProfileTransfer: radio connected from" << sock->peerAddress().toString();

    connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
        if (m_socket != sock) {
            return;
        }
        m_received += sock->readAll();
        if (m_received.size() > kMaxDownloadBytes) {
            finish(false, QStringLiteral("Download exceeds the size limit."));
            return;
        }
        armTimer(m_timings.idleTimeoutMs, QStringLiteral("Download stalled."));
        emit progressChanged(m_op == Op::Export ? 50 : -1,
                             QStringLiteral("Receiving %1 (%2 KB)...")
                                 .arg(m_downloadTarget).arg(m_received.size() / 1024));
    });
    connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
        if (m_socket == sock) {
            onDownloadComplete();
        }
    });
    connect(sock, &QTcpSocket::errorOccurred, this, [this, sock](QAbstractSocket::SocketError err) {
        if (m_socket != sock) {
            return;
        }
        if (err == QAbstractSocket::RemoteHostClosedError) {
            onDownloadComplete();
            return;
        }
        finish(false, QStringLiteral("Download failed: %1").arg(sock->errorString()));
    });
    if (sock->state() != QAbstractSocket::ConnectedState) {
        onDownloadComplete();
    }
}

void ProfileTransfer::onDownloadComplete()
{
    if (m_step != Step::Download || m_op == Op::None) {
        return;
    }
    m_timer->stop();
    if (m_socket) {
        m_received += m_socket->readAll();
    }
    m_step = Step::Idle;

    if (m_received.isEmpty()) {
        finish(false, QStringLiteral("The radio sent an empty %1.").arg(m_downloadTarget));
        return;
    }
    qCInfo(lcProtocol) << "ProfileTransfer: received" << m_received.size() << "bytes of" << m_downloadTarget;

    if (m_op == Op::FetchMeta) {
        const DbMetaData meta = ProfileDb::parseMetaData(m_received);
        emit metaDataReceived(meta);
        finish(true, QStringLiteral("Radio database contents loaded."));
        return;
    }

    if (m_op == Op::Export) {
        if (!m_received.startsWith("PK")) {
            finish(false, QStringLiteral("The radio's export is not a ZIP package."));
            return;
        }
        QSaveFile out(m_destPath);
        if (!out.open(QIODevice::WriteOnly) || out.write(m_received) != m_received.size()
            || !out.commit()) {
            finish(false, QStringLiteral("Cannot write %1: %2").arg(m_destPath, out.errorString()));
            return;
        }
        emit progressChanged(100, QStringLiteral("Export complete."));
        finish(true, QStringLiteral("Exported %1 KB to %2")
                         .arg(m_received.size() / 1024)
                         .arg(QFileInfo(m_destPath).fileName()));
    }
}

} // namespace AetherSDR
