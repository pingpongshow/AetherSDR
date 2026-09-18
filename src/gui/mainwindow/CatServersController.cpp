#include "gui/mainwindow/CatServersController.h"

#include <algorithm>

#include "gui/MainWindow.h"
#include "core/LogManager.h"
#include "gui/AppletPanel.h"
#include "gui/CatControlApplet.h"
#include "gui/DaxApplet.h"
#include "gui/TciApplet.h"
#include "gui/DaxIqApplet.h"
#include "models/SliceModel.h"
#include "core/AppSettings.h"
#include "core/CatKenwoodPty.h"
#include "core/CatKenwoodRadioAdapter.h"
#include "core/CatKenwoodServer.h"
#include "gui/mainwindow/SliceController.h"

namespace AetherSDR {

CatServersController::CatServersController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void CatServersController::createCatServers()
{
    // ── 8-channel CAT: rigctld + PTY (A-H, each bound to a slice) ────────────
    {
        static const char kLetters[] = "ABCDEFGH";
        for (int i = 0; i < kCatChannels; ++i) {
            m_rigctlServers[i] = new RigctlServer(&m_mw.m_radioModel, &m_mw);
            m_rigctlServers[i]->setSliceIndex(i);
            m_rigctlPtys[i] = new RigctlPty(&m_mw.m_radioModel, &m_mw);
            m_rigctlPtys[i]->setSliceIndex(i);
            m_rigctlPtys[i]->setSymlinkPath(
                QString("/tmp/AetherSDR-CAT-%1").arg(kLetters[i]));
        }
    }
    // ── SmartSDR-CAT-compatible Kenwood TS-2000 + ZZ ports ───────────────────
    m_kcatAdapter = new CatKenwoodRadioAdapter(&m_mw.m_radioModel, this);
    m_kcatAdapter->setActiveSliceProvider([this] {
        SliceModel* s = m_mw.activeSlice();
        return s ? s->sliceId() : -1;
    });
    connect(m_kcatAdapter, &CatKenwoodRadioAdapter::activeSliceRequested,
            this, [this](int sliceId) { m_mw.m_sliceCtl->setActiveSlice(sliceId); });
    for (int i = 0; i < kKenwoodCatPorts; ++i) {
        m_kcatServers[i] = new CatKenwoodServer(m_kcatAdapter, this);
        m_kcatPtys[i] = new CatKenwoodPty(m_kcatAdapter, this);
        m_kcatPtys[i]->setSymlinkPath(QString("/tmp/AetherSDR-KCAT-%1").arg(QChar('A' + i)));
        connect(m_kcatAdapter, &CatKenwoodRadioAdapter::stateChanged,
                m_kcatServers[i], &CatKenwoodServer::notifyRadioStateChanged);
        connect(m_kcatAdapter, &CatKenwoodRadioAdapter::stateChanged,
                m_kcatPtys[i], &CatKenwoodPty::notifyRadioStateChanged);
        applyKenwoodCatPortConfig(i);
    }

    m_mw.m_appletPanel->catControlApplet()->setRadioModel(&m_mw.m_radioModel);
    m_mw.m_appletPanel->catControlApplet()->setRigctlServers(m_rigctlServers, kCatChannels);
    m_mw.m_appletPanel->catControlApplet()->setRigctlPtys(m_rigctlPtys, kCatChannels);
    m_mw.m_appletPanel->daxApplet()->setRadioModel(&m_mw.m_radioModel);
    m_mw.m_appletPanel->daxIqApplet()->setRadioModel(&m_mw.m_radioModel);
#ifdef HAVE_WEBSOCKETS
    m_tciServer = new TciServer(&m_mw.m_radioModel, &m_mw);
    m_tciServer->setAudioEngine(m_mw.m_audio);
    m_mw.m_appletPanel->tciApplet()->setRadioModel(&m_mw.m_radioModel);
    m_mw.m_appletPanel->tciApplet()->setTciServer(m_tciServer);

    // TCI applet sliders → TciServer gain setters
    connect(m_mw.m_appletPanel->tciApplet(), &TciApplet::tciRxGainChanged,
            m_tciServer, &TciServer::setRxChannelGain);
    connect(m_mw.m_appletPanel->tciApplet(), &TciApplet::tciTxGainChanged,
            m_tciServer, &TciServer::setTxGain);

    // TciServer level signals → TCI applet meters
    connect(m_tciServer, &TciServer::rxLevel,
            m_mw.m_appletPanel->tciApplet(), &TciApplet::setTciRxLevel);
    connect(m_tciServer, &TciServer::txLevel,
            m_mw.m_appletPanel->tciApplet(), &TciApplet::setTciTxLevel);

    // Wire slice state changes → TCI broadcasts
    connect(&m_mw.m_radioModel, &RadioModel::sliceAdded, this, [this](SliceModel* s) {
        if (m_tciServer)
            m_tciServer->wireSlice(s->sliceId(), s);
    });
    // Wire existing slices (radio may already be connected with slices)
    for (auto* s : m_mw.m_radioModel.slices())
        m_tciServer->wireSlice(s->sliceId(), s);
    m_tciServer->wireSpotModel();

    // Wire RX audio from PanadapterStream → TCI server for audio streaming.
    // TCI audio feeds exclusively from DAX (not audioDataReady) so that
    // audio_mute doesn't kill TCI audio (#1331).
    if (m_mw.m_radioModel.panStream()) {
        connect(m_mw.m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
                m_tciServer, &TciServer::onDaxAudioReady);
        connect(m_mw.m_radioModel.panStream(), &PanadapterStream::iqDataReady,
                m_tciServer, &TciServer::onIqDataReady);
    }

    // TCI client count changes no longer auto-create/remove the audio stream.
    // Control-only TCI clients (StreamDeck) don't need audio, and auto-creating
    // the stream overrode the user's explicit PC Audio toggle. Users who need
    // TCI audio (WSJT-X) should enable PC Audio manually. (#1071)
#endif
}

void CatServersController::setRigctldAutoStart(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue("AutoStartRigctld", on ? "True" : "False");
    s.save();
    if (m_mw.m_radioModel.isConnected()) {
        const int basePort = s.value("CatTcpPort", "4532").toInt();
        for (int i = 0; i < kCatChannels; ++i) {
            if (on && m_rigctlServers[i] && !m_rigctlServers[i]->isRunning())
                m_rigctlServers[i]->start(static_cast<quint16>(basePort + i));
            else if (!on && m_rigctlServers[i] && m_rigctlServers[i]->isRunning())
                m_rigctlServers[i]->stop();
        }
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->catControlApplet())
            m_mw.m_appletPanel->catControlApplet()->setTcpEnabled(on);
    }
}

