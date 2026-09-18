#include "CommandPalette.h"
#include "core/ShortcutManager.h"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {
constexpr int kMaxResults = 200;
}

CommandPalette::CommandPalette(ShortcutManager* manager, QWidget* parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(tr("Command Palette"));
    setModal(true);
    resize(640, 440);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Type a command…"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setAccessibleName(tr("Command filter"));
    m_filter->setAccessibleDescription(
        tr("Type to filter the list of available commands. "
           "Use the up and down arrow keys to choose one, then press Enter to run it."));
    layout->addWidget(m_filter);

    m_list = new QListWidget(this);
    m_list->setAccessibleName(tr("Matching commands"));
    m_list->setUniformItemSizes(true);
    layout->addWidget(m_list, 1);

    m_hint = new QLabel(tr("Enter to run  •  Esc to close"), this);
    m_hint->setAlignment(Qt::AlignRight);
    // Decorative restatement of the accessible description above — hiding it
    // from assistive tech avoids reading the same hint twice.
    m_hint->setAccessibleDescription(QString());
    layout->addWidget(m_hint);

    connect(m_filter, &QLineEdit::textChanged, this, &CommandPalette::rebuildList);
    connect(m_filter, &QLineEdit::returnPressed, this, &CommandPalette::runSelected);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem*) {
        runSelected();
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem*) {
        runSelected();
    });

    // Arrow keys move the selection while focus stays in the filter box, so
    // the user never has to tab away to pick a result.
    m_filter->installEventFilter(this);
}

void CommandPalette::showPalette()
{
    m_filter->clear();
    rebuildList(QString());
    if (parentWidget()) {
        const QRect pg = parentWidget()->geometry();
        move(pg.center().x() - width() / 2,
             pg.top() + pg.height() / 6);
    }
    show();
    raise();
    activateWindow();
    m_filter->setFocus(Qt::OtherFocusReason);
}

// Subsequence match with positional bonuses. Returns -1 when `needle` is not a
// subsequence of `haystack`; otherwise higher scores are better matches.
int CommandPalette::fuzzyScore(const QString& needle, const QString& haystack)
{
    if (needle.isEmpty()) return 0;

    int score = 0;
    int hi = 0;
    int consecutive = 0;

    for (const QChar nc : needle) {
        bool found = false;
        while (hi < haystack.size()) {
            const QChar hc = haystack.at(hi);
            if (hc.toCaseFolded() == nc.toCaseFolded()) {
                // Start-of-word and start-of-string hits are far more likely to
                // be what the user meant than a letter buried mid-word.
                if (hi == 0) {
                    score += 10;
                } else if (!haystack.at(hi - 1).isLetterOrNumber()) {
                    score += 6;
                } else {
                    score += 1;
                }
                score += consecutive;   // reward runs of adjacent matches
                ++consecutive;
                ++hi;
                found = true;
                break;
            }
            consecutive = 0;
            ++hi;
        }
        if (!found) return -1;
    }

    // Prefer shorter names when scores are otherwise close: an exact short
    // command should outrank a long one that merely contains the letters.
    score -= haystack.size() / 16;
    return score;
}

void CommandPalette::rebuildList(const QString& filter)
{
    m_visible.clear();
    m_list->clear();
    if (!m_manager) return;

    const QString needle = filter.trimmed();

    for (const ShortcutManager::Action& a : m_manager->actions()) {
        if (!a.handler) continue;                    // nothing to run
        if (a.id == QLatin1String("command_palette")) continue;  // don't list ourselves

        // Match against "Category Name" so users can narrow by either.
        const QString haystack = a.category.isEmpty()
                                     ? a.displayName
                                     : a.category + ' ' + a.displayName;
        const int score = fuzzyScore(needle, haystack);
        if (score < 0) continue;

        Entry e;
        e.id           = a.id;
        e.displayName  = a.displayName;
        e.category     = a.category;
        e.shortcutText = a.currentKey.isEmpty()
                             ? QString()
                             : a.currentKey.toString(QKeySequence::NativeText);
        e.score        = score;
        m_visible.push_back(e);
    }

    std::stable_sort(m_visible.begin(), m_visible.end(),
                     [](const Entry& a, const Entry& b) {
                         if (a.score != b.score) return a.score > b.score;
                         return a.displayName.localeAwareCompare(b.displayName) < 0;
                     });

    if (m_visible.size() > kMaxResults)
        m_visible.resize(kMaxResults);

    for (const Entry& e : m_visible) {
        QString text = e.displayName;
        if (!e.category.isEmpty())
            text = e.category + "  ›  " + e.displayName;
        if (!e.shortcutText.isEmpty())
            text += "\t" + e.shortcutText;

        auto* item = new QListWidgetItem(text, m_list);
        // Screen readers get the shortcut spoken as part of the entry rather
        // than as an unexplained trailing token.
        item->setData(Qt::AccessibleTextRole,
                      e.shortcutText.isEmpty()
                          ? e.displayName
                          : tr("%1, shortcut %2").arg(e.displayName, e.shortcutText));
    }

    if (m_list->count() > 0)
        m_list->setCurrentRow(0);

    m_hint->setText(m_visible.isEmpty()
                        ? tr("No matching commands")
                        : tr("%n command(s)  •  Enter to run  •  Esc to close",
                             "", static_cast<int>(m_visible.size())));
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        switch (ke->key()) {
        case Qt::Key_Down:     moveSelection(1);   return true;
        case Qt::Key_Up:       moveSelection(-1);  return true;
        case Qt::Key_PageDown: moveSelection(10);  return true;
        case Qt::Key_PageUp:   moveSelection(-10); return true;
        default: break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CommandPalette::moveSelection(int delta)
{
    const int count = m_list->count();
    if (count == 0) return;
    int row = m_list->currentRow() + delta;
    // Wrap, so holding Up from the top jumps to the last entry.
    if (row < 0)       row = count - 1;
    if (row >= count)  row = 0;
    m_list->setCurrentRow(row);
}

void CommandPalette::runSelected()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_visible.size()) return;

    const QString id = m_visible.at(row).id;

    // Close before dispatching: handlers may open their own modal dialogs, and
    // the palette should not sit on top of whatever the command puts up.
    accept();

    if (!m_manager) return;
    if (ShortcutManager::Action* a = m_manager->action(id)) {
        if (a->handler) a->handler();
    }
}

} // namespace AetherSDR
