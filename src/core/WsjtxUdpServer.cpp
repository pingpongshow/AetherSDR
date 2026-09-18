#include "WsjtxUdpServer.h"

#include "AppSettings.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMetaType>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>
#include <utility>

Q_LOGGING_CATEGORY(lcWsjtx, "aether.wsjtx")

namespace AetherSDR {

namespace {

constexpr int kHousekeepingMs = 1000;
// Reference server (UDPExamples/MessageServer.cpp) drops a client after one
// silent pulse; WSJT-X heartbeats every pulse (15 s), so we allow two to
// ride out a single lost datagram.
constexpr qint64 kInstanceTimeoutMs = 2LL * Wsjtx::kPulseSeconds * 1000;
constexpr qint64 kSpotExpirySlackMs = 3000;   // decode lands ~2-3 s after period end

QString spotKey(const QString& instanceId, const QString& call)
{
    return instanceId + QLatin1Char('\n') + call;
}

Wsjtx::Color toWire(const QColor& c)
{
    if (!c.isValid()) {
        return Wsjtx::Color::invalid();
    }
    return Wsjtx::Color::rgb(static_cast<quint8>(c.red()), static_cast<quint8>(c.green()),
                             static_cast<quint8>(c.blue()), static_cast<quint8>(c.alpha()));
}

bool isServerToClientType(Wsjtx::Type t)
{
    switch (t) {
    case Wsjtx::Type::Reply:
    case Wsjtx::Type::Replay:
    case Wsjtx::Type::HaltTx:
    case Wsjtx::Type::FreeText:
    case Wsjtx::Type::Location:
    case Wsjtx::Type::HighlightCallsign:
    case Wsjtx::Type::SwitchConfiguration:
    case Wsjtx::Type::Configure:
    case Wsjtx::Type::AnnotationInfo:
        return true;
    default:
        return false;
    }
}

QString normaliseArgb(const QString& color, const QString& fallback)
{
    QString c = color.trimmed();
    if (c.length() == 7 && c.startsWith(QLatin1Char('#'))) {
        c = QStringLiteral("#FF") + c.mid(1);
    }
    if (c.length() != 9 || !c.startsWith(QLatin1Char('#'))) {
        return fallback;
    }
    return c.toUpper();
}

} // namespace

WsjtxUdpServer::WsjtxUdpServer(QObject* parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_relaySocket(new QUdpSocket(this))
    , m_housekeeping(new QTimer(this))
{
    qRegisterMetaType<WsjtxDecodeEvent>("WsjtxDecodeEvent");
    qRegisterMetaType<Wsjtx::Status>("Wsjtx::Status");
    qRegisterMetaType<Wsjtx::WsprDecode>("Wsjtx::WsprDecode");
    qRegisterMetaType<Wsjtx::QsoLogged>("Wsjtx::QsoLogged");

    connect(m_socket, &QUdpSocket::readyRead, this, &WsjtxUdpServer::onReadyRead);
    connect(m_relaySocket, &QUdpSocket::readyRead, this, &WsjtxUdpServer::onRelayReadyRead);
    m_housekeeping->setInterval(kHousekeepingMs);
    connect(m_housekeeping, &QTimer::timeout, this, &WsjtxUdpServer::onHousekeeping);
}

WsjtxUdpServer::~WsjtxUdpServer()
{
    // Don't emit into a half-destroyed receiver graph; just close sockets.
    m_housekeeping->stop();
    m_socket->close();
    m_relaySocket->close();
}

bool WsjtxUdpServer::isListening() const
{
    return m_socket->state() == QAbstractSocket::BoundState;
}

bool WsjtxUdpServer::start(quint16 port, const QString& multicastGroup)
{
    QHostAddress group;
    if (!multicastGroup.trimmed().isEmpty()) {
        group = QHostAddress(multicastGroup.trimmed());
        if (group.isNull() || !group.isMulticast()) {
            const QString err = QStringLiteral("Invalid multicast group address: %1").arg(multicastGroup);
            qCWarning(lcWsjtx) << err;
            emit errorOccurred(err);
            return false;
        }
    }
    if (port == 0) {
        port = Wsjtx::kDefaultPort;
    }
    if (isListening() && port == m_port && group == m_group) {
        return true;
    }
    stop();

    // Same bind strategy as the reference MessageServer: AnyIPv4 when joining
    // an IPv4 group, otherwise Any; ShareAddress|ReuseAddressHint so that
    // several listeners can coexist on a multicast port.
    const QHostAddress bindAddr =
        (!group.isNull() && group.protocol() == QAbstractSocket::IPv4Protocol)
            ? QHostAddress(QHostAddress::AnyIPv4)
            : QHostAddress(QHostAddress::Any);
    if (!m_socket->bind(bindAddr, port,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        const QString err = QStringLiteral("WSJT-X UDP: cannot bind port %1: %2")
                                .arg(port).arg(m_socket->errorString());
        qCWarning(lcWsjtx) << err;
        emit errorOccurred(err);
        return false;
    }
    m_port = port;
    m_group = group;

    if (!m_group.isNull()) {
        m_socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
        int joined = 0;
        const auto ifaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface& iface : ifaces) {
            const auto f = iface.flags();
            if ((f & QNetworkInterface::IsUp) && (f & QNetworkInterface::CanMulticast)) {
                if (m_socket->joinMulticastGroup(m_group, iface)) {
                    ++joined;
                }
            }
        }
        if (joined == 0 && !m_socket->joinMulticastGroup(m_group)) {
            qCWarning(lcWsjtx) << "WSJT-X UDP: failed to join multicast group" << m_group
                               << m_socket->errorString();
            emit errorOccurred(QStringLiteral("Failed to join multicast group %1")
                                   .arg(m_group.toString()));
        }
    }

    if (m_relaySocket->state() != QAbstractSocket::BoundState) {
        // Ephemeral port: relay targets see us as a "WSJT-X" client here and
        // send their Reply/Highlight traffic back to it.
        m_relaySocket->bind(QHostAddress(QHostAddress::AnyIPv4), 0);
    }

    m_housekeeping->start();
    qCInfo(lcWsjtx) << "WSJT-X UDP server listening on port" << m_port
                    << (m_group.isNull() ? QStringLiteral("(unicast)") : m_group.toString());
    emit listeningChanged(true);
    return true;
}

void WsjtxUdpServer::stop()
{
    const bool was = isListening();
    if (was && !m_group.isNull()) {
        m_socket->leaveMulticastGroup(m_group);
    }
    m_socket->close();
    m_relaySocket->close();
    m_housekeeping->stop();
    flushPendingQsos(true);
    clearSpots();
    const QStringList ids = m_instances.keys();
    for (const QString& id : ids) {
        removeInstance(id);
    }
    if (was) {
        emit listeningChanged(false);
    }
}

void WsjtxUdpServer::applySettings()
{
    auto& s = AppSettings::instance();
    setSpotsEnabled(s.value(QStringLiteral("WsjtxSpotsEnabled"), QStringLiteral("True")).toString()
                    == QLatin1String("True"));
    setCqOnly(s.value(QStringLiteral("WsjtxSpotsCqOnly"), QStringLiteral("False")).toString()
              == QLatin1String("True"));
    setSpotLifetimeCycles(s.value(QStringLiteral("WsjtxSpotLifetimeCycles"), 2).toInt());
    setSpotColors(s.value(QStringLiteral("WsjtxSpotColor"), QStringLiteral("#FF7FDBFF")).toString(),
                  s.value(QStringLiteral("WsjtxCqSpotColor"), QStringLiteral("#FF2ECC40")).toString());
    setRelayTargets(parseRelayTargets(
        s.value(QStringLiteral("WsjtxUdpRelayTargets"), QString()).toString()));

    const bool enabled = s.value(QStringLiteral("WsjtxUdpEnabled"), QStringLiteral("False")).toString()
                         == QLatin1String("True");
    if (!enabled) {
        stop();
        return;
    }
    const int port = s.value(QStringLiteral("WsjtxUdpPort"), int(Wsjtx::kDefaultPort)).toInt();
    const QString group = s.value(QStringLiteral("WsjtxUdpMulticastGroup"), QString()).toString();
    start(static_cast<quint16>(port > 0 && port < 65536 ? port : Wsjtx::kDefaultPort), group);
}

QList<WsjtxRelayTarget> WsjtxUdpServer::parseRelayTargets(const QString& csv)
{
    QList<WsjtxRelayTarget> out;
    const QStringList parts = csv.split(QRegularExpression(QStringLiteral("[,;\\s]+")),
                                        Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        const int colon = p.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0) {
            continue;
        }
        bool ok = false;
        const int port = p.mid(colon + 1).toInt(&ok);
        QString host = p.left(colon);
        if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']'))) {
            host = host.mid(1, host.size() - 2);
        }
        QHostAddress addr(host);
        if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) {
            addr = QHostAddress(QHostAddress::LocalHost);
        }
        if (!ok || port <= 0 || port > 65535 || addr.isNull()) {
            qCWarning(lcWsjtx) << "Ignoring bad WSJT-X relay target" << p;
            continue;
        }
        WsjtxRelayTarget t{addr, static_cast<quint16>(port)};
        if (!out.contains(t)) {
            out.append(t);
        }
    }
    return out;
}

