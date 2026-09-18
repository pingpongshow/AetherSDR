#include "CwxPanel.h"
#include "models/CwxModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTextEdit>
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QScrollArea>
#include <QDateTime>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTimer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// ── Painted chat bubble widget ──────────────────────────────────────────
// Each bubble tracks the radio CWX buffer index of its first character
// (from the "cwx send" reply) so "cwx sent=<index>" status can highlight
// the characters already keyed and "erase" status can mark characters the
// radio removed before keying them.
class CwxBubble : public QWidget {
public:
    CwxBubble(const QString& text, const QString& time, int block, QWidget* parent = nullptr)
        : QWidget(parent), m_text(text), m_time(time), m_block(block),
          m_erased(text.size(), false)
    {
        recalcSize();
    }

    int block() const { return m_block; }
    int startIndex() const { return m_startIndex; }
    int length() const { return static_cast<int>(m_text.size()); }
    void setStartIndex(int idx) { m_startIndex = idx; update(); }
    // Stop tracking radio indices (numbering restarted); keeps painted state.
    void retire() { m_startIndex = -1; }

    // Returns true if this bubble contains the radio buffer index.
    bool containsIndex(int radioIndex) const
    {
        return m_startIndex >= 0 && radioIndex >= m_startIndex
            && radioIndex < m_startIndex + length();
    }

    void setSentThrough(int radioIndex)
    {
        if (m_startIndex < 0) {
            return;
        }
        const int count = qBound(0, radioIndex - m_startIndex + 1, length());
        if (count > m_sentCount) {
            m_sentCount = count;
            update();
        }
    }

    void markErased(int startIdx, int stopIdx)
    {
        if (m_startIndex < 0) {
            return;
        }
        const int from = qMax(startIdx - m_startIndex, 0);
        const int to   = qMin(stopIdx - m_startIndex, length() - 1);
        bool changed = false;
        for (int i = from; i <= to; ++i) {
            if (i >= m_sentCount && !m_erased[i]) {
                m_erased[i] = true;
                changed = true;
            }
        }
        if (changed) {
            update();
        }
    }

    void resizeEvent(QResizeEvent*) override { recalcSize(); }

    void recalcSize()
    {
        QFont timeFont("monospace", 8);
        QFontMetrics sfm(timeFont);
        int availW = (parentWidget() ? parentWidget()->width() : 240) - 28;
        QTextLayout layout;
        const int textH = buildLayout(layout, availW);
        int h = textH + sfm.height() + 18;
        setFixedHeight(h);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        QRect r(4, 2, width() - 12, height() - 4);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x00, 0xb4, 0xd8));
        p.drawRoundedRect(r, 10, 10);

        // CW text — 12pt, left aligned, word wrap; keyed chars highlighted
        QRect textRect = r.adjusted(10, 4, -10, 0);
        QTextLayout layout;
        const int textH = buildLayout(layout, textRect.width());
        p.setPen(QColor(0, 0, 0));
        layout.draw(&p, textRect.topLeft(), formatRanges());

        // Timestamp — 8pt, right aligned, below text
        QFont timeFont("monospace", 8);
        p.setFont(timeFont);
        p.setPen(QColor(0x00, 0x30, 0x40));
        QRect timeRect = r.adjusted(10, textH + 6, -6, -2);
        p.drawText(timeRect, Qt::AlignRight | Qt::AlignTop, m_time);
    }

