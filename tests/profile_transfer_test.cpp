// Tests for SmartSDR profile database import/export (ProfileTransfer).
//
// Covers the meta data / meta subset text format, the dependency-free ZIP and
// DEFLATE code, and the full export/import/listing flows against a local fake
// radio (no real radio is contacted; import is only ever tested here).
//
// Build: produced by CMake as `profile_transfer_test`.
// Run:   ./build/profile_transfer_test [optional/path/to/real.ssdr_cfg]
//
// Exit code 0 on all pass, 1 on any failure.

#include "core/ProfileTransfer.h"

#include <QCoreApplication>
#include <QFile>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

// Captured verbatim from `file download db_meta_data` on a FLEX-6700,
// fw 4.1.5 (protocol 1.4.0.0). Note the 0x7F in "FELD Hell".
const QByteArray kRadioMetaData =
    "TNFS^\n"
    "MEMORIES^N0CALL|^FLEX-6700|^FLEX-6700|?^FLEX-6700|AVIATION^FLEX-6700|HFFAX^"
    "FLEX-6700|FELD\x7fHell^FLEX-6700|PSK31^FLEX-6700|Olivia^FLEX-6700|SSTV^\n"
    "BAND_PERSISTENCE^\n"
    "MODE_PERSISTENCE^\n"
    "GLOBAL_PERSISTENCE^\n"
    "GLOBAL_PERSIST_BY_CLIENT_ID^\n"
    "HW_PROFILES^iOS_default_Profile^iOS_FT8^macOS_APRS^macOS_default_Profile^"
    "macOS_FT8^macOS_PSK^macOS_RTTY^macOS_SSTV^\n"
    "MIC_PROFILES^iOS_default_Profile^macOS_default_Profile^\n"
    "GLOBAL_PROFILES^iOS_default_Profile^macOS_default_Profile^SO2RDefault^\n"
    "USB_CABLES^\n"
    "XVTRS^\n";

// qCompress = 4-byte BE length + zlib header(2) + raw deflate + adler32(4).
QByteArray rawDeflate(const QByteArray& data, int level)
{
    const QByteArray z = qCompress(data, level);
    return z.mid(6, z.size() - 10);
}

void testMetaData()
{
    const DbMetaData meta = ProfileDb::parseMetaData(kRadioMetaData);
    check(meta.globalProfiles.size() == 3, "meta: 3 global profiles");
    check(meta.txProfiles.size() == 8, "meta: 8 TX (HW) profiles");
    check(meta.micProfiles.size() == 2, "meta: 2 MIC profiles");
    check(meta.memoryGroups.size() == 9, "meta: 9 memory groups");
    check(meta.memoryGroups[0].owner == "N0CALL" && meta.memoryGroups[0].group.isEmpty(),
          "meta: owner-only memory group");
    check(ProfileDb::displayToken(meta.memoryGroups[5].group) == "FELD Hell",
          "meta: 0x7F decodes to space");
    check(meta.preferences() && meta.globalPersistByClientId, "meta: preferences flags");
    check(meta.tnfs && meta.usbCables && meta.xvtrs, "meta: TNF/USB/XVTR flags");
    check(meta.unknownLines.isEmpty(), "meta: no unknown lines");

    check(ProfileDb::buildMetaSubset(meta) == kRadioMetaData,
          "subset: full selection round-trips byte-exact to radio listing");

    DbMetaData sel;
    sel.globalProfiles = {"SO2RDefault"};
    sel.memoryGroups = {{"FLEX-6700", "FELD Hell"}};
    sel.xvtrs = true;
    check(ProfileDb::buildMetaSubset(sel)
              == QByteArray("MEMORIES^FLEX-6700|FELD\x7fHell^\nGLOBAL_PROFILES^SO2RDefault^\nXVTRS^\n"),
          "subset: partial selection, spaces in memory tokens encoded as 0x7F");

    DbMetaData evil;
    evil.globalProfiles = {"a^b\nc", ""};
    check(ProfileDb::buildMetaSubset(evil) == QByteArray("GLOBAL_PROFILES^abc^\n"),
          "subset: separators stripped from tokens, empty tokens dropped");
    check(ProfileDb::buildMetaSubset(DbMetaData{}).isEmpty(), "subset: empty selection is empty");

    const DbMetaData crlf = ProfileDb::parseMetaData("GLOBAL_PROFILES^A^B^\r\nFOO_NEW^x^\r\n");
    check(crlf.globalProfiles == QStringList({"A", "B"}), "meta: tolerates CRLF");
    check(crlf.unknownLines == QStringList({"FOO_NEW^x^"}), "meta: unknown tag preserved");
}