void WsjtxUdpServer::setRelayTargets(const QList<WsjtxRelayTarget>& targets)
{
    m_relayTargets.clear();
    for (const WsjtxRelayTarget& t : targets) {
        // Never relay to ourselves (would loop forever).
        if (t.port == m_port && (t.address.isLoopback() || t.address == QHostAddress::Any
                                 || t.address == QHostAddress::AnyIPv4)) {
            qCWarning(lcWsjtx) << "Ignoring WSJT-X relay target that points at our own port" << t.port;
            continue;
        }
        m_relayTargets.append(t);
    }
}

void WsjtxUdpServer::setSpotsEnabled(bool on)
{
    if (m_spotsEnabled == on) {
        return;
    }
    m_spotsEnabled = on;
    if (!on) {
        clearSpots();
    }
}

void WsjtxUdpServer::setSpotColors(const QString& normalArgb, const QString& cqArgb)
{
    m_spotColor = normaliseArgb(normalArgb, QStringLiteral("#FF7FDBFF"));
    m_cqSpotColor = normaliseArgb(cqArgb, QStringLiteral("#FF2ECC40"));
}

// ── Receive path ────────────────────────────────────────────────────────────

void WsjtxUdpServer::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        if (!dg.isValid()) {
            continue;
        }
        const QByteArray data = dg.data();
        relay(data);
        handleDatagram(data, dg.senderAddress(), static_cast<quint16>(dg.senderPort()));
    }
}

