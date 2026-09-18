// Rotator control tests — fake Hamlib rotctld (QTcpServer) + fake PstRotator
// (QUdpSocket) + great-circle / Maidenhead math.  No real rotator or radio
// is touched; everything runs on 127.0.0.1 with ephemeral ports.
//
// Fake rotctld speaks the Extended Response Protocol exactly as described in
// the Hamlib rotctld(1) man page: every reply ends with "RPRT <n>".

#include "core/CtyDatParser.h"
#include "core/GeoUtils.h"
#include "core/RotatorBackends.h"
#include "core/RotatorClient.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QTest>
#include <QUdpSocket>

#include <cmath>
#include <memory>
#include <numbers>

using namespace AetherSDR;

namespace {

// ── Fake rotctld ────────────────────────────────────────────────────────────
class FakeRotctld {
public:
    double az{123.0};
    double el{0.0};
    double minAz{0.0};
    double maxAz{450.0};
    bool   silent{false};           // swallow commands (→ client timeout)
    bool   rejectSetPos{false};     // answer set_pos with RPRT -1
    int    batchReplies{0};         // hold replies until N commands queued, then flush in one write
    QStringList received;           // raw command lines, in arrival order
    int    connections{0};

    bool listen(quint16 port = 0)
    {
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                ++connections;
                client = s;
                QObject::connect(s, &QTcpSocket::readyRead, [this, s]() { onRead(s); });
            }
        });
        return server.listen(QHostAddress::LocalHost, port);
    }
    quint16 port() const { return server.serverPort(); }
    void dropClient()
    {
        if (client) {
            client->abort();
        }
    }

    QTcpServer server;
    QPointer<QTcpSocket> client;

private:
    void onRead(QTcpSocket* s)
    {
        rx += s->readAll();
        qsizetype nl = -1;
        while ((nl = rx.indexOf('\n')) >= 0) {
            const QString line = QString::fromLatin1(rx.left(nl)).trimmed();
            rx.remove(0, nl + 1);
            received.append(line);
            if (silent) {
                continue;
            }
            pendingOut += reply(line);
            ++pendingCount;
        }
        if (pendingCount > 0 && pendingCount >= batchReplies) {
            // Split the write in two to exercise partial-line reassembly.
            const qsizetype half = pendingOut.size() / 2;
            s->write(pendingOut.left(half));
            s->flush();
            s->write(pendingOut.mid(half));
            pendingOut.clear();
            pendingCount = 0;
        }
    }

    QByteArray reply(const QString& line)
    {
        if (line == QLatin1String("+p")) {
            return QStringLiteral("get_pos:\nAzimuth: %1\nElevation: %2\nRPRT 0\n")
                .arg(az, 0, 'f', 6).arg(el, 0, 'f', 6).toLatin1();
        }
        if (line.startsWith(QLatin1String("+P "))) {
            const QStringList a = line.mid(3).split(' ', Qt::SkipEmptyParts);
            const double newAz = a.value(0).toDouble();
            if (rejectSetPos || newAz < minAz || newAz > maxAz) {
                return QStringLiteral("set_pos: %1\nRPRT -1\n").arg(line.mid(3)).toLatin1();
            }
            az = newAz;   // "instant" rotator
            el = a.value(1).toDouble();
            return QStringLiteral("set_pos: %1\nRPRT 0\n").arg(line.mid(3)).toLatin1();
        }
        if (line == QLatin1String("+S")) {
            return QByteArrayLiteral("stop:\nRPRT 0\n");
        }
        if (line == QLatin1String("+K")) {
            az = 0.0;
            return QByteArrayLiteral("park:\nRPRT 0\n");
        }
        return QByteArrayLiteral("RPRT -4\n");   // RIG_ENIMPL
    }

    QByteArray rx;
    QByteArray pendingOut;
    int pendingCount{0};
};

