#include "GuiTestServer.h"

#include "core/LogManager.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QStatusBar>
#include <QDoubleSpinBox>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMetaMethod>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPixmap>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QTimer>
#include <QWheelEvent>

namespace AetherSDR {

namespace {

QString widgetText(const QWidget* w)
{
    if (auto* b = qobject_cast<const QAbstractButton*>(w)) {
        return b->text();
    }
    if (auto* l = qobject_cast<const QLabel*>(w)) {
        return l->text();
    }
    if (auto* e = qobject_cast<const QLineEdit*>(w)) {
        return e->text();
    }
    if (auto* c = qobject_cast<const QComboBox*>(w)) {
        return c->currentText();
    }
    if (auto* t = qobject_cast<const QTextEdit*>(w)) {
        return t->toPlainText().left(500);
    }
    if (auto* p = qobject_cast<const QPlainTextEdit*>(w)) {
        return p->toPlainText().left(500);
    }
    if (auto* sp = qobject_cast<const QAbstractSpinBox*>(w)) {
        return sp->text();
    }
    return {};
}

QJsonObject err(const QString& msg)
{
    return QJsonObject{{"ok", false}, {"error", msg}};
}

void collectActions(QMenu* menu, const QString& prefix, QJsonArray& out,
                    QHash<QString, QPointer<QAction>>* index)
{
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) {
            continue;
        }
        const QString path = prefix + "/" + a->text().remove('&');
        if (a->menu()) {
            collectActions(a->menu(), path, out, index);
            continue;
        }
        out.append(QJsonObject{{"path", path}, {"enabled", a->isEnabled()},
                               {"checkable", a->isCheckable()}, {"checked", a->isChecked()}});
        if (index) {
            index->insert(path, a);
        }
    }
}

QMainWindow* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* mw = qobject_cast<QMainWindow*>(w)) {
            return mw;
        }
    }
    return nullptr;
}

} // namespace

GuiTestServer::GuiTestServer(quint16 port, QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &GuiTestServer::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        qCWarning(lcGui) << "GuiTestServer: cannot listen on port" << port << m_server->errorString();
        return;
    }
    qCWarning(lcGui) << "GuiTestServer: listening on 127.0.0.1:" << port
                     << "(TX interlock active)";
}

void GuiTestServer::onNewConnection()
{
    while (QTcpSocket* sock = m_server->nextPendingConnection()) {
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() { onReadyRead(sock); });
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }
}

void GuiTestServer::onReadyRead(QTcpSocket* rawSock)
{
    // A handler can spin a nested event loop (modal dialog) during which the
    // client may disconnect and the socket get deleted — guard every use.
    const QPointer<QTcpSocket> sock(rawSock);
    while (sock && sock->canReadLine()) {
        const QByteArray line = sock->readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        QJsonObject reply;
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            reply = err(QStringLiteral("bad json: %1").arg(pe.errorString()));
        } else {
            reply = handle(doc.object());
        }
        if (!sock) {
            return;
        }
        sock->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + "\n");
        sock->flush();
    }
}

QJsonObject GuiTestServer::handle(const QJsonObject& req)
{
    const QString cmd = req.value("cmd").toString();
    if (cmd == "windows") { return cmdWindows(); }
    if (cmd == "tree")    { return cmdTree(req); }
    if (cmd == "click")   { return cmdClick(req); }
    if (cmd == "mouse")   { return cmdMouse(req); }
    if (cmd == "set")     { return cmdSet(req); }
    if (cmd == "key")     { return cmdKey(req); }
    if (cmd == "actions") { return cmdActions(); }
    if (cmd == "trigger") { return cmdTrigger(req); }
    if (cmd == "grab")    { return cmdGrab(req); }
    if (cmd == "close")   { return cmdClose(req); }
    if (cmd == "ping")    { return QJsonObject{{"ok", true}}; }
    if (cmd == "activate") {
        // Apps launched from a background shell are never activated by macOS,
        // so their native menu bar isn't installed. Raise + activate like a
        // user click would.
        if (QMainWindow* mw = mainWindow()) {
            mw->raise();
            mw->activateWindow();
        }
        return QJsonObject{{"ok", true}};
    }
    if (cmd == "connected") {
        // Is signal <signature> of widget <id> (or a child of class <childClass>) connected?
        QObject* obj = widgetFor(req.value("id"));
        if (!obj) {
            return err("no such widget");
        }
        const QByteArray sig = QMetaObject::normalizedSignature(req.value("signal").toString().toLatin1());
        const int idx = obj->metaObject()->indexOfSignal(sig.constData());
        if (idx < 0) {
            return err("no such signal " + QString::fromLatin1(sig));
        }
        // receivers() is protected; this test-only accessor exposes it.
        struct ReceiverCount : QObject {
            static int of(const QObject* o, const char* s)
            {
                return static_cast<const ReceiverCount*>(o)->receivers(s);
            }
        };
        const QByteArray withCode = QByteArray("2") + sig;  // SIGNAL() macro encoding
        const int n = ReceiverCount::of(obj, withCode.constData());
        return QJsonObject{{"ok", true}, {"connected", n > 0}, {"receivers", n}};
    }
    if (cmd == "status") {
        QMainWindow* mw = mainWindow();
        return QJsonObject{{"ok", mw != nullptr},
                           {"message", mw ? mw->statusBar()->currentMessage() : QString()}};
    }
    return err("unknown cmd " + cmd);
}