#ifndef _WIN32
void CatServersController::setCatPtyAutoStart(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue("AutoStartCAT", on ? "True" : "False");
    s.save();
    if (m_mw.m_radioModel.isConnected()) {
        for (int i = 0; i < kCatChannels; ++i) {
            if (on && m_rigctlPtys[i] && !m_rigctlPtys[i]->isRunning())
                m_rigctlPtys[i]->start();
            else if (!on && m_rigctlPtys[i] && m_rigctlPtys[i]->isRunning())
                m_rigctlPtys[i]->stop();
        }
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->catControlApplet())
            m_mw.m_appletPanel->catControlApplet()->setPtyEnabled(on);
    }
}
#endif

void CatServersController::setTciAutoStart(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue("AutoStartTCI", on ? "True" : "False");
    s.save();
#ifdef HAVE_WEBSOCKETS
    if (m_tciServer) {
        if (on && !m_tciServer->isRunning()) {
            int port = s.value("TciPort", "50001").toInt();
            m_tciServer->start(static_cast<quint16>(port));
        } else if (!on && m_tciServer->isRunning()) {
            m_tciServer->stop();
        }
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->tciApplet())
            m_mw.m_appletPanel->tciApplet()->setTciEnabled(on);
    }
#endif
}

void CatServersController::applyKenwoodCatPortConfig(int i)
{
    auto& s = AppSettings::instance();
    CatKenwoodPortConfig cfg;
    const QString slice = s.value(QString("CatKenwoodSlice_%1").arg(i),
                                  QString(QChar('A' + i))).toString();
    if (slice == "Active" || slice.isEmpty()) {
        cfg.vfoASlice = CatKenwoodProtocol::kActiveSlice;
    } else {
        cfg.vfoASlice = std::clamp(slice.at(0).toUpper().toLatin1() - 'A', 0, 7);
    }
    cfg.autoSwitchTxSlice =
        s.value(QString("CatKenwoodAutoSwitchTx_%1").arg(i), "True").toString() == "True";
    cfg.splitMode = s.value(QString("CatKenwoodSplitMode_%1").arg(i), "SO2R").toString() == "SO2V"
        ? CatKenwoodProtocol::SplitMode::So2v : CatKenwoodProtocol::SplitMode::So2r;
    m_kcatServers[i]->setPortConfig(cfg);
    m_kcatPtys[i]->setPortConfig(cfg);
}

