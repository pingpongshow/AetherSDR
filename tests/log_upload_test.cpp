// Tests for LogUploaders: request formatting, response parsing, retry /
// suspend behaviour, and the LoTW TQSL batch path.
//
// NO real services are contacted: every HTTP request goes to a local
// QTcpServer on 127.0.0.1, and "tqsl" is a shell script written to a temp
// directory.  Credentials use the in-memory backend.
//
// Build: `log_upload_test`.  Run: ./build/log_upload_test

#include "core/LogUploaders.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <cstdio>
#include <functional>
#include <string>

using namespace AetherSDR;
using Outcome = UploadApi::Result::Outcome;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

std::string s(const QString& q) { return q.toStdString(); }

bool waitFor(const std::function<bool()>& pred, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return pred();
}

QMap<QString, QString> decodeForm(const QByteArray& body)
{
    QMap<QString, QString> m;
    for (const QByteArray& part : body.split('&')) {
        const int eq = part.indexOf('=');
        if (eq < 0) {
            continue;
        }
        m.insert(QUrl::fromPercentEncoding(part.left(eq)),
                 QUrl::fromPercentEncoding(part.mid(eq + 1)));
    }
    return m;
}

// Minimal HTTP/1.1 server: records requests, replies from a script.
class FakeHttpServer : public QObject {
public:
    struct Request {
        QByteArray method;
        QByteArray path;
        QMap<QByteArray, QByteArray> headers;
        QByteArray body;
    };
    struct Reply {
        int status{200};
        QByteArray body;
    };

    FakeHttpServer()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* sock = m_server.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                connect(sock, &QTcpSocket::readyRead, this, [this, sock, buf]() {
                    *buf += sock->readAll();
                    const int he = buf->indexOf("\r\n\r\n");
                    if (he < 0) {
                        return;
                    }
                    Request r;
                    const QList<QByteArray> lines = buf->left(he).split('\n');
                    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
                    r.method = first.value(0);
                    r.path = first.value(1);
                    for (int i = 1; i < lines.size(); ++i) {
                        const int c = lines[i].indexOf(':');
                        if (c > 0) {
                            r.headers.insert(lines[i].left(c).trimmed().toLower(),
                                             lines[i].mid(c + 1).trimmed());
                        }
                    }
                    const int len = r.headers.value("content-length").toInt();
                    if (buf->size() - (he + 4) < len) {
                        return;
                    }
                    r.body = buf->mid(he + 4, len);
                    requests.append(r);
                    const Reply rep = !script.isEmpty() ? script.takeFirst()
                                    : (handler ? handler(r) : Reply{});
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(rep.status) + " X\r\n"
                                     "Content-Type: text/plain\r\nConnection: close\r\n"
                                     "Content-Length: " + QByteArray::number(rep.body.size()) +
                                     "\r\n\r\n" + rep.body;
                    sock->write(out);
                    sock->disconnectFromHost();
                });
                connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
            }
        });
    }
    QUrl url(const QString& path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
    }

    QList<Request> requests;
    QList<Reply> script;
    std::function<Reply(const Request&)> handler;   // used when script is empty

private:
    QTcpServer m_server;
};

LogQso sampleQso(const QString& call)
{
    LogQso q;
    q.call = call;
    q.freqMhz = 14.025;
    q.band = "20m";
    q.mode = "CW";
    q.timeOn = QDateTime(QDate(2026, 9, 1), QTime(12, 0), QTimeZone::UTC);
    q.rstSent = "599";
    q.rstRcvd = "599";
    q.stationCallsign = "K1ABC";
    q.recordingPath = "/Users/secret/rec.wav";
    q.sliceId = 0;
    return q;
}

