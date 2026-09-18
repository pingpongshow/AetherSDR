// Tests for the built-in logbook: ADIF codec, SQLite storage, dupe / worked
// queries, import/export, WSJT-X LoggedADIF ingest.
//
// Build: `logbook_test`.  Run: ./build/logbook_test [path/to/cty.dat]
// Exit code 0 on all pass, 1 on any failure.  Never touches the network or
// the user's real settings (QStandardPaths test mode).

#include "models/Logbook.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;
QString g_ctyPath;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

std::string s(const QString& q) { return q.toStdString(); }

LogQso makeQso(const QString& call, double mhz, const QString& mode, const QDateTime& t)
{
    LogQso q;
    q.call = call;
    q.freqMhz = mhz;
    q.band = LogUtil::freqToBand(mhz);
    q.mode = mode;
    q.timeOn = t;
    q.rstSent = "599";
    q.rstRcvd = "579";
    return q;
}

void testUtil()
{
    report("freqToBand 14.074 → 20m", LogUtil::freqToBand(14.074) == "20m");
    report("freqToBand 7.3 → 40m (edge)", LogUtil::freqToBand(7.3) == "40m");
    report("freqToBand 0.475 → 630m", LogUtil::freqToBand(0.475) == "630m");
    report("freqToBand 12.0 → empty", LogUtil::freqToBand(12.0).isEmpty());
    report("modeGroup SSB/USB → PHONE", LogUtil::modeGroup("SSB", "USB") == "PHONE");
    report("modeGroup MFSK/FT4 → DATA", LogUtil::modeGroup("MFSK", "FT4") == "DATA");
    report("modeGroup DIGITALVOICE/FREEDV → PHONE",
           LogUtil::modeGroup("DIGITALVOICE", "FREEDV") == "PHONE");
    report("baseCall W1AW/P", LogUtil::baseCall("w1aw/p") == "W1AW");
    report("baseCall VP2E/K1ABC", LogUtil::baseCall("VP2E/K1ABC") == "K1ABC");
    report("baseCall K1ABC/VP9", LogUtil::baseCall("K1ABC/VP9") == "K1ABC");

    QString m, sub;
    LogUtil::flexModeToAdif("USB", m, sub);
    report("flex USB → SSB/USB", m == "SSB" && sub == "USB");
    LogUtil::flexModeToAdif("FDV", m, sub);
    report("flex FDV → DIGITALVOICE/FREEDV", m == "DIGITALVOICE" && sub == "FREEDV");
    LogUtil::flexModeToAdif("DIGU", m, sub);
    report("flex DIGU → FT8 (default)", m == "FT8" && sub.isEmpty(), s(m + "/" + sub));

    double lat = 0, lon = 0;
    report("grid FN31pr valid", LogUtil::gridToLatLon("FN31pr", lat, lon));
    report("grid FN31pr ≈ 41.7N 72.7W",
           std::abs(lat - 41.73) < 0.05 && std::abs(lon + 72.71) < 0.05,
           std::to_string(lat) + "," + std::to_string(lon));
    double lat2 = 0, lon2 = 0;
    LogUtil::gridToLatLon("IO91wm", lat2, lon2);
    const double d = LogUtil::distanceKm(lat, lon, lat2, lon2);
    const double b = LogUtil::bearingDeg(lat, lon, lat2, lon2);
    report("FN31pr→IO91wm distance 5300..5600 km", d > 5300 && d < 5600, std::to_string(d));
    report("FN31pr→IO91wm bearing 45..60°", b > 45 && b < 60, std::to_string(b));
    report("grid 'ZZ99' invalid", !LogUtil::isValidGrid("ZZ99"));
}

