#include "WsjtxPanel.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

enum InstanceCol { kInstId, kInstVersion, kInstDial, kInstMode, kInstDx, kInstTxEn, kInstTx,
                   kInstDecoding, kInstCols };
enum DecodeCol { kDecTime, kDecInstance, kDecSnr, kDecDt, kDecFreq, kDecMode, kDecMessage,
                 kDecCols };

QTableWidgetItem* roItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);   // ownership passes to the table
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString yesNo(bool v)
{
    return v ? QStringLiteral("Yes") : QStringLiteral("—");
}

QString mhz(quint64 hz)
{
    if (hz == 0) {
        return QStringLiteral("—");
    }
    return QString::number(static_cast<double>(hz) / 1e6, 'f', 6);
}

} // namespace

WsjtxPanel::WsjtxPanel(WsjtxUdpServer* server, QWidget* parent)
    : QWidget(parent)
    , m_server(server)
{
    setWindowTitle(tr("WSJT-X"));

    auto* root = new QVBoxLayout(this);
    m_statusLabel = new QLabel(this);
    root->addWidget(m_statusLabel);

    auto* split = new QSplitter(Qt::Vertical, this);
    root->addWidget(split, 1);

    m_instances = new QTableWidget(0, kInstCols, split);
    m_instances->setHorizontalHeaderLabels({tr("Instance"), tr("Version"), tr("Dial MHz"),
                                            tr("Mode"), tr("DX Call"), tr("TX En"), tr("TX"),
                                            tr("Decoding")});
    m_instances->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_instances->setSelectionMode(QAbstractItemView::SingleSelection);
    m_instances->verticalHeader()->hide();
    m_instances->horizontalHeader()->setStretchLastSection(true);
    split->addWidget(m_instances);

    m_decodes = new QTableWidget(0, kDecCols, split);
    m_decodes->setHorizontalHeaderLabels({tr("UTC"), tr("Instance"), tr("SNR"), tr("DT"),
                                          tr("Freq MHz"), tr("Mode"), tr("Message")});
    m_decodes->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_decodes->setSelectionMode(QAbstractItemView::SingleSelection);
    m_decodes->verticalHeader()->hide();
    m_decodes->horizontalHeader()->setStretchLastSection(true);
    split->addWidget(m_decodes);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

    auto* buttons = new QHBoxLayout;
    m_cqOnly = new QCheckBox(tr("CQ only"), this);
    m_replyBtn = new QPushButton(tr("Reply"), this);
    m_replyBtn->setToolTip(tr("Ask WSJT-X to reply to the selected decode "
                              "(same as double-clicking it in WSJT-X)"));
    m_haltBtn = new QPushButton(tr("Halt TX"), this);
    m_haltBtn->setToolTip(tr("Ask the selected WSJT-X instance to stop transmitting"));
    m_clearBtn = new QPushButton(tr("Clear"), this);
    buttons->addWidget(m_cqOnly);
    buttons->addStretch(1);
    buttons->addWidget(m_replyBtn);
    buttons->addWidget(m_haltBtn);
    buttons->addWidget(m_clearBtn);
    root->addLayout(buttons);

    connect(m_cqOnly, &QCheckBox::toggled, this, &WsjtxPanel::rebuildDecodes);
    connect(m_replyBtn, &QPushButton::clicked, this, &WsjtxPanel::onReplyClicked);
    connect(m_haltBtn, &QPushButton::clicked, this, &WsjtxPanel::onHaltTxClicked);
    connect(m_clearBtn, &QPushButton::clicked, this, [this] {
        m_rows.clear();
        m_decodes->setRowCount(0);
        updateButtons();
    });
    connect(m_decodes, &QTableWidget::cellDoubleClicked, this, [this](int, int) { onReplyClicked(); });
    connect(m_decodes, &QTableWidget::itemSelectionChanged, this, &WsjtxPanel::updateButtons);
    connect(m_instances, &QTableWidget::itemSelectionChanged, this, &WsjtxPanel::updateButtons);

    if (m_server) {
        connect(m_server, &WsjtxUdpServer::instanceAdded, this, &WsjtxPanel::onInstanceAdded);
        connect(m_server, &WsjtxUdpServer::instanceRemoved, this, &WsjtxPanel::onInstanceRemoved);
        connect(m_server, &WsjtxUdpServer::instanceStatusChanged, this, &WsjtxPanel::onStatusChanged);
        connect(m_server, &WsjtxUdpServer::decodeReceived, this, &WsjtxPanel::onDecode);
        connect(m_server, &WsjtxUdpServer::decodesCleared, this, &WsjtxPanel::onDecodesCleared);
        connect(m_server, &WsjtxUdpServer::listeningChanged, this, &WsjtxPanel::onListeningChanged);

        const QList<WsjtxInstance> insts = m_server->instances();
        for (const WsjtxInstance& inst : insts) {
            onInstanceAdded(inst.id);
        }
        const QVector<WsjtxDecodeEvent>& recent = m_server->recentDecodes();
        const qsizetype first = std::max<qsizetype>(0, recent.size() - kMaxRows);
        for (qsizetype i = first; i < recent.size(); ++i) {
            onDecode(recent[i]);
        }
        onListeningChanged(m_server->isListening());
    } else {
        onListeningChanged(false);
    }
    updateButtons();
    resize(720, 480);
}