RotatorConfig fastConfig(quint16 port)
{
    RotatorConfig c;
    c.backend = RotatorConfig::Backend::Rotctld;
    c.host = QStringLiteral("127.0.0.1");
    c.port = port;
    c.pollIntervalMs = 100;
    c.commandTimeoutMs = 400;
    c.connectTimeoutMs = 1000;
    c.reconnectMinMs = 100;
    c.reconnectMaxMs = 400;
    c.minAz = 0.0;
    c.maxAz = 450.0;
    return c;
}

quint16 freeUdpPort()
{
    QUdpSocket s;
    s.bind(QHostAddress::LocalHost, 0);
    return s.localPort();
}

// Minimal cty.dat excerpt (AD1C format, longitude +West).
constexpr const char* kMiniCty =
    "England:                  14:  27:  EU:   52.77:     1.47:     0.0:  G:\n"
    "    G,GX,M;\n"
    "United States:            05:  08:  NA:   37.60:    91.87:     5.0:  K:\n"
    "    AA,K,N,W;\n"
    "Japan:                    25:  45:  AS:   36.40:  -138.38:    -9.0:  JA:\n"
    "    JA,JE;\n";

} // namespace

class RotatorTest : public QObject {
    Q_OBJECT

private slots:
    // ── Geo math ────────────────────────────────────────────────────────────
    void gridDecode();
    void gridEncodeRoundTrip();
    void invalidGrids();
    void bearingFn31ToIo91();
    void bearingBasics();
    void ctyLongitudeSign();
    void parseTurnTarget();
    void findGridInText();
    void overlapMapping();
    void rprtStrings();

    // ── rotctld backend ─────────────────────────────────────────────────────
    void connectAndPoll();
    void setPositionAndReach();
    void overlapCommandedAzimuth();
    void rprtErrorOnSetPos();
    void stopAndParkInOrder();
    void pipelinedBatchedReplies();
    void commandTimeoutForcesReconnect();
    void reconnectAfterServerDrop();
    void reconnectWhenServerAppearsLater();
    void notConnectedFailsFast();

    // ── UDP backends ────────────────────────────────────────────────────────
    void pstRotatorRoundTrip();
    void n1mmParseAndSend();
};

// ═══════════════════════════════════════════════════════════════════════════

void RotatorTest::gridDecode()
{
    const auto fn31 = GeoUtils::gridToLatLon(QStringLiteral("FN31"));
    QVERIFY(fn31);
    QCOMPARE(fn31->lat, 41.5);
    QCOMPARE(fn31->lon, -73.0);

    const auto io91wm = GeoUtils::gridToLatLon(QStringLiteral("io91wm"));
    QVERIFY(io91wm);
    QVERIFY(std::abs(io91wm->lat - 51.520833) < 1e-5);
    QVERIFY(std::abs(io91wm->lon - (-0.125)) < 1e-9);

    const auto ext = GeoUtils::gridToLatLon(QStringLiteral("IO91WM55"));
    QVERIFY(ext);
    QVERIFY(std::abs(ext->lat - io91wm->lat) < 1.0 / 24.0);
    QVERIFY(std::abs(ext->lon - io91wm->lon) < 2.0 / 24.0);
}

void RotatorTest::gridEncodeRoundTrip()
{
    QCOMPARE(GeoUtils::latLonToGrid({51.520833, -0.125}, 6), QStringLiteral("IO91wm"));
    QCOMPARE(GeoUtils::latLonToGrid({41.5, -73.0}, 4), QStringLiteral("FN31"));
    QCOMPARE(GeoUtils::latLonToGrid({-33.87, 151.21}, 6), QStringLiteral("QF56od"));   // Sydney
    QCOMPARE(GeoUtils::latLonToGrid({90.0, 180.0}, 4), QStringLiteral("RR99"));       // clamps inside
}