void testPortsAndCrc()
{
    check(ProfileDb::parseTransferPort("42607").value_or(0) == 42607, "port: bare");
    check(ProfileDb::parseTransferPort(" 4995\n").value_or(0) == 4995, "port: whitespace");
    check(!ProfileDb::parseTransferPort("").has_value(), "port: empty");
    check(!ProfileDb::parseTransferPort("99999").has_value(), "port: out of range");
    check(ProfileDb::crc32("123456789") == 0xCBF43926U, "crc32: check value");
}

void testInflate()
{
    QByteArray text;
    for (int i = 0; i < 2000; ++i) {
        text += "MEMORY,N0CALL,,7.075900,,LSB,100,SIMPLEX,0.000000,OFF,67.0," + QByteArray::number(i) + "\n";
    }
    QByteArray random(70000, '\0');
    for (char& c : random) {
        c = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }

    QString err;
    check(ProfileDb::inflateRaw(rawDeflate(text, 9), text.size(), &err) == text,
          "inflate: dynamic Huffman text");
    check(ProfileDb::inflateRaw(rawDeflate(random, 6), random.size(), &err) == random,
          "inflate: random data (stored blocks)");
    check(ProfileDb::inflateRaw(rawDeflate(text, 0), text.size(), &err) == text,
          "inflate: level 0 stored");
    check(ProfileDb::inflateRaw(rawDeflate("abc", 9), 3, &err) == "abc", "inflate: tiny (fixed Huffman)");

    err.clear();
    const QByteArray d = rawDeflate(text, 9);
    check(ProfileDb::inflateRaw(d.left(d.size() / 2), text.size(), &err).isEmpty() && !err.isEmpty(),
          "inflate: truncated stream rejected");
    err.clear();
    check(ProfileDb::inflateRaw(d, 100, &err).isEmpty() && !err.isEmpty(),
          "inflate: output size cap enforced");
}

QByteArray makeFakePackage(const QByteArray& flexPayload)
{
    ProfileDb::ZipEntry meta;
    meta.name = "meta_data";
    meta.method = 8;
    meta.crc32 = ProfileDb::crc32(kRadioMetaData);
    meta.uncompressedSize = static_cast<quint32>(kRadioMetaData.size());
    meta.rawData = rawDeflate(kRadioMetaData, 9);

    ProfileDb::ZipEntry payload;
    payload.name = "flex_payload";
    payload.method = 8;
    payload.crc32 = ProfileDb::crc32(flexPayload);
    payload.uncompressedSize = static_cast<quint32>(flexPayload.size());
    payload.rawData = rawDeflate(flexPayload, 1);

    const QByteArray csv = ", OWNER, GROUP\nMEMORY,a\nMEMORY,b\n";
    return ProfileDb::writeZip({meta, payload, ProfileDb::makeStoredEntry("memories.csv", csv)});
}