private:
    int buildLayout(QTextLayout& layout, int width) const
    {
        layout.setText(m_text);
        layout.setFont(QFont("monospace", 12));
        QTextOption opt(Qt::AlignLeft);
        opt.setWrapMode(QTextOption::WordWrap);
        layout.setTextOption(opt);
        layout.beginLayout();
        qreal y = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(qMax(width, 1));
            line.setPosition(QPointF(0, y));
            y += line.height();
        }
        layout.endLayout();
        return qMax(static_cast<int>(std::ceil(y)), QFontMetrics(layout.font()).height());
    }

    QList<QTextLayout::FormatRange> formatRanges() const
    {
        QList<QTextLayout::FormatRange> ranges;
        if (m_sentCount > 0) {
            QTextLayout::FormatRange sent;
            sent.start = 0;
            sent.length = m_sentCount;
            sent.format.setForeground(QColor(0xff, 0xff, 0xff));
            ranges.append(sent);
        }
        for (int i = 0; i < length(); ++i) {
            if (!m_erased[i]) {
                continue;
            }
            QTextLayout::FormatRange er;
            er.start = i;
            er.length = 1;
            er.format.setForeground(QColor(0x00, 0x50, 0x68));
            er.format.setFontStrikeOut(true);
            ranges.append(er);
        }
        return ranges;
    }

    QString m_text, m_time;
    int m_block{-1};
    int m_startIndex{-1};
    int m_sentCount{0};
    QVector<bool> m_erased;
};

static const char* kBtnStyle =
    "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
    "border-radius: 3px; color: #c8d8e8; font-size: 11px; font-weight: bold; "
    "padding: 4px 10px; }"
    "QPushButton:checked { background: #00b4d8; color: #000; border: 1px solid #00d4f8; }"
    "QPushButton:hover { background: #203040; }";

static const char* kEditStyle =
    "QLineEdit { background: #ffffff; color: #000000; border: 1px solid #304050; "
    "border-radius: 2px; padding: 4px; font-size: 11px; }";

static const char* kTextStyle =
    "QTextEdit { background: #0a0a14; color: #c8d8e8; border: none; "
    "font-family: monospace; font-size: 13px; padding: 8px; }";

CwxPanel::CwxPanel(CwxModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setFixedWidth(250);
    setStyleSheet("QWidget { background: #0f0f1a; }");

    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Title
    auto* title = new QLabel("CWX");
    title->setStyleSheet("QLabel { color: #00b4d8; font-size: 14px; font-weight: bold; "
                         "padding: 6px 8px; background: #0a0a14; }");
    vbox->addWidget(title);

    // Stacked widget for Send/Live vs Setup
    m_stack = new QStackedWidget;
    vbox->addWidget(m_stack, 1);

    buildSendView();
    buildSetupView();

    m_stack->addWidget(m_sendPage);
    m_stack->addWidget(m_setupPage);
    m_stack->setCurrentWidget(m_sendPage);

    // ── Bottom bar ─────────────────────────────────────────────
    auto* bar = new QWidget;
    bar->setStyleSheet("QWidget { background: #0a0a14; }");
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(4, 4, 4, 4);
    barLayout->setSpacing(4);

    m_sendBtn = new QPushButton("Send");
    m_sendBtn->setCheckable(true);
    m_sendBtn->setChecked(true);
    m_sendBtn->setStyleSheet(kBtnStyle);
    barLayout->addWidget(m_sendBtn);

    m_liveBtn = new QPushButton("Live");
    m_liveBtn->setCheckable(true);
    m_liveBtn->setStyleSheet(kBtnStyle);
    barLayout->addWidget(m_liveBtn);

    m_setupBtn = new QPushButton("Setup");
    m_setupBtn->setCheckable(true);
    m_setupBtn->setStyleSheet(QString(kBtnStyle) +
        " QPushButton { padding: 4px 6px; }");
    barLayout->addWidget(m_setupBtn);

    auto* speedLabel = new QLabel("Speed:");
    speedLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");
    barLayout->addWidget(speedLabel);

    m_speedSpin = new QSpinBox;
    m_speedSpin->setRange(5, 100);
    m_speedSpin->setValue(20);
    m_speedSpin->setFixedWidth(50);
    m_speedSpin->setStyleSheet(
        "QSpinBox { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050; "
        "border-radius: 2px; font-size: 11px; padding: 2px; }");
    barLayout->addWidget(m_speedSpin);

    vbox->addWidget(bar);

    // ── Connections ─────────────────────────────────────────────

    // Send/Live/Setup toggle — mutual exclusion
    connect(m_sendBtn, &QPushButton::clicked, this, [this]() {
        m_sendBtn->setChecked(true);
        m_liveBtn->setChecked(false);
        m_setupBtn->setChecked(false);
        if (m_model) m_model->setLive(false);
        resetLiveTracking();
        showSendView();
    });
    connect(m_liveBtn, &QPushButton::clicked, this, [this]() {
        m_sendBtn->setChecked(false);
        m_liveBtn->setChecked(true);
        m_setupBtn->setChecked(false);
        if (m_model) m_model->setLive(true);
        resetLiveTracking();
        showSendView();
    });
    connect(m_setupBtn, &QPushButton::clicked, this, [this]() {
        m_sendBtn->setChecked(false);
        m_liveBtn->setChecked(false);
        m_setupBtn->setChecked(true);
        showSetupView();
    });

    // Speed
    connect(m_speedSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (m_model) m_model->setSpeed(v); });

    // Wire model signals
    // ── F1-F12 hotkeys — active app-wide when the active slice is in a CW
    //    mode (CW or CWL).  Guard prevents collisions with a future DVK
    //    macro panel or other function-key users. (#1552)
    for (int i = 0; i < 12; ++i) {
        auto* sc = new QShortcut(Qt::Key_F1 + i, window());
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this, [this, i]() {
            if (!m_model) return;
            if (m_activeModeProvider) {
                const QString mode = m_activeModeProvider();
                if (mode != QLatin1String("CW") && mode != QLatin1String("CWL"))
                    return;
            }
            m_model->sendMacro(i + 1);
        });
    }

    // ── ESC — abort CW transmission.  Fires unconditionally: during a CW
    //    macro the interlock state flickers TRANSMITTING↔READY every
    //    dit/dah (~150ms), so gating on "is the radio TXing right now"
    //    misses most key-presses.  clearBuffer() on an idle CWX is a
    //    harmless no-op, and Qt's widget-level ESC handling (dialog
    //    reject, text unfocus) runs before our ApplicationShortcut
    //    anyway, so normal UI ESC behavior is preserved. (#1552)
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), window());
    esc->setContext(Qt::ApplicationShortcut);
    connect(esc, &QShortcut::activated, this, [this]() {
        if (m_model)
            m_model->clearBuffer();
    });

    if (m_model) setModel(m_model);
}

