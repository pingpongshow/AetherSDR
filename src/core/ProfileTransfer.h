#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace AetherSDR {

// ── SmartSDR profile database (.ssdr_cfg) import / export ──────────────────
//
// Protocol (verified against FLEX-6700 fw 4.1.5 / protocol v1.4.0.0 for the
// export path; import path mirrors FlexLib Radio.cs SendDBImportFile):
//
//   Listing:  C|file download db_meta_data   → R|0|<port>  (port is 42607)
//             The RADIO connects to the CLIENT on <port> and streams a
//             "meta data" text file, then closes.
//
//   Export:   C|file upload <len> db_meta_subset → R|0|<port>
//             CLIENT connects to radio:<port> and writes the selection file.
//             Wait for the radio to settle (FlexLib sleeps 5 s; requesting
//             db_package too early yields "file server busy").
//             C|file download db_package  → R|0|<port>
//             RADIO connects to CLIENT:<port> and streams the .ssdr_cfg ZIP.
//
//   Import:   C|file upload <len> db_import → R|0|<port>
//             CLIENT connects to radio:<port> and writes a ZIP containing
//             "meta_subset" (selection) + "flex_payload" (from the .ssdr_cfg).
//             THIS WRITES THE RADIO DATABASE.
//
// Because the radio connects back for downloads, export does not work over
// SmartLink/WAN or through a NAT between client and radio.
//
// Meta data / meta subset text format (one category per line, '\n'):
//   TAG^item^item^...^      e.g. GLOBAL_PROFILES^Default^SO2R^
//   TAG^                    for flag-only categories (TNFS, XVTRS, ...)
//   MEMORIES items are "owner|group", spaces encoded as 0x7F.
//   TX profiles use the tag HW_PROFILES.
//
// .ssdr_cfg package = ZIP with entries: meta_data, flex_payload (opaque,
// OpenSSL-encrypted), memories.csv.

struct DbMemoryGroup {
    QString owner;   // raw token (0x7F in place of spaces)
    QString group;   // raw token

    bool operator==(const DbMemoryGroup& o) const { return owner == o.owner && group == o.group; }
};

struct DbMetaData {
    QStringList globalProfiles;  // GLOBAL_PROFILES (raw tokens)
    QStringList txProfiles;      // HW_PROFILES
    QStringList micProfiles;     // MIC_PROFILES
    QList<DbMemoryGroup> memoryGroups;  // MEMORIES
    bool bandPersistence{false};        // BAND_PERSISTENCE
    bool modePersistence{false};        // MODE_PERSISTENCE
    bool globalPersistence{false};      // GLOBAL_PERSISTENCE
    bool globalPersistByClientId{false};// GLOBAL_PERSIST_BY_CLIENT_ID
    bool tnfs{false};                   // TNFS
    bool usbCables{false};              // USB_CABLES
    bool xvtrs{false};                  // XVTRS
    QStringList unknownLines;           // tags we don't understand, kept verbatim

    bool preferences() const
    {
        return bandPersistence || modePersistence || globalPersistence || globalPersistByClientId;
    }
    void setPreferences(bool on)
    {
        bandPersistence = modePersistence = globalPersistence = globalPersistByClientId = on;
    }
    bool isEmpty() const
    {
        return globalProfiles.isEmpty() && txProfiles.isEmpty() && micProfiles.isEmpty()
            && memoryGroups.isEmpty() && !preferences() && !tnfs && !usbCables && !xvtrs
            && unknownLines.isEmpty();
    }
};