void testPureHelpers()
{
    const QByteArray enc = UploadApi::formEncode({{"ADIF", "<CALL:4>W1AW a&b=c+d"}});
    report("formEncode escapes < > : & = + space",
           enc == "ADIF=%3CCALL%3A4%3EW1AW%20a%26b%3Dc%2Bd", s(QString::fromLatin1(enc)));

    const QByteArray rec = Adif::record(sampleQso("DL1ABC"));
    UploadApi::HttpRequest r = UploadApi::qrzInsert(QUrl("http://x/api"), "ABCD-1234", rec, false);
    QMap<QString, QString> f = decodeForm(r.body);
    report("QRZ insert: KEY/ACTION/ADIF", f.value("KEY") == "ABCD-1234" && f.value("ACTION") == "INSERT" &&
           f.value("ADIF").contains("<CALL:6>DL1ABC") && !f.contains("OPTION"));
    report("QRZ user agent AetherSDR/…", r.userAgent.startsWith("AetherSDR/"));
    r = UploadApi::qrzInsert(QUrl("http://x/api"), "K", rec, true);
    report("QRZ replace → OPTION=REPLACE", decodeForm(r.body).value("OPTION") == "REPLACE");
    r = UploadApi::qrzStatus(QUrl("http://x/api"), "K");
    report("QRZ status body", decodeForm(r.body).value("ACTION") == "STATUS");

    UploadApi::Result res = UploadApi::parseQrzResponse(200, "RESULT=OK&LOGID=130877825&COUNT=1");
    report("QRZ OK + LOGID", res.outcome == Outcome::Ok && res.remoteId == "130877825");
    res = UploadApi::parseQrzResponse(200, "RESULT=FAIL&REASON=Unable to add QSO to database: duplicate&EXTENDED=");
    report("QRZ duplicate", res.outcome == Outcome::Duplicate);
    res = UploadApi::parseQrzResponse(200, "RESULT=AUTH&REASON=invalid api key");
    report("QRZ auth", res.outcome == Outcome::AuthFailed);
    res = UploadApi::parseQrzResponse(200, "RESULT=FAIL&REASON=wrong station_callsign");
    report("QRZ rejected", res.outcome == Outcome::Rejected);
    res = UploadApi::parseQrzResponse(503, "");
    report("QRZ 503 → retry", res.outcome == Outcome::RetryLater);
    const auto kv = UploadApi::parseQrzKv("RESULT=OK&COUNT=5&CALLSIGN=K1ABC");
    report("QRZ kv parse", kv.value("COUNT") == "5" && kv.value("CALLSIGN") == "K1ABC");

    r = UploadApi::clubLogRealtime(QUrl("http://x/realtime.php"), "me@example.com", "app-pw",
                                   "K1ABC", "APIKEY", rec);
    f = decodeForm(r.body);
    report("Club Log fields", f.value("email") == "me@example.com" && f.value("password") == "app-pw" &&
           f.value("callsign") == "K1ABC" && f.value("api") == "APIKEY" &&
           f.value("adif").endsWith("<EOR>\n"));
    report("Club Log 200 OK", UploadApi::parseClubLogResponse(200, "QSO OK").outcome == Outcome::Ok);
    report("Club Log 200 Modified", UploadApi::parseClubLogResponse(200, "QSO Modified").outcome == Outcome::Ok);
    report("Club Log 200 Duplicate", UploadApi::parseClubLogResponse(200, "QSO Duplicate").outcome == Outcome::Duplicate);
    report("Club Log 400 rejected", UploadApi::parseClubLogResponse(400, "QSO Rejected").outcome == Outcome::Rejected);
    report("Club Log 403 auth", UploadApi::parseClubLogResponse(403, "Forbidden").outcome == Outcome::AuthFailed);
    report("Club Log 500 retry", UploadApi::parseClubLogResponse(500, "").outcome == Outcome::RetryLater);

    r = UploadApi::eqslImport(QUrl("http://x/importADIF.cfm"), "K1ABC", "pw", Adif::header("t") + rec);
    f = decodeForm(r.body);
    report("eQSL fields", f.value("EQSL_USER") == "K1ABC" && f.value("EQSL_PSWD") == "pw" &&
           f.value("ADIFData").contains("<EOH>"));
    report("eQSL ok", UploadApi::parseEqslResponse(200,
           "<HTML><BODY>Result: 1 out of 1 records added<BR></BODY></HTML>").outcome == Outcome::Ok);
    report("eQSL dupe", UploadApi::parseEqslResponse(200,
           "Result: 0 out of 1 records added<BR>Warning: Bad record: Duplicate").outcome == Outcome::Duplicate);
    report("eQSL auth", UploadApi::parseEqslResponse(200,
           "Error: No match on eQSL_User/eQSL_Pswd").outcome == Outcome::AuthFailed);

    const QStringList args = UploadApi::tqslArguments("Home QTH", "/tmp/x.adi");
    report("tqsl args: -x -d -u -l loc file, never -p",
           args.contains("-x") && args.contains("-u") && args.contains("-d") &&
           args.indexOf("-l") >= 0 && args.value(args.indexOf("-l") + 1) == "Home QTH" &&
           args.last() == "/tmp/x.adi" && !args.contains("-p"));
    report("tqsl exit 0 ok", UploadApi::parseTqslExit(0, "").outcome == Outcome::Ok);
    report("tqsl exit 8 all dupes", UploadApi::parseTqslExit(8, "").outcome == Outcome::Duplicate);
    report("tqsl exit 9 some dupes ok", UploadApi::parseTqslExit(9, "").outcome == Outcome::Ok);
    report("tqsl exit 11 retry", UploadApi::parseTqslExit(11, "").outcome == Outcome::RetryLater);
    report("tqsl exit 2 rejected", UploadApi::parseTqslExit(2, "").outcome == Outcome::Rejected);
}