void CwxPanel::setModel(CwxModel* model)
{
    m_model = model;
    if (!m_model) return;

    connect(m_model, &CwxModel::charSent, this, &CwxPanel::onCharSent);
    connect(m_model, &CwxModel::messageQueued, this, &CwxPanel::onMessageQueued);
    connect(m_model, &CwxModel::erased, this, &CwxPanel::onErased);
    connect(m_model, &CwxModel::speedChanged, this, &CwxPanel::onSpeedChanged);
    connect(m_model, &CwxModel::macroChanged, this, [this](int idx, const QString& text) {
        if (idx >= 0 && idx < 12 && m_macroEdits[idx]) {
            QSignalBlocker b(m_macroEdits[idx]);
            m_macroEdits[idx]->setPlainText(text);
        }
    });
    connect(m_model, &CwxModel::delayChanged, this, [this](int ms) {
        if (m_delaySpin) {
            QSignalBlocker b(m_delaySpin);
            m_delaySpin->setValue(ms);
        }
    });
    connect(m_model, &CwxModel::qskChanged, this, [this](bool on) {
        if (m_qskBtn) {
            QSignalBlocker b(m_qskBtn);
            m_qskBtn->setChecked(on);
        }
    });
}

void CwxPanel::buildSendView()
{
    m_sendPage = new QWidget;
    auto* vbox = new QVBoxLayout(m_sendPage);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(2);

    // History — scroll area with painted bubbles, scrolls from bottom up
    m_historyScroll = new QScrollArea;
    m_historyScroll->setWidgetResizable(true);
    m_historyScroll->setStyleSheet("QScrollArea { background: #0a0a14; border: none; }");
    m_historyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_historyContainer = new QWidget;
    m_historyContainer->setStyleSheet("QWidget { background: #0a0a14; }");
    m_historyLayout = new QVBoxLayout(m_historyContainer);
    m_historyLayout->setContentsMargins(0, 0, 0, 4);
    m_historyLayout->setSpacing(4);
    m_historyLayout->addStretch(1);  // push bubbles to bottom
    m_historyScroll->setWidget(m_historyContainer);
    vbox->addWidget(m_historyScroll, 1);

    // Input area at the bottom (editable, where user types)
    m_textEdit = new QTextEdit;
    m_textEdit->setStyleSheet(kTextStyle +
        QString(" QTextEdit { border-top: 1px solid #304050; }"));
    m_textEdit->setPlaceholderText("Type CW message...");
    m_textEdit->setAcceptRichText(false);
    m_textEdit->setFixedHeight(60);
    m_textEdit->installEventFilter(this);
    vbox->addWidget(m_textEdit, 0);
}