void RotatorTest::invalidGrids()
{
    QVERIFY(!GeoUtils::isValidGrid(QStringLiteral("")));
    QVERIFY(!GeoUtils::isValidGrid(QStringLiteral("SZ12")));     // field > R
    QVERIFY(!GeoUtils::isValidGrid(QStringLiteral("FN3")));
    QVERIFY(!GeoUtils::isValidGrid(QStringLiteral("FN31ZZ")));   // subsquare > X
    QVERIFY(!GeoUtils::isValidGrid(QStringLiteral("K1ABC")));
    QVERIFY(GeoUtils::isValidGrid(QStringLiteral("fn31pr")));
    QVERIFY(!GeoUtils::gridToLatLon(QStringLiteral("XX99")));
}

void RotatorTest::bearingFn31ToIo91()
{
    const auto a = GeoUtils::gridToLatLon(QStringLiteral("FN31"));
    const auto b = GeoUtils::gridToLatLon(QStringLiteral("IO91"));
    QVERIFY(a && b);
    const GreatCirclePath p = GeoUtils::pathBetween(*a, *b);
    qInfo("FN31->IO91: SP %.2f deg %.0f km, LP %.2f deg %.0f km",
          p.shortPathDeg, p.shortPathKm, p.longPathDeg, p.longPathKm);
    QVERIFY2(p.shortPathDeg > 48.0 && p.shortPathDeg < 54.0, "short path ~50-52 deg");
    QVERIFY(std::abs(p.longPathDeg - GeoUtils::normalizeBearing(p.shortPathDeg + 180.0)) < 1e-9);
    QVERIFY(p.shortPathKm > 5300.0 && p.shortPathKm < 5700.0);
    QVERIFY(std::abs(p.shortPathKm + p.longPathKm - 2.0 * std::numbers::pi * GeoUtils::kEarthRadiusKm) < 1e-6);

    // Reverse direction: London → Connecticut is roughly WNW (~288°).
    const GreatCirclePath r = GeoUtils::pathBetween(*b, *a);
    QVERIFY(r.shortPathDeg > 282.0 && r.shortPathDeg < 294.0);
    QVERIFY(std::abs(r.shortPathKm - p.shortPathKm) < 1e-6);
}

void RotatorTest::bearingBasics()
{
    const LatLon origin{0.0, 0.0};
    QVERIFY(std::abs(GeoUtils::initialBearing(origin, {10.0, 0.0}) - 0.0) < 1e-9);
    QVERIFY(std::abs(GeoUtils::initialBearing(origin, {0.0, 90.0}) - 90.0) < 1e-9);
    QVERIFY(std::abs(GeoUtils::initialBearing(origin, {-10.0, 0.0}) - 180.0) < 1e-9);
    QVERIFY(std::abs(GeoUtils::initialBearing(origin, {0.0, -90.0}) - 270.0) < 1e-9);
    QVERIFY(std::abs(GeoUtils::distanceKm(origin, {0.0, 90.0}) - std::numbers::pi / 2.0 * 6371.0) < 1e-6);
    QCOMPARE(GeoUtils::normalizeBearing(-90.0), 270.0);
    QCOMPARE(GeoUtils::normalizeBearing(720.0), 0.0);
    QCOMPARE(GeoUtils::normalizeBearing(450.0), 90.0);
}

void RotatorTest::ctyLongitudeSign()
{
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write(kMiniCty);
    f.flush();
    CtyDatParser cty;
    QVERIFY(cty.loadFromFile(f.fileName()));

    const auto g = GeoUtils::latLonForCallsign(cty, QStringLiteral("G4ABC"));
    QVERIFY(g);
    QCOMPARE(g->lon, -1.47);      // cty "1.47" is 1.47°W
    const auto k = GeoUtils::latLonForCallsign(cty, QStringLiteral("k1abc"));
    QVERIFY(k);
    QCOMPARE(k->lon, -91.87);
    const auto ja = GeoUtils::latLonForCallsign(cty, QStringLiteral("JA1XYZ"));
    QVERIFY(ja);
    QCOMPARE(ja->lon, 138.38);    // cty "-138.38" is 138.38°E

    // From FN31 a G call is NE (~50°), a JA call is NNW over the pole (~330°).
    const auto home = GeoUtils::gridToLatLon(QStringLiteral("FN31"));
    const double toG = GeoUtils::initialBearing(*home, *g);
    const double toJa = GeoUtils::initialBearing(*home, *ja);
    QVERIFY2(toG > 40.0 && toG < 60.0, qPrintable(QString::number(toG)));
    QVERIFY2(toJa > 315.0 && toJa < 345.0, qPrintable(QString::number(toJa)));
}