void testQueue(const QString& dir)
{
    FakeHttpServer srv;
    Logbook lb;
    lb.open(dir + "/up.sqlite");
    CredentialStore creds(CredentialStore::Backend::Memory);
    creds.write(CredentialStore::kQrzApiKey, "QRZ-KEY");
    creds.write(CredentialStore::kClubLogPassword, "cl-app-pw");
    creds.write(CredentialStore::kClubLogApiKey, "cl-api");
    creds.write(CredentialStore::kEqslPassword, "eq-pw");
    report("memory credential store is 'secure'", creds.isSecure());
    report("plain AppSettings store flagged insecure",
           !CredentialStore(CredentialStore::Backend::AppSettingsPlain).isSecure());

    LogUploadQueue q(&lb, &creds);
    q.setEndpoint(UploadService::Qrz, srv.url("/api"));
    q.setEndpoint(UploadService::ClubLog, srv.url("/realtime.php"));
    q.setEndpoint(UploadService::Eqsl, srv.url("/qslcard/importADIF.cfm"));
    q.setRetryBaseMs(20);
    q.setClubLogAccount("me@example.com", "K1ABC");
    q.setEqslUser("K1ABC");

    // ── QRZ success ──
    const qint64 id1 = lb.addQso(sampleQso("DL1ABC"));
    q.setEnabled(UploadService::Qrz, true);
    srv.script = {{200, "RESULT=OK&LOGID=42&COUNT=1"}};
    q.enqueue(id1, UploadService::Qrz);
    report("QRZ enqueue marks Pending (sync)",
           lb.qso(id1).uploadState(UploadService::Qrz) == UploadState::Pending ||
           lb.qso(id1).uploadState(UploadService::Qrz) == UploadState::Uploaded);
    waitFor([&]() { return lb.qso(id1).uploadState(UploadService::Qrz) == UploadState::Uploaded; });
    report("QRZ uploaded via fake server", lb.qso(id1).uploadState(UploadService::Qrz) == UploadState::Uploaded &&
           lb.qso(id1).uploadDate[1].isValid());
    const FakeHttpServer::Request r0 = srv.requests.value(0);
    const auto f0 = decodeForm(r0.body);
    report("QRZ POST /api with form + UA", r0.method == "POST" && r0.path == "/api" &&
           r0.headers.value("content-type").startsWith("application/x-www-form-urlencoded") &&
           r0.headers.value("user-agent").startsWith("AetherSDR/") && f0.value("KEY") == "QRZ-KEY");
    report("uploaded ADIF has no local path / APP_ fields",
           f0.value("ADIF").contains("<CALL:6>DL1ABC") && !f0.value("ADIF").contains("secret") &&
           !f0.value("ADIF").contains("APP_AETHERSDR"));

    // ── QRZ modified → REPLACE ──
    LogQso edited = lb.qso(id1);
    edited.comment = "edited";
    lb.updateQso(edited);
    srv.script = {{200, "RESULT=REPLACE&LOGID=42&COUNT=1"}};
    const int before = static_cast<int>(srv.requests.size());
    q.enqueue(id1, UploadService::Qrz);
    waitFor([&]() { return srv.requests.size() > before &&
                           lb.qso(id1).uploadState(UploadService::Qrz) == UploadState::Uploaded; });
    report("QRZ modified QSO re-sent with OPTION=REPLACE",
           decodeForm(srv.requests.value(before).body).value("OPTION") == "REPLACE");

    // ── Club Log: 500 then OK → retry ──
    const qint64 id2 = lb.addQso(sampleQso("G3XYZ"));
    q.setEnabled(UploadService::ClubLog, true);
    srv.requests.clear();
    srv.script = {{500, "Internal error"}, {200, "QSO OK"}};
    q.enqueue(id2, UploadService::ClubLog);
    waitFor([&]() { return lb.qso(id2).uploadState(UploadService::ClubLog) == UploadState::Uploaded; });
    report("Club Log retried after 500 then uploaded",
           lb.qso(id2).uploadState(UploadService::ClubLog) == UploadState::Uploaded &&
           srv.requests.size() == 2, std::to_string(srv.requests.size()));
    const auto fc = decodeForm(srv.requests.value(0).body);
    report("Club Log POST fields", srv.requests.value(0).path == "/realtime.php" &&
           fc.value("email") == "me@example.com" && fc.value("password") == "cl-app-pw" &&
           fc.value("api") == "cl-api" && fc.value("callsign") == "K1ABC");

    // ── Club Log 403 → suspend, no further requests ──
    const qint64 id3 = lb.addQso(sampleQso("F5AAA"));
    const qint64 id4 = lb.addQso(sampleQso("F5BBB"));
    srv.requests.clear();
    srv.script = {{403, "Forbidden"}};
    bool suspendedSignal = false;
    QObject::connect(&q, &LogUploadQueue::serviceSuspended,
                     [&](UploadService svc, const QString&) { suspendedSignal |= (svc == UploadService::ClubLog); });
    q.enqueue(QVector<qint64>{id3, id4}, UploadService::ClubLog);
    waitFor([&]() { return q.isSuspended(UploadService::ClubLog); });
    waitFor([]() { return false; }, 200);   // give it a chance to misbehave
    report("Club Log 403 suspends service", q.isSuspended(UploadService::ClubLog) && suspendedSignal);
    report("Club Log 403: only one request sent", srv.requests.size() == 1,
           std::to_string(srv.requests.size()));
    report("Club Log 403: QSOs remain Pending",
           lb.qso(id4).uploadState(UploadService::ClubLog) == UploadState::Pending);

    // ── Missing credentials → suspend without network traffic ──
    creds.remove(CredentialStore::kEqslPassword);
    q.setEnabled(UploadService::Eqsl, true);
    srv.requests.clear();
    q.enqueue(id1, UploadService::Eqsl);
    waitFor([&]() { return q.isSuspended(UploadService::Eqsl); }, 1000);
    report("eQSL without password: suspended, no request",
           q.isSuspended(UploadService::Eqsl) && srv.requests.isEmpty());

    // ── eQSL success after credentials restored ──
    creds.write(CredentialStore::kEqslPassword, "eq-pw");
    srv.script = {{200, "Result: 1 out of 1 records added"}};
    q.resume(UploadService::Eqsl);
    waitFor([&]() { return lb.qso(id1).uploadState(UploadService::Eqsl) == UploadState::Uploaded; });
    report("eQSL uploaded after resume", lb.qso(id1).uploadState(UploadService::Eqsl) == UploadState::Uploaded);
    report("eQSL request has header + record",
           decodeForm(srv.requests.value(0).body).value("ADIFData").contains("<EOH>"));

    // ── Rejected (400) → Failed, no retry ──
    q.resume(UploadService::ClubLog);   // re-queues id3/id4 (Pending)
    srv.requests.clear();
    srv.script = {{400, "QSO Rejected"}, {200, "QSO OK"}};
    waitFor([&]() {
        return lb.qso(id3).uploadState(UploadService::ClubLog) != UploadState::Pending &&
               lb.qso(id4).uploadState(UploadService::ClubLog) != UploadState::Pending;
    });
    const UploadState s3 = lb.qso(id3).uploadState(UploadService::ClubLog);
    const UploadState s4 = lb.qso(id4).uploadState(UploadService::ClubLog);
    report("Club Log 400 → one Failed, other Uploaded",
           (s3 == UploadState::Failed && s4 == UploadState::Uploaded) ||
           (s4 == UploadState::Failed && s3 == UploadState::Uploaded));

    // ── Retry exhaustion → Failed ──
    q.setMaxAttempts(2);
    const qint64 id5 = lb.addQso(sampleQso("OH2ZZ"));
    srv.script = {{500, ""}, {500, ""}, {500, ""}};
    q.enqueue(id5, UploadService::Qrz);
    waitFor([&]() { return lb.qso(id5).uploadState(UploadService::Qrz) == UploadState::Failed; });
    report("retry exhaustion → Failed", lb.qso(id5).uploadState(UploadService::Qrz) == UploadState::Failed);

    // ── Auto-upload new QSOs ──
    q.setMaxAttempts(8);
    q.setAutoUpload(true);
    srv.requests.clear();
    // Three services run concurrently → reply by path, not by order.
    srv.script.clear();
    srv.handler = [](const FakeHttpServer::Request& r) -> FakeHttpServer::Reply {
        if (r.path == "/api") { return {200, "RESULT=OK&LOGID=7&COUNT=1"}; }
        if (r.path == "/realtime.php") { return {200, "QSO OK"}; }
        return {200, "Result: 1 out of 1 records added"};
    };
    const qint64 id6 = lb.addQso(sampleQso("JA1AAA"));
    waitFor([&]() {
        const LogQso x = lb.qso(id6);
        return x.uploadState(UploadService::Qrz) == UploadState::Uploaded &&
               x.uploadState(UploadService::ClubLog) == UploadState::Uploaded &&
               x.uploadState(UploadService::Eqsl) == UploadState::Uploaded;
    });
    const LogQso x6 = lb.qso(id6);
    report("auto-upload to all enabled services",
           x6.uploadState(UploadService::Qrz) == UploadState::Uploaded &&
           x6.uploadState(UploadService::ClubLog) == UploadState::Uploaded &&
           x6.uploadState(UploadService::Eqsl) == UploadState::Uploaded &&
           x6.uploadState(UploadService::Lotw) == UploadState::NotSent);
    q.setAutoUpload(false);
}