void WsjtxUdpServer::relay(const QByteArray& datagram)
{
    if (m_relayTargets.isEmpty() || m_relaySocket->state() != QAbstractSocket::BoundState) {
        return;
    }
    // Forward only client→server traffic; server→client types seen on a
    // shared multicast group are not ours to repeat.
    Wsjtx::Type type = Wsjtx::Type::Unknown;
    if (Wsjtx::peekHeader(datagram, nullptr, &type, nullptr) && isServerToClientType(type)) {
        return;
    }
    for (const WsjtxRelayTarget& t : std::as_const(m_relayTargets)) {
        if (t.port == m_port && t.address.isLoopback()) {
            continue;   // port may have changed since setRelayTargets(); avoid a loop
        }
        m_relaySocket->writeDatagram(datagram, t.address, t.port);
    }
}

void WsjtxUdpServer::onRelayReadyRead()
{
    // Replies from GridTracker/JTAlert etc. — route to the WSJT-X instance
    // named in the header.  Relay-target heartbeats (schema negotiation
    // replies) are forwarded too; WSJT-X only ever lowers/raises its schema
    // to what the peer supports, which is harmless.
    while (m_relaySocket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_relaySocket->receiveDatagram();
        if (!dg.isValid()) {
            continue;
        }
        QString id;
        if (!Wsjtx::peekHeader(dg.data(), nullptr, nullptr, &id)) {
            continue;
        }
        auto it = m_instances.constFind(id);
        if (it == m_instances.constEnd()) {
            continue;
        }
        m_socket->writeDatagram(dg.data(), it->address, it->port);
    }
}