void RotatorTest::parseTurnTarget()
{
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write(kMiniCty);
    f.flush();
    CtyDatParser cty;
    QVERIFY(cty.loadFromFile(f.fileName()));

    using K = GeoUtils::TurnTarget::Kind;
    auto t = GeoUtils::parseTurnTarget(QStringLiteral("275"), &cty);
    QCOMPARE(t.kind, K::Bearing);
    QCOMPARE(t.bearingDeg, 275.0);
    t = GeoUtils::parseTurnTarget(QStringLiteral("-10\u00B0"), &cty);
    QCOMPARE(t.kind, K::Bearing);
    QCOMPARE(t.bearingDeg, 350.0);
    t = GeoUtils::parseTurnTarget(QStringLiteral("io91wm"), &cty);
    QCOMPARE(t.kind, K::Grid);
    QCOMPARE(t.label, QStringLiteral("IO91WM"));
    t = GeoUtils::parseTurnTarget(QStringLiteral("G4ABC"), &cty);
    QCOMPARE(t.kind, K::Callsign);
    QVERIFY(t.label.contains(QStringLiteral("England")));
    t = GeoUtils::parseTurnTarget(QStringLiteral("ZZ9ZZZ"), &cty);   // no cty match
    QCOMPARE(t.kind, K::Invalid);
    t = GeoUtils::parseTurnTarget(QStringLiteral("G4ABC"), nullptr);
    QCOMPARE(t.kind, K::Invalid);
    t = GeoUtils::parseTurnTarget(QStringLiteral("   "), &cty);
    QCOMPARE(t.kind, K::Invalid);
}

void RotatorTest::findGridInText()
{
    QCOMPARE(GeoUtils::findGridInText(QStringLiteral("CQ DX FN31")), QStringLiteral("FN31"));
    QCOMPARE(GeoUtils::findGridInText(QStringLiteral("FT8 -12 dB 1234 Hz io91wm")), QStringLiteral("IO91WM"));
    QCOMPARE(GeoUtils::findGridInText(QStringLiteral("K1ABC RR73")), QString());
    QCOMPARE(GeoUtils::findGridInText(QStringLiteral("CW 23 WPM 17 dB")), QString());
    QCOMPARE(GeoUtils::findGridInText(QStringLiteral("KN2ABC5")), QString());   // not a whole word
    QCOMPARE(GeoUtils::findGridInText(QString()), QString());
}

void RotatorTest::overlapMapping()
{
    using RC = RotatorClient;
    // Plain 0..360
    QCOMPARE(*RC::mapToRotatorRange(90.0, 0, 360, std::nullopt), 90.0);
    QCOMPARE(*RC::mapToRotatorRange(0.0, 0, 360, 355.0), 360.0);   // nearer end
    QCOMPARE(*RC::mapToRotatorRange(0.0, 0, 360, 5.0), 0.0);
    // 0..450 overlap: 30° reachable as 30 or 390
    QCOMPARE(*RC::mapToRotatorRange(30.0, 0, 450, 420.0), 390.0);
    QCOMPARE(*RC::mapToRotatorRange(30.0, 0, 450, 100.0), 30.0);
    QCOMPARE(*RC::mapToRotatorRange(30.0, 0, 450, std::nullopt), 30.0);
    QCOMPARE(*RC::mapToRotatorRange(100.0, 0, 450, 420.0), 100.0);  // 460 out of range
    // South-centred -180..180
    QCOMPARE(*RC::mapToRotatorRange(270.0, -180, 180, std::nullopt), -90.0);
    QCOMPARE(*RC::mapToRotatorRange(180.0, -180, 180, 170.0), 180.0);
    QCOMPARE(*RC::mapToRotatorRange(180.0, -180, 180, -170.0), -180.0);
    // Limited arc 0..270: 300 unreachable
    QVERIFY(!RC::mapToRotatorRange(300.0, 0, 270, std::nullopt));
}