void CwxPanel::buildSetupView()
{
    m_setupPage = new QWidget;
    auto* vbox = new QVBoxLayout(m_setupPage);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    // Delay + QSK
    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel("Delay:"));
    m_delaySpin = new QSpinBox;
    m_delaySpin->setRange(0, 2000);
    m_delaySpin->setValue(5);
    m_delaySpin->setFixedWidth(60);
    m_delaySpin->setStyleSheet(
        "QSpinBox { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050; "
        "border-radius: 2px; font-size: 11px; }");
    topRow->addWidget(m_delaySpin);

    m_qskBtn = new QPushButton("QSK");
    m_qskBtn->setCheckable(true);
    m_qskBtn->setStyleSheet(kBtnStyle);
    topRow->addWidget(m_qskBtn);
    topRow->addStretch(1);
    vbox->addLayout(topRow);

    connect(m_delaySpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (m_model) m_model->setDelay(v); });
    connect(m_qskBtn, &QPushButton::toggled,
            this, [this](bool on) { if (m_model) m_model->setQsk(on); });

    // Style labels
    for (auto* lbl : m_setupPage->findChildren<QLabel*>())
        lbl->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");

    // F1-F12 macro slots — each stretches to fill available height
    auto* macroWidget = new QWidget;
    auto* macroGrid = new QGridLayout(macroWidget);
    macroGrid->setContentsMargins(0, 0, 0, 0);
    macroGrid->setSpacing(2);

    for (int i = 0; i < 12; ++i) {
        auto* label = new QPushButton(QString("F%1").arg(i + 1));
        label->setFixedWidth(28);
        label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        label->setStyleSheet(
            "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
            "border-radius: 2px; color: #c8d8e8; font-size: 10px; font-weight: bold; "
            "padding: 2px; }"
            "QPushButton:hover { background: #203040; }");
        macroGrid->addWidget(label, i, 0);

        m_macroEdits[i] = new QTextEdit;
        m_macroEdits[i]->setStyleSheet(
            "QTextEdit { background: #ffffff; color: #000000; border: 1px solid #304050; "
            "border-radius: 2px; padding: 2px; font-size: 11px; }");
        m_macroEdits[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_macroEdits[i]->setPlaceholderText(QString("F%1 macro...").arg(i + 1));
        m_macroEdits[i]->setAcceptRichText(false);
        m_macroEdits[i]->setLineWrapMode(QTextEdit::WidgetWidth);
        macroGrid->addWidget(m_macroEdits[i], i, 1);

        macroGrid->setRowStretch(i, 1);

        // Click F-key label → send macro
        connect(label, &QPushButton::clicked, this, [this, i]() {
            if (m_model) m_model->sendMacro(i + 1);
        });

        // Edit → save macro (debounced — save when focus leaves)
        connect(m_macroEdits[i], &QTextEdit::textChanged, this, [this, i]() {
            if (m_model && m_macroEdits[i])
                m_model->saveMacro(i, m_macroEdits[i]->toPlainText().trimmed());
        });
    }

    vbox->addWidget(macroWidget, 1);

    // Prosign legend
    auto* legend = new QLabel("Prosigns: = (BT)  + (AR)  ( (KN)  & (BK)  $ (SK)");
    legend->setStyleSheet("QLabel { color: #607080; font-size: 9px; padding: 4px; }");
    legend->setWordWrap(true);
    vbox->addWidget(legend);
}