QString GuiTestServer::idFor(QWidget* w)
{
    const QString key = w->property("_guiTestId").toString();
    if (!key.isEmpty() && m_ids.value(key) == w) {
        return key;
    }
    const QString id = QString::number(m_nextId++);
    w->setProperty("_guiTestId", id);
    m_ids.insert(id, w);
    return id;
}

QWidget* GuiTestServer::widgetFor(const QJsonValue& id) const
{
    const QString key = id.isDouble() ? QString::number(id.toInteger()) : id.toString();
    return m_ids.value(key).data();
}

QJsonObject GuiTestServer::describe(QWidget* w)
{
    const QRect g(w->mapToGlobal(QPoint(0, 0)), w->size());
    QJsonObject o{
        {"id", idFor(w)},
        {"class", QString::fromLatin1(w->metaObject()->className())},
        {"name", w->objectName()},
        {"text", widgetText(w)},
        {"acc", w->accessibleName()},
        {"tip", w->toolTip().left(160)},
        {"visible", w->isVisible()},
        {"enabled", w->isEnabled()},
        {"geom", QJsonArray{g.x(), g.y(), g.width(), g.height()}},
    };
    if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->isCheckable()) {
        o.insert("checked", b->isChecked());
    }
    if (auto* s = qobject_cast<QAbstractSlider*>(w)) {
        o.insert("value", s->value());
        o.insert("range", QJsonArray{s->minimum(), s->maximum()});
    }
    if (auto* sp = qobject_cast<QSpinBox*>(w)) {
        o.insert("value", sp->value());
    }
    if (auto* dsp = qobject_cast<QDoubleSpinBox*>(w)) {
        o.insert("value", dsp->value());
    }
    if (auto* c = qobject_cast<QComboBox*>(w)) {
        QJsonArray items;
        for (int i = 0; i < c->count(); ++i) {
            items.append(c->itemText(i));
        }
        o.insert("items", items);
        o.insert("index", c->currentIndex());
    }
    if (auto* tb = qobject_cast<QTabBar*>(w)) {
        QJsonArray tabs;
        for (int i = 0; i < tb->count(); ++i) {
            tabs.append(tb->tabText(i));
        }
        o.insert("tabs", tabs);
        o.insert("index", tb->currentIndex());
    }
    return o;
}

namespace {
// Synthetic events bypass Qt's modality checks; refuse input a real user
// couldn't deliver while an application-modal dialog is open.
bool blockedByModal(const QWidget* w)
{
    const QWidget* modal = QApplication::activeModalWidget();
    return modal && w && modal != w->window() && !modal->isAncestorOf(w);
}
} // namespace

bool GuiTestServer::looksLikeTransmitControl(const QWidget* w)
{
    // Belt-and-braces with the command interlock: refuse to press controls
    // whose label says they key the radio.
    static const QRegularExpression kTxLabel(
        QStringLiteral(R"(^\s*(MOX|TUNE|XMIT|PTT|VOX|ATU|ATU START|TGXL TUNE|SEND|TX TUNE|KEY|PLAY)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString text = widgetText(w);
    return kTxLabel.match(text).hasMatch() || kTxLabel.match(w->accessibleName()).hasMatch();
}

QJsonObject GuiTestServer::cmdWindows()
{
    QJsonArray arr;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (!w->isVisible()) {
            continue;
        }
        QJsonObject o = describe(w);
        o.insert("title", w->windowTitle());
        arr.append(o);
    }
    return QJsonObject{{"ok", true}, {"windows", arr}};
}

QJsonObject GuiTestServer::cmdTree(const QJsonObject& req)
{
    QWidget* root = req.contains("id") ? widgetFor(req.value("id")) : mainWindow();
    if (!root) {
        return err("no such widget");
    }
    const bool visibleOnly = req.value("visibleOnly").toBool(true);
    const QString match = req.value("match").toString();
    const QRegularExpression re(match, QRegularExpression::CaseInsensitiveOption);
    QJsonArray arr;
    const auto children = root->findChildren<QWidget*>();
    for (QWidget* w : children) {
        if (visibleOnly && !w->isVisible()) {
            continue;
        }
        if (!match.isEmpty()) {
            const QString hay = QStringLiteral("%1|%2|%3|%4|%5")
                .arg(QString::fromLatin1(w->metaObject()->className()), w->objectName(),
                     widgetText(w), w->accessibleName(), w->toolTip());
            if (!re.match(hay).hasMatch()) {
                continue;
            }
        }
        arr.append(describe(w));
    }
    return QJsonObject{{"ok", true}, {"root", describe(root)}, {"widgets", arr}};
}

QJsonObject GuiTestServer::cmdClick(const QJsonObject& req)
{
    QWidget* w = widgetFor(req.value("id"));
    if (!w) {
        return err("no such widget");
    }
    if (blockedByModal(w)) {
        return err("blocked: a modal dialog is open");
    }
    if (looksLikeTransmitControl(w)) {
        return err("refused: transmit control");
    }
    if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        // Queued: a slot that opens a modal dialog must not block the reply.
        QTimer::singleShot(0, b, [b]() { b->click(); });
        return QJsonObject{{"ok", true}, {"queued", true}};
    }
    if (auto* tb = qobject_cast<QTabBar*>(w); tb && req.contains("index")) {
        tb->setCurrentIndex(req.value("index").toInt());
        return QJsonObject{{"ok", true}, {"widget", describe(w)}};
    }
    QJsonObject m = req;
    m.insert("x", w->width() / 2);
    m.insert("y", w->height() / 2);
    return cmdMouse(m);
}

