#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// Builds the File/Settings/Profiles/View/Help menus (Mode menu lives in DigitalModesController).
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class MenuBuilder : public QObject {
public:
    explicit MenuBuilder(MainWindow& mw);

    void buildMenuBar();
    void buildFileMenu();
    void buildSettingsMenu();
    void buildProfilesMenu();
    void buildViewMenu();
    void buildLogMenu();
    void buildHelpMenu();

private:
    MainWindow& m_mw;

    QPointer<WhatsNewDialog> m_whatsNewDialog;
    // Menus
    QMenu*           m_profilesMenu{nullptr};
};

} // namespace AetherSDR
