#pragma once

// Small non-modal panel listing WSJT-X/JTDX instances seen by
// WsjtxUdpServer plus a rolling list of recent decodes.  Reply (or a
// double-click on a decode row) asks WSJT-X to answer that decode, exactly
// as if the operator had double-clicked it in WSJT-X's Band Activity window.
// Halt TX asks WSJT-X to stop transmitting.  Nothing here keys the radio.
//
// Visual styling is intentionally plain (default widget palette) — styling
// is a maintainer decision.

#include "core/WsjtxUdpServer.h"

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace AetherSDR {

class WsjtxPanel : public QWidget {
    Q_OBJECT

public:
    explicit WsjtxPanel(WsjtxUdpServer* server, QWidget* parent = nullptr);

    static constexpr int kMaxRows = 200;

private slots:
    void onInstanceAdded(const QString& id);
    void onInstanceRemoved(const QString& id);
    void onStatusChanged(const QString& id, const Wsjtx::Status& status);
    void onDecode(const WsjtxDecodeEvent& ev);
    void onDecodesCleared(const QString& id);
    void onListeningChanged(bool listening);
    void onReplyClicked();
    void onHaltTxClicked();
    void updateButtons();

private:
    int instanceRow(const QString& id) const;
    void setInstanceRow(int row, const WsjtxInstance& inst);
    QString selectedInstanceId() const;
    void rebuildDecodes();
    bool passesFilter(const WsjtxDecodeEvent& ev) const;
    void insertDecodeRow(int row, const WsjtxDecodeEvent& ev);

    QPointer<WsjtxUdpServer> m_server;
    QLabel* m_statusLabel{nullptr};
    QTableWidget* m_instances{nullptr};
    QTableWidget* m_decodes{nullptr};
    QCheckBox* m_cqOnly{nullptr};
    QPushButton* m_replyBtn{nullptr};
    QPushButton* m_haltBtn{nullptr};
    QPushButton* m_clearBtn{nullptr};
    QVector<WsjtxDecodeEvent> m_rows;   // parallel to m_decodes rows
};

} // namespace AetherSDR