QJsonObject GuiTestServer::cmdMouse(const QJsonObject& req)
{
    QWidget* w = widgetFor(req.value("id"));
    if (!w) {
        return err("no such widget");
    }
    if (blockedByModal(w)) {
        return err("blocked: a modal dialog is open");
    }
    if (looksLikeTransmitControl(w)) {
        return err("refused: transmit control");
    }
    const QPointF local(req.value("x").toDouble(), req.value("y").toDouble());
    const QPointF global = w->mapToGlobal(local);
    const QString btn = req.value("button").toString("left");
    const Qt::MouseButton b = (btn == "right") ? Qt::RightButton
                            : (btn == "middle") ? Qt::MiddleButton : Qt::LeftButton;
    const QString action = req.value("action").toString("click");
    if (action == "context") {
        QContextMenuEvent ce(QContextMenuEvent::Mouse, local.toPoint(), global.toPoint());
        QApplication::sendEvent(w, &ce);
        return QJsonObject{{"ok", true}};
    }
    if (action == "wheel") {
        const int delta = req.value("delta").toInt(120);
        QWheelEvent we(local, global, QPoint(), QPoint(0, delta), Qt::NoButton,
                       Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &we);
        return QJsonObject{{"ok", true}};
    }
    QMouseEvent move(QEvent::MouseMove, local, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
    if (action == "move") {
        return QJsonObject{{"ok", true}};
    }
    // Queued so a press that opens a modal menu/dialog doesn't block the reply.
    const QPointer<QWidget> target(w);
    const bool dbl = (action == "double");
    QTimer::singleShot(0, this, [target, local, global, b, dbl]() {
        if (!target) {
            return;
        }
        QMouseEvent press(QEvent::MouseButtonPress, local, global, b, b, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        if (!target) {
            return;
        }
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, b, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &release);
        if (dbl && target) {
            QMouseEvent dblEv(QEvent::MouseButtonDblClick, local, global, b, b, Qt::NoModifier);
            QApplication::sendEvent(target, &dblEv);
        }
    });
    return QJsonObject{{"ok", true}, {"queued", true}};
}

QJsonObject GuiTestServer::cmdSet(const QJsonObject& req)
{
    QWidget* w = widgetFor(req.value("id"));
    if (!w) {
        return err("no such widget");
    }
    if (blockedByModal(w)) {
        return err("blocked: a modal dialog is open");
    }
    const QJsonValue v = req.value("value");
    if (auto* s = qobject_cast<QAbstractSlider*>(w)) {
        s->setValue(v.toInt());
    } else if (auto* sp = qobject_cast<QSpinBox*>(w)) {
        sp->setValue(v.toInt());
        emit sp->editingFinished();
    } else if (auto* dsp = qobject_cast<QDoubleSpinBox*>(w)) {
        dsp->setValue(v.toDouble());
        emit dsp->editingFinished();
    } else if (auto* c = qobject_cast<QComboBox*>(w)) {
        if (v.isDouble()) {
            c->setCurrentIndex(v.toInt());
        } else {
            const int idx = c->findText(v.toString());
            if (idx < 0) {
                return err("combo item not found");
            }
            c->setCurrentIndex(idx);
        }
        emit c->activated(c->currentIndex());
    } else if (auto* e = qobject_cast<QLineEdit*>(w)) {
        e->setText(v.toString());
        emit e->textEdited(e->text());
        emit e->editingFinished();
        if (req.value("enter").toBool()) {
            emit e->returnPressed();
        }
    } else if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->isCheckable()) {
        if (b->isChecked() != v.toBool()) {
            if (looksLikeTransmitControl(w)) {
                return err("refused: transmit control");
            }
            QTimer::singleShot(0, b, [b]() { b->click(); });
        }
    } else if (auto* pe = qobject_cast<QPlainTextEdit*>(w); pe && !pe->isReadOnly()) {
        pe->setPlainText(v.toString());
    } else if (auto* te = qobject_cast<QTextEdit*>(w); te && !te->isReadOnly()) {
        te->setPlainText(v.toString());
    } else if (auto* tw = qobject_cast<QTabWidget*>(w)) {
        tw->setCurrentIndex(v.toInt());
    } else if (auto* tb = qobject_cast<QTabBar*>(w)) {
        tb->setCurrentIndex(v.toInt());
    } else {
        return err("unsupported widget type for set");
    }
    return QJsonObject{{"ok", true}, {"widget", describe(w)}};
}