void RotatorTest::rprtStrings()
{
    QCOMPARE(RotatorClient::rprtErrorString(0), QStringLiteral("Command completed successfully"));
    QCOMPARE(RotatorClient::rprtErrorString(-1), QStringLiteral("Invalid parameter"));
    QCOMPARE(RotatorClient::rprtErrorString(-5), QStringLiteral("Communication timed out"));
    QCOMPARE(RotatorClient::rprtErrorString(-99), QStringLiteral("Hamlib error -99"));
}

// ═══════════════════════════════════════════════════════════════════════════

void RotatorTest::connectAndPoll()
{
    FakeRotctld fake;
    QVERIFY(fake.listen());
    RotatorClient c;
    QSignalSpy pos(&c, &RotatorClient::positionChanged);
    c.setConfig(fastConfig(fake.port()));
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(c.hasPosition(), 2000);
    QCOMPARE(c.azimuth(), 123.0);
    QVERIFY(pos.count() >= 1);
    QVERIFY(fake.received.contains(QStringLiteral("+p")));

    // Polling continues; only one get_pos is ever outstanding.
    fake.az = 124.5;
    QTRY_COMPARE_WITH_TIMEOUT(c.azimuth(), 124.5, 2000);

    c.stop();
    QCOMPARE(c.state(), RotatorClient::State::Disabled);
}