void testZip()
{
    const QByteArray flex = QByteArray("Salted__") + QByteArray(5000, 'x');
    const QByteArray pkg = makeFakePackage(flex);

    QString err;
    const QList<ProfileDb::ZipEntry> entries = ProfileDb::readZip(pkg, &err);
    check(entries.size() == 3 && err.isEmpty(), "zip: read 3 entries");
    check(ProfileDb::extractZipEntry(entries.value(1), &err) == flex, "zip: deflated entry extracts");

    const auto info = ProfileDb::inspectPackage(pkg, &err);
    check(info.has_value() && info->hasFlexPayload, "package: inspect ok");
    check(info && info->meta.txProfiles.size() == 8, "package: meta_data parsed");
    check(info && info->memoryCsvRows == 2, "package: memories.csv rows counted");

    ProfileDb::ZipEntry corrupt = entries.value(1);
    corrupt.crc32 ^= 1U;
    err.clear();
    check(ProfileDb::extractZipEntry(corrupt, &err).isEmpty() && !err.isEmpty(),
          "zip: CRC mismatch detected");

    err.clear();
    check(!ProfileDb::inspectPackage("not a zip", &err).has_value() && !err.isEmpty(),
          "package: garbage rejected");
    const QByteArray noPayload = ProfileDb::writeZip({ProfileDb::makeStoredEntry("meta_data", kRadioMetaData)});
    err.clear();
    check(!ProfileDb::inspectPackage(noPayload, &err).has_value(), "package: missing flex_payload rejected");

    DbMetaData sel;
    sel.micProfiles = {"macOS_default_Profile"};
    const QByteArray upload = ProfileDb::buildImportPackage(pkg, sel, &err);
    const QList<ProfileDb::ZipEntry> up = ProfileDb::readZip(upload, &err);
    check(up.size() == 2 && up.value(0).name == "meta_subset" && up.value(1).name == "flex_payload",
          "import package: {meta_subset, flex_payload}");
    check(ProfileDb::extractZipEntry(up.value(0), &err) == "MIC_PROFILES^macOS_default_Profile^\n",
          "import package: meta_subset reflects selection");
    check(up.value(1).rawData == entries.value(1).rawData && up.value(1).method == 8,
          "import package: flex_payload passed through without recompression");
    check(ProfileDb::extractZipEntry(up.value(1), &err) == flex, "import package: flex_payload intact");

    // Malformed: many central-directory records aliasing one large local
    // entry must be rejected instead of copying the data N times.
    {
        const QByteArray big(20000, 'z');
        const QByteArray one = ProfileDb::writeZip({ProfileDb::makeStoredEntry("flex_payload", big)});
        const qsizetype eocd = one.size() - 22;
        const qsizetype cdStart = 30 + 12 + big.size();
        const QByteArray cdRec = one.mid(cdStart, eocd - cdStart);
        QByteArray bomb = one.left(cdStart);
        const int copies = 50;
        for (int i = 0; i < copies; ++i) {
            bomb += cdRec;
        }
        QByteArray tail = one.mid(eocd);
        tail[8] = static_cast<char>(copies);
        tail[10] = static_cast<char>(copies);
        const quint32 cdSize = static_cast<quint32>(cdRec.size() * copies);
        tail[12] = static_cast<char>(cdSize & 0xFF);
        tail[13] = static_cast<char>((cdSize >> 8) & 0xFF);
        bomb += tail;
        err.clear();
        check(ProfileDb::readZip(bomb, &err).isEmpty() && !err.isEmpty(),
              "zip: overlapping central-directory entries rejected");
    }

    // Malformed: deflated entry claiming a multi-GB uncompressed size.
    {
        ProfileDb::ZipEntry huge = entries.value(1);
        huge.uncompressedSize = 0xF0000000U;
        err.clear();
        check(ProfileDb::extractZipEntry(huge, &err).isEmpty() && !err.isEmpty(),
              "zip: oversize uncompressed size rejected");
    }
}

// ── Fake radio ─────────────────────────────────────────────────────────────

struct FakeRadio {
    QTcpServer uploadServer;              // radio side for "file upload"
    QByteArray lastUploadKind;
    QByteArray uploaded;
    QStringList commands;
    QByteArray downloadBody;              // what the radio sends on "file download"
    quint16 downloadPort{0};
    int rejectCode{0};

