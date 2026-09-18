#pragma once

#include "gui/MainWindow.h"

#include <QObject>

class QString;

namespace AetherSDR {

class CatKenwoodRadioAdapter;
class CatKenwoodServer;
class CatKenwoodPty;

// CAT servers: 8-channel rigctld TCP + virtual serial PTYs and the TCI WebSocket server.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class CatServersController : public QObject {
public:
    explicit CatServersController(MainWindow& mw);

    void createCatServers();
    void setRigctldAutoStart(bool on);
    void setTciAutoStart(bool on);
    void autoStartCatServers();
    // SmartSDR-CAT-compatible Kenwood/ZZ ports (TCP 5002+ and /tmp/AetherSDR-KCAT-A..H)
    void setKenwoodCatTcpEnabled(bool on);
    void setKenwoodCatPtyEnabled(bool on);
    void setKenwoodCatSliceBinding(int port, int sliceIdOrActive);
#ifndef _WIN32
    void setCatPtyAutoStart(bool on);
#endif

private:
    void applyKenwoodCatPortConfig(int i);
    void startKenwoodCat();

    MainWindow& m_mw;

    static constexpr int kKenwoodCatPorts = 8;
    CatKenwoodRadioAdapter* m_kcatAdapter{nullptr};
    CatKenwoodServer*       m_kcatServers[kKenwoodCatPorts]{};
    CatKenwoodPty*          m_kcatPtys[kKenwoodCatPorts]{};

    // 8-channel CAT: each channel (A-H) binds to a slice index (0-7)
    static constexpr int kCatChannels = 8;
    RigctlServer*     m_rigctlServers[kCatChannels]{};
    RigctlPty*        m_rigctlPtys[kCatChannels]{};
#ifdef HAVE_WEBSOCKETS
    TciServer*        m_tciServer{nullptr};
#endif
};

} // namespace AetherSDR