void WsjtxPanel::onListeningChanged(bool listening)
{
    if (!m_server || !listening) {
        m_statusLabel->setText(tr("WSJT-X UDP: not listening"));
        return;
    }
    const QString group = m_server->multicastGroup();
    m_statusLabel->setText(group.isEmpty()
                               ? tr("WSJT-X UDP: listening on port %1").arg(m_server->port())
                               : tr("WSJT-X UDP: listening on %1:%2").arg(group).arg(m_server->port()));
}

int WsjtxPanel::instanceRow(const QString& id) const
{
    for (int r = 0; r < m_instances->rowCount(); ++r) {
        const QTableWidgetItem* it = m_instances->item(r, kInstId);
        if (it && it->text() == id) {
            return r;
        }
    }
    return -1;
}

void WsjtxPanel::setInstanceRow(int row, const WsjtxInstance& inst)
{
    m_instances->setItem(row, kInstId, roItem(inst.id));
    m_instances->setItem(row, kInstVersion,
                         roItem(inst.revision.isEmpty() ? inst.version
                                                        : inst.version + QLatin1Char(' ') + inst.revision));
    const Wsjtx::Status& st = inst.status;
    m_instances->setItem(row, kInstDial, roItem(inst.hasStatus ? mhz(st.dialFrequencyHz) : QString()));
    m_instances->setItem(row, kInstMode, roItem(st.subMode.isEmpty() ? st.mode : st.mode + QLatin1Char(' ') + st.subMode));
    m_instances->setItem(row, kInstDx, roItem(st.dxCall));
    m_instances->setItem(row, kInstTxEn, roItem(yesNo(st.txEnabled)));
    m_instances->setItem(row, kInstTx, roItem(yesNo(st.transmitting)));
    m_instances->setItem(row, kInstDecoding, roItem(yesNo(st.decoding)));
}

void WsjtxPanel::onInstanceAdded(const QString& id)
{
    if (!m_server) {
        return;
    }
    int row = instanceRow(id);
    if (row < 0) {
        row = m_instances->rowCount();
        m_instances->insertRow(row);
    }
    setInstanceRow(row, m_server->instance(id));
    if (m_instances->currentRow() < 0) {
        m_instances->selectRow(row);
    }
}

void WsjtxPanel::onInstanceRemoved(const QString& id)
{
    const int row = instanceRow(id);
    if (row >= 0) {
        m_instances->removeRow(row);
    }
    updateButtons();
}