    ProfileTransfer::CommandFn commandFn()
    {
        return [this](const QString& cmd, ProfileTransfer::ReplyFn reply) {
            commands << cmd;
            const QStringList w = cmd.split(' ');
            if (rejectCode != 0) {
                QTimer::singleShot(0, [reply, code = rejectCode] { reply(code, {}); });
                return;
            }
            if (w.value(0) == "file" && w.value(1) == "upload") {
                lastUploadKind = w.value(3).toUtf8();
                uploaded.clear();
                QTimer::singleShot(0, [this, reply] {
                    reply(0, QString::number(uploadServer.serverPort()));
                });
            } else if (w.value(0) == "file" && w.value(1) == "download") {
                QTimer::singleShot(0, [this, reply] {
                    reply(0, QString::number(downloadPort));
                    // Radio connects back to the client and streams the file.
                    QTimer::singleShot(50, [this] {
                        auto* s = new QTcpSocket;
                        QObject::connect(s, &QTcpSocket::connected, [this, s] {
                            s->write(downloadBody);
                            s->disconnectFromHost();
                        });
                        QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                        s->connectToHost(QHostAddress::LocalHost, downloadPort);
                    });
                });
            } else {
                QTimer::singleShot(0, [reply] { reply(0x50000016, {}); });
            }
        };
    }

    FakeRadio()
    {
        uploadServer.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&uploadServer, &QTcpServer::newConnection, [this] {
            QTcpSocket* s = uploadServer.nextPendingConnection();
            QObject::connect(s, &QTcpSocket::readyRead, [this, s] { uploaded += s->readAll(); });
            QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        });
        QTcpServer probe;
        probe.listen(QHostAddress::LocalHost, 0);
        downloadPort = probe.serverPort();
        probe.close();
    }
};

ProfileTransfer::Timings fastTimings(quint16 downloadPort)
{
    ProfileTransfer::Timings t;
    t.commandTimeoutMs = 2000;
    t.connectTimeoutMs = 2000;
    t.idleTimeoutMs = 2000;
    t.subsetSettleMs = 50;
    t.importSettleMs = 50;
    t.defaultDownloadPort = downloadPort;
    return t;
}

void testFetchMeta()
{
    FakeRadio radio;
    radio.downloadBody = kRadioMetaData;
    ProfileTransfer xfer(radio.commandFn(), [] { return QHostAddress(QHostAddress::LocalHost); });
    xfer.setTimings(fastTimings(radio.downloadPort));
    QSignalSpy metaSpy(&xfer, &ProfileTransfer::metaDataReceived);
    QSignalSpy doneSpy(&xfer, &ProfileTransfer::finished);
    xfer.fetchMetaData();
    doneSpy.wait(5000);
    check(doneSpy.size() == 1 && doneSpy.at(0).at(0).toBool(), "fake fetch: finished ok");
    check(radio.commands == QStringList({"file download db_meta_data"}), "fake fetch: command sequence");
    check(metaSpy.size() == 1
              && metaSpy.at(0).at(0).value<DbMetaData>().globalProfiles.size() == 3,
          "fake fetch: meta data delivered");
}

void testExport(const QString& dir)
{
    FakeRadio radio;
    radio.downloadBody = makeFakePackage("Salted__payload");
    ProfileTransfer xfer(radio.commandFn(), [] { return QHostAddress(QHostAddress::LocalHost); });
    xfer.setTimings(fastTimings(radio.downloadPort));
    QSignalSpy doneSpy(&xfer, &ProfileTransfer::finished);

    const QString path = dir + "/out.ssdr_cfg";
    xfer.exportDatabase(ProfileDb::parseMetaData(kRadioMetaData), path);
    doneSpy.wait(5000);
    check(doneSpy.size() == 1 && doneSpy.at(0).at(0).toBool(), "fake export: finished ok");
    check(radio.commands.size() == 2
              && radio.commands.value(0) == QString("file upload %1 db_meta_subset").arg(kRadioMetaData.size())
              && radio.commands.value(1) == "file download db_package",
          "fake export: upload db_meta_subset then download db_package");
    check(radio.lastUploadKind == "db_meta_subset" && radio.uploaded == kRadioMetaData,
          "fake export: meta subset bytes delivered");
    QFile f(path);
    check(f.open(QIODevice::ReadOnly) && f.readAll() == radio.downloadBody, "fake export: file written");

    // Rejected command surfaces as failure, not a hang.
    radio.rejectCode = 0x50001000;
    doneSpy.clear();
    xfer.exportDatabase(ProfileDb::parseMetaData(kRadioMetaData), dir + "/rej.ssdr_cfg");
    doneSpy.wait(3000);
    check(doneSpy.size() == 1 && !doneSpy.at(0).at(0).toBool(), "fake export: radio rejection reported");
    check(!xfer.isBusy(), "fake export: idle after failure");
}

