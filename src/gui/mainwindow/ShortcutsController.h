#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// Keyboard shortcut actions (ShortcutManager registrations) and the command palette.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class ShortcutsController : public QObject {
public:
    explicit ShortcutsController(MainWindow& mw);

    void registerShortcutActions();
    // Type-to-run palette over every registered shortcut action (Ctrl+Shift+P).
    void showCommandPalette();

    // Shared shortcut guard (was file-scope in MainWindow.cpp).
    static bool isTextInputFocused();
    static bool shortcutGuard();
    static void setShortcutsGuardEnabled(bool on);

private:
    MainWindow& m_mw;

    CommandPalette* m_commandPalette{nullptr};  // lazily created, parented to the window
};

} // namespace AetherSDR