void WsjtxUdpServer::handleDatagram(const QByteArray& datagram, const QHostAddress& sender,
                                    quint16 senderPort)
{
    QString err;
    const std::optional<Wsjtx::Message> msg = Wsjtx::decode(datagram, &err);
    if (!msg) {
        qCDebug(lcWsjtx) << "Ignoring non-WSJT-X datagram from" << sender << ":" << err;
        return;
    }
    if (msg->type == Wsjtx::Type::Unknown || isServerToClientType(msg->type)) {
        return;  // spec rule: silently ignore unknown types
    }
    if (msg->id.isEmpty()) {
        return;
    }

    bool isNew = false;
    WsjtxInstance& inst = touchInstance(*msg, sender, senderPort, &isNew);
    const QString id = inst.id;

    switch (msg->type) {
    case Wsjtx::Type::Heartbeat: {
        const auto& hb = std::get<Wsjtx::Heartbeat>(msg->body);
        if (!hb.version.isEmpty()) {
            inst.version = hb.version;
        }
        if (!hb.revision.isEmpty()) {
            inst.revision = hb.revision;
        }
        break;
    }
    case Wsjtx::Type::Status: {
        const auto& st = std::get<Wsjtx::Status>(msg->body);
        const bool bandChanged = inst.hasStatus
                                 && inst.status.dialFrequencyHz != st.dialFrequencyHz;
        inst.status = st;
        inst.hasStatus = true;
        if (bandChanged) {
            // Spots were placed relative to the old dial frequency.
            removeSpotsForInstance(id);
        }
        emit instanceStatusChanged(id, st);
        break;
    }
    case Wsjtx::Type::Decode:
        handleDecode(id, std::get<Wsjtx::Decode>(msg->body));
        break;
    case Wsjtx::Type::Clear:
        handleClear(id);
        break;
    case Wsjtx::Type::QsoLogged: {
        const auto& q = std::get<Wsjtx::QsoLogged>(msg->body);
        emit qsoLoggedDetails(id, q);
        // WSJT-X sends QSOLogged immediately followed by LoggedADIF
        // (widgets/mainwindow.cpp acceptQSO).  Hold the synthesised record
        // briefly; LoggedADIF supersedes it so the log gets exactly one copy.
        flushPendingQsos(false);
        m_pendingQso[id] = PendingQso{Wsjtx::adifFromQsoLogged(q),
                                      QDateTime::currentMSecsSinceEpoch() + kPendingQsoLoggedMs};
        QTimer::singleShot(kPendingQsoLoggedMs + 50, this, [this] { flushPendingQsos(false); });
        break;
    }
    case Wsjtx::Type::LoggedAdif: {
        const auto& a = std::get<Wsjtx::LoggedAdif>(msg->body);
        m_pendingQso.remove(id);
        const QString rec = Wsjtx::adifRecordFromLoggedAdif(a.adif);
        if (!rec.isEmpty()) {
            emit qsoLogged(rec);
        }
        break;
    }
    case Wsjtx::Type::Close:
        removeInstance(id);
        return;
    case Wsjtx::Type::WsprDecode: {
        const auto& w = std::get<Wsjtx::WsprDecode>(msg->body);
        emit wsprDecodeReceived(id, w);
        if (m_spotsEnabled && !m_cqOnly && w.isNew && !w.offAir
            && Wsjtx::looksLikeCallsign(w.callsign.toUpper())) {
            WsjtxDecodeEvent ev;
            ev.instanceId = id;
            ev.modeName = QStringLiteral("WSPR");
            ev.parsed.deCall = w.callsign.toUpper();
            ev.parsed.grid = w.grid;
            ev.decode.snr = w.snr;
            ev.decode.time = w.time;
            ev.decode.message = QStringLiteral("%1 %2 %3").arg(w.callsign, w.grid).arg(w.powerDbm);
            ev.dialFrequencyHz = inst.status.dialFrequencyHz;
            // WSPR frequency is absolute RF (Radio::frequency(MHz)).
            ev.rfFrequencyHz = w.frequencyHz;
            ev.receivedMs = QDateTime::currentMSecsSinceEpoch();
            updateSpot(ev);
        }
        break;
    }
    default:
        break;
    }
}

