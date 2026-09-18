#include "RotatorBackends.h"

#include <QHostInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR {

namespace {

// Hamlib rig_errcode_e values used locally (rig.h).
constexpr int kRigENIMPL   = -4;
constexpr int kRigETIMEOUT = -5;
constexpr int kRigEIO      = -6;

// Fast path for literal addresses / localhost; nullopt → needs DNS.
std::optional<QHostAddress> literalAddress(const QString& host)
{
    const QString h = host.trimmed();
    if (h.isEmpty() || h.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        return QHostAddress(QHostAddress::LocalHost);
    }
    QHostAddress a(h);
    if (!a.isNull()) {
        return a;
    }
    return std::nullopt;
}

QString fmtAz(double v)
{
    return QString::number(v, 'f', 1);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// RotctldBackend
// ═══════════════════════════════════════════════════════════════════════════

RotctldBackend::RotctldBackend(const RotatorConfig& cfg, QObject* parent)
    : RotatorBackend(cfg, parent)
{
    m_backoffMs = m_cfg.reconnectMinMs;

    m_connectTimer.setSingleShot(true);
    m_cmdTimer.setSingleShot(true);
    m_reconnectTimer.setSingleShot(true);

    m_sock.setSocketOption(QAbstractSocket::LowDelayOption, 1);

    connect(&m_sock, &QTcpSocket::connected, this, [this]() {
        m_connectTimer.stop();
        m_linkUp = true;
        m_backoffMs = m_cfg.reconnectMinMs;
        qCInfo(lcRotator) << "rotctld connected" << m_cfg.host << m_cfg.port;
        emit connected();
    });
    connect(&m_sock, &QTcpSocket::disconnected, this, [this]() {
        linkLost(QStringLiteral("rotctld closed the connection"));
    });
    connect(&m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        linkLost(QStringLiteral("rotctld: %1").arg(m_sock.errorString()));
    });
    connect(&m_sock, &QTcpSocket::readyRead, this, [this]() {
        m_rx += m_sock.readAll();
        if (m_rx.size() > kMaxRxBuffer) {
            linkLost(QStringLiteral("rotctld: receive buffer overflow (no line terminator)"));
            return;
        }
        qsizetype nl = -1;
        while (m_attemptActive && (nl = m_rx.indexOf('\n')) >= 0) {
            QByteArray raw = m_rx.left(nl);
            m_rx.remove(0, nl + 1);
            if (raw.endsWith('\r')) {
                raw.chop(1);
            }
            handleLine(QString::fromLatin1(raw));
        }
    });
    connect(&m_connectTimer, &QTimer::timeout, this, [this]() {
        linkLost(QStringLiteral("rotctld: connect timed out"));
    });
    connect(&m_cmdTimer, &QTimer::timeout, this, [this]() {
        if (!m_pending.empty()) {
            emit commandFailed(m_pending.front().name, kRigETIMEOUT,
                               RotatorClient::rprtErrorString(kRigETIMEOUT));
            m_pending.pop_front();
        }
        // The reply may still arrive later and would be matched against the
        // wrong command — resync by reconnecting.
        linkLost(QStringLiteral("rotctld: command timed out"));
    });
    connect(&m_reconnectTimer, &QTimer::timeout, this, &RotctldBackend::connectNow);
}

void RotctldBackend::open()
{
    m_wantOpen = true;
    m_backoffMs = m_cfg.reconnectMinMs;
    connectNow();
}

void RotctldBackend::close()
{
    m_wantOpen = false;
    m_reconnectTimer.stop();
    m_connectTimer.stop();
    m_cmdTimer.stop();
    m_attemptActive = false;
    m_linkUp = false;
    m_pending.clear();      // intentional close: drop silently, nobody is listening
    m_respLines.clear();
    m_sock.abort();
    m_rx.clear();
}

void RotctldBackend::connectNow()
{
    m_attemptActive = false;
    m_sock.abort();
    m_rx.clear();
    m_respLines.clear();
    m_pending.clear();
    m_linkUp = false;
    m_attemptActive = true;
    qCDebug(lcRotator) << "rotctld connecting to" << m_cfg.host << m_cfg.port;
    m_sock.connectToHost(m_cfg.host, m_cfg.port);
    m_connectTimer.start(m_cfg.connectTimeoutMs);
}

void RotctldBackend::linkLost(const QString& reason)
{
    if (!m_attemptActive) {
        return;   // already handled (error + disconnected both fire)
    }
    m_attemptActive = false;
    const bool wasUp = m_linkUp;
    m_linkUp = false;
    m_connectTimer.stop();
    m_cmdTimer.stop();
    failAllPending(reason);
    m_sock.abort();
    m_rx.clear();

    qCWarning(lcRotator) << reason << (wasUp ? "" : "(while connecting)");
    emit disconnected(reason);

    if (m_wantOpen) {
        m_reconnectTimer.start(m_backoffMs);
        m_backoffMs = std::min(m_backoffMs * 2, m_cfg.reconnectMaxMs);
    }
}

void RotctldBackend::failAllPending(const QString& reason)
{
    std::deque<Pending> pending;
    pending.swap(m_pending);
    m_respLines.clear();
    for (const Pending& p : pending) {
        if (!p.isPoll) {
            emit commandFailed(p.name, kRigEIO, reason);
        }
    }
}

void RotctldBackend::send(const QString& name, const QByteArray& wire, bool isPoll)
{
    if (!m_linkUp) {
        if (!isPoll) {
            emit commandFailed(name, kRigEIO, QStringLiteral("rotctld not connected"));
        }
        return;
    }
    if (static_cast<int>(m_pending.size()) >= kMaxOutstanding) {
        emit commandFailed(name, kRigEIO, QStringLiteral("too many outstanding commands"));
        return;
    }
    m_pending.push_back(Pending{name, isPoll});
    m_sock.write(wire);
    if (!m_cmdTimer.isActive()) {
        m_cmdTimer.start(m_cfg.commandTimeoutMs);
    }
}

void RotctldBackend::handleLine(const QString& line)
{
    if (m_pending.empty()) {
        if (!line.isEmpty()) {
            qCDebug(lcRotator) << "rotctld unsolicited line ignored:" << line;
        }
        return;
    }
    if (line.startsWith(QLatin1String("RPRT "))) {
        bool ok = false;
        const int code = line.mid(5).trimmed().toInt(&ok);
        finishFront(ok ? code : -8 /* RIG_EPROTO */);
        return;
    }
    m_respLines.append(line);
}

void RotctldBackend::finishFront(int rprt)
{
    const Pending front = m_pending.front();
    m_pending.pop_front();
    const QStringList lines = std::exchange(m_respLines, {});

    if (m_pending.empty()) {
        m_cmdTimer.stop();
    } else {
        m_cmdTimer.start(m_cfg.commandTimeoutMs);
    }

    if (rprt != 0) {
        emit commandFailed(front.name, rprt, RotatorClient::rprtErrorString(rprt));
        return;
    }
    if (!front.isPoll) {
        return;
    }

    // Extended get_pos body: "Azimuth: 123.000000" / "Elevation: 0.000000".
    std::optional<double> az;
    double el = 0.0;
    for (const QString& l : lines) {
        const qsizetype colon = l.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            continue;
        }
        const QString key = l.left(colon).trimmed();
        bool ok = false;
        const double v = l.mid(colon + 1).trimmed().toDouble(&ok);   // C locale, like rotctld's %f
        if (!ok) {
            continue;
        }
        if (key.compare(QLatin1String("Azimuth"), Qt::CaseInsensitive) == 0) {
            az = v;
        } else if (key.compare(QLatin1String("Elevation"), Qt::CaseInsensitive) == 0) {
            el = v;
        }
    }
    if (az && std::isfinite(*az)) {
        emit positionReported(*az, el);
    } else {
        emit commandFailed(front.name, -8, QStringLiteral("get_pos reply had no Azimuth"));
    }
}