void testLotw(const QString& dir)
{
    Logbook lb;
    lb.open(dir + "/lotw.sqlite");
    CredentialStore creds(CredentialStore::Backend::Memory);
    LogUploadQueue q(&lb, &creds);
    q.setRetryBaseMs(20);

    // Fake tqsl: record args + signed file, exit with code from a file.
    const QString script = dir + "/tqsl";
    const QString argsOut = dir + "/tqsl_args.txt";
    const QString adiOut = dir + "/tqsl_input.adi";
    const QString codeFile = dir + "/tqsl_code";
    {
        QFile f(script);
        f.open(QIODevice::WriteOnly);
        f.write(QStringLiteral(
            "#!/bin/sh\n"
            "printf '%s\\n' \"$@\" > '%1'\n"
            "for last; do :; done\n"
            "cp \"$last\" '%2'\n"
            "exit $(cat '%3')\n").arg(argsOut, adiOut, codeFile).toUtf8());
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    auto setCode = [&](int c) {
        QFile f(codeFile);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(QByteArray::number(c));
    };

    q.setTqslPath(script);
    q.setTqslStationLocation("Home QTH");
    q.setEnabled(UploadService::Lotw, true);

    setCode(0);
    const qint64 a = lb.addQso(sampleQso("DL1ABC"));
    const qint64 b = lb.addQso(sampleQso("G3XYZ"));
    q.enqueue(QVector<qint64>{a, b}, UploadService::Lotw);
    waitFor([&]() { return lb.qso(b).uploadState(UploadService::Lotw) == UploadState::Uploaded; });
    report("LoTW batch uploaded (exit 0)",
           lb.qso(a).uploadState(UploadService::Lotw) == UploadState::Uploaded &&
           lb.qso(b).uploadState(UploadService::Lotw) == UploadState::Uploaded);
    QFile af(argsOut);
    af.open(QIODevice::ReadOnly);
    const QStringList args = QString::fromUtf8(af.readAll()).split('\n', Qt::SkipEmptyParts);
    report("tqsl invoked with -l 'Home QTH' and no -p",
           args.contains("Home QTH") && args.contains("-u") && !args.contains("-p"),
           s(args.join(' ')));
    QFile adi(adiOut);
    adi.open(QIODevice::ReadOnly);
    const QByteArray signedAdi = adi.readAll();
    report("tqsl input ADIF has both QSOs, no local paths",
           signedAdi.contains("<CALL:6>DL1ABC") && signedAdi.contains("<CALL:5>G3XYZ") &&
           !signedAdi.contains("secret") && signedAdi.contains("<EOH>"));

    // Connection error → retry, then success
    setCode(11);
    const qint64 c = lb.addQso(sampleQso("OH2AA"));
    q.enqueue(c, UploadService::Lotw);
    waitFor([&]() { return QFile::exists(argsOut) && q.pendingCount() > 0; }, 1000);
    setCode(0);
    waitFor([&]() { return lb.qso(c).uploadState(UploadService::Lotw) == UploadState::Uploaded; });
    report("LoTW exit 11 retried then uploaded",
           lb.qso(c).uploadState(UploadService::Lotw) == UploadState::Uploaded);

    // No station location → suspended, tqsl not run
    QFile::remove(argsOut);
    q.setTqslStationLocation(QString());
    const qint64 d = lb.addQso(sampleQso("VK2AA"));
    q.enqueue(d, UploadService::Lotw);
    waitFor([&]() { return q.isSuspended(UploadService::Lotw); }, 1000);
    report("LoTW without station location: suspended, tqsl not run",
           q.isSuspended(UploadService::Lotw) && !QFile::exists(argsOut));
}

} // namespace

int main(int argc, char** argv)
{
    QStandardPaths::setTestModeEnabled(true);   // never touch real settings
    QCoreApplication app(argc, argv);
    app.setApplicationVersion("0.0-test");
    QTemporaryDir dir;
    testPureHelpers();
    testQueue(dir.path());
    testLotw(dir.path());
    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
