#include "CwSkimmerPanel.h"

#include "ComboStyle.h"
#include "core/AppSettings.h"

#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {

enum Column { ColFreq = 0, ColCall, ColWpm, ColSnr, ColText, ColCount };

constexpr const char* kBtnStyle =
    "QPushButton { background: #1a3a5a; border: 1px solid #205070; border-radius: 3px; "
    "color: #c8d8e8; font-size: 10px; font-weight: bold; padding: 2px 8px; }"
    "QPushButton:hover { background: #204060; }"
    "QPushButton:checked { background-color: #006040; color: #00ff88; "
    "border: 1px solid #00a060; }";

} // namespace

CwSkimmerPanel::CwSkimmerPanel(QWidget* parent)
    : QWidget(parent)
{
    setStyleSheet("QWidget { background: #0f0f1a; color: #c8d8e8; }");

    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    auto& s = AppSettings::instance();

    // ── Header: enable + DAX IQ channel + status ────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);

        m_enableBtn = new QPushButton("Skimmer");
        m_enableBtn->setCheckable(true);
        m_enableBtn->setStyleSheet(kBtnStyle);
        m_enableBtn->setAccessibleName("CW skimmer enable");
        m_enableBtn->setToolTip("Decode every CW signal in the selected DAX IQ stream");
        m_enableBtn->setChecked(s.value("CwSkimmerEnabled", "False").toString() == "True");
        row->addWidget(m_enableBtn);

        auto* lbl = new QLabel("DAX IQ:");
        lbl->setStyleSheet("QLabel { color: #8090a0; font-size: 11px; }");
        row->addWidget(lbl);

        m_channelCombo = new QComboBox;
        for (int ch = 1; ch <= 4; ++ch) {
            m_channelCombo->addItem(QString::number(ch), ch);
        }
        m_channelCombo->setAccessibleName("CW skimmer DAX IQ channel");
        applyComboStyle(m_channelCombo);
        const int saved = std::clamp(s.value("CwSkimmerDaxIqChannel", 1).toInt(), 1, 4);
        m_channelCombo->setCurrentIndex(saved - 1);
        row->addWidget(m_channelCombo);

        row->addStretch();

        m_status = new QLabel("Off");
        m_status->setStyleSheet("QLabel { color: #8090a0; font-size: 11px; }");
        row->addWidget(m_status);

        vbox->addLayout(row);
    }

    // ── Signal table ────────────────────────────────────────────────────
    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({"kHz", "Call", "WPM", "SNR", "Text"});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAccessibleName("CW skimmer decoded signals");
    m_table->setToolTip("Click a signal to tune to it");
    m_table->horizontalHeader()->setSectionResizeMode(ColText, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColFreq, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColCall, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColWpm, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColSnr, QHeaderView::ResizeToContents);
    m_table->setStyleSheet(
        "QTableWidget { background: #0a0a18; gridline-color: #1e2e3e; font-size: 11px; }"
        "QHeaderView::section { background: #1a2a3a; color: #8090a0; border: none; "
        "padding: 2px; font-size: 10px; }"
        "QTableWidget::item:selected { background: #0070c0; }");
    vbox->addWidget(m_table, 1);

    connect(m_enableBtn, &QPushButton::toggled, this, [this](bool on) {
        auto& as = AppSettings::instance();
        as.setValue("CwSkimmerEnabled", on ? "True" : "False");
        as.save();
        emit enableToggled(on);
    });
    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
        const int ch = idx + 1;
        auto& as = AppSettings::instance();
        as.setValue("CwSkimmerDaxIqChannel", ch);
        as.save();
        m_channels.clear();
        m_lastSpot.clear();
        rebuildRows();
        emit daxIqChannelChanged(ch);
    });
    connect(m_table, &QTableWidget::cellClicked, this, &CwSkimmerPanel::onCellClicked);
}

bool CwSkimmerPanel::isSkimmerEnabled() const
{
    return m_enableBtn->isChecked();
}

int CwSkimmerPanel::daxIqChannel() const
{
    return m_channelCombo->currentIndex() + 1;
}

void CwSkimmerPanel::setRunning(bool running)
{
    {
        const QSignalBlocker block(m_enableBtn);
        m_enableBtn->setChecked(running);
    }
    if (!running) {
        m_channels.clear();
        m_lastSpot.clear();
        rebuildRows();
        m_status->setText("Off");
    }
}

void CwSkimmerPanel::setChannels(const QVector<CwSkimmerChannelInfo>& channels)
{
    m_channels = channels;
    rebuildRows();
}

void CwSkimmerPanel::addSpot(const CwSkimmerSpot& spot)
{
    m_lastSpot.insert(spot.channelId, spot);
    rebuildRows();
}

void CwSkimmerPanel::removeChannel(int channelId)
{
    m_lastSpot.remove(channelId);
    for (int i = 0; i < m_channels.size(); ++i) {
        if (m_channels[i].id == channelId) {
            m_channels.removeAt(i);
            break;
        }
    }
    rebuildRows();
}

void CwSkimmerPanel::rebuildRows()
{
    // Preserve the selected carrier across refreshes.
    int selectedId = -1;
    const int cur = m_table->currentRow();
    if (cur >= 0) {
        if (QTableWidgetItem* it = m_table->item(cur, ColFreq)) {
            selectedId = it->data(Qt::UserRole).toInt();
        }
    }

    m_table->setRowCount(static_cast<int>(m_channels.size()));
    int selectRow = -1;
    for (int r = 0; r < m_channels.size(); ++r) {
        const CwSkimmerChannelInfo& c = m_channels[r];
        auto setCell = [this, r](int col, const QString& text) -> QTableWidgetItem* {
            QTableWidgetItem* it = m_table->item(r, col);
            if (!it) {
                it = new QTableWidgetItem;   // owned by the table
                m_table->setItem(r, col, it);
            }
            it->setText(text);
            return it;
        };

        QTableWidgetItem* f = setCell(ColFreq, QString::number(c.freqHz / 1000.0, 'f', 1));
        f->setData(Qt::UserRole, c.id);
        f->setData(Qt::UserRole + 1, c.freqHz);

        const auto spotIt = m_lastSpot.constFind(c.id);
        QTableWidgetItem* call = setCell(ColCall, c.lastCall);
        QFont font = call->font();
        font.setBold(spotIt != m_lastSpot.constEnd() && spotIt->isCq);
        call->setFont(font);
        call->setForeground(QColor(spotIt != m_lastSpot.constEnd() && spotIt->isCq
                                   ? "#00ff88" : "#c8d8e8"));

        setCell(ColWpm, c.wpm > 0.0f ? QString::number(qRound(c.wpm)) : QString());
        setCell(ColSnr, QString::number(qRound(c.snrDb)));
        setCell(ColText, c.text.right(80));

        if (c.id == selectedId) {
            selectRow = r;
        }
    }
    if (selectRow >= 0) {
        const QSignalBlocker block(m_table);
        m_table->selectRow(selectRow);
    }
    m_status->setText(m_enableBtn->isChecked()
        ? QString("%1 signal%2").arg(m_channels.size()).arg(m_channels.size() == 1 ? "" : "s")
        : QString("Off"));
}

void CwSkimmerPanel::onCellClicked(int row, int /*column*/)
{
    QTableWidgetItem* it = m_table->item(row, ColFreq);
    if (!it) {
        return;
    }
    const double hz = it->data(Qt::UserRole + 1).toDouble();
    if (hz > 0.0) {
        emit tuneRequested(hz / 1.0e6);
    }
}

} // namespace AetherSDR
