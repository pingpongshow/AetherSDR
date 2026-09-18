#include "DxClusterClient.h"
#include "LogManager.h"

#include <QRegularExpression>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <algorithm>

namespace AetherSDR {

DxClusterClient::DxClusterClient(QObject* parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_reconnectTimer(new QTimer(this))
{
    connect(m_socket, &QTcpSocket::connected,    this, &DxClusterClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &DxClusterClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,    this, &DxClusterClient::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &DxClusterClient::onSocketError);

    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &DxClusterClient::onReconnectTimer);
}

DxClusterClient::~DxClusterClient()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer->stop();
    m_logFile.close();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

QString DxClusterClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/AetherSDR/" + m_logFileName;
}

void DxClusterClient::connectToCluster(const QString& host, quint16 port, const QString& callsign)
{
    if (m_connected || m_socket->state() != QAbstractSocket::UnconnectedState) {
        qCWarning(lcDxCluster) << "DxClusterClient: already connected or connecting";
        return;
    }

    m_host = host;
    m_port = port;
    m_callsign = callsign;
    m_loggedIn = false;
    m_intentionalDisconnect = false;
    m_readBuffer.clear();

    qCDebug(lcDxCluster) << "DxClusterClient: connecting to" << host << ":" << port;
    m_socket->connectToHost(host, port);

    // Connection timeout. The generation check stops a timer left over from
    // an earlier attempt from aborting a newer one early.
    const quint64 generation = ++m_connectGeneration;
    QTimer::singleShot(ConnectTimeoutMs, this, [this, generation] {
        if (generation != m_connectGeneration)
            return;
        if (!m_connected && m_socket->state() != QAbstractSocket::ConnectedState
                && m_socket->state() != QAbstractSocket::UnconnectedState) {
            qCWarning(lcDxCluster) << "DxClusterClient: connection timeout";
            m_socket->abort();   // abort() of a connecting socket emits no signal
            emit connectionError("Connection timeout");
            if (m_reconnectAttempts > 0)
                scheduleReconnect();
        }
    });
}

void DxClusterClient::scheduleReconnect()
{
    if (m_intentionalDisconnect || m_reconnectTimer->isActive())
        return;
    // Clamp the exponent: 1 << n overflows long before the 60 s cap matters.
    const int exponent = std::min(m_reconnectAttempts, 8);
    int delay = std::min(InitialReconnectDelayMs * (1 << exponent), MaxReconnectDelayMs);
    qCDebug(lcDxCluster) << "DxClusterClient: reconnecting in" << delay << "ms";
    m_reconnectTimer->start(delay);
    m_reconnectAttempts++;
}

void DxClusterClient::disconnect()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer->stop();
    m_reconnectAttempts = 0;
    ++m_connectGeneration;
    if (m_connected) {
        m_socket->write("bye\r\n");
        m_socket->flush();
    }
    m_socket->disconnectFromHost();
}

void DxClusterClient::sendCommand(const QString& cmd)
{
    if (!m_connected) return;
    qCDebug(lcDxCluster) << "DxClusterClient TX:" << cmd;
    if (m_logFile.isOpen()) {
        m_logFile.write(("> " + cmd + "\n").toUtf8());
        m_logFile.flush();
    }
    m_socket->write((cmd + "\r\n").toLatin1());
}

// ── Socket slots ────────────────────────────────────────────────────────────

void DxClusterClient::onConnected()
{
    qCDebug(lcDxCluster) << "DxClusterClient: TCP connected to" << m_host;
    m_connected = true;
    // Backoff is reset in onDisconnected() only if the session was stable: a
    // node that accepts and immediately drops us must not be hammered every 5 s.
    m_connectedAtMs = QDateTime::currentMSecsSinceEpoch();

    // Open log file (truncate on each new connection)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- Connected to %1:%2 at %3 ---\n")
            .arg(m_host).arg(m_port)
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC"))
            .toUtf8());
        m_logFile.flush();
    }

    emit connected();
}

void DxClusterClient::onDisconnected()
{
    qCDebug(lcDxCluster) << "DxClusterClient: disconnected";
    bool wasConnected = m_connected;
    m_connected = false;
    m_loggedIn = false;

    if (wasConnected) {
        if (QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs >= StableConnectionMs)
            m_reconnectAttempts = 0;
        emit disconnected();
    }

    if (!m_intentionalDisconnect)
        scheduleReconnect();
}

void DxClusterClient::onSocketError(QAbstractSocket::SocketError /*err*/)
{
    QString msg = m_socket->errorString();
    qCWarning(lcDxCluster) << "DxClusterClient: socket error:" << msg;
    emit connectionError(msg);

    // A failed *re*connect attempt (refused, host lookup failure) never emits
    // disconnected(), so without this the auto-reconnect loop silently stopped
    // after the first failure. Initial user-initiated attempts are not retried.
    if (!m_connected && m_reconnectAttempts > 0
            && m_socket->state() == QAbstractSocket::UnconnectedState) {
        scheduleReconnect();
    }
}