void testAdifCodec()
{
    LogQso q = makeQso("DL1ABC", 14.0253, "CW",
                       QDateTime(QDate(2026, 3, 1), QTime(23, 59, 30), QTimeZone::UTC));
    q.timeOff = q.timeOn.addSecs(90);   // crosses midnight
    q.name = QString::fromUtf8("Jürgen");
    q.comment = QString::fromUtf8("Tnx <fer> QSO: 73 ☺");
    q.stationCallsign = "K1ABC";
    q.myGrid = "FN31pr";
    q.txPowerW = 100;
    q.contestId = "CQ-WW-CW";
    q.stx = 12;
    q.srx = 345;
    q.sliceId = 1;
    q.so2rRole = "RUN";
    q.recordingPath = "/tmp/rec.wav";
    q.setUploadState(UploadService::Lotw, UploadState::Uploaded);
    q.uploadDate[0] = QDate(2026, 3, 2);
    q.setUploadState(UploadService::Qrz, UploadState::Modified);
    q.extra.insert("SIG", "POTA");
    q.extra.insert("SIG_INFO", "K-1234");

    const QByteArray rec = Adif::record(q);
    report("record ends with <EOR>", rec.trimmed().endsWith("<EOR>"));
    report("NAME length counts UTF-8 bytes (7)", rec.contains("<NAME:7>"));

    const QByteArray file = Adif::header("0.0-test") + rec;
    report("header has ADIF_VER 3.1.4", file.contains("<ADIF_VER:5>3.1.4"));
    LogQso back;
    int n = 0;
    Adif::parse(file, [&](const QMap<QString, QString>& f) { back = Adif::toQso(f); ++n; return true; });
    report("round-trip: one record", n == 1);
    report("round-trip: call/freq/mode", back.call == "DL1ABC" &&
           std::abs(back.freqMhz - 14.0253) < 1e-9 && back.mode == "CW");
    report("round-trip: UTF-8 name + comment with '<'",
           back.name == q.name && back.comment == q.comment, s(back.comment));
    report("round-trip: time on/off (midnight)", back.timeOn == q.timeOn && back.timeOff == q.timeOff,
           s(back.timeOff.toString(Qt::ISODate)));
    report("round-trip: contest serials", back.stx == 12 && back.srx == 345 &&
           back.contestId == "CQ-WW-CW");
    report("round-trip: app fields", back.sliceId == 1 && back.so2rRole == "RUN" &&
           back.recordingPath == "/tmp/rec.wav");
    report("round-trip: upload status", back.uploadState(UploadService::Lotw) == UploadState::Uploaded &&
           back.uploadDate[0] == QDate(2026, 3, 2) &&
           back.uploadState(UploadService::Qrz) == UploadState::Modified);
    report("round-trip: extra fields", back.extra.value("SIG") == "POTA" &&
           back.extra.value("SIG_INFO") == "K-1234");

    // Upload-safe options drop local paths and status fields.
    Adif::WriteOptions up;
    up.includeAppFields = false;
    up.includeUploadStatus = false;
    const QByteArray upRec = Adif::record(q, up);
    report("upload record omits APP_/status fields",
           !upRec.contains("APP_AETHERSDR") && !upRec.contains("LOTW_QSL_SENT") &&
           upRec.contains("<SIG:4>POTA"));

    // Tolerance: char-count lengths, Latin-1, mixed case, type indicator, no header.
    QByteArray odd;
    odd += "<call:5>G4XYZ <Name:6>";
    odd += QString::fromUtf8("Jürgen").toUtf8();   // 6 chars, 7 bytes
    odd += " <freq:6:N>7.0123<MODE:3>SSB<submode:3>LSB<qso_date:8>20250101<time_on:4>1200";
    odd += "<QTH:7>M\xfcnchen<eor>";                  // Latin-1 ü (7 bytes)
    odd += "<CALL:4>W1AW<BAND:2>20<MODE:2>CW<QSO_DATE:8>20250102<TIME_ON:6>010203<EOR>";
    QVector<LogQso> got;
    Adif::parse(odd, [&](const QMap<QString, QString>& f) { got << Adif::toQso(f); return true; });
    report("tolerant parse: two records", got.size() == 2, std::to_string(got.size()));
    if (got.size() == 2) {
        report("char-count length handled", got[0].name == QString::fromUtf8("Jürgen") &&
               std::abs(got[0].freqMhz - 7.0123) < 1e-9, s(got[0].name));
        report("Latin-1 value decoded", got[0].qth == QString::fromUtf8("München"), s(got[0].qth));
        report("band from freq", got[0].band == "40m");
        report("bare band '20' normalised", got[1].band == "20m");
        report("TIME_ON HHMMSS", got[1].timeOn.time() == QTime(1, 2, 3));
    }

    // Malformed input must not crash
    int m = 0;
    Adif::parse("<CALL:999>X<EOR><<><:::><EOR", [&](const QMap<QString, QString>&) { ++m; return true; });
    report("malformed input survives", true, std::to_string(m));
}

