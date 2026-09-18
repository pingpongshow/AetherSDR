#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QLineEdit;
class QListWidget;
class QLabel;

namespace AetherSDR {

class ShortcutManager;

// Type-to-run command palette over every registered ShortcutManager action.
//
// AetherSDR exposes 70+ menu actions across a deep menu tree, so discovery —
// not capability — is the limiting factor. ShortcutManager already holds an
// id/name/category/key/handler record for each action, so the palette is a
// view over that registry rather than a second list to keep in sync: an action
// becomes reachable here the moment it is registered.
//
// Matching is subsequence-based ("swr" finds "Show SWR Meter"), ranked so
// word-boundary and prefix hits sort above scattered ones.
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    // `manager` must outlive the palette; the palette does not take ownership.
    explicit CommandPalette(ShortcutManager* manager, QWidget* parent = nullptr);

    // Show centred on the parent window with the filter cleared and focused.
    void showPalette();

protected:
    // Routes Up/Down/PageUp/PageDown from the filter box to the result list so
    // the user can pick a command without leaving the text field.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry {
        QString id;
        QString displayName;
        QString category;
        QString shortcutText;
        int     score{0};
    };

    void rebuildList(const QString& filter);
    void runSelected();
    void moveSelection(int delta);

    // Returns a match score for `needle` against `haystack`, or -1 for no
    // match. Higher is better.
    static int fuzzyScore(const QString& needle, const QString& haystack);

    ShortcutManager* m_manager{nullptr};
    QLineEdit*       m_filter{nullptr};
    QListWidget*     m_list{nullptr};
    QLabel*          m_hint{nullptr};
    QVector<Entry>   m_visible;
};

} // namespace AetherSDR