void RotctldBackend::poll()
{
    const bool pollPending = std::any_of(m_pending.begin(), m_pending.end(),
                                         [](const Pending& p) { return p.isPoll; });
    if (pollPending) {
        return;
    }
    send(QStringLiteral("get_pos"), QByteArrayLiteral("+p\n"), true);
}

void RotctldBackend::setPosition(double az, double el)
{
    const QString args = fmtAz(az) + QLatin1Char(' ') + fmtAz(el);
    send(QStringLiteral("set_pos ") + args, ("+P " + args + "\n").toLatin1(), false);
}

void RotctldBackend::stop()
{
    send(QStringLiteral("stop"), QByteArrayLiteral("+S\n"), false);
}

void RotctldBackend::park()
{
    send(QStringLiteral("park"), QByteArrayLiteral("+K\n"), false);
}

// ═══════════════════════════════════════════════════════════════════════════
// PstRotatorBackend
//
// PstRotator listens for XML-ish commands on UDP 12000 and answers the
// "<PST>AZ?</PST>" query with "AZ:<deg>" sent to UDP port+1 (12001).
// ARSWIN accepts the same <PST> command set.  (PstRotator manual, "UDP
// Control".)  Verified only against the fake in tests/rotator_test.cpp.
// ═══════════════════════════════════════════════════════════════════════════