void CatServersController::setKenwoodCatSliceBinding(int port, int sliceIdOrActive)
{
    if (port < 0 || port >= kKenwoodCatPorts) {
        return;
    }
    auto& s = AppSettings::instance();
    s.setValue(QString("CatKenwoodSlice_%1").arg(port),
               sliceIdOrActive == CatKenwoodProtocol::kActiveSlice
                   ? QString("Active") : QString(QChar('A' + sliceIdOrActive)));
    s.save();
    applyKenwoodCatPortConfig(port);
}

void CatServersController::startKenwoodCat()
{
    auto& s = AppSettings::instance();
    const int base = s.value("CatKenwoodTcpBasePort", "5002").toInt();
    const bool tcp = s.value("CatKenwoodEnabled", "False").toString() == "True";
    const bool pty = s.value("CatKenwoodPtyEnabled", "False").toString() == "True";
    for (int i = 0; i < kKenwoodCatPorts; ++i) {
        if (tcp && !m_kcatServers[i]->isRunning()) {
            if (!m_kcatServers[i]->start(static_cast<quint16>(base + i))) {
                qCWarning(lcGui) << "Kenwood CAT: could not listen on port" << (base + i);
            }
        } else if (!tcp && m_kcatServers[i]->isRunning()) {
            m_kcatServers[i]->stop();
        }
#ifndef Q_OS_WIN
        if (pty && !m_kcatPtys[i]->isRunning()) {
            m_kcatPtys[i]->start();
        } else if (!pty && m_kcatPtys[i]->isRunning()) {
            m_kcatPtys[i]->stop();
        }
#endif
    }
}

void CatServersController::setKenwoodCatTcpEnabled(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue("CatKenwoodEnabled", on ? "True" : "False");
    s.save();
    if (m_mw.m_radioModel.isConnected()) {
        startKenwoodCat();
    }
}

void CatServersController::setKenwoodCatPtyEnabled(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue("CatKenwoodPtyEnabled", on ? "True" : "False");
    s.save();
    if (m_mw.m_radioModel.isConnected()) {
        startKenwoodCat();
    }
}

void CatServersController::autoStartCatServers()
{
    startKenwoodCat();

    // Auto-start 4-channel rigctld TCP servers if enabled
    auto& as = AppSettings::instance();
    if (as.value("AutoStartRigctld", "False").toString() == "True") {
        const int basePort = as.value("CatTcpPort", "4532").toInt();
        for (int i = 0; i < kCatChannels; ++i) {
            if (m_rigctlServers[i] && !m_rigctlServers[i]->isRunning()) {
                m_rigctlServers[i]->start(
                    static_cast<quint16>(basePort + i));
                qCDebug(lcGui) << "AutoStart: rigctld ch" << i
                         << "on port" << (basePort + i);
            }
        }
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->catControlApplet())
            m_mw.m_appletPanel->catControlApplet()->setTcpEnabled(true);
    }
    // Auto-start 8-channel CAT virtual serial ports if enabled
    if (as.value("AutoStartCAT", "False").toString() == "True") {
        for (int i = 0; i < kCatChannels; ++i) {
            if (m_rigctlPtys[i] && !m_rigctlPtys[i]->isRunning()) {
                m_rigctlPtys[i]->start();
                qCDebug(lcGui) << "AutoStart: PTY ch" << i
                         << m_rigctlPtys[i]->symlinkPath();
            }
        }
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->catControlApplet())
            m_mw.m_appletPanel->catControlApplet()->setPtyEnabled(true);
    }
#ifdef HAVE_WEBSOCKETS
    // Auto-start TCI WebSocket server if enabled
    if (as.value("AutoStartTCI", "False").toString() == "True") {
        if (m_tciServer && !m_tciServer->isRunning()) {
            int tciPort = as.value("TciPort", "50001").toInt();
            m_tciServer->start(static_cast<quint16>(tciPort));
            qCDebug(lcGui) << "AutoStart: TCI on port" << tciPort
                     << " running=" << m_tciServer->isRunning();
        }
        // Only light up the Enable button if the server actually bound —
        // otherwise the UI shows a green "Enable" while the port is in use.
        if (m_mw.m_appletPanel && m_mw.m_appletPanel->tciApplet())
            m_mw.m_appletPanel->tciApplet()->setTciEnabled(
                m_tciServer && m_tciServer->isRunning());
    }
#endif
}

} // namespace AetherSDR