void CwxPanel::showSendView()
{
    m_stack->setCurrentWidget(m_sendPage);
    m_textEdit->setFocus();
}

void CwxPanel::showSetupView()
{
    m_stack->setCurrentWidget(m_setupPage);
}

void CwxPanel::sendBuffer()
{
    if (!m_model || !m_textEdit) return;
    QString text = m_textEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;

    // Move text to history — painted bubble in scroll area
    if (m_historyLayout) {
        // Add painted bubble to history scroll area
        QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
        const int block = m_model->send(text);
        auto* bubble = new CwxBubble(text, ts, block, m_historyContainer);
        m_historyLayout->addWidget(bubble);
        m_bubbles.append(bubble);
        // Bound tracked bubbles; old ones keep their final painted state.
        while (m_bubbles.size() > kMaxTrackedBubbles) {
            m_bubbles.removeFirst();
        }
        // Keep scrolled to bottom
        QTimer::singleShot(10, this, [this]() {
            auto* sb = m_historyScroll->verticalScrollBar();
            sb->setValue(sb->maximum());
        });
    }
    m_textEdit->clear();
}

void CwxPanel::onMessageQueued(int block, int radioIndex)
{
    // No catch-up from CwxModel::sentIndex() here: the radio may restart
    // buffer numbering once idle, so a stale index could falsely highlight.
    // The send reply always precedes the first "sent=" for that block.
    // If numbering restarted, older bubbles/live chars at or beyond this
    // index are stale and must not react to new status.
    for (const QPointer<CwxBubble>& b : m_bubbles) {
        if (b && b->block() != block && b->startIndex() >= radioIndex) {
            b->retire();
        }
    }
    for (LiveChar& lc : m_liveChars) {
        if (lc.radioIndex >= radioIndex) {
            lc.radioIndex = -1;
        }
    }

    // Live mode: single-character block → map radio index to the character
    auto live = m_livePendingBlocks.find(block);
    if (live != m_livePendingBlocks.end()) {
        const int charId = live.value();
        m_livePendingBlocks.erase(live);
        for (LiveChar& lc : m_liveChars) {
            if (lc.id == charId) {
                lc.radioIndex = radioIndex;
                break;
            }
        }
        return;
    }

    for (const QPointer<CwxBubble>& b : m_bubbles) {
        if (b && b->block() == block) {
            b->setStartIndex(radioIndex);
            return;
        }
    }
}

void CwxPanel::onCharSent(int index)
{
    // "cwx sent=<index>": index is the radio CWX buffer position of the
    // character just keyed (SmartSDR API v1.4.0.0 CWX status).
    for (const QPointer<CwxBubble>& b : m_bubbles) {
        if (b && b->startIndex() >= 0 && index >= b->startIndex()) {
            b->setSentThrough(index);
        }
    }
    updateLiveHighlight(index);
}

void CwxPanel::onErased(int start, int stop)
{
    if (stop < start) {
        return;
    }
    for (const QPointer<CwxBubble>& b : m_bubbles) {
        if (b) {
            b->markErased(start, stop);
        }
    }

    // Live mode: the radio removed these characters before keying them.
    // Remove any that are still displayed in the input area (e.g. cleared
    // by ESC or another client) and forget their mapping.
    if (!m_textEdit) {
        return;
    }
    QTextDocument* doc = m_textEdit->document();
    for (int i = m_liveChars.size() - 1; i >= 0; --i) {
        const LiveChar lc = m_liveChars[i];
        if (lc.radioIndex < start || lc.radioIndex > stop) {
            continue;
        }
        m_liveChars.removeAt(i);
        if (lc.docPos >= 0 && lc.docPos < doc->characterCount() - 1) {
            QTextCursor c(doc);
            c.setPosition(lc.docPos);
            c.setPosition(lc.docPos + 1, QTextCursor::KeepAnchor);
            if (c.selectedText() == lc.ch) {
                QSignalBlocker blk(m_textEdit);
                c.removeSelectedText();
                for (LiveChar& other : m_liveChars) {
                    if (other.docPos > lc.docPos) {
                        --other.docPos;
                    }
                }
            }
        }
    }
}