PstRotatorBackend::PstRotatorBackend(const RotatorConfig& cfg, QObject* parent)
    : RotatorBackend(cfg, parent)
{
    m_rebindTimer.setSingleShot(true);
    connect(&m_rebindTimer, &QTimer::timeout, this, &PstRotatorBackend::open);
    connect(&m_sock, &QUdpSocket::readyRead, this, &PstRotatorBackend::onReadyRead);
}

void PstRotatorBackend::open()
{
    m_open = true;
    m_alive = false;
    const quint16 listen = m_cfg.listenPort != 0 ? m_cfg.listenPort
                                                  : static_cast<quint16>(m_cfg.port + 1);
    m_sock.abort();
    if (!m_sock.bind(QHostAddress::AnyIPv4, listen,
                     QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        const QString reason = QStringLiteral("PstRotator: cannot bind UDP %1: %2")
                                   .arg(listen).arg(m_sock.errorString());
        qCWarning(lcRotator) << reason;
        emit disconnected(reason);
        m_rebindTimer.start(m_cfg.reconnectMaxMs);
        return;
    }
    resolveHost();
}

void PstRotatorBackend::resolveHost()
{
    if (const std::optional<QHostAddress> a = literalAddress(m_cfg.host)) {
        m_addr = *a;
        poll();
        return;
    }
    m_lookupId = QHostInfo::lookupHost(m_cfg.host, this, [this](const QHostInfo& info) {
        m_lookupId = -1;
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            emit disconnected(QStringLiteral("PstRotator: cannot resolve %1").arg(m_cfg.host));
            m_rebindTimer.start(m_cfg.reconnectMaxMs);
            return;
        }
        m_addr = info.addresses().constFirst();
        poll();
    });
}

void PstRotatorBackend::close()
{
    m_open = false;
    m_alive = false;
    m_rebindTimer.stop();
    if (m_lookupId >= 0) {
        QHostInfo::abortHostLookup(m_lookupId);
        m_lookupId = -1;
    }
    m_sock.abort();
    m_addr.clear();
}

void PstRotatorBackend::sendDatagram(const QString& name, const QByteArray& payload)
{
    if (m_addr.isNull()) {
        emit commandFailed(name, kRigEIO, QStringLiteral("PstRotator host not resolved"));
        return;
    }
    m_sock.writeDatagram(payload, m_addr, m_cfg.port);
}

void PstRotatorBackend::poll()
{
    if (!m_open || m_addr.isNull()) {
        return;
    }
    // Liveness: no AZ reply within ~3 poll periods → report link down.
    const qint64 staleMs = 3LL * m_cfg.pollIntervalMs + m_cfg.commandTimeoutMs;
    if (m_alive && m_lastReply.isValid() && m_lastReply.elapsed() > staleMs) {
        m_alive = false;
        emit disconnected(QStringLiteral("PstRotator: no reply to AZ? query"));
    }
    m_sock.writeDatagram(QByteArrayLiteral("<PST>AZ?</PST>"), m_addr, m_cfg.port);
}

void PstRotatorBackend::setPosition(double az, double /*el*/)
{
    sendDatagram(QStringLiteral("set_pos %1").arg(fmtAz(az)),
                 QStringLiteral("<PST><AZIMUTH>%1</AZIMUTH></PST>")
                     .arg(QString::number(std::lround(az))).toLatin1());
}

void PstRotatorBackend::stop()
{
    sendDatagram(QStringLiteral("stop"), QByteArrayLiteral("<PST><STOP>1</STOP></PST>"));
}

void PstRotatorBackend::park()
{
    sendDatagram(QStringLiteral("park"), QByteArrayLiteral("<PST><PARK>1</PARK></PST>"));
}