WsjtxInstance& WsjtxUdpServer::touchInstance(const Wsjtx::Message& msg, const QHostAddress& sender,
                                             quint16 senderPort, bool* isNew)
{
    auto it = m_instances.find(msg.id);
    *isNew = (it == m_instances.end());
    if (*isNew) {
        WsjtxInstance inst;
        inst.id = msg.id;
        inst.schema = msg.schema;
        if (msg.type == Wsjtx::Type::Heartbeat) {
            const auto& hb = std::get<Wsjtx::Heartbeat>(msg.body);
            // Negotiate min(ours, theirs) — MessageServer.cpp parse_message().
            inst.schema = std::min(Wsjtx::kMaxSchema, qMax(hb.maxSchema, Wsjtx::kPreNegotiationSchema));
        }
        it = m_instances.insert(msg.id, inst);
    }
    it->address = sender;
    it->port = senderPort;
    it->lastSeenMs = QDateTime::currentMSecsSinceEpoch();

    if (*isNew) {
        qCInfo(lcWsjtx) << "WSJT-X instance discovered:" << msg.id << "at" << sender << senderPort
                        << "schema" << it->schema;
        if (msg.type == Wsjtx::Type::Heartbeat) {
            // Tell the client which schema we settled on.
            Wsjtx::Heartbeat hb;
            hb.maxSchema = Wsjtx::kMaxSchema;
            hb.version = QCoreApplication::applicationName().isEmpty()
                             ? QStringLiteral("AetherSDR")
                             : QCoreApplication::applicationName();
            hb.revision = QCoreApplication::applicationVersion();
            m_socket->writeDatagram(Wsjtx::encode(msg.id, hb, it->schema), sender, senderPort);
        }
        emit instanceAdded(msg.id);
        // Ask for the band-activity backlog + a fresh Status so we learn the
        // dial frequency without waiting for the next state change.
        sendTo(msg.id, Wsjtx::Replay{});
    }
    return *it;
}

void WsjtxUdpServer::handleDecode(const QString& id, const Wsjtx::Decode& d)
{
    const WsjtxInstance& inst = m_instances[id];

    WsjtxDecodeEvent ev;
    ev.instanceId = id;
    ev.decode = d;
    ev.parsed = Wsjtx::parseMessageText(d.message);
    ev.modeName = Wsjtx::modeFromSymbol(d.mode);
    if (ev.modeName.isEmpty() && inst.hasStatus) {
        ev.modeName = inst.status.mode.toUpper();
    }
    ev.dialFrequencyHz = inst.hasStatus ? inst.status.dialFrequencyHz : 0;
    ev.rfFrequencyHz = ev.dialFrequencyHz > 0 ? ev.dialFrequencyHz + d.deltaFrequency : 0;
    ev.receivedMs = QDateTime::currentMSecsSinceEpoch();

    const bool ownCall = inst.hasStatus && !inst.status.deCall.isEmpty()
                         && ev.parsed.deCall.compare(inst.status.deCall, Qt::CaseInsensitive) == 0;
    if (m_spotsEnabled && d.isNew && !d.offAir && !d.lowConfidence && !ownCall
        && !ev.parsed.deCall.isEmpty() && ev.rfFrequencyHz > 0
        && (!m_cqOnly || ev.parsed.isCq)) {
        updateSpot(ev);
    }

    m_recent.append(ev);
    if (m_recent.size() > kMaxRecentDecodes) {
        m_recent.remove(0, m_recent.size() - kMaxRecentDecodes);
    }
    emit decodeReceived(ev);
}

