#pragma once

#include "gui/MainWindow.h"

#include <QObject>
#include <QPointer>

namespace AetherSDR {

class ScannerPanel;

// Radio memories: Memory dialog, quick-add, memory spot feed, memory browse overlay and band-stack bookmarks.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class MemoryController : public QObject {
public:
    explicit MemoryController(MainWindow& mw);

    void showMemoryDialog();
    void showQuickAddMemoryDialog(const QString& preferredPanId = {});
    void syncMemorySpot(int memoryIndex);
    void removeMemorySpot(int memoryIndex);
    void clearMemorySpotFeed();
    void rebuildMemorySpotFeed();
    void refreshMemoryBrowsePanel();
    SliceModel* preferredMemorySlice(const QString& preferredPanId) const;
    bool activateMemorySpot(int memoryIndex, const QString& preferredPanId = {});
    void wireBandStackPanel();

    // Memory / band / frequency-list scanner window (user-started scans only).
    void showScannerPanel();
    ScannerPanel* scannerPanel() const;

    // Memory spots use negative SpotModel indices (see memorySpotId()).
    static int memoryIndexFromSpotId(int spotIndex);

private:
    MainWindow& m_mw;
    QPointer<ScannerPanel> m_scannerPanel;
};

} // namespace AetherSDR