void testLogbook(const QString& dir)
{
    Logbook lb;
    report("open sqlite logbook", lb.open(dir + "/test.sqlite"), s(lb.lastError()));
    if (!g_ctyPath.isEmpty()) {
        report("load cty.dat", lb.loadCtyDat(g_ctyPath));
    }
    lb.setStationCallsign("K1ABC");

    const QDateTime t0(QDate(2026, 9, 1), QTime(12, 0), QTimeZone::UTC);
    const qint64 id1 = lb.addQso(makeQso("dl1abc", 14.025, "CW", t0));
    report("addQso returns id", id1 > 0);
    bool ok = false;
    LogQso got = lb.qso(id1, &ok);
    report("stored call normalised", ok && got.call == "DL1ABC");
    if (lb.cty().isLoaded()) {
        report("DXCC filled from cty.dat", got.dxccPrefix == "DL" && got.cqZone == 14 &&
               got.continent == "EU", s(got.dxccPrefix + " " + got.country));
    }

    lb.addQso(makeQso("DL1ABC", 7.025, "CW", t0.addSecs(600)));
    lb.addQso(makeQso("G3XYZ", 14.250, "SSB", t0.addSecs(1200)));
    LogQso cq = makeQso("JA1AAA", 21.025, "CW", t0.addSecs(1800));
    cq.contestId = "CQ-WW-CW";
    cq.stx = 7;
    lb.addQso(cq);
    report("count == 4", lb.count() == 4);

    DupeRules r;
    report("dupe same band+mode", lb.isDupe("DL1ABC", "20m", "CW", r));
    report("not dupe other mode", !lb.isDupe("DL1ABC", "20m", "SSB", r));
    report("dupe ignoring mode", lb.isDupe("DL1ABC", "20m", "SSB", DupeRules{true, false, {}, {}}));
    report("not dupe other band", !lb.isDupe("DL1ABC", "15m", "CW", r));
    report("/P is a different call for dupes", !lb.isDupe("DL1ABC/P", "20m", "CW", r));
    DupeRules contest{true, true, "CQ-WW-CW", {}};
    report("contest dupe only within contest", lb.isDupe("JA1AAA", "15m", "CW", contest) &&
           !lb.isDupe("DL1ABC", "20m", "CW", contest));
    DupeRules since{true, true, {}, t0.addSecs(3600)};
    report("dupe window 'since' excludes older", !lb.isDupe("DL1ABC", "20m", "CW", since));
    report("maxSerialSent", lb.maxSerialSent("CQ-WW-CW") == 7);

    if (lb.cty().isLoaded()) {
        WorkedStatus w = lb.workedStatus("DL9ZZZ", "20m", "SSB");
        report("worked: DL entity on 20m but not SSB → NewMode",
               w.dxccStatus() == DxccStatus::NewMode && !w.callWorked);
        w = lb.workedStatus("DL1ABC", "40m", "CW");
        report("worked: call on band+mode", w.callBandModeWorked && w.callQsoCount == 2);
        report("worked: VK new DXCC", lb.workedStatus("VK2XX", "20m", "CW").dxccStatus() == DxccStatus::NewDxcc);
        report("worked: DL on 80m → NewBand", lb.dxccStatus("DL", "80m", "CW") == DxccStatus::NewBand);
    }

    // Search
    LogFilter f;
    f.text = "dl1";
    report("search text", lb.search(f).size() == 2);
    f = {};
    f.band = "20m";
    f.modeGroup = "PHONE";
    report("search band+mode", lb.search(f).size() == 1);
    f = {};
    f.text = "100%_";
    report("search escapes LIKE wildcards", lb.search(f).isEmpty());

    // Update → Modified flag for QRZ
    lb.setUploadState(id1, UploadService::Qrz, UploadState::Uploaded, QDate(2026, 9, 2));
    lb.setUploadState(id1, UploadService::Lotw, UploadState::Uploaded, QDate(2026, 9, 2));
    got = lb.qso(id1);
    got.name = "Hans";
    report("updateQso", lb.updateQso(got));
    got = lb.qso(id1);
    report("edit after upload → QRZ Modified, LoTW unchanged",
           got.uploadState(UploadService::Qrz) == UploadState::Modified &&
           got.uploadState(UploadService::Lotw) == UploadState::Uploaded && got.name == "Hans");
    report("qsosWithUploadState", lb.qsosWithUploadState(UploadService::Qrz, {UploadState::Modified}).size() == 1);

    // Recording link
    const int linked = lb.linkRecording("/tmp/a.wav", t0.addSecs(-30), t0.addSecs(60));
    report("linkRecording links QSO in window", linked == 1 && lb.qso(id1).recordingPath == "/tmp/a.wav",
           std::to_string(linked));

    // Draft from slice freq/mode
    const LogQso d = lb.draftFromFrequency(14.250, "USB", 0);
    report("draft USB → SSB/USB 20m RST 59", d.mode == "SSB" && d.submode == "USB" &&
           d.band == "20m" && d.rstSent == "59" && d.sliceId == 0 && d.stationCallsign == "K1ABC");

    // Ingest (WSJT-X style record without header)
    const QString wsjt = "<call:6>JA1XYZ <gridsquare:4>PM95 <mode:3>FT8 <rst_sent:3>-10 "
                         "<rst_rcvd:3>-15 <qso_date:8>20260901 <time_on:6>123015 "
                         "<qso_date_off:8>20260901 <time_off:6>123115 <band:3>20m "
                         "<freq:9>14.075123 <station_callsign:5>K1ABC <my_gridsquare:6>FN31pr <eor>";
    const qint64 wid = lb.ingestAdifRecord(wsjt);
    report("ingest WSJT-X record", wid > 0);
    report("ingest duplicate rejected", lb.ingestAdifRecord(wsjt) == 0);

    QByteArray dg;
    {
        QDataStream out(&dg, QIODevice::WriteOnly);
        out.setByteOrder(QDataStream::BigEndian);
        out << quint32(0xadbccbda) << quint32(3) << quint32(12)
            << QByteArray("WSJT-X") << wsjt.toUtf8();
    }
    report("wsjtxLoggedAdif decodes type 12", Logbook::wsjtxLoggedAdif(dg) == wsjt);
    QByteArray hb;
    {
        QDataStream out(&hb, QIODevice::WriteOnly);
        out << quint32(0xadbccbda) << quint32(3) << quint32(0) << QByteArray("WSJT-X");
    }
    report("wsjtxLoggedAdif ignores heartbeat", Logbook::wsjtxLoggedAdif(hb).isEmpty());

    // Delete
    report("deleteQso", lb.deleteQso(wid) && lb.count() == 4);
    report("cache updated after delete", !lb.workedStatus("JA1XYZ", "20m", "DATA").callWorked);

    // Export → import into a fresh logbook
    const QString exp = dir + "/export.adi";
    report("exportAdif", lb.exportAdif(exp));
    Logbook lb2;
    lb2.open(dir + "/test2.sqlite");
    ImportResult ir = lb2.importAdif(exp);
    report("re-import all 4", ir.added == 4 && ir.errors == 0, std::to_string(ir.added));
    ir = lb2.importAdif(exp);
    report("second import skips dupes", ir.added == 0 && ir.skippedDupes == 4);
    const LogQso r1 = lb2.search({QString(), "DL1ABC", "20m", {}, {}, {}, {}, 0}).value(0);
    report("re-imported fields preserved", r1.name == "Hans" && r1.recordingPath == "/tmp/a.wav" &&
           r1.uploadState(UploadService::Qrz) == UploadState::Modified);
}