void RotatorTest::setPositionAndReach()
{
    FakeRotctld fake;
    QVERIFY(fake.listen());
    RotatorClient c;
    c.setConfig(fastConfig(fake.port()));
    QSignalSpy target(&c, &RotatorClient::targetChanged);
    QSignalSpy reached(&c, &RotatorClient::targetReached);
    QSignalSpy failed(&c, &RotatorClient::commandFailed);
    c.start();
    QTRY_VERIFY_WITH_TIMEOUT(c.hasPosition(), 2000);

    QVERIFY(c.turnTo(200.0));
    QCOMPARE(target.count(), 1);
    QVERIFY(c.hasTarget());
    QTRY_VERIFY_WITH_TIMEOUT(fake.received.contains(QStringLiteral("+P 200.0 0.0")), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(reached.count(), 1, 2000);
    QVERIFY(!c.hasTarget());
    QCOMPARE(c.azimuth(), 200.0);
    QCOMPARE(failed.count(), 0);

    // turnToGrid: FN31 → IO91 short and long path.
    c.setStationGrid(QStringLiteral("fn31"));
    QVERIFY(c.turnToGrid(QStringLiteral("IO91")));
    const double sp = c.targetAzimuth();
    QVERIFY(sp > 48.0 && sp < 54.0);
    QTRY_COMPARE_WITH_TIMEOUT(reached.count(), 2, 2000);
    QVERIFY(c.turnToGrid(QStringLiteral("IO91"), true));
    QVERIFY(std::abs(c.targetAzimuth() - (sp + 180.0)) < 1e-9);
    QTRY_COMPARE_WITH_TIMEOUT(reached.count(), 3, 2000);

    // Grid hint beats cty lookup for callsigns.
    QVERIFY(c.turnToCallsign(QStringLiteral("W1AW"), false, QStringLiteral("IO91")));
    QVERIFY(std::abs(c.targetAzimuth() - sp) < 1e-9);
}

void RotatorTest::overlapCommandedAzimuth()
{
    FakeRotctld fake;
    fake.az = 420.0;    // rotator currently in the overlap (= 60° compass)
    QVERIFY(fake.listen());
    RotatorClient c;
    c.setConfig(fastConfig(fake.port()));
    c.start();
    QTRY_VERIFY_WITH_TIMEOUT(c.hasPosition(), 2000);
    QCOMPARE(c.azimuth(), 60.0);
    QCOMPARE(c.rawAzimuth(), 420.0);

    QVERIFY(c.turnTo(30.0));    // 390 is 30° away, 30 would be a 390° swing
    QTRY_VERIFY_WITH_TIMEOUT(fake.received.contains(QStringLiteral("+P 390.0 0.0")), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(c.rawAzimuth(), 390.0, 2000);

    QVERIFY(c.turnTo(100.0));   // 460 > max 450 → must use 100
    QTRY_VERIFY_WITH_TIMEOUT(fake.received.contains(QStringLiteral("+P 100.0 0.0")), 2000);
}

void RotatorTest::rprtErrorOnSetPos()
{
    FakeRotctld fake;
    fake.rejectSetPos = true;
    QVERIFY(fake.listen());
    RotatorClient c;
    c.setConfig(fastConfig(fake.port()));
    QSignalSpy failed(&c, &RotatorClient::commandFailed);
    QSignalSpy cleared(&c, &RotatorClient::targetCleared);
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 2000);

    QVERIFY(c.turnTo(90.0));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
    QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("set_pos 90.0 0.0"));
    QCOMPARE(failed.at(0).at(1).toInt(), -1);
    QCOMPARE(failed.at(0).at(2).toString(), QStringLiteral("Invalid parameter"));
    QCOMPARE(cleared.count(), 1);
    QVERIFY(!c.hasTarget());
    // An RPRT error is not a link error — still connected and polling.
    QCOMPARE(c.state(), RotatorClient::State::Connected);
    QCOMPARE(fake.connections, 1);
    const int polls = fake.received.count(QStringLiteral("+p"));
    QTRY_VERIFY_WITH_TIMEOUT(fake.received.count(QStringLiteral("+p")) > polls, 2000);
}

void RotatorTest::stopAndParkInOrder()
{
    FakeRotctld fake;
    QVERIFY(fake.listen());
    RotatorClient c;
    RotatorConfig cfg = fastConfig(fake.port());
    cfg.pollIntervalMs = 5000;   // keep polls out of the ordering check
    c.setConfig(cfg);
    QSignalSpy failed(&c, &RotatorClient::commandFailed);
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(c.hasPosition(), 2000);
    fake.received.clear();

    QVERIFY(c.turnTo(270.0));
    c.stopRotation();
    c.park();
    QVERIFY(!c.hasTarget());
    QTRY_COMPARE_WITH_TIMEOUT(fake.received.size(), 3, 2000);
    QCOMPARE(fake.received, (QStringList{QStringLiteral("+P 270.0 0.0"),
                                         QStringLiteral("+S"), QStringLiteral("+K")}));
    QTest::qWait(200);
    QCOMPARE(failed.count(), 0);
    c.requestPosition();
    QTRY_COMPARE_WITH_TIMEOUT(c.azimuth(), 0.0, 2000);   // parked
}

void RotatorTest::pipelinedBatchedReplies()
{
    FakeRotctld fake;
    fake.batchReplies = 3;   // hold replies until 3 commands are queued
    QVERIFY(fake.listen());
    RotatorClient c;
    RotatorConfig cfg = fastConfig(fake.port());
    cfg.pollIntervalMs = 5000;
    cfg.commandTimeoutMs = 2000;
    c.setConfig(cfg);
    QSignalSpy failed(&c, &RotatorClient::commandFailed);
    QSignalSpy pos(&c, &RotatorClient::positionChanged);
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 2000);
    // Connected → initial poll (1 queued).  Add set_pos + stop → 3 → flush.
    QVERIFY(c.turnTo(45.0));
    c.stopRotation();
    QTRY_COMPARE_WITH_TIMEOUT(pos.count(), 1, 2000);
    QCOMPARE(c.azimuth(), 123.0);   // poll answered before set_pos was applied
    QTest::qWait(100);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(fake.connections, 1);
}

