#pragma once

// Developer-only GUI automation server. Compiled only with
// -DAETHER_GUI_TEST=ON and started only when AETHER_GUI_TEST_PORT is set.
//
// Speaks newline-delimited JSON on 127.0.0.1:<port> so an external script can
// enumerate widgets, click/set them, trigger menu actions and grab window
// images while the app is connected to a live radio. Intended for RX-side
// regression testing without OS accessibility permissions.
//
// Safety: while this server is compiled in, TxSafety::isTransmitCommand()
// blocks every radio command that could key the transmitter, and UDP sends
// to the radio (TX audio / netcw) are dropped — see TxSafety.h.

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QTcpServer;
class QTcpSocket;
class QWidget;
class QAction;

namespace AetherSDR {

class GuiTestServer : public QObject {
    Q_OBJECT
public:
    explicit GuiTestServer(quint16 port, QObject* parent = nullptr);

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* sock);
    QJsonObject handle(const QJsonObject& req);

    QJsonObject cmdWindows();
    QJsonObject cmdTree(const QJsonObject& req);
    QJsonObject cmdClick(const QJsonObject& req);
    QJsonObject cmdMouse(const QJsonObject& req);
    QJsonObject cmdSet(const QJsonObject& req);
    QJsonObject cmdKey(const QJsonObject& req);
    QJsonObject cmdActions();
    QJsonObject cmdTrigger(const QJsonObject& req);
    QJsonObject cmdGrab(const QJsonObject& req);
    QJsonObject cmdClose(const QJsonObject& req);

    QString idFor(QWidget* w);
    QWidget* widgetFor(const QJsonValue& id) const;
    QJsonObject describe(QWidget* w);
    static bool looksLikeTransmitControl(const QWidget* w);

    QTcpServer* m_server{nullptr};
    QHash<QString, QPointer<QWidget>> m_ids;
    quint64 m_nextId{1};
};

} // namespace AetherSDR