void CwxPanel::updateLiveHighlight(int sentIndex)
{
    if (!m_textEdit || m_liveChars.isEmpty()) {
        return;
    }
    QTextDocument* doc = m_textEdit->document();
    QTextCharFormat sentFmt;
    sentFmt.setForeground(QColor(0x00, 0xb4, 0xd8));
    for (LiveChar& lc : m_liveChars) {
        if (lc.sent || lc.radioIndex < 0 || lc.radioIndex > sentIndex) {
            continue;
        }
        lc.sent = true;
        if (lc.docPos >= 0 && lc.docPos < doc->characterCount() - 1) {
            QTextCursor c(doc);
            c.setPosition(lc.docPos);
            c.setPosition(lc.docPos + 1, QTextCursor::KeepAnchor);
            if (c.selectedText() == lc.ch) {
                QSignalBlocker blk(m_textEdit);
                c.mergeCharFormat(sentFmt);
            }
        }
    }
    // Drop fully-keyed entries so the list stays small.
    m_liveChars.erase(std::remove_if(m_liveChars.begin(), m_liveChars.end(),
                                     [](const LiveChar& lc) { return lc.sent; }),
                      m_liveChars.end());
}

void CwxPanel::resetLiveTracking()
{
    m_liveChars.clear();
    m_livePendingBlocks.clear();
}

void CwxPanel::onSpeedChanged(int wpm)
{
    QSignalBlocker b(m_speedSpin);
    m_speedSpin->setValue(wpm);
}

} // namespace AetherSDR

// Event filter for text edit — handle Enter and per-key sending
bool AetherSDR::CwxPanel::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_textEdit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);

        if (ke->key() == Qt::Key_Escape) {
            if (m_model) m_model->clearBuffer();
            m_textEdit->clear();
            resetLiveTracking();
            return true;
        }

        if (m_model && m_model->isLive()) {
            // Live mode: send each character immediately
            QString text = ke->text();
            if (!text.isEmpty() && ke->key() != Qt::Key_Return && ke->key() != Qt::Key_Enter) {
                if (ke->key() == Qt::Key_Backspace) {
                    m_model->erase(1);
                    // Local deletion happens optimistically; forget the
                    // mapping of the last unsent character so a later
                    // erase status does not remove a different char.
                    if (!m_liveChars.isEmpty()) {
                        m_liveChars.removeLast();
                    }
                } else {
                    // Live keying is append-only on the radio side, so keep
                    // the local cursor at the end to match buffer order.
                    m_textEdit->moveCursor(QTextCursor::End);
                    const int block = m_model->sendChar(text);
                    if (block >= 0 && text.size() == 1) {
                        LiveChar lc;
                        lc.id = m_nextLiveCharId++;
                        lc.ch = text;
                        lc.docPos = m_textEdit->document()->characterCount() - 1;
                        m_liveChars.append(lc);
                        m_livePendingBlocks.insert(block, lc.id);
                        while (m_liveChars.size() > kMaxLiveChars) {
                            // The hash is keyed by block, not by LiveChar id:
                            // drop the entry that maps *to* the evicted char.
                            const int evictedId = m_liveChars.first().id;
                            m_livePendingBlocks.removeIf([evictedId](const auto& entry) {
                                return entry.value() == evictedId;
                            });
                            m_liveChars.removeFirst();
                        }
                    }
                }
            }
            // Still let the text edit display the character
            return false;
        }

        // Send mode: Enter sends the buffer
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            sendBuffer();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}