void RotatorTest::commandTimeoutForcesReconnect()
{
    FakeRotctld fake;
    fake.silent = true;
    QVERIFY(fake.listen());
    RotatorClient c;
    c.setConfig(fastConfig(fake.port()));
    QSignalSpy failed(&c, &RotatorClient::commandFailed);
    QSignalSpy states(&c, &RotatorClient::stateChanged);
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 2000);
    QVERIFY(c.turnTo(10.0));   // queued behind the silent poll
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() >= 1, 2000);
    // The first outstanding command (the poll) times out with RIG_ETIMEOUT;
    // the queued set_pos is failed with RIG_EIO when the link is reset.
    bool sawTimeout = false;
    bool sawSetPos = false;
    for (const QList<QVariant>& f : failed) {
        sawTimeout |= f.at(1).toInt() == -5;
        sawSetPos |= f.at(0).toString().startsWith(QLatin1String("set_pos"));
    }
    QVERIFY(sawTimeout);
    QVERIFY(sawSetPos);
    QTRY_VERIFY_WITH_TIMEOUT(fake.connections >= 2, 3000);   // reconnected

    fake.silent = false;
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(c.hasPosition(), 3000);
}

void RotatorTest::reconnectAfterServerDrop()
{
    FakeRotctld fake;
    QVERIFY(fake.listen());
    RotatorClient c;
    c.setConfig(fastConfig(fake.port()));
    QSignalSpy errors(&c, &RotatorClient::errorOccurred);
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 2000);

    fake.dropClient();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Reconnecting, 2000);
    QVERIFY(errors.count() >= 1);
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 3000);
    QCOMPARE(fake.connections, 2);
}

void RotatorTest::reconnectWhenServerAppearsLater()
{
    // Grab a free port, release it, point the client at it (connection refused).
    quint16 port = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        port = probe.serverPort();
    }
    RotatorClient c;
    c.setConfig(fastConfig(port));
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Reconnecting, 2000);
    QTest::qWait(300);   // a couple of backoff cycles

    FakeRotctld fake;
    QVERIFY(fake.listen(port));
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(c.hasPosition(), 2000);

    // stop() must not reconnect afterwards.
    c.stop();
    const int conns = fake.connections;
    QTest::qWait(600);
    QCOMPARE(fake.connections, conns);
    QCOMPARE(c.state(), RotatorClient::State::Disabled);
}

void RotatorTest::notConnectedFailsFast()
{
    RotatorClient c;
    QSignalSpy errors(&c, &RotatorClient::errorOccurred);
    QVERIFY(!c.turnTo(90.0));            // not started
    QCOMPARE(errors.count(), 1);
    QVERIFY(!c.turnToGrid(QStringLiteral("IO91")));   // no station grid
}

// ═══════════════════════════════════════════════════════════════════════════