std::optional<double> PstRotatorBackend::parseAzReply(const QByteArray& datagram)
{
    static const QRegularExpression re(QStringLiteral(R"(AZ\s*:\s*(-?\d+(?:\.\d+)?))"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(datagram));
    if (!m.hasMatch()) {
        return std::nullopt;
    }
    return m.captured(1).toDouble();
}

void PstRotatorBackend::onReadyRead()
{
    while (m_sock.hasPendingDatagrams()) {
        QByteArray buf(static_cast<int>(std::max<qint64>(m_sock.pendingDatagramSize(), 0)), '\0');
        m_sock.readDatagram(buf.data(), buf.size());
        const std::optional<double> az = parseAzReply(buf);
        if (!az) {
            continue;
        }
        m_lastReply.start();
        if (!m_alive) {
            m_alive = true;
            emit connected();
        }
        emit positionReported(*az, 0.0);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// N1mmRotorBackend
//
// N1MM Logger+ drives rotor programs (N1MM Rotor, PstRotator) by sending
// <N1MMRotor> XML to UDP 12040; the rotor program broadcasts its heading as
// "<rotor name> @ <heading × 10>" on UDP 13010.  (N1MM Logger+ docs,
// "Rotor Control".)  There is no position query, so the link is reported
// up as soon as the sockets are ready.
// ═══════════════════════════════════════════════════════════════════════════

N1mmRotorBackend::N1mmRotorBackend(const RotatorConfig& cfg, QObject* parent)
    : RotatorBackend(cfg, parent)
{
    connect(&m_rx, &QUdpSocket::readyRead, this, &N1mmRotorBackend::onReadyRead);
}

void N1mmRotorBackend::open()
{
    const quint16 listen = m_cfg.listenPort != 0 ? m_cfg.listenPort : kDefaultStatusPort;
    m_rx.abort();
    if (!m_rx.bind(QHostAddress::AnyIPv4, listen,
                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        // Sending still works without the status listener.
        qCWarning(lcRotator) << "N1MM rotor: cannot bind status port" << listen << m_rx.errorString();
    }
    resolveHost();
}

void N1mmRotorBackend::resolveHost()
{
    if (const std::optional<QHostAddress> a = literalAddress(m_cfg.host)) {
        m_addr = *a;
        emit connected();
        return;
    }
    m_lookupId = QHostInfo::lookupHost(m_cfg.host, this, [this](const QHostInfo& info) {
        m_lookupId = -1;
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            emit disconnected(QStringLiteral("N1MM rotor: cannot resolve %1").arg(m_cfg.host));
            return;
        }
        m_addr = info.addresses().constFirst();
        emit connected();
    });
}

void N1mmRotorBackend::close()
{
    if (m_lookupId >= 0) {
        QHostInfo::abortHostLookup(m_lookupId);
        m_lookupId = -1;
    }
    m_rx.abort();
    m_tx.abort();
    m_addr.clear();
}

void N1mmRotorBackend::sendDatagram(const QString& name, const QByteArray& payload)
{
    if (m_addr.isNull()) {
        emit commandFailed(name, kRigEIO, QStringLiteral("N1MM rotor host not resolved"));
        return;
    }
    m_tx.writeDatagram(payload, m_addr, m_cfg.port);
}

void N1mmRotorBackend::setPosition(double az, double /*el*/)
{
    const QString rotor = m_cfg.n1mmRotorName.toHtmlEscaped();
    sendDatagram(QStringLiteral("set_pos %1").arg(fmtAz(az)),
                 QStringLiteral("<N1MMRotor><rotor>%1</rotor><goazi>%2</goazi>"
                                "<offset>0.0</offset><bidirectional>0</bidirectional>"
                                "<freqband>0</freqband></N1MMRotor>")
                     .arg(rotor, fmtAz(az)).toUtf8());
}

void N1mmRotorBackend::stop()
{
    sendDatagram(QStringLiteral("stop"),
                 QStringLiteral("<N1MMRotor><stop>%1</stop></N1MMRotor>")
                     .arg(m_cfg.n1mmRotorName.toHtmlEscaped()).toUtf8());
}

void N1mmRotorBackend::park()
{
    emit commandFailed(QStringLiteral("park"), kRigENIMPL,
                       QStringLiteral("Park is not part of the N1MM rotor protocol"));
}

std::optional<double> N1mmRotorBackend::parseHeadingBroadcast(const QByteArray& datagram,
                                                              const QString& name)
{
    const QString text = QString::fromUtf8(datagram).trimmed();
    const qsizetype at = text.lastIndexOf(QLatin1Char('@'));
    if (at < 0) {
        return std::nullopt;
    }
    const QString who = text.left(at).trimmed();
    if (!name.isEmpty() && who.compare(name.trimmed(), Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }
    bool ok = false;
    const double tenths = text.mid(at + 1).trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(tenths)) {
        return std::nullopt;
    }
    return tenths / 10.0;
}

void N1mmRotorBackend::onReadyRead()
{
    while (m_rx.hasPendingDatagrams()) {
        QByteArray buf(static_cast<int>(std::max<qint64>(m_rx.pendingDatagramSize(), 0)), '\0');
        m_rx.readDatagram(buf.data(), buf.size());
        if (const std::optional<double> az = parseHeadingBroadcast(buf, m_cfg.n1mmRotorName)) {
            emit positionReported(*az, 0.0);
        }
    }
}

} // namespace AetherSDR