namespace ProfileDb {

// Parse the radio's db_meta_data / a package's meta_data entry.
DbMetaData parseMetaData(const QByteArray& raw);

// Build a db_meta_subset / meta_subset file. Line order and '\n' endings
// mirror what fw 4.1.5 emits in db_meta_data (echoing that file back
// verbatim was verified to export everything).
QByteArray buildMetaSubset(const DbMetaData& selection);

// Display helpers: raw token → human-readable text (0x7F → space).
QString displayToken(const QString& raw);
QString memoryGroupLabel(const DbMemoryGroup& g);

// Parse the port from an "R<seq>|0|<port>" reply body.
std::optional<quint16> parseTransferPort(const QString& body);

// Minimal ZIP support (stored + deflate read, stored/raw-copy write) so we
// don't add a zlib/minizip dependency.
struct ZipEntry {
    QString    name;
    quint16    method{0};        // 0 = stored, 8 = deflate
    quint32    crc32{0};
    quint32    uncompressedSize{0};
    QByteArray rawData;          // data exactly as stored in the archive
};

QList<ZipEntry> readZip(const QByteArray& zip, QString* error);
QByteArray extractZipEntry(const ZipEntry& entry, QString* error);
QByteArray writeZip(const QList<ZipEntry>& entries);
ZipEntry makeStoredEntry(const QString& name, const QByteArray& data);
quint32 crc32(const QByteArray& data);
QByteArray inflateRaw(const QByteArray& deflated, qint64 maxOut, QString* error);

// Contents of a .ssdr_cfg file on disk.
struct PackageInfo {
    DbMetaData meta;
    QStringList entryNames;
    bool hasFlexPayload{false};
    int memoryCsvRows{0};
};
std::optional<PackageInfo> inspectPackage(const QByteArray& pkg, QString* error);

// Build the db_import upload: ZIP{meta_subset, flex_payload}. flex_payload is
// copied without recompression.
QByteArray buildImportPackage(const QByteArray& pkg, const DbMetaData& selection, QString* error);

} // namespace ProfileDb

// Async transfer engine. Decoupled from RadioModel through two functions so
// it can be exercised against a fake radio in unit tests.
class ProfileTransfer : public QObject {
    Q_OBJECT
public:
    using ReplyFn   = std::function<void(int code, const QString& body)>;
    using CommandFn = std::function<void(const QString& cmd, ReplyFn reply)>;
    using AddressFn = std::function<QHostAddress()>;

    struct Timings {
        int commandTimeoutMs{10000};
        int connectTimeoutMs{15000};
        int idleTimeoutMs{30000};
        int subsetSettleMs{5000};    // FlexLib: Thread.Sleep(5000) after meta subset
        int importSettleMs{5000};    // FlexLib: Thread.Sleep(5000) after db_import
        quint16 defaultDownloadPort{42607};
        quint16 defaultUploadPort{4995};
    };

    ProfileTransfer(CommandFn sendCommand, AddressFn radioAddress, QObject* parent = nullptr);
    ~ProfileTransfer() override;

    void setTimings(const Timings& t) { m_timings = t; }

    // Read-only: ask the radio what can be exported. Emits metaDataReceived.
    void fetchMetaData();
    // Export the selected items to destPath (.ssdr_cfg).
    void exportDatabase(const DbMetaData& selection, const QString& destPath);
    // Import the selected items from srcPath. Overwrites radio data.
    void importDatabase(const QString& srcPath, const DbMetaData& selection);
    void cancel();

    bool isBusy() const { return m_op != Op::None; }

signals:
    // percent < 0 means indeterminate.
    void progressChanged(int percent, const QString& status);
    void metaDataReceived(const AetherSDR::DbMetaData& meta);
    void finished(bool success, const QString& message);

private:
    enum class Op { None, FetchMeta, Export, Import };
    enum class Step { Idle, Upload, Settle, Download };

    bool begin(Op op);
    void finish(bool ok, const QString& message);
    void teardownSockets();

    void startUpload(const QByteArray& payload, const QString& kind);
    void onUploadReply(int code, const QString& body);
    void connectUpload(quint16 port);
    void sendNextChunk();
    void onUploadComplete();

    void startDownload(const QString& target);
    bool listenOn(quint16 port);
    void onDownloadReply(int code, const QString& body);
    void onDownloadConnection();
    void onDownloadComplete();

    void armTimer(int ms, const QString& message);

    CommandFn  m_sendCommand;
    AddressFn  m_radioAddress;
    Timings    m_timings;

    Op         m_op{Op::None};
    Step       m_step{Step::Idle};
    quint64    m_generation{0};

    QByteArray m_payload;       // upload buffer
    qint64     m_sent{0};
    QString    m_uploadKind;
    quint16    m_uploadPort{0};
    bool       m_triedFallback{false};

    QString    m_downloadTarget;
    QByteArray m_received;
    QString    m_destPath;

    QPointer<QTcpSocket> m_socket;
    QPointer<QTcpServer> m_server;
    QTimer*    m_timer{nullptr};
    QString    m_timeoutMessage;

    static constexpr qint64 kChunkSize = 64 * 1024;
    static constexpr qint64 kMaxDownloadBytes = 64LL * 1024 * 1024;
    static constexpr qint64 kMaxImportBytes = 64LL * 1024 * 1024;
    static constexpr quint16 kFallbackPort = 42607;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DbMetaData)
