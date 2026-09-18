// Tests for the command palette's filtering, ranking, and dispatch.
//
// The palette is the discovery path for 70+ actions, so a ranking regression
// (the right command not appearing first) is a real usability bug even though
// nothing crashes. These assert ordering, not just membership.
//
// Build: produced by CMake as `command_palette_test`.
// Run:   QT_QPA_PLATFORM=offscreen ./build/command_palette_test

#include "gui/CommandPalette.h"
#include "core/ShortcutManager.h"

#include <QApplication>
#include <QLineEdit>
#include <QListWidget>

#include <cstdio>
#include <string>

using AetherSDR::CommandPalette;
using AetherSDR::ShortcutManager;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-52s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// Build a manager stocked with representative AetherSDR actions.
void populate(ShortcutManager& m, int* tuneUpCounter)
{
    m.registerAction("tune_up_1", "Tune Up (1 step)", "Frequency",
                     QKeySequence("Right"), [tuneUpCounter] { ++(*tuneUpCounter); });
    m.registerAction("tune_down_1", "Tune Down (1 step)", "Frequency",
                     QKeySequence("Left"), [] {});
    m.registerAction("show_swr_meter", "Show SWR Meter", "View",
                     QKeySequence(), [] {});
    m.registerAction("toggle_waterfall", "Toggle Waterfall", "View",
                     QKeySequence("Ctrl+W"), [] {});
    m.registerAction("mox_toggle", "Toggle MOX", "Transmit",
                     QKeySequence("Ctrl+M"), [] {});
    m.registerAction("command_palette", "Command Palette", "View",
                     QKeySequence("Ctrl+Shift+P"), [] {});
}

QLineEdit*   filterBox(CommandPalette& p) { return p.findChild<QLineEdit*>(); }
QListWidget* resultList(CommandPalette& p) { return p.findChild<QListWidget*>(); }

QString rowText(CommandPalette& p, int row)
{
    auto* list = resultList(p);
    if (!list || row < 0 || row >= list->count()) return {};
    return list->item(row)->text();
}

// An empty filter lists everything except the palette's own entry.
void testEmptyFilterListsAllButSelf()
{
    ShortcutManager m;
    int counter = 0;
    populate(m, &counter);
    CommandPalette p(&m);
    p.showPalette();

    auto* list = resultList(p);
    const bool ok = list && list->count() == 5;   // 6 registered, minus self
    report("Empty filter lists all actions except itself", ok,
           "count=" + std::to_string(list ? list->count() : -1));
}

// Subsequence matching: "swr" should surface "Show SWR Meter".
void testSubsequenceMatch()
{
    ShortcutManager m;
    int counter = 0;
    populate(m, &counter);
    CommandPalette p(&m);
    p.showPalette();
    filterBox(p)->setText("swr");

    const bool ok = rowText(p, 0).contains("SWR");
    report("Subsequence match finds SWR meter", ok, rowText(p, 0).toStdString());
}

// Acronym-style matching across words: "tw" → "Toggle Waterfall".
void testWordBoundaryRanksFirst()
{
    ShortcutManager m;
    int counter = 0;
    populate(m, &counter);
    CommandPalette p(&m);
    p.showPalette();
    filterBox(p)->setText("tw");

    const bool ok = rowText(p, 0).contains("Toggle Waterfall");
    report("Word-boundary match ranks first", ok, rowText(p, 0).toStdString());
}

// Filtering by category name must work, since the haystack is "Category Name".
void testCategoryFilter()
{
    ShortcutManager m;
    int counter = 0;
    populate(m, &counter);
    CommandPalette p(&m);
    p.showPalette();
    filterBox(p)->setText("frequency");

    auto* list = resultList(p);
    bool allFrequency = list && list->count() == 2;
    if (allFrequency) {
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->text().contains("Frequency")) { allFrequency = false; break; }
        }
    }
    report("Category name filters to that category", allFrequency,
           "count=" + std::to_string(list ? list->count() : -1));
}

// A filter matching nothing must clear the list rather than show stale results.
void testNoMatchClearsList()
{
    ShortcutManager m;
    int counter = 0;
    populate(m, &counter);
    CommandPalette p(&m);
    p.showPalette();
    filterBox(p)->setText("swr");
    filterBox(p)->setText("zzzzqqq");

    auto* list = resultList(p);
    report("Non-matching filter clears the list", list && list->count() == 0,
           "count=" + std::to_string(list ? list->count() : -1));
}

// Selecting a row and confirming must invoke that action's handler exactly once.
void testActivationRunsHandler()
{
    ShortcutManager m;
    int counter = 0;
    populate(m, &counter);
    CommandPalette p(&m);
    p.showPalette();
    filterBox(p)->setText("tune up");

    const bool matched = rowText(p, 0).contains("Tune Up");
    emit filterBox(p)->returnPressed();

    report("Enter runs the selected action's handler",
           matched && counter == 1,
           "matched=" + std::to_string(matched) + " calls=" + std::to_string(counter));
}

// The palette must not run anything when there is nothing selected.
void testActivationWithNoMatchIsSafe()
{
    ShortcutManager m;
    int counter = 0;
    populate(m, &counter);
    CommandPalette p(&m);
    p.showPalette();
    filterBox(p)->setText("zzzzqqq");
    emit filterBox(p)->returnPressed();

    report("Enter with no match runs nothing", counter == 0,
           "calls=" + std::to_string(counter));
}

// Registered shortcuts should be shown so the palette teaches key bindings.
void testShortcutTextIsShown()
{
    ShortcutManager m;
    int counter = 0;
    populate(m, &counter);
    CommandPalette p(&m);
    p.showPalette();
    filterBox(p)->setText("toggle waterfall");

    const QString text = rowText(p, 0);
    // Compare against the platform's own rendering: macOS shows Ctrl+W as ⌘W.
    const QString expected =
        QKeySequence("Ctrl+W").toString(QKeySequence::NativeText);
    report("Result row shows the bound shortcut",
           !expected.isEmpty() && text.contains(expected),
           text.toStdString() + "  expected≈" + expected.toStdString());
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    testEmptyFilterListsAllButSelf();
    testSubsequenceMatch();
    testWordBoundaryRanksFirst();
    testCategoryFilter();
    testNoMatchClearsList();
    testActivationRunsHandler();
    testActivationWithNoMatchIsSafe();
    testShortcutTextIsShown();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "ALL PASS" : "FAILURES",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