void RotatorTest::pstRotatorRoundTrip()
{
    QCOMPARE(*PstRotatorBackend::parseAzReply("AZ:123"), 123.0);
    QCOMPARE(*PstRotatorBackend::parseAzReply("AZ:45.5\r"), 45.5);
    QVERIFY(!PstRotatorBackend::parseAzReply("EL:10"));

    QUdpSocket fake;
    QVERIFY(fake.bind(QHostAddress::LocalHost, 0));
    const quint16 replyPort = freeUdpPort();
    QStringList got;
    QObject::connect(&fake, &QUdpSocket::readyRead, [&]() {
        while (fake.hasPendingDatagrams()) {
            QByteArray buf(static_cast<int>(fake.pendingDatagramSize()), '\0');
            QHostAddress from;
            fake.readDatagram(buf.data(), buf.size(), &from);
            got.append(QString::fromLatin1(buf));
            if (buf == "<PST>AZ?</PST>") {
                fake.writeDatagram("AZ:77\r", QHostAddress::LocalHost, replyPort);
            }
        }
    });

    RotatorConfig cfg;
    cfg.backend = RotatorConfig::Backend::PstRotator;
    cfg.host = QStringLiteral("localhost");
    cfg.port = fake.localPort();
    cfg.listenPort = replyPort;
    cfg.pollIntervalMs = 100;
    RotatorClient c;
    c.setConfig(cfg);
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(c.azimuth(), 77.0, 2000);

    QVERIFY(c.turnTo(450.0));   // compass bearing is sent, not the overlap value
    c.stopRotation();
    c.park();
    QTRY_VERIFY_WITH_TIMEOUT(got.contains(QStringLiteral("<PST><AZIMUTH>90</AZIMUTH></PST>")), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(got.contains(QStringLiteral("<PST><STOP>1</STOP></PST>")), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(got.contains(QStringLiteral("<PST><PARK>1</PARK></PST>")), 2000);
    c.stop();
}

void RotatorTest::n1mmParseAndSend()
{
    QCOMPARE(*N1mmRotorBackend::parseHeadingBroadcast("Rotor @ 1234", QStringLiteral("rotor")), 123.4);
    QCOMPARE(*N1mmRotorBackend::parseHeadingBroadcast("Tower 2 @ 900", QString()), 90.0);
    QVERIFY(!N1mmRotorBackend::parseHeadingBroadcast("Other @ 900", QStringLiteral("Rotor")));
    QVERIFY(!N1mmRotorBackend::parseHeadingBroadcast("garbage", QStringLiteral("Rotor")));

    QUdpSocket fake;
    QVERIFY(fake.bind(QHostAddress::LocalHost, 0));
    QStringList got;
    QObject::connect(&fake, &QUdpSocket::readyRead, [&]() {
        while (fake.hasPendingDatagrams()) {
            QByteArray buf(static_cast<int>(fake.pendingDatagramSize()), '\0');
            fake.readDatagram(buf.data(), buf.size());
            got.append(QString::fromUtf8(buf));
        }
    });

    RotatorConfig cfg;
    cfg.backend = RotatorConfig::Backend::N1mm;
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.port = fake.localPort();
    cfg.listenPort = freeUdpPort();
    cfg.n1mmRotorName = QStringLiteral("Tri<band>");
    RotatorClient c;
    QSignalSpy failed(&c, &RotatorClient::commandFailed);
    c.setConfig(cfg);
    c.start();
    QTRY_COMPARE_WITH_TIMEOUT(c.state(), RotatorClient::State::Connected, 2000);
    QVERIFY(c.turnTo(123.4));
    c.stopRotation();
    c.park();   // not in protocol → RIG_ENIMPL
    QTRY_COMPARE_WITH_TIMEOUT(got.size(), 2, 2000);
    QVERIFY(got.at(0).contains(QStringLiteral("<rotor>Tri&lt;band&gt;</rotor><goazi>123.4</goazi>")));
    QCOMPARE(got.at(1), QStringLiteral("<N1MMRotor><stop>Tri&lt;band&gt;</stop></N1MMRotor>"));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(1).toInt(), -4);

    // Heading broadcast to our status port.
    QUdpSocket bcast;
    bcast.writeDatagram("Tri<band> @ 2705", QHostAddress::LocalHost, cfg.listenPort);
    QTRY_COMPARE_WITH_TIMEOUT(c.azimuth(), 270.5, 2000);
    c.stop();
}

QTEST_GUILESS_MAIN(RotatorTest)
#include "rotator_test.moc"