void testLargeImport(const QString& dir)
{
    constexpr int kN = 20000;
    QByteArray big = Adif::header("test");
    const QDateTime t0(QDate(2020, 1, 1), QTime(0, 0), QTimeZone::UTC);
    for (int i = 0; i < kN; ++i) {
        LogQso q = makeQso(QStringLiteral("W%1ABC").arg(i % 10) + QString::number(i),
                           (i % 2) ? 14.074 : 7.074, (i % 3) ? "FT8" : "CW", t0.addSecs(i * 120));
        big += Adif::record(q);
    }
    const QString path = dir + "/big.adi";
    {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(big);
    }
    Logbook lb;
    lb.open(dir + "/big.sqlite");
    QElapsedTimer et;
    et.start();
    const ImportResult r = lb.importAdif(path);
    const qint64 ms = et.elapsed();
    report("sync import 20k records", r.added == kN && lb.count() == kN,
           std::to_string(r.added) + " in " + std::to_string(ms) + " ms");

    // Async import into another log — GUI thread stays free
    Logbook lb2;
    lb2.open(dir + "/big2.sqlite");
    int progressSignals = 0;
    ImportResult asyncRes;
    bool done = false;
    QObject::connect(&lb2, &Logbook::importProgress, [&](qint64, qint64, qint64) { ++progressSignals; });
    QObject::connect(&lb2, &Logbook::importFinished, [&](const ImportResult& res) {
        asyncRes = res;
        done = true;
    });
    report("async import starts", lb2.importAdifAsync(path));
    report("second async import refused while running", !lb2.importAdifAsync(path));
    int ticks = 0;
    QEventLoop loop;
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, [&]() {
        ++ticks;
        if (done || ticks > 3000) {
            loop.quit();
        }
    });
    tick.start(5);
    loop.exec();
    report("async import finished", done && asyncRes.added == kN && lb2.count() == kN,
           std::to_string(asyncRes.added) + " progress=" + std::to_string(progressSignals) +
           " ticks=" + std::to_string(ticks));
    report("async import: cache rebuilt", lb2.isDupe("W1ABC1", "20m", "DATA"));
}

} // namespace

int main(int argc, char** argv)
{
    QStandardPaths::setTestModeEnabled(true);   // never touch real settings
    QCoreApplication app(argc, argv);
#ifdef AETHER_CTY_DAT_PATH
    g_ctyPath = QStringLiteral(AETHER_CTY_DAT_PATH);
#endif
    if (argc > 1) {
        g_ctyPath = QString::fromLocal8Bit(argv[1]);
    }
    if (!g_ctyPath.isEmpty() && !QFile::exists(g_ctyPath)) {
        g_ctyPath.clear();
    }
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("cannot create temp dir\n");
        return 1;
    }
    testUtil();
    testAdifCodec();
    testLogbook(dir.path());
    testLargeImport(dir.path());
    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