void DxClusterClient::onReconnectTimer()
{
    if (m_intentionalDisconnect) return;
    qCDebug(lcDxCluster) << "DxClusterClient: attempting reconnect";
    connectToCluster(m_host, m_port, m_callsign);
}

// ── Line-buffered read ──────────────────────────────────────────────────────

void DxClusterClient::stripTelnetIAC()
{
    // Remove telnet IAC sequences (0xFF + command byte + option byte)
    int i = 0;
    while (i < m_readBuffer.size()) {
        if (static_cast<unsigned char>(m_readBuffer[i]) == 0xFF && i + 2 < m_readBuffer.size()) {
            m_readBuffer.remove(i, 3);
        } else {
            i++;
        }
    }
}

void DxClusterClient::onReadyRead()
{
    m_readBuffer.append(m_socket->readAll());
    stripTelnetIAC();
    if (m_readBuffer.size() > MaxReadBufferBytes && m_readBuffer.indexOf('\n') < 0) {
        // A node streaming without line breaks must not grow memory unbounded.
        qCWarning(lcDxCluster) << "DxClusterClient: discarding" << m_readBuffer.size()
                               << "bytes without a line terminator";
        m_readBuffer.clear();
        return;
    }

    while (true) {
        int idx = m_readBuffer.indexOf('\n');
        if (idx < 0) {
            // No newline yet — check for login prompt (may not end with \n)
            if (!m_loggedIn) {
                QString partial = QString::fromLatin1(m_readBuffer).trimmed();
                if (isLoginPrompt(partial)) {
                    qCDebug(lcDxCluster) << "DxClusterClient: login prompt detected (no newline):" << partial;
                    m_readBuffer.clear();
                    m_socket->write((m_callsign + "\r\n").toLatin1());
                    m_loggedIn = true;
                    qCDebug(lcDxCluster) << "DxClusterClient: sent callsign" << m_callsign;
                }
            }
            break;
        }

        QString line = QString::fromLatin1(m_readBuffer.left(idx)).trimmed();
        m_readBuffer.remove(0, idx + 1);

        if (line.isEmpty()) continue;

        // Write to log file
        if (m_logFile.isOpen()) {
            m_logFile.write((line + "\n").toUtf8());
            m_logFile.flush();
        }

        emit rawLineReceived(line);
        handleLine(line);
    }
}

void DxClusterClient::handleLine(const QString& line)
{
    // Login prompt detection (line-based)
    if (!m_loggedIn && isLoginPrompt(line)) {
        qCDebug(lcDxCluster) << "DxClusterClient: login prompt:" << line;
        m_socket->write((m_callsign + "\r\n").toLatin1());
        m_loggedIn = true;
        qCDebug(lcDxCluster) << "DxClusterClient: sent callsign" << m_callsign;
        return;
    }

    // Try to parse as a DX spot
    DxSpot spot;
    if (parseDxSpotLine(line, spot)) {
        qCDebug(lcDxCluster) << "DxClusterClient: spot" << spot.dxCall
                 << spot.freqMhz << "MHz de" << spot.spotterCall;
        emit spotReceived(spot);
    }
}

// ── Login prompt detection ──────────────────────────────────────────────────

bool DxClusterClient::isLoginPrompt(const QString& line) const
{
    // DX cluster servers vary: "login:", "call:", "callsign:", "Please enter your call",
    // "your call>", "Enter your callsign"
    QString lower = line.toLower();
    if (lower.endsWith("login:") || lower.endsWith("call:") || lower.endsWith("callsign:"))
        return true;
    if (lower.contains("enter your call") || lower.contains("your call"))
        return true;
    return false;
}

// ── DX spot line parser ─────────────────────────────────────────────────────

bool DxClusterClient::parseDxSpotLine(const QString& line, DxSpot& spot) const
{
    // Standard format: DX de W3LPL:     14025.0  JA1ABC       CW big signal       1824Z
    // Z is the terminator — ignore any trailing chars (some nodes append BEL/NUL)
    static const QRegularExpression rx(
        R"(^DX\s+de\s+(\S+?):\s+(\d+\.?\d*)\s+(\S+)\s+(.*?)\s+(\d{4})Z)",
        QRegularExpression::CaseInsensitiveOption);

    auto match = rx.match(line);
    if (!match.hasMatch())
        return false;

    spot.spotterCall = match.captured(1);
    double freqKhz   = match.captured(2).toDouble();
    spot.freqMhz     = freqKhz / 1000.0;
    spot.dxCall       = match.captured(3);
    spot.comment      = match.captured(4).trimmed();

    QString timeStr = match.captured(5);
    int hh = timeStr.left(2).toInt();
    int mm = timeStr.mid(2, 2).toInt();
    spot.utcTime = QTime(hh, mm);

    return spot.freqMhz > 0.0 && !spot.dxCall.isEmpty();
}

} // namespace AetherSDR
