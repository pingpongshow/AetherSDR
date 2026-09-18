#include "CatKenwoodServer.h"

#include "LogManager.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <utility>

namespace AetherSDR {

CatKenwoodServer::CatKenwoodServer(CatRadioInterface* radio, QObject* parent)
    : QObject(parent)
    , m_radio(radio)
{}

CatKenwoodServer::~CatKenwoodServer()
{
    stop();
}

bool CatKenwoodServer::start(quint16 port)
{
    if (m_server) {
        return m_server->isListening();
    }
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &CatKenwoodServer::onNewConnection);

    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        qCWarning(lcCat) << "CatKenwoodServer: failed to listen on port" << port
                         << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    qCInfo(lcCat) << "CatKenwoodServer: listening on port" << m_server->serverPort()
                  << "vfoA slice" << m_cfg.vfoASlice;
    return true;
}

void CatKenwoodServer::stop()
{
    if (!m_server) {
        return;
    }
    const QList<std::shared_ptr<Client>> clients = std::exchange(m_clients, {});
    for (const auto& c : clients) {
        if (c->socket) {
            // Prevent synchronous disconnected() re-entering while tearing down.
            c->socket->disconnect(this);
            c->socket->close();
            c->socket->deleteLater();
        }
    }
    emit clientCountChanged(0);

    m_server->close();
    delete m_server;
    m_server = nullptr;
    qCInfo(lcCat) << "CatKenwoodServer: stopped";
}

bool CatKenwoodServer::isRunning() const
{
    return m_server && m_server->isListening();
}

quint16 CatKenwoodServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

void CatKenwoodServer::setPortConfig(const CatKenwoodPortConfig& cfg)
{
    m_cfg = cfg;
    for (const auto& c : m_clients) {
        applyConfig(*c->protocol);
    }
}

void CatKenwoodServer::applyConfig(CatKenwoodProtocol& p) const
{
    p.setVfoASlice(m_cfg.vfoASlice);
    p.setAutoSwitchTxSlice(m_cfg.autoSwitchTxSlice);
    p.setSplitMode(m_cfg.splitMode);
}

CatKenwoodServer::Client* CatKenwoodServer::findClient(QTcpSocket* socket)
{
    for (const auto& c : m_clients) {
        if (c->socket == socket) {
            return c.get();
        }
    }
    return nullptr;
}

void CatKenwoodServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        auto client = std::make_shared<Client>();
        client->socket = socket;
        client->protocol = std::make_unique<CatKenwoodProtocol>(m_radio);
        applyConfig(*client->protocol);
        m_clients.append(client);

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onClientData(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() { onClientDisconnected(socket); });

        qCInfo(lcCat) << "CatKenwoodServer: client connected from"
                      << socket->peerAddress().toString() << "on port" << port();
        emit clientCountChanged(clientCount());
    }
}

void CatKenwoodServer::onClientData(QTcpSocket* socket)
{
    Client* c = findClient(socket);
    if (!c) {
        return;
    }
    const QByteArray answer = c->protocol->feed(socket->readAll());
    if (!answer.isEmpty()) {
        qCDebug(lcCat) << "CAT(kenwood) port" << port() << "->" << answer.left(80);
        socket->write(answer);
    }
}

void CatKenwoodServer::onClientDisconnected(QTcpSocket* socket)
{
    for (qsizetype i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i]->socket == socket) {
            m_clients.removeAt(i);
            break;
        }
    }
    socket->deleteLater();
    qCInfo(lcCat) << "CatKenwoodServer: client disconnected," << m_clients.size() << "remaining";
    emit clientCountChanged(clientCount());
}

// Auto-information push (AI1;/ZZAI1;): each client diffs its own snapshot.
void CatKenwoodServer::notifyRadioStateChanged()
{
    for (const auto& c : m_clients) {
        if (!c->protocol->autoInfoEnabled() || !c->socket) {
            continue;
        }
        const QByteArray push = c->protocol->pollAutoInfo();
        if (!push.isEmpty()) {
            c->socket->write(push);
        }
    }
}

} // namespace AetherSDR