void WsjtxUdpServer::handleClear(const QString& id)
{
    // Prior decodes can no longer be Reply'd to (spec: Clear).
    m_recent.erase(std::remove_if(m_recent.begin(), m_recent.end(),
                                  [&id](const WsjtxDecodeEvent& e) { return e.instanceId == id; }),
                   m_recent.end());
    removeSpotsForInstance(id);
    emit decodesCleared(id);
}

void WsjtxUdpServer::removeInstance(const QString& id)
{
    if (!m_instances.contains(id)) {
        return;
    }
    removeSpotsForInstance(id);
    m_recent.erase(std::remove_if(m_recent.begin(), m_recent.end(),
                                  [&id](const WsjtxDecodeEvent& e) { return e.instanceId == id; }),
                   m_recent.end());
    m_instances.remove(id);
    qCInfo(lcWsjtx) << "WSJT-X instance gone:" << id;
    emit instanceRemoved(id);
}

// ── Spots ──────────────────────────────────────────────────────────────────

int WsjtxUdpServer::allocateSpotId()
{
    for (int tries = 0; tries < kSpotIdSpan; ++tries) {
        const int id = kSpotIdBase + m_nextSpotOffset;
        m_nextSpotOffset = (m_nextSpotOffset + 1) % kSpotIdSpan;
        if (!m_spotKeyById.contains(id)) {
            return id;
        }
    }
    return kSpotIdBase;
}

double WsjtxUdpServer::periodSecondsFor(const QString& instanceId, const QString& mode) const
{
    auto it = m_instances.constFind(instanceId);
    if (it != m_instances.constEnd() && it->hasStatus
        && it->status.trPeriod != Wsjtx::kNotApplicable && it->status.trPeriod > 0
        && mode != QLatin1String("WSPR")) {
        return static_cast<double>(it->status.trPeriod);
    }
    return Wsjtx::nominalPeriodSeconds(mode);
}

void WsjtxUdpServer::updateSpot(WsjtxDecodeEvent& ev)
{
    const QString call = ev.parsed.deCall;
    const QString key = spotKey(ev.instanceId, call);
    auto it = m_spotsByKey.find(key);
    if (it == m_spotsByKey.end()) {
        SpotEntry e;
        e.spotId = allocateSpotId();
        e.instanceId = ev.instanceId;
        e.callsign = call;
        it = m_spotsByKey.insert(key, e);
        m_spotKeyById.insert(e.spotId, key);
    }
    const double period = periodSecondsFor(ev.instanceId, ev.modeName);
    const qint64 lifetimeMs = static_cast<qint64>(std::ceil(period * m_lifetimeCycles * 1000.0));
    it->expiresMs = ev.receivedMs + lifetimeMs + kSpotExpirySlackMs;
    ev.spotId = it->spotId;
    it->lastDecode = ev;

    const QString freq = QString::number(static_cast<double>(ev.rfFrequencyHz) / 1e6, 'f', 6);
    QString comment = QStringLiteral("%1 dB  %2").arg(ev.decode.snr).arg(ev.decode.message.simplified());

    QMap<QString, QString> kvs;
    kvs.insert(QStringLiteral("callsign"), call);
    kvs.insert(QStringLiteral("rx_freq"), freq);
    kvs.insert(QStringLiteral("tx_freq"), freq);
    kvs.insert(QStringLiteral("mode"), ev.modeName);
    kvs.insert(QStringLiteral("source"), kSpotSource);
    kvs.insert(QStringLiteral("color"), ev.parsed.isCq ? m_cqSpotColor : m_spotColor);
    kvs.insert(QStringLiteral("comment"), comment);
    kvs.insert(QStringLiteral("timestamp"),
               QString::number(ev.receivedMs / 1000));
    kvs.insert(QStringLiteral("lifetime_seconds"),
               QString::number((lifetimeMs + kSpotExpirySlackMs + 999) / 1000));
    auto inst = m_instances.constFind(ev.instanceId);
    if (inst != m_instances.constEnd() && inst->hasStatus && !inst->status.deCall.isEmpty()) {
        kvs.insert(QStringLiteral("spotter_callsign"), inst->status.deCall);
    }
    emit spotUpsert(it->spotId, kvs);
}

