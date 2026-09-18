#pragma once

#include "CatKenwoodProtocol.h"

#include <QList>
#include <QObject>

#include <memory>

class QTcpServer;
class QTcpSocket;

namespace AetherSDR {

// Per-port settings shared by the TCP and PTY transports (SmartSDR CAT
// "Port Edit" window: VFO A Slice, Auto Switch TX Slice, Split Mode).
struct CatKenwoodPortConfig {
    int  vfoASlice{0};                 // 0-7 or CatKenwoodProtocol::kActiveSlice
    bool autoSwitchTxSlice{true};
    CatKenwoodProtocol::SplitMode splitMode{CatKenwoodProtocol::SplitMode::So2r};
};

// One SmartSDR-CAT-style TCP CAT port (SmartSDR CAT creates its first TCP
// port on 5002). Each client gets its own CatKenwoodProtocol, so AI state and
// FR/FT state are per connection, but all clients share the port's slice
// binding. Runs on the main thread (same as RigctlServer).
//
// The radio is any CatRadioInterface (CatKenwoodRadioAdapter in the app, a
// fake in tests). Connect CatKenwoodRadioAdapter::stateChanged to
// notifyRadioStateChanged() to drive auto-information pushes.
class CatKenwoodServer : public QObject {
    Q_OBJECT

public:
    explicit CatKenwoodServer(CatRadioInterface* radio, QObject* parent = nullptr);
    ~CatKenwoodServer() override;

    bool start(quint16 port);
    void stop();

    bool    isRunning() const;
    quint16 port() const;
    int     clientCount() const { return static_cast<int>(m_clients.size()); }

    // Applies to existing and future clients.
    void setPortConfig(const CatKenwoodPortConfig& cfg);
    CatKenwoodPortConfig portConfig() const { return m_cfg; }

public slots:
    // Push AI updates (FA/FB/MD/IF) to every client that enabled AI.
    void notifyRadioStateChanged();

signals:
    void clientCountChanged(int count);

private slots:
    void onNewConnection();

private:
    struct Client {
        QTcpSocket*                         socket{nullptr};
        std::unique_ptr<CatKenwoodProtocol> protocol;
    };

    void onClientData(QTcpSocket* socket);
    void onClientDisconnected(QTcpSocket* socket);
    Client* findClient(QTcpSocket* socket);
    void applyConfig(CatKenwoodProtocol& p) const;

    CatRadioInterface*      m_radio;
    QTcpServer*             m_server{nullptr};   // Qt parent-owned
    QList<std::shared_ptr<Client>> m_clients;
    CatKenwoodPortConfig    m_cfg;
};

} // namespace AetherSDR