QJsonObject GuiTestServer::cmdKey(const QJsonObject& req)
{
    QWidget* w = req.contains("id") ? widgetFor(req.value("id")) : QApplication::focusWidget();
    if (!w) {
        w = mainWindow();
    }
    if (!w) {
        return err("no target");
    }
    const QString key = req.value("key").toString();
    // Space / T / M are common PTT/MOX/tune hotkeys — never inject them.
    static const QHash<QString, int> kKeys{
        {"Return", Qt::Key_Return}, {"Escape", Qt::Key_Escape}, {"Up", Qt::Key_Up},
        {"Down", Qt::Key_Down}, {"Left", Qt::Key_Left}, {"Right", Qt::Key_Right},
        {"Tab", Qt::Key_Tab}, {"PageUp", Qt::Key_PageUp}, {"PageDown", Qt::Key_PageDown},
    };
    if (!kKeys.contains(key)) {
        return err("key not allowed: " + key);
    }
    QKeyEvent press(QEvent::KeyPress, kKeys.value(key), Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, kKeys.value(key), Qt::NoModifier);
    QApplication::sendEvent(w, &release);
    return QJsonObject{{"ok", true}};
}

QJsonObject GuiTestServer::cmdActions()
{
    QJsonArray arr;
    if (QMainWindow* mw = mainWindow()) {
        for (QMenuBar* bar : mw->findChildren<QMenuBar*>()) {
            for (QAction* top : bar->actions()) {
                if (top->menu()) {
                    collectActions(top->menu(), top->text().remove('&'), arr, nullptr);
                }
            }
        }
    }
    return QJsonObject{{"ok", true}, {"actions", arr}};
}

QJsonObject GuiTestServer::cmdTrigger(const QJsonObject& req)
{
    const QString path = req.value("path").toString();
    static const QRegularExpression kTxAction(
        QStringLiteral(R"(\b(MOX|Tune|Transmit|PTT|VOX|ATU|Send)\b)"));
    if (kTxAction.match(path).hasMatch()) {
        return err("refused: transmit action");
    }
    QMainWindow* mw = mainWindow();
    if (!mw) {
        return err("no main window");
    }
    QJsonArray dummy;
    QHash<QString, QPointer<QAction>> index;
    for (QMenuBar* bar : mw->findChildren<QMenuBar*>()) {
        for (QAction* top : bar->actions()) {
            if (top->menu()) {
                collectActions(top->menu(), top->text().remove('&'), dummy, &index);
            }
        }
    }
    QAction* a = index.value(path).data();
    if (!a) {
        return err("no such action: " + path);
    }
    // Queue so modal dialogs (exec()) don't block this reply.
    QTimer::singleShot(0, a, [a]() { a->trigger(); });
    return QJsonObject{{"ok", true}};
}

QJsonObject GuiTestServer::cmdGrab(const QJsonObject& req)
{
    QWidget* w = req.contains("id") ? widgetFor(req.value("id")) : mainWindow();
    if (!w) {
        return err("no such widget");
    }
    const QString file = req.value("file").toString();
    if (file.isEmpty()) {
        return err("file required");
    }
    const QPixmap pm = w->grab();
    if (!pm.save(file)) {
        return err("save failed");
    }
    return QJsonObject{{"ok", true}, {"file", file}, {"size", QJsonArray{pm.width(), pm.height()}}};
}

QJsonObject GuiTestServer::cmdClose(const QJsonObject& req)
{
    QWidget* w = widgetFor(req.value("id"));
    if (!w) {
        return err("no such widget");
    }
    QTimer::singleShot(0, w, [w]() { w->close(); });
    return QJsonObject{{"ok", true}};
}

} // namespace AetherSDR