void WsjtxUdpServer::removeSpotsForInstance(const QString& id)
{
    for (auto it = m_spotsByKey.begin(); it != m_spotsByKey.end();) {
        if (it->instanceId == id) {
            const int spotId = it->spotId;
            m_spotKeyById.remove(spotId);
            it = m_spotsByKey.erase(it);
            emit spotRemove(spotId);
        } else {
            ++it;
        }
    }
}

void WsjtxUdpServer::clearSpots()
{
    const QList<int> ids = m_spotKeyById.keys();
    m_spotKeyById.clear();
    m_spotsByKey.clear();
    for (int id : ids) {
        emit spotRemove(id);
    }
}

void WsjtxUdpServer::onHousekeeping()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (auto it = m_spotsByKey.begin(); it != m_spotsByKey.end();) {
        if (now >= it->expiresMs) {
            const int spotId = it->spotId;
            m_spotKeyById.remove(spotId);
            it = m_spotsByKey.erase(it);
            emit spotRemove(spotId);
        } else {
            ++it;
        }
    }

    QStringList stale;
    for (auto it = m_instances.cbegin(); it != m_instances.cend(); ++it) {
        if (now - it->lastSeenMs > kInstanceTimeoutMs) {
            stale.append(it.key());
        }
    }
    for (const QString& id : stale) {
        removeInstance(id);
    }

    flushPendingQsos(false);
}

void WsjtxUdpServer::flushPendingQsos(bool force)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_pendingQso.begin(); it != m_pendingQso.end();) {
        if (force || now >= it->deadlineMs) {
            const QString adif = it->adif;
            it = m_pendingQso.erase(it);
            emit qsoLogged(adif);
        } else {
            ++it;
        }
    }
}

// ── Send path (messages to WSJT-X only) ─────────────────────────────────────

bool WsjtxUdpServer::sendTo(const QString& instanceId, const Wsjtx::Body& body)
{
    auto it = m_instances.constFind(instanceId);
    if (it == m_instances.constEnd() || !isListening()) {
        return false;
    }
    const QByteArray dg = Wsjtx::encode(instanceId, body, it->schema);
    if (dg.isEmpty()) {
        return false;
    }
    return m_socket->writeDatagram(dg, it->address, it->port) == dg.size();
}

bool WsjtxUdpServer::sendReply(const WsjtxDecodeEvent& ev, quint8 modifiers)
{
    // WSJT-X matches the reply against its band-activity text exactly
    // (mainwindow.cpp replyToCQ), so echo the Decode fields verbatim.
    Wsjtx::Reply r;
    r.time = ev.decode.time;
    r.snr = ev.decode.snr;
    r.deltaTime = ev.decode.deltaTime;
    r.deltaFrequency = ev.decode.deltaFrequency;
    r.mode = ev.decode.mode;
    r.message = ev.decode.message;
    r.lowConfidence = ev.decode.lowConfidence;
    r.modifiers = modifiers;
    const bool ok = sendTo(ev.instanceId, r);
    qCInfo(lcWsjtx) << "Reply to" << ev.instanceId << ev.decode.message << (ok ? "sent" : "FAILED");
    return ok;
}