void WsjtxPanel::onStatusChanged(const QString& id, const Wsjtx::Status&)
{
    onInstanceAdded(id);
    updateButtons();
}

bool WsjtxPanel::passesFilter(const WsjtxDecodeEvent& ev) const
{
    return !m_cqOnly->isChecked() || ev.parsed.isCq;
}

void WsjtxPanel::insertDecodeRow(int row, const WsjtxDecodeEvent& ev)
{
    m_decodes->insertRow(row);
    const QString time = ev.decode.time.isValid()
                             ? ev.decode.time.toString(QStringLiteral("HHmmss"))
                             : QString();
    m_decodes->setItem(row, kDecTime, roItem(time));
    m_decodes->setItem(row, kDecInstance, roItem(ev.instanceId));
    m_decodes->setItem(row, kDecSnr, roItem(QString::number(ev.decode.snr)));
    m_decodes->setItem(row, kDecDt, roItem(QString::number(ev.decode.deltaTime, 'f', 1)));
    m_decodes->setItem(row, kDecFreq, roItem(mhz(ev.rfFrequencyHz)));
    m_decodes->setItem(row, kDecMode, roItem(ev.modeName));
    m_decodes->setItem(row, kDecMessage, roItem(ev.decode.message));
}

void WsjtxPanel::onDecode(const WsjtxDecodeEvent& ev)
{
    if (!passesFilter(ev)) {
        return;
    }
    // Newest on top.
    m_rows.prepend(ev);
    insertDecodeRow(0, ev);
    while (m_rows.size() > kMaxRows) {
        m_rows.removeLast();
        m_decodes->removeRow(m_decodes->rowCount() - 1);
    }
}

void WsjtxPanel::rebuildDecodes()
{
    m_rows.clear();
    m_decodes->setRowCount(0);
    if (!m_server) {
        return;
    }
    const QVector<WsjtxDecodeEvent>& recent = m_server->recentDecodes();
    for (const WsjtxDecodeEvent& ev : recent) {
        onDecode(ev);
    }
    updateButtons();
}

void WsjtxPanel::onDecodesCleared(const QString& id)
{
    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (m_rows[i].instanceId == id) {
            m_rows.removeAt(i);
            m_decodes->removeRow(i);
        }
    }
    updateButtons();
}

QString WsjtxPanel::selectedInstanceId() const
{
    const int decRow = m_decodes->currentRow();
    if (decRow >= 0 && decRow < m_rows.size() && m_decodes->selectionModel()->hasSelection()) {
        return m_rows[decRow].instanceId;
    }
    const int instRow = m_instances->currentRow();
    if (instRow >= 0) {
        if (const QTableWidgetItem* it = m_instances->item(instRow, kInstId)) {
            return it->text();
        }
    }
    if (m_instances->rowCount() == 1) {
        if (const QTableWidgetItem* it = m_instances->item(0, kInstId)) {
            return it->text();
        }
    }
    return {};
}

void WsjtxPanel::onReplyClicked()
{
    const int row = m_decodes->currentRow();
    if (!m_server || row < 0 || row >= m_rows.size()) {
        return;
    }
    if (!m_server->sendReply(m_rows[row])) {
        m_statusLabel->setText(tr("Reply failed — WSJT-X instance \"%1\" not reachable")
                                   .arg(m_rows[row].instanceId));
    }
}

void WsjtxPanel::onHaltTxClicked()
{
    const QString id = selectedInstanceId();
    if (m_server && !id.isEmpty()) {
        m_server->sendHaltTx(id, false);
    }
}

void WsjtxPanel::updateButtons()
{
    const int row = m_decodes->currentRow();
    const bool haveDecode = row >= 0 && row < m_rows.size()
                            && m_rows[row].modeName != QLatin1String("WSPR");
    m_replyBtn->setEnabled(m_server && haveDecode && m_server->hasInstance(m_rows[row].instanceId));
    m_haltBtn->setEnabled(m_server && !selectedInstanceId().isEmpty());
}

} // namespace AetherSDR
