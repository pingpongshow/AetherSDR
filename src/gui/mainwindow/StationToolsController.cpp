#include "gui/mainwindow/StationToolsController.h"

#include "core/ContestEngine.h"
#include "core/CwSkimmer.h"
#include "core/LogUploaders.h"
#include "core/QsoRecorder.h"
#include "core/WsjtxUdpServer.h"
#include "models/Logbook.h"
#include "models/SpotModel.h"
#include "gui/LogbookDialog.h"
#include "core/DxClusterClient.h"
#include "core/GeoUtils.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/PanadapterStream.h"
#include "core/RotatorClient.h"
#include "core/RotatorSettings.h"
#include "core/SpectrumRecorder.h"
#include "core/IqRecorder.h"
#include "gui/AppletPanel.h"
#include "gui/MainWindow.h"
#include "gui/PanadapterStack.h"
#include "gui/RecordingPanel.h"
#include "gui/CwSkimmerPanel.h"
#include "gui/mainwindow/SpotController.h"
#include "gui/RotatorWidget.h"
#include "models/PanadapterModel.h"
#include "models/SliceModel.h"

#include <QTimer>

#include <cmath>

namespace AetherSDR {

StationToolsController::StationToolsController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

// ── Rotator ────────────────────────────────────────────────────────────────

void StationToolsController::createRotator()
{
    m_rotatorClient = new RotatorClient(&m_mw);
    if (RotatorWidget* rot = m_mw.m_appletPanel ? m_mw.m_appletPanel->rotatorApplet() : nullptr) {
        rot->setRotatorClient(m_rotatorClient);
        rot->applySavedSettings();   // starts the client only if RotatorEnabled=True
    }
}

void StationToolsController::turnRotatorToCallsign(const QString& call, const QString& hint,
                                                   bool longPath)
{
    if (!m_rotatorClient) {
        return;
    }
    m_rotatorClient->setStationGrid(RotatorSettings::stationGrid());
    QString grid = GeoUtils::findGridInText(hint);
    m_rotatorClient->turnToCallsign(call, longPath, grid);
}

// ── Spectrum / IQ recording ──────────────────────────────────────────────────

void StationToolsController::ensureRecordingPanel()
{
    if (m_recordingPanel) {
        return;
    }
    auto* panel = new RecordingPanel(&m_mw);
    panel->setWindowFlag(Qt::Window);
    panel->setWindowTitle(QStringLiteral("Recordings"));
    m_recordingPanel = panel;

    // Network thread → recorders. DirectConnection is intentional: the submit
    // entry points are lock-free and only post to the recorder writer threads.
    if (PanadapterStream* ps = m_mw.m_radioModel.panStream()) {
        SpectrumRecorderBank* bank = panel->recorderBank();
        connect(ps, &PanadapterStream::spectrumReady,
                bank, &SpectrumRecorderBank::onSpectrumReady, Qt::DirectConnection);
        connect(ps, &PanadapterStream::waterfallRowReady,
                bank, &SpectrumRecorderBank::onWaterfallRowReady, Qt::DirectConnection);
        connect(ps, &PanadapterStream::iqDataReady,
                panel->iqRecorder(), &IqRecorder::submitIq, Qt::DirectConnection);
    }

    panel->setSpectrumResolver([this](const QString& panId) -> SpectrumWidget* {
        SpectrumWidget* sw = m_mw.m_panStack ? m_mw.m_panStack->spectrum(panId) : nullptr;
        return sw ? sw : m_mw.spectrum();
    });

    // IQ stream on demand (radio-side DAX IQ; RX only).
    connect(panel, &RecordingPanel::iqStreamSetupRequested, this, [this](int ch, int rate) {
        DaxIqModel& iq = m_mw.m_radioModel.daxIqModel();
        if (!iq.stream(ch).exists) {
            iq.createStream(ch);
        }
        iq.setSampleRate(ch, rate);
    });

    connect(&m_mw.m_radioModel, &RadioModel::panadapterAdded, panel, [this](PanadapterModel* pan) {
        wireRecordingPan(pan);
        syncRecordingPans();
    });
    connect(&m_mw.m_radioModel, &RadioModel::panadapterRemoved, panel, [this](const QString&) {
        syncRecordingPans();
    });
    for (PanadapterModel* pan : m_mw.m_radioModel.panadapters()) {
        wireRecordingPan(pan);
    }
    syncRecordingPans();

    // Slice markers are recorded as metadata; a 1 Hz refresh is plenty and
    // avoids hooking every slice signal.
    m_markerTimer = new QTimer(panel);
    m_markerTimer->setInterval(1000);
    connect(m_markerTimer, &QTimer::timeout, this, [this] { pushSliceMarkers(); });
    m_markerTimer->start();
    pushSliceMarkers();
}

void StationToolsController::showRecordingPanel()
{
    ensureRecordingPanel();
    m_recordingPanel->show();
    m_recordingPanel->raise();
    m_recordingPanel->activateWindow();
}

void StationToolsController::syncRecordingPans()
{
    if (!m_recordingPanel) {
        return;
    }
    QVector<RecordingPanel::PanInfo> v;
    for (PanadapterModel* pan : m_mw.m_radioModel.panadapters()) {
        v.append({pan->panId(),
                  QStringLiteral("%1 — %2 MHz").arg(pan->panId()).arg(pan->centerMhz(), 0, 'f', 3),
                  pan->panStreamId(), pan->wfStreamId(),
                  {pan->centerMhz(), pan->bandwidthMhz(), pan->minDbm(), pan->maxDbm()}});
    }
    m_recordingPanel->setPanadapters(v);
}

void StationToolsController::wireRecordingPan(PanadapterModel* pan)
{
    if (!pan || !m_recordingPanel) {
        return;
    }
    RecordingPanel* panel = m_recordingPanel;
    auto pushState = [panel, pan] {
        panel->updatePanState(pan->panId(),
            {pan->centerMhz(), pan->bandwidthMhz(), pan->minDbm(), pan->maxDbm()});
        if (pan->daxiqChannel() >= 1) {
            panel->setIqChannelCenter(pan->daxiqChannel(), qRound64(pan->centerMhz() * 1e6));
        }
    };
    // Context = panel: connections die with the panel or the pan, whichever goes first.
    connect(pan, &PanadapterModel::infoChanged, panel, pushState);
    connect(pan, &PanadapterModel::levelChanged, panel, pushState);
    connect(pan, &PanadapterModel::daxiqChannelChanged, panel, pushState);
    connect(pan, &PanadapterModel::waterfallIdChanged, panel, [this] { syncRecordingPans(); });
    pushState();
}

void StationToolsController::pushSliceMarkers()
{
    if (!m_recordingPanel) {
        return;
    }
    for (PanadapterModel* pan : m_mw.m_radioModel.panadapters()) {
        QVector<SpecRec::SliceMarker> mk;
        for (SliceModel* s : m_mw.m_radioModel.slices()) {
            if (s->panId() == pan->panId()) {
                mk.append({s->sliceId(), s->frequency(), s->filterLow(), s->filterHigh(),
                           s->isActive(), s->isTxSlice(), s->mode()});
            }
        }
        m_recordingPanel->updateSliceMarkers(pan->panId(), mk);
    }
}

void StationToolsController::shutdownRecording()
{
    if (!m_recordingPanel) {
        return;
    }
    if (m_markerTimer) {
        m_markerTimer->stop();
    }
    // Recorder slots are DirectConnection from the network thread; detach
    // them before the panel (and its writer threads) can be destroyed.
    if (PanadapterStream* ps = m_mw.m_radioModel.panStream()) {
        disconnect(ps, nullptr, m_recordingPanel->recorderBank(), nullptr);
        disconnect(ps, nullptr, m_recordingPanel->iqRecorder(), nullptr);
    }
    m_recordingPanel->close();
    delete m_recordingPanel.data();
}

// ── CW skimmer ───────────────────────────────────────────────────────────────

void StationToolsController::createCwSkimmer()
{
    m_cwSkimmer = new CwSkimmer(this);
    auto& as = AppSettings::instance();
    m_cwSkimmer->setDaxIqChannel(as.value("CwSkimmerDaxIqChannel", "1").toInt());
    if (PanadapterStream* ps = m_mw.m_radioModel.panStream()) {
        // feedRawIqPacket is thread-safe (bounded queue to the skimmer worker).
        connect(ps, &PanadapterStream::iqDataReady,
                m_cwSkimmer, &CwSkimmer::feedRawIqPacket, Qt::DirectConnection);
    }
    connect(&m_mw.m_radioModel.daxIqModel(), &DaxIqModel::streamChanged, this,
            [this](int) { updateSkimmerCenter(); });
    connect(&m_mw.m_radioModel, &RadioModel::panadapterAdded, this, [this](PanadapterModel* pan) {
        connect(pan, &PanadapterModel::infoChanged, this, [this] { updateSkimmerCenter(); });
    });
    for (PanadapterModel* pan : m_mw.m_radioModel.panadapters()) {
        connect(pan, &PanadapterModel::infoChanged, this, [this] { updateSkimmerCenter(); });
    }

    connect(m_cwSkimmer, &CwSkimmer::spotDetected, this, [this](const CwSkimmerSpot& s) {
        DxSpot spot;
        spot.dxCall = s.callsign;
        spot.freqMhz = std::round(s.freqHz / 100.0) / 1.0e4;   // 100 Hz resolution
        spot.spotterCall = AppSettings::instance().value("DxClusterCallsign", "SKIMMER").toString();
        spot.comment = QString("CW %1 WPM %2 dB%3").arg(s.wpm).arg(qRound(s.snrDb))
                           .arg(s.isCq ? " CQ" : "");
        spot.snr = qRound(s.snrDb);
        spot.source = "CWSkimmer";
        spot.color = AppSettings::instance().value("CwSkimmerSpotColor", "#FF00FF88").toString();
        m_mw.m_spotCtl->submitSpot(spot, "CWSkimmer");
    });
    updateSkimmerCenter();
    if (as.value("CwSkimmerEnabled", "False").toString() == "True") {
        m_cwSkimmer->start();
    }
}

void StationToolsController::updateSkimmerCenter()
{
    if (!m_cwSkimmer) {
        return;
    }
    const auto& st = m_mw.m_radioModel.daxIqModel().stream(m_cwSkimmer->daxIqChannel());
    if (PanadapterModel* pan = m_mw.m_radioModel.panadapter(st.panId)) {
        m_cwSkimmer->setCenterFrequencyMhz(pan->centerMhz());
    }
}

void StationToolsController::showCwSkimmerPanel()
{
    if (!m_cwSkimmer) {
        return;
    }
    if (!m_cwSkimmerPanel) {
        auto* panel = new CwSkimmerPanel(&m_mw);
        panel->setWindowFlag(Qt::Window);
        panel->setWindowTitle(QStringLiteral("CW Skimmer"));
        connect(m_cwSkimmer, &CwSkimmer::channelsUpdated, panel, &CwSkimmerPanel::setChannels);
        connect(m_cwSkimmer, &CwSkimmer::spotDetected, panel, &CwSkimmerPanel::addSpot);
        connect(m_cwSkimmer, &CwSkimmer::channelRemoved, panel, &CwSkimmerPanel::removeChannel);
        connect(m_cwSkimmer, &CwSkimmer::runningChanged, panel, &CwSkimmerPanel::setRunning);
        connect(panel, &CwSkimmerPanel::enableToggled, this, [this](bool on) {
            if (on) {
                m_cwSkimmer->start();
            } else {
                m_cwSkimmer->stop();
            }
        });
        connect(panel, &CwSkimmerPanel::daxIqChannelChanged, this, [this](int ch) {
            m_cwSkimmer->setDaxIqChannel(ch);
            updateSkimmerCenter();
        });
        connect(panel, &CwSkimmerPanel::tuneRequested, this, [this](double mhz) {
            if (SliceModel* s = m_mw.activeSlice()) {
                s->setFrequency(mhz);
            }
        });
        m_cwSkimmerPanel = panel;
    }
    m_cwSkimmerPanel->show();
    m_cwSkimmerPanel->raise();
    m_cwSkimmerPanel->activateWindow();
}

void StationToolsController::shutdownCwSkimmer()
{
    if (!m_cwSkimmer) {
        return;
    }
    if (PanadapterStream* ps = m_mw.m_radioModel.panStream()) {
        disconnect(ps, nullptr, m_cwSkimmer, nullptr);
    }
    m_cwSkimmer->stop();
}

// ── Logbook / contest / uploads / SO2R ──────────────────────────────────────

void StationToolsController::createLogbook()
{
    m_logbook = new Logbook(this);
    m_logbook->loadCtyDat(QStringLiteral(":/cty.dat"));
    if (!m_logbook->open()) {
        qCWarning(lcGui) << "Logbook: could not open the log database";
    }
    m_logbook->setStationCallsign(m_mw.m_radioModel.callsign());
    connect(&m_mw.m_radioModel, &RadioModel::infoChanged, this, [this] {
        if (!m_mw.m_radioModel.callsign().isEmpty()) {
            m_logbook->setStationCallsign(m_mw.m_radioModel.callsign());
        }
    });

    m_contest = new ContestEngine(m_logbook, this);
    m_contest->loadSettings();
    m_contest->setCwxModel(&m_mw.m_radioModel.cwxModel());
    m_contest->setDvkModel(&m_mw.m_radioModel.dvkModel());
    // Macros additionally require the per-session "Enable macro TX" arm switch.
    m_contest->setTxGuard([this] {
        return m_mw.m_radioModel.isConnected()
            && !m_mw.m_radioModel.transmitModel().txInhibit();
    });

    m_logCreds = new CredentialStore(CredentialStore::Backend::Auto, this);
    m_logUploads = new LogUploadQueue(m_logbook, m_logCreds, this);
    m_logUploads->loadSettings();
    m_logUploads->enqueueOutstanding();

    m_so2r = new So2rController(this);

    // Refresh spot colours/labels when the log or contest score changes.
    auto refresh = [this] {
        if (spotAnnotationsEnabled()) {
            m_mw.m_radioModel.spotModel().refresh();
        }
    };
    connect(m_logbook, &Logbook::qsoAdded, this, refresh);
    connect(m_logbook, &Logbook::logReset, this, refresh);
    connect(m_contest, &ContestEngine::scoreChanged, this, refresh);

    // Link QSO audio recordings to QSOs by time.
    if (m_mw.m_qsoRecorder) {
        connect(m_mw.m_qsoRecorder, &QsoRecorder::recordingStopped, this,
                [this](const QString& path, int secs) {
            const QDateTime end = QDateTime::currentDateTimeUtc();
            m_logbook->linkRecording(path, end.addSecs(-secs), end);
        });
    }

    // WSJT-X / JTDX "QSO logged" → logbook.
    if (WsjtxUdpServer* ws = m_mw.m_spotCtl->wsjtxServer()) {
        connect(ws, &WsjtxUdpServer::qsoLogged, this,
                [this](const QString& adif) { ingestAdifRecord(adif); });
    }
}

void StationToolsController::ingestAdifRecord(const QString& adif)
{
    if (m_logbook && m_logbook->ingestAdifRecord(adif) < 0) {
        qCDebug(lcGui) << "Logbook: ADIF record not added (dupe or invalid)";
    }
}

void StationToolsController::showLogbook()
{
    if (!m_logbook) {
        return;
    }
    if (!m_logbookDialog) {
        auto* dlg = new LogbookDialog(m_logbook, m_contest, m_logUploads, m_logCreds, &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setSo2r(m_so2r);
        connect(dlg, &LogbookDialog::rotatorTurnRequested, this,
                [this](const QString& call, const QString& grid, bool longPath) {
            turnRotatorToCallsign(call, grid, longPath);
        });
        m_logbookDialog = dlg;
    }
    m_logbookDialog->setActiveSlice(m_mw.activeSlice());
    m_logbookDialog->show();
    m_logbookDialog->raise();
    m_logbookDialog->activateWindow();
}

void StationToolsController::focusLogQuickEntry()
{
    showLogbook();
    if (m_logbookDialog) {
        m_logbookDialog->focusQuickEntry();
    }
}

void StationToolsController::setLogbookActiveSlice(SliceModel* slice)
{
    if (m_logbookDialog) {
        m_logbookDialog->setActiveSlice(slice);
    }
}

bool StationToolsController::spotAnnotationsEnabled() const
{
    return m_contest
        && AppSettings::instance().value("LogbookSpotAnnotations", "False").toString() == "True";
}

SpotAnnotation StationToolsController::annotateSpot(const SpotData& spot) const
{
    return m_contest ? m_contest->annotateSpot(spot) : SpotAnnotation{};
}

} // namespace AetherSDR