bool WsjtxUdpServer::replyToSpot(int spotId, quint8 modifiers)
{
    const auto keyIt = m_spotKeyById.constFind(spotId);
    if (keyIt == m_spotKeyById.constEnd()) {
        return false;
    }
    const auto it = m_spotsByKey.constFind(*keyIt);
    if (it == m_spotsByKey.constEnd() || it->lastDecode.modeName == QLatin1String("WSPR")) {
        return false;   // WSPR decodes cannot be replied to
    }
    return sendReply(it->lastDecode, modifiers);
}

bool WsjtxUdpServer::sendHaltTx(const QString& instanceId, bool autoTxOnly)
{
    return sendTo(instanceId, Wsjtx::HaltTx{autoTxOnly});
}

bool WsjtxUdpServer::sendFreeText(const QString& instanceId, const QString& text, bool send)
{
    return sendTo(instanceId, Wsjtx::FreeText{text, send});
}

bool WsjtxUdpServer::sendLocation(const QString& instanceId, const QString& grid)
{
    static const QRegularExpression kLocator(
        QStringLiteral("^[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?$"));
    if (!kLocator.match(grid.trimmed()).hasMatch()) {
        return false;   // spec: only 4- or 6-char Maidenhead accepted
    }
    return sendTo(instanceId, Wsjtx::Location{grid.trimmed()});
}

bool WsjtxUdpServer::sendHighlightCallsign(const QString& instanceId, const QString& callsign,
                                           const QColor& background, const QColor& foreground,
                                           bool highlightLast)
{
    const QString call = callsign.trimmed().toUpper();
    if (call.isEmpty()) {
        return false;
    }
    const bool clearing = !background.isValid() || !foreground.isValid();
    if (clearing) {
        m_highlights.remove(call);
    } else {
        if (!m_highlights.contains(call) && m_highlights.size() >= kMaxHighlights) {
            qCDebug(lcWsjtx) << "HighlightCallsign cap reached; skipping" << call;
            return false;
        }
        m_highlights.insert(call, qMakePair(background, foreground));
    }
    Wsjtx::HighlightCallsign h;
    h.callsign = call;
    h.background = toWire(background);
    h.foreground = toWire(foreground);
    h.highlightLast = highlightLast;

    if (!instanceId.isEmpty()) {
        return sendTo(instanceId, h);
    }
    bool any = false;
    const QStringList ids = m_instances.keys();
    for (const QString& id : ids) {
        any = sendTo(id, h) || any;
    }
    return any;
}

void WsjtxUdpServer::clearAllHighlights()
{
    // Invalid colours per call remove the on-screen highlight; "CLEARALL!"
    // additionally drops WSJT-X's internal table (upstream master only —
    // older builds treat it as a no-op highlight of a non-existent call).
    const QStringList calls = m_highlights.keys();
    for (const QString& c : calls) {
        sendHighlightCallsign(QString(), c, QColor(), QColor());
    }
    m_highlights.clear();
    Wsjtx::HighlightCallsign all;
    all.callsign = QString::fromLatin1(Wsjtx::kClearAllHighlights);
    const QStringList ids = m_instances.keys();
    for (const QString& id : ids) {
        sendTo(id, all);
    }
}

bool WsjtxUdpServer::sendReplay(const QString& instanceId)
{
    return sendTo(instanceId, Wsjtx::Replay{});
}

bool WsjtxUdpServer::sendClear(const QString& instanceId, quint8 window)
{
    Wsjtx::Clear c;
    c.window = window;
    return sendTo(instanceId, c);
}

} // namespace AetherSDR