void testImport(const QString& dir)
{
    FakeRadio radio;
    const QByteArray flex = QByteArray("Salted__") + QByteArray(300000, 'q');
    const QString src = dir + "/in.ssdr_cfg";
    {
        QFile f(src);
        f.open(QIODevice::WriteOnly);
        f.write(makeFakePackage(flex));
    }
    ProfileTransfer xfer(radio.commandFn(), [] { return QHostAddress(QHostAddress::LocalHost); });
    xfer.setTimings(fastTimings(radio.downloadPort));
    QSignalSpy doneSpy(&xfer, &ProfileTransfer::finished);

    DbMetaData sel;
    sel.globalProfiles = {"SO2RDefault"};
    sel.tnfs = true;
    xfer.importDatabase(src, sel);
    doneSpy.wait(5000);
    check(doneSpy.size() == 1 && doneSpy.at(0).at(0).toBool(), "fake import: finished ok");
    check(radio.commands.size() == 1 && radio.commands.value(0).endsWith(" db_import"),
          "fake import: single file upload ... db_import");
    QString err;
    const QList<ProfileDb::ZipEntry> up = ProfileDb::readZip(radio.uploaded, &err);
    check(radio.commands.value(0) == QString("file upload %1 db_import").arg(radio.uploaded.size()),
          "fake import: announced size matches bytes sent");
    check(up.size() == 2 && ProfileDb::extractZipEntry(up.value(0), &err) == "TNFS^\nGLOBAL_PROFILES^SO2RDefault^\n",
          "fake import: meta_subset matches selection");
    check(ProfileDb::extractZipEntry(up.value(1), &err) == flex, "fake import: flex_payload intact");

    // Cancel mid-flight leaves the engine idle.
    doneSpy.clear();
    xfer.importDatabase(src, sel);
    xfer.cancel();
    check(doneSpy.size() == 1 && !doneSpy.at(0).at(0).toBool() && !xfer.isBusy(), "fake import: cancel");

    // Invalid file never reaches the radio.
    radio.commands.clear();
    doneSpy.clear();
    xfer.importDatabase(dir + "/does-not-exist.ssdr_cfg", sel);
    check(doneSpy.size() == 1 && !doneSpy.at(0).at(0).toBool() && radio.commands.isEmpty(),
          "fake import: missing file rejected before any command");
}

void testRealPackage(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        check(false, "real package: open");
        return;
    }
    QString err;
    const auto info = ProfileDb::inspectPackage(f.readAll(), &err);
    check(info.has_value(), "real package: inspect");
    if (info) {
        std::printf("      entries=%s global=%lld tx=%lld mic=%lld memGroups=%lld csvRows=%d\n",
                    qPrintable(info->entryNames.join(',')),
                    static_cast<long long>(info->meta.globalProfiles.size()),
                    static_cast<long long>(info->meta.txProfiles.size()),
                    static_cast<long long>(info->meta.micProfiles.size()),
                    static_cast<long long>(info->meta.memoryGroups.size()),
                    info->memoryCsvRows);
    } else {
        std::printf("      error: %s\n", qPrintable(err));
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;

    testMetaData();
    testPortsAndCrc();
    testInflate();
    testZip();
    testFetchMeta();
    testExport(tmp.path());
    testImport(tmp.path());
    if (argc > 1) {
        testRealPackage(QString::fromLocal8Bit(argv[1]));
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
