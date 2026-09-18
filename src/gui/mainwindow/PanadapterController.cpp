#include "gui/mainwindow/PanadapterController.h"

#include "core/WsjtxUdpServer.h"
#include "gui/mainwindow/SpotController.h"
#include "gui/mainwindow/StationToolsController.h"
#include "core/ContestEngine.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/AudioDspController.h"
#include "gui/mainwindow/ConnectionController.h"
#include "gui/mainwindow/MemoryController.h"
#include "gui/mainwindow/SliceController.h"
#include "gui/mainwindow/SpotController.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/CommandParser.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "gui/SpectrumWidget.h"
#include "gui/SpectrumOverlayMenu.h"
#include "gui/VfoWidget.h"
#include "gui/AppletPanel.h"
#include "gui/RxApplet.h"
#include "models/SliceModel.h"
#include "models/BandPlanManager.h"
#include <algorithm>
#include <memory>
#include <functional>
#include <QTimer>
#include <QLabel>
#include <QFileDialog>
#include <QPointer>
#include "core/AppSettings.h"
#include <QPushButton>

namespace AetherSDR {

namespace {

// Replace the named per-applet receiver context. Every connection that uses
// the old context as receiver is dropped when it is destroyed, so re-running
// a wiring function never stacks duplicate connections. Destruction is
// immediate (not deleteLater) so no duplicate delivery can slip through
// before the event loop runs.
QObject* resetWireContext(QObject* owner, const QString& name)
{
    const QList<QObject*> old =
        owner->findChildren<QObject*>(name, Qt::FindDirectChildrenOnly);
    for (QObject* o : old) {
        std::unique_ptr<QObject> doomed(o);
    }
    auto* ctx = new QObject(owner);
    ctx->setObjectName(name);
    return ctx;
}

} // namespace

PanadapterController::PanadapterController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void PanadapterController::wireStreamsAndLifecycle()
{
    // ── Panadapter stream → spectrum widget ───────────────────────────────
    // Route FFT/waterfall data to the correct SpectrumWidget by stream ID
    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::spectrumReady,
            this, [this](quint32 streamId, const QVector<float>& bins) {
        for (auto* pan : m_mw.m_radioModel.panadapters()) {
            if (pan->panStreamId() == streamId) {
                if (auto* sw = m_mw.m_panStack->spectrum(pan->panId())) {
                    sw->updateSpectrum(bins);
                    m_mw.m_connectionCtl->finishPanadapterConnectionAnimation();
                }
                return;
            }
        }
        // Fallback: active spectrum (covers "default" pan before radio connects)
        if (auto* sw = m_mw.spectrum()) {
            sw->updateSpectrum(bins);
            m_mw.m_connectionCtl->finishPanadapterConnectionAnimation();
        }
    });
    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::waterfallRowReady,
            this, [this](quint32 streamId, const QVector<float>& bins,
                         double low, double high, quint32 tc) {
        for (auto* pan : m_mw.m_radioModel.panadapters()) {
            if (pan->wfStreamId() == streamId) {
                if (auto* sw = m_mw.m_panStack->spectrum(pan->panId())) {
                    sw->updateWaterfallRow(bins, low, high, tc);
                    m_mw.m_connectionCtl->finishPanadapterConnectionAnimation();
                }
                return;
            }
        }
        if (auto* sw = m_mw.spectrum()) {
            sw->updateWaterfallRow(bins, low, high, tc);
            m_mw.m_connectionCtl->finishPanadapterConnectionAnimation();
        }
    });
    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::waterfallAutoBlackLevel,
            this, [this](quint32 streamId, quint32 autoBlack) {
        for (auto* pan : m_mw.m_radioModel.panadapters()) {
            if (pan->wfStreamId() == streamId) {
                if (auto* sw = m_mw.m_panStack->spectrum(pan->panId())) {
                    if (sw->wfAutoBlack()) {
                        const int level = std::clamp(static_cast<int>(autoBlack), 0, 125);
                        sw->setWfBlackLevel(level);
                    }
                }
                return;
            }
        }
    });
    // Legacy panadapterInfoChanged — only used for initial display settings push.
    // Per-pan frequency/level tracking is done via PanadapterModel signals in panadapterAdded.
    connect(&m_mw.m_radioModel, &RadioModel::panadapterInfoChanged,
            this, [this]() {
        if (!m_mw.m_displaySettingsPushed) {
            auto* sw = m_mw.spectrum();
            if (!sw) return;  // pan not yet available
            m_mw.m_displaySettingsPushed = true;
            m_mw.m_radioModel.setPanAverage(sw->fftAverage());
            m_mw.m_radioModel.setPanFps(sw->fftFps());
            m_mw.m_radioModel.setPanWeightedAverage(sw->fftWeightedAvg());
            m_mw.m_radioModel.setWaterfallColorGain(sw->wfColorGain());
            m_mw.m_radioModel.setWaterfallBlackLevel(sw->wfBlackLevel());
            m_mw.m_radioModel.setWaterfallAutoBlack(sw->wfAutoBlack());
            int rate = sw->wfLineDuration();
            m_mw.m_radioModel.setWaterfallLineDuration(rate);
            // WNB and RF gain are radio-authoritative (the radio restores them
            // per pan). Show the radio's values; never push a client-saved copy,
            // which overrode the radio's session restore on every connect.
            auto& s = AppSettings::instance();
            if (auto* pan = m_mw.m_radioModel.panadapter(m_mw.m_radioModel.panId())) {
                sw->setWnbActive(pan->wnbActive());
                sw->setRfGain(pan->rfGain());
                sw->overlayMenu()->setWnbState(pan->wnbActive(), pan->wnbLevel());
                sw->overlayMenu()->setRfGain(pan->rfGain());
            }
            QString bgPath = s.value(sw->settingsKey("BackgroundImage")).toString();
            if (!bgPath.isEmpty())
                sw->setBackgroundImage(bgPath);
            int bgOpacity = s.value(sw->settingsKey("BackgroundOpacity"), "80").toInt();
            sw->setBackgroundOpacity(bgOpacity);
            // Nudge rate to force waterfall tile re-sync
            QTimer::singleShot(500, this, [this, rate]() {
                m_mw.m_radioModel.setWaterfallLineDuration(rate + 1);
                m_mw.m_radioModel.setWaterfallLineDuration(rate);
            });
        }
    });
    // NOTE: panadapterLevelChanged → spectrum()::setDbmRange has been removed.
    // Level updates are routed per-pan via PanadapterModel::levelChanged in
    // wirePanadapter() so that PanadapterModel's change-guard prevents stale
    // echo-backs from overwriting in-flight user changes.
    // ── Multi-panadapter lifecycle ──────────────────────────────────────────
    connect(&m_mw.m_radioModel, &RadioModel::panadapterAdded,
            this, [this](PanadapterModel* pan) {
        // During layout application, applyLayout/createPansSequentially handles
        // applet creation and wiring — don't duplicate here.
        if (m_mw.m_applyingLayout) return;

        // Skip if this pan already has an applet (e.g. reconnect: the old
        // PanadapterModel was deleted on disconnect but the applet survived).
        // Re-attach the full set of per-pan model connections to the new model.
        if (auto* existing = m_mw.m_panStack->panadapter(pan->panId())) {
            wirePanadapterModel(existing, pan);
            return;
        }

        PanadapterApplet* applet = nullptr;

        // If applyLayout already created this applet, just wire signals
        if (m_mw.m_panStack->panadapter(pan->panId())) {
            applet = m_mw.m_panStack->panadapter(pan->panId());
        }
        // Reuse the "default" placeholder for the first real pan
        else if (m_mw.m_panStack->panadapter("default")) {
            applet = m_mw.m_panStack->panadapter("default");
            applet->setPanId(pan->panId());
            m_mw.m_panStack->rekey("default", pan->panId());
        } else {
            applet = m_mw.m_panStack->addPanadapter(pan->panId());
        }
        setActivePanApplet(applet);
        wirePanadapter(applet);
        if (m_mw.m_panadapterConnectionAnimationVisible) {
            applet->spectrumWidget()->setConnectionAnimationVisible(
                true, m_mw.m_panadapterConnectionAnimationLabel);
        }
        // NOTE: all PanadapterModel → widget connections (info, level, wide,
        // WNB, RF gain, DAX IQ) are made by wirePanadapterModel(), called from
        // wirePanadapter() above. Don't connect them here again.

        // Push display dimensions to the radio so it sends full-size FFT bins.
        // Without this, the radio uses xpixels=50 ypixels=20 (default) and
        // FFT data is essentially empty/unusable. Use actual widget dimensions
        // for 1:1 bin-to-pixel mapping (matches SmartSDR behavior from pcap).
        auto* sw = applet->spectrumWidget();
        int xpix = sw ? sw->width() : 1024;
        int ypix = sw ? sw->height() : 700;
        if (xpix < 100) xpix = 1024;  // widget may not be laid out yet
        if (ypix < 100) ypix = 700;
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 xpixels=%2 ypixels=%3")
                .arg(pan->panId()).arg(xpix).arg(ypix));

        // Tell PanadapterStream the ypixels for FFT bin→dBm conversion
        if (pan->panStreamId())
            m_mw.m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);

        qCDebug(lcGui) << "MainWindow: added panadapter applet for" << pan->panId();

        // Debounced layout restore: after all pans are added on connect,
        // rearrange to the saved layout (e.g. 2h instead of default vertical).
        if (!m_layoutRestoreTimer) {
            m_layoutRestoreTimer = new QTimer(&m_mw);
            m_layoutRestoreTimer->setSingleShot(true);
            m_layoutRestoreTimer->setInterval(1000);
            connect(m_layoutRestoreTimer, &QTimer::timeout, this, [this]() {
                // The radio restores pans from the GUIClientID session.
                // Accept whatever the radio gives and arrange based on count.
                int panCount = m_mw.m_panStack->count();
                if (panCount <= 1) return;  // single pan, nothing to arrange
                // Pick a layout based on the number of pans the radio restored
                const QString saved = AppSettings::instance()
                    .value("PanadapterLayout", "1").toString();
                // Only rearrange if the saved layout matches the pan count
                static const QMap<QString, int> layoutPanCount = {
                    {"1", 1}, {"2v", 2}, {"2h", 2}, {"2h1", 3}, {"12h", 3}, {"3v", 3}, {"2x2", 4}, {"4v", 4}
                };
                if (layoutPanCount.value(saved, 1) == panCount)
                    m_mw.m_panStack->rearrangeLayout(saved);
                else if (panCount == 2)
                    m_mw.m_panStack->rearrangeLayout("2v");  // default 2-pan to vertical
                else if (panCount == 3)
                    m_mw.m_panStack->rearrangeLayout("2h1"); // default 3-pan
                else if (panCount >= 4)
                    m_mw.m_panStack->rearrangeLayout("2x2"); // default 4-pan

                // Optimistically set local yPixels immediately so FFT frames
                // arriving before the radio echoes back use correct scaling (#1511).
                for (auto* a : m_mw.m_panStack->allApplets()) {
                    auto* s = a->spectrumWidget();
                    auto* p = m_mw.m_radioModel.panadapter(a->panId());
                    if (!s || !p || !p->panStreamId()) continue;
                    int y = s->height();
                    if (y >= 100)
                        m_mw.m_radioModel.panStream()->setYPixels(p->panStreamId(), y);
                }

                // Defensive re-push xpixels for all pans after layout settles.
                // Covers race where radio hadn't finished pan init when first push arrived.
                QTimer::singleShot(500, this, [this]() {
                    for (auto* applet : m_mw.m_panStack->allApplets()) {
                        auto* sw = applet->spectrumWidget();
                        auto* pan = m_mw.m_radioModel.panadapter(applet->panId());
                        if (!sw || !pan) continue;
                        int xpix = qMax(sw->width(), 1024);
                        int ypix = qMax(sw->height(), 200);
                        m_mw.m_radioModel.sendCommand(
                            QString("display pan set %1 xpixels=%2 ypixels=%3")
                                .arg(pan->panId()).arg(xpix).arg(ypix));
                        if (pan->panStreamId())
                            m_mw.m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);
                    }
                });
            });
        }
        m_layoutRestoreTimer->start();  // restart on each new pan
    });
    // Re-push xpixels/ypixels when the radio requests it (profile change, reconnect, etc.)
    connect(&m_mw.m_radioModel, &RadioModel::panDimensionsNeeded,
            this, [this](const QString& panId) {
        auto* applet = m_mw.m_panStack->panadapter(panId);
        if (!applet) return;
        auto* sw = applet->spectrumWidget();
        auto* pan = m_mw.m_radioModel.panadapter(panId);
        if (!sw || !pan) return;
        int xpix = sw->width();
        int ypix = sw->height();
        if (xpix < 100) xpix = 1024;
        if (ypix < 100) ypix = 700;
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 xpixels=%2 ypixels=%3")
                .arg(panId).arg(xpix).arg(ypix));
        if (pan->panStreamId())
            m_mw.m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);
    });

    connect(&m_mw.m_radioModel, &RadioModel::panadapterRemoved,
            this, [this](const QString& panId) {
        // Disconnect all signals from the dying applet's widgets to prevent
        // dangling pointer crashes in wirePanadapter lambdas (#242)
        if (auto* applet = m_mw.m_panStack->panadapter(panId)) {
            if (auto* sw = applet->spectrumWidget()) {
                m_mw.disconnectFromWindow(sw);
                if (auto* menu = sw->overlayMenu()) {
                    m_mw.disconnectFromWindow(menu);
                    menu->setPanadapterModel(nullptr);
                }
            }
        }
        m_mw.m_panStack->removePanadapter(panId);
        qCDebug(lcGui) << "MainWindow: removed panadapter applet for" << panId;

        // Rearrange remaining pans to a sensible layout
        int remaining = m_mw.m_panStack->count();
        if (remaining == 1)
            AppSettings::instance().setValue("PanadapterLayout", "1");
        else if (remaining == 2)
            m_mw.m_panStack->rearrangeLayout("2v");
        else if (remaining == 3)
            m_mw.m_panStack->rearrangeLayout("2h1");
    });
}

void PanadapterController::wireActivePanTracking()
{
    // Sync RadioModel's active pan/wf IDs when PanadapterStack focus changes.
    // This ensures display setting commands (fps, average, black_level, etc.)
    // target the correct pan.
    // Sync RadioModel's active pan ID when PanadapterStack focus changes.
    // This ensures display setting commands (fps, average, black_level, etc.)
    // target the correct pan — activeWfId() derives from activePanadapter()
    // which uses m_activePanId.
    connect(m_mw.m_panStack, &PanadapterStack::activePanChanged,
            this, [this](const QString& panId) {
        m_mw.m_radioModel.setActivePanId(panId);

        // Update m_panApplet for the new active pan
        if (auto* applet = m_mw.m_panStack->panadapter(panId))
            setActivePanApplet(applet);

        // Show/hide CW decode panel based on the new active pan's slice mode
        for (auto* sl : m_mw.m_radioModel.slices()) {
            if (sl->panId() == panId) {
                bool isCw = (sl->mode() == "CW" || sl->mode() == "CWL");
                bool decodeOn = AppSettings::instance().value("CwDecodeOverlay", "True").toString() == "True";
                if (auto* applet = m_mw.m_panStack->panadapter(panId))
                    applet->setCwPanelVisible(isCw && decodeOn);
                if (isCw && !m_mw.m_cwDecoder.isRunning())
                    m_mw.m_cwDecoder.start();
                else if (!isCw && m_mw.m_cwDecoder.isRunning())
                    m_mw.m_cwDecoder.stop();
                break;
            }
        }
    });
}

// All PanadapterModel → widget connections for one applet. Called from
// wirePanadapter() and from panadapterAdded when an existing applet is reused
// for a new model (reconnect). Connections use a per-applet context object so
// calling this repeatedly never duplicates them.
void PanadapterController::wirePanadapterModel(PanadapterApplet* applet, PanadapterModel* pan)
{
    if (!applet || !pan) {
        return;
    }
    auto* sw = applet->spectrumWidget();
    if (!sw) {
        return;
    }
    auto* menu = sw->overlayMenu();
    QObject* ctx = resetWireContext(applet, QStringLiteral("MainWindowPanModelCtx"));
    QPointer<SpectrumWidget> swGuard(sw);
    QPointer<SpectrumOverlayMenu> menuGuard(menu);

    connect(pan, &PanadapterModel::infoChanged, ctx,
            [swGuard](double centerMhz, double bandwidthMhz) {
        if (swGuard) {
            swGuard->setFrequencyRange(centerMhz, bandwidthMhz);
        }
    });

    // Route confirmed level changes (min_dbm / max_dbm) from the radio to
    // this pan's spectrum widget. Using the per-pan PanadapterModel signal
    // (guarded by change detection) rather than the RadioModel-level
    // panadapterLevelChanged signal ensures that:
    //   a) stale echo-backs (same value) don't overwrite the user's in-flight
    //      local change while waiting for the radio to confirm the command, and
    //   b) in multi-pan setups, a level update on pan B doesn't incorrectly
    //      update pan A's dBm scale.
    connect(pan, &PanadapterModel::levelChanged, ctx,
            [swGuard](float minDbm, float maxDbm) {
        if (swGuard) {
            swGuard->setDbmRange(minDbm, maxDbm);
        }
    });

    connect(pan, &PanadapterModel::wideChanged, ctx, [swGuard](bool active) {
        if (swGuard) {
            swGuard->setWideActive(active);
        }
    });
    sw->setWideActive(pan->wideActive());

    // Reflect radio-reported WNB state (display pan wnb=/wnb_level=) in the
    // overlay menu. Previously only local clicks updated it, so changes from
    // another client, a profile load or the radio's session restore never showed.
    connect(pan, &PanadapterModel::wnbChanged, ctx,
            [swGuard, menuGuard](bool active, int level) {
        if (swGuard) {
            swGuard->setWnbActive(active);
        }
        if (menuGuard) {
            menuGuard->setWnbState(active, level);
        }
    });

    // RF gain range (display pan rfgain_info reply) and value
    connect(pan, &PanadapterModel::rfGainInfoChanged, ctx,
            [menuGuard](int low, int high, int step) {
        if (menuGuard) {
            menuGuard->setRfGainRange(low, high, step);
        }
    });
    connect(pan, &PanadapterModel::rfGainChanged, ctx,
            [swGuard, menuGuard](int gain) {
        if (swGuard) {
            swGuard->setRfGain(gain);
        }
        if (menuGuard) {
            menuGuard->setRfGain(gain);
        }
    });

    // Sync DAX IQ combo from radio status (#1221). The radio persists
    // daxiq_channel on the pan, so it arrives via status echo.
    connect(pan, &PanadapterModel::daxiqChannelChanged, ctx,
            [menuGuard](int channel) {
        if (menuGuard) {
            menuGuard->syncDaxIqChannel(channel);
        }
    });
    if (menu) {
        menu->syncDaxIqChannel(pan->daxiqChannel());
    }

    // PanadapterModel protocol commands (loopa/loopb, n1mm_*) → radio
    connect(pan, &PanadapterModel::commandReady, ctx, [this](const QString& cmd) {
        m_mw.m_radioModel.sendCommand(cmd);
    });
    if (menu) {
        // Loop A/B buttons + N1MM+ dialog bind to this pan's model; the
        // button state follows the radio's loopa/loopb echo.
        menu->setPanadapterModel(pan);
        const auto updateLoopSupport = [this, menuGuard]() {
            if (!menuGuard) {
                return;
            }
            const QString model = m_mw.m_radioModel.model();
            menuGuard->setRxLoopsSupported(model.contains("6600") || model.contains("6700")
                                           || model.contains("8600"));
        };
        updateLoopSupport();
        // The radio model may not be known yet when the pan is first wired.
        connect(&m_mw.m_radioModel, &RadioModel::infoChanged, ctx, updateLoopSupport);
    }
}

// ── Per-panadapter signal wiring ──────────────────────────────────────────────
// Called once per PanadapterApplet. Connects the SpectrumWidget and its
// OverlayMenu signals to RadioModel, TnfModel, and MainWindow handlers.
// In multi-pan mode (Phase 6+), called for each new panadapter.

void PanadapterController::setActivePanApplet(PanadapterApplet* applet)
{
    if (applet == m_mw.m_panApplet) return;
    m_mw.m_panApplet = applet;

    // Re-route CW decoder output: the active slice may now belong to this pan
    routeCwDecoderOutput();
}

// Route CW decoder text/stats output to the pan that owns the active slice,
// so decoded text appears in the correct pan's CW widget (#864).
void PanadapterController::routeCwDecoderOutput()
{
    // Determine which applet should receive CW decoder output:
    // the pan that owns the active audio slice (whose audio feeds the decoder).
    PanadapterApplet* target = nullptr;
    if (auto* s = m_mw.activeSlice(); s && m_mw.m_panStack && !s->panId().isEmpty())
        target = m_mw.m_panStack->panadapter(s->panId());
    if (!target)
        target = m_mw.m_panApplet;  // fallback to active pan

    if (target == m_mw.m_cwDecoderApplet) return;

    // Disconnect from old applet
    if (m_mw.m_cwDecoderApplet) {
        disconnect(&m_mw.m_cwDecoder, &CwDecoder::textDecoded,
                   m_mw.m_cwDecoderApplet, &PanadapterApplet::appendCwText);
        disconnect(&m_mw.m_cwDecoder, &CwDecoder::statsUpdated,
                   m_mw.m_cwDecoderApplet, &PanadapterApplet::setCwStats);
        if (auto* pb = m_mw.m_cwDecoderApplet->lockPitchButton())
            disconnect(pb, &QPushButton::toggled,
                       &m_mw.m_cwDecoder, &CwDecoder::lockPitch);
        if (auto* sb = m_mw.m_cwDecoderApplet->lockSpeedButton())
            disconnect(sb, &QPushButton::toggled,
                       &m_mw.m_cwDecoder, &CwDecoder::lockSpeed);
        disconnect(m_mw.m_cwDecoderApplet, &PanadapterApplet::pitchRangeChanged,
                   &m_mw.m_cwDecoder, &CwDecoder::setPitchRange);
        disconnect(m_mw.m_cwDecoderApplet, &PanadapterApplet::cwPanelCloseRequested,
                   &m_mw.m_cwDecoder, &CwDecoder::stop);
    }

    m_mw.m_cwDecoderApplet = target;

    // Connect to new applet
    if (m_mw.m_cwDecoderApplet) {
        connect(&m_mw.m_cwDecoder, &CwDecoder::textDecoded,
                m_mw.m_cwDecoderApplet, &PanadapterApplet::appendCwText);
        connect(&m_mw.m_cwDecoder, &CwDecoder::statsUpdated,
                m_mw.m_cwDecoderApplet, &PanadapterApplet::setCwStats);
        connect(m_mw.m_cwDecoderApplet->lockPitchButton(), &QPushButton::toggled,
                &m_mw.m_cwDecoder, &CwDecoder::lockPitch);
        connect(m_mw.m_cwDecoderApplet->lockSpeedButton(), &QPushButton::toggled,
                &m_mw.m_cwDecoder, &CwDecoder::lockSpeed);
        connect(m_mw.m_cwDecoderApplet, &PanadapterApplet::pitchRangeChanged,
                &m_mw.m_cwDecoder, &CwDecoder::setPitchRange);
        connect(m_mw.m_cwDecoderApplet, &PanadapterApplet::cwPanelCloseRequested,
                &m_mw.m_cwDecoder, &CwDecoder::stop);
    }
}

void PanadapterController::wirePanadapter(PanadapterApplet* applet)
{
    auto* sw = applet->spectrumWidget();
    auto* menu = sw->overlayMenu();

    // Guard: wirePanadapter() is called once at startup (for m_panApplet) and
    // again from panadapterAdded for the same widget.  Without these disconnects
    // every sw/menu/applet → this signal would be connected twice, causing each
    // user gesture to fire two identical radio commands (e.g. duplicate
    // "display pan set … min_dbm= max_dbm=").  Clearing prior connections here
    // replaces the scattered per-signal disconnect guards below.
    m_mw.disconnectFromWindow(sw);
    m_mw.disconnectFromWindow(menu);
    m_mw.disconnectFromWindow(applet);

    // Connections whose sender is NOT sw/menu/applet (TnfModel, SpotModel,
    // RadioModel, the resize timer) aren't cleared by the disconnects above.
    // Route those through a per-applet context object that is replaced on
    // every call, so re-wiring drops the previous set. Plain member-function
    // connections to other receivers use Qt::UniqueConnection instead.
    QObject* ctx = resetWireContext(applet, QStringLiteral("MainWindowPanWireCtx"));

    // Wire band plan manager to this spectrum widget
    sw->setBandPlanManager(m_mw.m_bandPlanMgr);

    // Set panadapter bandwidth zoom limits based on radio model
    sw->setBandwidthLimits(m_mw.m_radioModel.minPanBandwidthMhz(),
                           m_mw.m_radioModel.maxPanBandwidthMhz());

    // Set panId on the overlay menu so +RX routes to the correct pan
    menu->setPanId(applet->panId());
    menu->setMemories(m_mw.m_radioModel.memories());

    // Antenna list → this overlay menu (per-pan, mirrors VfoWidget pattern) (#1260)
    connect(&m_mw.m_radioModel, &RadioModel::antListChanged,
            menu, &SpectrumOverlayMenu::setAntennaList, Qt::UniqueConnection);
    menu->setAntennaList(m_mw.m_radioModel.antennaList());

    // Apply current prop forecast state to this (possibly new) panadapter
    if (m_mw.m_propForecast) {
        sw->setPropForecastVisible(m_mw.m_propForecast->isEnabled());
        if (m_mw.m_propForecast->isEnabled()) {
            PropForecast fc = m_mw.m_propForecast->lastForecast();
            sw->setPropForecast(fc.kIndex, fc.aIndex, fc.sfi);
        }
    }

    // Click on K/A/SFI overlay → open prop dashboard
    connect(sw, &SpectrumWidget::propForecastClicked,
            &m_mw, &MainWindow::showPropDashboard);

    // PanadapterModel → widget (info, level, wide, WNB, RF gain, DAX IQ)
    if (auto* pan = m_mw.m_radioModel.panadapter(applet->panId())) {
        wirePanadapterModel(applet, pan);
    }

    // ── Debounced resize → re-push xpixels/ypixels to the radio (#1511) ───
    // When the layout changes (e.g. adding a second pan, splitting, resizing),
    // the widget dimensions change but the radio keeps encoding FFT bins to the
    // old yPixels scale.  This causes a ~5 dB noise-floor offset until restart.
    // A 300ms debounce avoids flooding the radio during animated resizes.
    {
        // Parented to the wire context: re-wiring destroys the old timer and
        // its dimensionsChanged hookup, so only one timer exists per widget.
        auto* resizeTimer = new QTimer(ctx);
        resizeTimer->setSingleShot(true);
        resizeTimer->setInterval(300);
        connect(sw, &SpectrumWidget::dimensionsChanged,
                resizeTimer, [resizeTimer](int, int) { resizeTimer->start(); });
        connect(resizeTimer, &QTimer::timeout,
                ctx, [this, applet, sw]() {
            auto* pan = m_mw.m_radioModel.panadapter(applet->panId());
            if (!pan || !sw) return;
            int xpix = sw->width();
            int ypix = sw->height();
            if (xpix < 100 || ypix < 100) return;
            m_mw.m_radioModel.sendCommand(
                QString("display pan set %1 xpixels=%2 ypixels=%3")
                    .arg(pan->panId()).arg(xpix).arg(ypix));
            if (pan->panStreamId())
                m_mw.m_radioModel.panStream()->setYPixels(pan->panStreamId(), ypix);
        });
    }

    // ── Tuning step size → this pan's spectrum widget ─────────────────────
    // The global connection only covers AppSettings + radio command.
    // Each pan must also receive stepSizeChanged so scroll-to-tune
    // uses the correct step regardless of which pan is active.
    connect(m_mw.m_appletPanel->rxApplet(), &RxApplet::stepSizeChanged,
            sw, &SpectrumWidget::setStepSize, Qt::UniqueConnection);
    connect(m_mw.m_appletPanel->rxApplet(), &RxApplet::stepSizeSyncedFromRadio,
            sw, &SpectrumWidget::setStepSize, Qt::UniqueConnection);

    // ── Pan activation: clicking on this pan makes it active ─────────────
    connect(applet, &PanadapterApplet::activated,
            m_mw.m_panStack, &PanadapterStack::setActivePan, Qt::UniqueConnection);

    // ── Close pan: X button on title bar closes this pan ────────────────
    connect(applet, &PanadapterApplet::closeRequested,
            this, [this](const QString& panId) {
        // Don't close the last pan
        if (m_mw.m_panStack->count() <= 1) return;
        m_mw.m_radioModel.sendCommand(QString("display pan remove %1").arg(panId));
    });

    // ── User drag actions from spectrum → radio (per-pan) ──────────────────
    connect(sw, &SpectrumWidget::bandwidthChangeRequested,
            this, [this, applet](double bw) {
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 bandwidth=%2").arg(applet->panId()).arg(bw, 0, 'f', 6));
    });
    connect(sw, &SpectrumWidget::centerChangeRequested,
            this, [this, applet, sw](double center) {
        const double minCenter = sw ? sw->bandwidthMhz() / 2.0 : 0.0;
        center = std::max(center, minCenter);
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 center=%2").arg(applet->panId()).arg(center, 0, 'f', 6));
    });
    connect(sw, &SpectrumWidget::bandZoomRequested,
            this, [this, applet, bandZoomOn = std::make_shared<bool>(false)]() mutable {
        *bandZoomOn = !*bandZoomOn;
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 band_zoom=%2")
                .arg(applet->panId()).arg(*bandZoomOn ? 1 : 0));
    });
    connect(sw, &SpectrumWidget::segmentZoomRequested,
            this, [this, applet, segZoomOn = std::make_shared<bool>(false)]() mutable {
        *segZoomOn = !*segZoomOn;
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 segment_zoom=%2")
                .arg(applet->panId()).arg(*segZoomOn ? 1 : 0));
    });
    connect(sw, &SpectrumWidget::filterChangeRequested,
            this, [this](int lo, int hi) {
        if (auto* s = m_mw.activeSlice()) s->setFilterWidth(lo, hi);
    });
    connect(sw, &SpectrumWidget::dbmRangeChangeRequested,
            this, [this, applet](float minDbm, float maxDbm) {
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 min_dbm=%2 max_dbm=%3")
                .arg(applet->panId())
                .arg(static_cast<double>(minDbm), 0, 'f', 2)
                .arg(static_cast<double>(maxDbm), 0, 'f', 2));
    });

    // ── TNF signals ──────────────────────────────────────────────────────
    // QPointer guards against dangling sw when a pan is removed (#381)
    QPointer<SpectrumWidget> swGuard(sw);
    auto* tnf = &m_mw.m_radioModel.tnfModel();
    auto rebuildTnfMarkers = [this, swGuard]() {
        if (!swGuard) return;
        auto* t = &m_mw.m_radioModel.tnfModel();
        QVector<SpectrumWidget::TnfMarker> markers;
        for (const auto& e : t->tnfs())
            markers.append({e.id, e.freqMhz, e.widthHz, e.depthDb, e.permanent});
        swGuard->setTnfMarkers(markers);
    };
    connect(tnf, &TnfModel::tnfChanged,  ctx, rebuildTnfMarkers);
    connect(tnf, &TnfModel::tnfRemoved,  ctx, rebuildTnfMarkers);
    connect(tnf, &TnfModel::tnfsCleared, ctx, rebuildTnfMarkers);
    connect(tnf, &TnfModel::globalEnabledChanged,
            sw, &SpectrumWidget::setTnfGlobalEnabled, Qt::UniqueConnection);
    connect(tnf, &TnfModel::globalEnabledChanged,
            ctx, [this](bool on) {
        m_mw.m_tnfIndicator->setStyleSheet(on
            ? "QLabel { color: #00e060; font-weight: bold; font-size: 24px; }"
            : "QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
    });

    // FDX indicator style update
    connect(&m_mw.m_radioModel, &RadioModel::infoChanged, ctx, [this]() {
        bool fdx = m_mw.m_radioModel.fullDuplexEnabled();
        m_mw.m_fdxIndicator->setStyleSheet(fdx
            ? "QLabel { color: #00e060; font-weight: bold; font-size: 24px; }"
            : "QLabel { color: #404858; font-weight: bold; font-size: 24px; }");
    });
    connect(sw, &SpectrumWidget::tnfCreateRequested,   tnf, &TnfModel::createTnf, Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfMoveRequested,     tnf, &TnfModel::setTnfFreq, Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfRemoveRequested,   tnf, &TnfModel::requestRemoveTnf, Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfWidthRequested,    tnf, &TnfModel::setTnfWidth, Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfDepthRequested,    tnf, &TnfModel::setTnfDepth, Qt::UniqueConnection);
    connect(sw, &SpectrumWidget::tnfPermanentRequested,tnf, &TnfModel::setTnfPermanent, Qt::UniqueConnection);

    // ── Spot markers ─────────────────────────────────────────────────────
    auto* spots = &m_mw.m_radioModel.spotModel();
    auto rebuildSpots = [this, swGuard]() {
        if (!swGuard) return;  // widget destroyed (layout change)
        auto* s = &m_mw.m_radioModel.spotModel();
        const bool showMemories =
            AppSettings::instance().value("IsMemorySpotsEnabled", "False").toString() == "True";
        QVector<SpectrumWidget::SpotMarker> markers;
        for (const auto& spot : s->spots()) {
            if (spot.source == "Memory" && !showMemories)
                continue;
            qint64 tsMs = 0;
            if (spot.source != "Memory") {
                tsMs = (spot.timestamp.isValid() && spot.timestamp.toMSecsSinceEpoch() > 0)
                    ? spot.timestamp.toMSecsSinceEpoch()
                    : spot.addedMs;
            }
            QColor dxccCol;
            QString comment = spot.comment;
            if (spot.source != "Memory") {
                if (m_mw.m_toolsCtl->spotAnnotationsEnabled()) {
                    // Logbook/contest band-map: dupe / new mult / worked.
                    const SpotAnnotation ann = m_mw.m_toolsCtl->annotateSpot(spot);
                    if (ann.color.isValid()) {
                        dxccCol = ann.color;
                    }
                    if (!ann.label.isEmpty()) {
                        comment = ann.label + (comment.isEmpty() ? QString() : " · " + comment);
                    }
                } else if (m_mw.m_dxccProvider.isEnabled()) {
                    dxccCol = m_mw.m_dxccProvider.colorForSpot(spot.callsign, spot.rxFreqMhz, spot.mode);
                }
            }
            markers.append({spot.index, spot.callsign, spot.rxFreqMhz, spot.color, spot.mode,
                            dxccCol, spot.source, spot.spotterCallsign, comment, tsMs});
        }
        swGuard->setSpotMarkers(markers);
    };
    connect(spots, &SpotModel::spotAdded,   ctx, rebuildSpots);
    connect(spots, &SpotModel::spotUpdated, ctx, rebuildSpots);
    connect(spots, &SpotModel::spotRemoved, ctx, rebuildSpots);
    connect(spots, &SpotModel::spotsCleared,ctx, rebuildSpots);
    connect(spots, &SpotModel::spotsRefreshed, ctx, rebuildSpots);
    {
        auto& s = AppSettings::instance();
        sw->setShowSpots(s.value("IsSpotsEnabled", "True").toString() == "True");
        sw->setSpotFontSize(s.value("SpotFontSize", "16").toInt());
        sw->setSpotMaxLevels(s.value("SpotsMaxLevel", "3").toInt());
        sw->setSpotStartPct(s.value("SpotsStartingHeightPercentage", "50").toInt());
        sw->setSpotOverrideColors(s.value("IsSpotsOverrideColorsEnabled", "False").toString() == "True");
        sw->setSpotOverrideBg(s.value("IsSpotsOverrideBackgroundColorsEnabled", "True").toString() == "True");
        sw->setSpotColor(QColor(s.value("SpotsOverrideColor", "#FFFF00").toString()));
        sw->setSpotBgColor(QColor(s.value("SpotsOverrideBgColor", "#000000").toString()));
        sw->setSpotBgOpacity(s.value("SpotsBackgroundOpacity", 48).toInt());
    }

    // ── Per-pan display controls (client-side) ───────────────────────────
    connect(menu, &SpectrumOverlayMenu::fftFillAlphaChanged,
            sw, &SpectrumWidget::setFftFillAlpha, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::fftFillColorChanged,
            sw, &SpectrumWidget::setFftFillColor, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::fftHeatMapChanged,
            sw, &SpectrumWidget::setFftHeatMap, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::showGridChanged,
            sw, &SpectrumWidget::setShowGrid, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::cursorFreqLabelChanged,
            sw, &SpectrumWidget::setShowCursorFreq, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::freqGridSpacingChanged,
            sw, &SpectrumWidget::setFreqGridSpacing, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::fftLineWidthChanged,
            sw, &SpectrumWidget::setFftLineWidth, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::noiseFloorPositionChanged,
            sw, &SpectrumWidget::setNoiseFloorPosition, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::noiseFloorEnableChanged,
            sw, &SpectrumWidget::setNoiseFloorEnable, Qt::UniqueConnection);

    // Pop out / dock panadapter
    connect(sw, &SpectrumWidget::popOutRequested, this, [this, applet](bool popOut) {
        if (popOut) {
            m_mw.m_panStack->floatPanadapter(applet->panId());
        } else {
            m_mw.m_panStack->dockPanadapter(applet->panId());
        }
    });
    connect(applet, &PanadapterApplet::popOutClicked, this, [this, applet]() {
        m_mw.m_panStack->floatPanadapter(applet->panId());
    });
    // UniqueConnection: wirePanadapter() can run twice for the first pan, and a
    // doubled toggle would maximize then immediately restore.
    connect(applet, &PanadapterApplet::maximizeRequested,
            m_mw.m_panStack, &PanadapterStack::toggleMaximize, Qt::UniqueConnection);

    // ── DAX IQ pan routing from overlay menu ───────────────────────────
    // The overlay controls which pan feeds which IQ channel (routing only).
    // Stream create/destroy and rate are managed by the DIGI applet.
    connect(menu, &SpectrumOverlayMenu::daxIqChannelChanged,
            this, [this, applet](int channel) {
        auto* pan = m_mw.m_radioModel.panadapter(applet->panId());
        if (!pan) return;
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 daxiq_channel=%2")
                .arg(applet->panId()).arg(channel));
    });

    // DAX IQ combo ← radio status (#1221) is wired in wirePanadapterModel().

    // ── Per-pan display controls → radio commands ────────────────────────
    // Each pan's overlay sends commands with its own panId/wfId, not the
    // global active pan. This ensures display settings work independently.
    connect(menu, &SpectrumOverlayMenu::fftAverageChanged,
            this, [this, applet, sw](int v) {
        sw->setFftAverage(v);
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 average=%2").arg(applet->panId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::fftFpsChanged,
            this, [this, applet, sw](int v) {
        sw->setFftFps(v);
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 fps=%2").arg(applet->panId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::fftWeightedAverageChanged,
            this, [this, applet, sw](bool on) {
        sw->setFftWeightedAvg(on);
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 weighted_average=%2").arg(applet->panId()).arg(on ? 1 : 0));
    });
    connect(menu, &SpectrumOverlayMenu::wfColorSchemeChanged,
            sw, &SpectrumWidget::setWfColorScheme, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::wfColorGainChanged,
            this, [this, applet, sw](int v) {
        sw->setWfColorGain(v);
        auto* pan = m_mw.m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_mw.m_radioModel.sendCommand(
                QString("display panafall set %1 color_gain=%2").arg(pan->waterfallId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::wfBlackLevelChanged,
            this, [this, applet, sw](int v) {
        sw->setWfBlackLevel(v);
        auto* pan = m_mw.m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_mw.m_radioModel.sendCommand(
                QString("display panafall set %1 black_level=%2").arg(pan->waterfallId()).arg(v));
    });
    connect(menu, &SpectrumOverlayMenu::wfAutoBlackChanged,
            this, [this, sw](bool on) {
        sw->setWfAutoBlack(on);
    });
    connect(menu, &SpectrumOverlayMenu::wfLineDurationChanged,
            this, [this, applet, sw](int ms) {
        sw->setWfLineDuration(ms);
        auto* pan = m_mw.m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty())
            m_mw.m_radioModel.sendCommand(
                QString("display panafall set %1 line_duration=%2").arg(pan->waterfallId()).arg(ms));
    });
    // NB Waterfall Blanker (#277)
    connect(menu, &SpectrumOverlayMenu::wfBlankerEnabledChanged,
            sw, &SpectrumWidget::setWfBlankerEnabled, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::wfBlankerThresholdChanged,
            sw, &SpectrumWidget::setWfBlankerThreshold, Qt::UniqueConnection);
    connect(menu, &SpectrumOverlayMenu::backgroundImageRequested,
            this, [this, sw] {
        QString path = QFileDialog::getOpenFileName(sw, "Choose Background Image",
            QString(), "Images (*.png *.jpg *.jpeg *.bmp)");
        if (path.isEmpty()) return;
        sw->setBackgroundImage(path);
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("BackgroundImage"), path);
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::backgroundImageCleared,
            this, [sw] {
        sw->setBackgroundImage(":/bg-default.jpg");
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("BackgroundImage"), ":/bg-default.jpg");
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::backgroundOpacityChanged,
            this, [sw](int pct) {
        sw->setBackgroundOpacity(pct);
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("BackgroundOpacity"), QString::number(pct));
        s.save();
    });
    connect(menu, &SpectrumOverlayMenu::displaySettingsReset,
            this, [this, applet, sw, menu] {
        // Apply all SpectrumWidget defaults
        sw->setFftAverage(0);
        sw->setFftFps(25);
        sw->setFftFillAlpha(0.70f);
        sw->setFftFillColor(QColor(0x00, 0xe5, 0xff));
        sw->setFftWeightedAvg(false);
        sw->setFftHeatMap(true);
        sw->setWfColorScheme(0);
        sw->setWfColorGain(50);
        sw->setWfBlackLevel(15);
        sw->setWfAutoBlack(true);
        sw->setWfLineDuration(100);
        sw->setWfBlankerEnabled(false);
        sw->setWfBlankerThreshold(1.15f);
        sw->setWfBlankerMode(0);
        sw->setShowCursorFreq(false);
        sw->setBackgroundImage(":/bg-default.jpg");
        sw->setBackgroundOpacity(80);
        sw->setNoiseFloorEnable(false);
        sw->setNoiseFloorPosition(75);
        sw->setFreqGridSpacing(0);  // Auto (#1390)

        // Radio commands for radio-authoritative display settings
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 average=0").arg(applet->panId()));
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 fps=25").arg(applet->panId()));
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 weighted_average=0").arg(applet->panId()));
        auto* pan = m_mw.m_radioModel.panadapter(applet->panId());
        if (pan && !pan->waterfallId().isEmpty()) {
            m_mw.m_radioModel.sendCommand(
                QString("display panafall set %1 color_gain=50").arg(pan->waterfallId()));
            m_mw.m_radioModel.sendCommand(
                QString("display panafall set %1 black_level=15").arg(pan->waterfallId()));
            m_mw.m_radioModel.sendCommand(
                QString("display panafall set %1 line_duration=100").arg(pan->waterfallId()));
        }

        // Persist all defaults to AppSettings
        auto& s = AppSettings::instance();
        s.setValue(sw->settingsKey("DisplayFftAverage"),          "0");
        s.setValue(sw->settingsKey("DisplayFftFps"),              "25");
        s.setValue(sw->settingsKey("DisplayFftFillAlpha"),        "0.70");
        s.setValue(sw->settingsKey("DisplayFftFillColor"),        "#00e5ff");
        s.setValue(sw->settingsKey("DisplayFftWeightedAvg"),      "False");
        s.setValue(sw->settingsKey("DisplayFftHeatMap"),          "True");
        s.setValue(sw->settingsKey("DisplayWfColorScheme"),       "0");
        s.setValue(sw->settingsKey("DisplayWfColorGain"),         "50");
        s.setValue(sw->settingsKey("DisplayWfBlackLevel"),        "15");
        s.setValue(sw->settingsKey("DisplayWfAutoBlack"),         "True");
        s.setValue(sw->settingsKey("DisplayWfLineDuration"),      "100");
        s.setValue(sw->settingsKey("WaterfallBlankingEnabled"),   "False");
        s.setValue(sw->settingsKey("WaterfallBlankingThreshold"), "1.15");
        s.setValue(sw->settingsKey("WaterfallBlankingMode"),      "0");
        s.setValue(sw->settingsKey("CursorFreqLabel"),            "False");
        s.setValue(sw->settingsKey("BackgroundImage"),            ":/bg-default.jpg");
        s.setValue(sw->settingsKey("BackgroundOpacity"),          "80");
        s.setValue(sw->settingsKey("DisplayFreqGridSpacing"),     "0");
        s.save();

        // Sync all Display panel UI controls
        menu->syncDisplaySettings(0, 25, 70, false, QColor(0x00, 0xe5, 0xff),
                                  50, 15, true, 100, 75, false, true, 0);
        menu->syncExtraDisplaySettings(false, 1.15f, 80, 0, false);
    });

    // ── Click-to-tune ────────────────────────────────────────────────────
    // Uses "slice m <freq> pan=<panId>" (matches SmartSDR protocol).
    // The radio routes the tune to the correct slice for that pan.
    connect(sw, &SpectrumWidget::frequencyClicked,
            this, [this, sw](double mhz) {
        // Find the panId for this spectrum widget
        QString panId;
        for (auto* applet : m_mw.m_panStack->allApplets()) {
            if (applet->spectrumWidget() == sw) {
                panId = applet->panId();
                break;
            }
        }

        if (!panId.isEmpty()) {
            // Check if the active slice is on a DIFFERENT pan — if so, switch
            // to a slice on the clicked pan. Don't switch when multiple slices
            // share the same pan (that causes alternating tune targets).
            auto* curSlice = m_mw.activeSlice();
            bool differentPan = curSlice && curSlice->panId() != panId;

            if (differentPan) {
                for (auto* s : m_mw.m_radioModel.slices()) {
                    if (s->panId() == panId) {
                        m_mw.m_sliceCtl->setActiveSlice(s->sliceId());
                        break;
                    }
                }
                m_mw.m_panStack->setActivePan(panId);

                if (auto* s = m_mw.activeSlice(); s && !s->isLocked()) {
                    m_mw.m_radioModel.sendCommand(
                        QString("slice m %1 pan=%2").arg(mhz, 0, 'f', 6).arg(panId));
                    sw->setVfoFrequency(mhz);
                }
            } else {
                m_mw.m_sliceCtl->onFrequencyChanged(mhz);
            }
        } else {
            m_mw.m_sliceCtl->onFrequencyChanged(mhz);
        }
    });

    connect(sw, &SpectrumWidget::rotatorTurnRequested, this,
            [this](const QString& call, const QString& comment, bool longPath) {
        m_mw.m_toolsCtl->turnRotatorToCallsign(call, comment, longPath);
    });

    // ── Spot trigger — notify the radio when a spot label is clicked (#341)
    connect(sw, &SpectrumWidget::spotTriggered, this, [this, applet](int spotIndex) {
        const auto& spots = m_mw.m_radioModel.spotModel().spots();
        auto it = spots.find(spotIndex);
        if (it == spots.end()) return;

        if (it->source == "Memory") {
            int memoryIndex = MemoryController::memoryIndexFromSpotId(spotIndex);
            if (memoryIndex >= 0)
                m_mw.m_memoryCtl->activateMemorySpot(memoryIndex, applet->panId());
            return;
        }

        if (it->source == WsjtxUdpServer::kSpotSource) {
            // Client-side spot: ask WSJT-X to answer this decode (as if
            // double-clicked in its Band Activity). Never keys TX itself.
            if (WsjtxUdpServer* ws = m_mw.m_spotCtl->wsjtxServer()) {
                ws->replyToSpot(spotIndex);
            }
            return;
        }

        if (it->source == "TCI") {
            // TCI spots live only in the client-side SpotModel (the radio
            // doesn't know their IDs), so "spot trigger" would never echo.
            // Raise the click locally so TCI clients get clicked_on_spot.
            emit m_mw.m_radioModel.spotModel().spotTriggered(spotIndex, applet->panId());
        } else {
            m_mw.m_radioModel.sendCommand(QString("spot trigger %1").arg(spotIndex));
        }

        // Auto-switch mode from spot metadata (#424)
        if (AppSettings::instance().value("SpotAutoSwitchMode", "False").toString() != "True")
            return;
        auto* s = m_mw.activeSlice();
        if (!s) return;

        // Extract mode: prefer explicit mode field, fall back to comment text
        QString spotMode = it->mode.toUpper().trimmed();
        if (spotMode.isEmpty() && !it->comment.isEmpty()) {
            // RBN: mode is first word ("CW  6 dB 28 WPM CQ")
            // POTA/Cluster: mode is often last word ("JP-1277 Higashimurayama CW")
            static const QSet<QString> knownModes = {
                "CW", "SSB", "USB", "LSB", "AM", "FM", "FT8", "FT4",
                "JS8", "RTTY", "PSK31", "PSK63", "PSK", "OLIVIA",
                "JT65", "JT9", "SAM", "NFM", "DIGU", "DIGL"
            };
            QStringList words = it->comment.split(' ', Qt::SkipEmptyParts);
            // Check first word (RBN format)
            if (!words.isEmpty() && knownModes.contains(words.first().toUpper()))
                spotMode = words.first().toUpper();
            // Check last word (POTA/Cluster format)
            else if (!words.isEmpty() && knownModes.contains(words.last().toUpper()))
                spotMode = words.last().toUpper();
        }
        // Fallback: infer mode from frequency using band plan
        if (spotMode.isEmpty()) {
            double f = it->rxFreqMhz;
            // CW sub-bands (bottom of each HF band)
            if ((f >= 1.800 && f < 1.850) || (f >= 3.500 && f < 3.600) ||
                (f >= 7.000 && f < 7.050) || (f >= 10.100 && f < 10.140) ||
                (f >= 14.000 && f < 14.070) || (f >= 18.068 && f < 18.095) ||
                (f >= 21.000 && f < 21.070) || (f >= 24.890 && f < 24.920) ||
                (f >= 28.000 && f < 28.070) || (f >= 50.000 && f < 50.100))
                spotMode = "CW";
            // Digital sub-bands
            else if ((f >= 1.840 && f < 1.850) || (f >= 3.570 && f < 3.600) ||
                     (f >= 7.040 && f < 7.050) || (f >= 10.130 && f < 10.150) ||
                     (f >= 14.070 && f < 14.100) || (f >= 18.095 && f < 18.110) ||
                     (f >= 21.070 && f < 21.100) || (f >= 24.915 && f < 24.930) ||
                     (f >= 28.070 && f < 28.150))
                spotMode = "DIGU";
            // Phone — default by convention
            else if (f >= 10.0)
                spotMode = "USB";
            else if (f >= 1.8)
                spotMode = "LSB";
        }
        if (spotMode.isEmpty()) return;

        // Map spot mode string → radio mode
        static const QMap<QString, QString> modeMap = {
            {"CW", "CW"}, {"CWL", "CW"}, {"CWU", "CW"},
            {"USB", "USB"}, {"LSB", "LSB"},
            {"FT8", "DIGU"}, {"FT4", "DIGU"}, {"JS8", "DIGU"},
            {"PSK31", "DIGU"}, {"PSK63", "DIGU"}, {"PSK", "DIGU"},
            {"OLIVIA", "DIGU"}, {"JT65", "DIGU"}, {"JT9", "DIGU"},
            {"RTTY", "DIGL"},
            {"AM", "AM"}, {"SAM", "SAM"},
            {"FM", "FM"}, {"NFM", "NFM"},
        };
        QString radioMode;
        if (modeMap.contains(spotMode)) {
            radioMode = modeMap[spotMode];
        } else if (spotMode == "SSB") {
            // SSB without sideband: ≥10 MHz → USB, <10 MHz → LSB
            radioMode = (it->rxFreqMhz >= 10.0) ? "USB" : "LSB";
        }
        if (!radioMode.isEmpty() && radioMode != s->mode())
            s->setMode(radioMode);
    });

    // ── Manual spot add/remove (#36)
    connect(sw, &SpectrumWidget::spotAddRequested, this,
            [this](double freqMhz, const QString& callsign, const QString& comment,
                   int lifetimeSec, bool forwardToCluster) {
        m_mw.m_spotCtl->addManualSpot(freqMhz, callsign, comment, lifetimeSec, forwardToCluster);
    });
    connect(sw, &SpectrumWidget::spotRemoveRequested, this, [this](int spotIndex) {
        m_mw.m_radioModel.sendCommand(QString("spot remove %1").arg(spotIndex));
    });

    // ── +RX / +TNF buttons ───────────────────────────────────────────────
    connect(menu, &SpectrumOverlayMenu::addRxClicked,
            this, [this](const QString& panId) {
        int limit = m_mw.m_radioModel.maxSlices();
        if (m_mw.m_radioModel.slices().size() < limit) {
            m_mw.m_radioModel.addSliceOnPan(panId);
        } else {
            m_mw.statusBar()->showMessage(
                QString("%1 supports a maximum of %2 slices")
                    .arg(m_mw.m_radioModel.model()).arg(limit), 4000);
        }
    });
    connect(menu, &SpectrumOverlayMenu::addTnfClicked,
            this, [this]() {
        auto* s = m_mw.activeSlice();
        if (!s) return;
        double tnfFreq = s->frequency()
            + (s->filterLow() + s->filterHigh()) / 2.0 / 1.0e6;
        m_mw.m_radioModel.tnfModel().createTnf(tnfFreq);
    });
    connect(menu, &SpectrumOverlayMenu::memoryActivated,
            this, [this](int memoryIndex, const QString& panId) {
        m_mw.m_memoryCtl->activateMemorySpot(memoryIndex, panId);
    });
    connect(menu, &SpectrumOverlayMenu::quickAddMemoryRequested,
            this, [this](const QString& panId) {
        m_mw.m_memoryCtl->showQuickAddMemoryDialog(panId);
    });

    // ── Slice marker clicks ──────────────────────────────────────────────
    connect(sw, &SpectrumWidget::sliceClicked,
            m_mw.m_sliceCtl, &SliceController::setActiveSlice);
    connect(sw, &SpectrumWidget::sliceTxRequested,
            this, [this](int sliceId) {
        if (auto* s = m_mw.m_radioModel.slice(sliceId))
            s->setTxSlice(true);
    });
    connect(sw, &SpectrumWidget::sliceCloseRequested,
            this, [this](int sliceId) {
        if (m_mw.m_radioModel.slices().size() <= 1) return;
        m_mw.m_radioModel.sendCommand(QString("slice remove %1").arg(sliceId));
    });
    connect(sw, &SpectrumWidget::sliceTuneRequested,
            this, [this](int sliceId, double freqMhz) {
        if (auto* s = m_mw.m_radioModel.slice(sliceId))
            s->setFrequency(freqMhz);
    });

    // ── Band selection ───────────────────────────────────────────────────
    connect(menu, &SpectrumOverlayMenu::bandSelected,
            this, [this, applet](const QString& bandName, double freqMhz, const QString& mode) {
        qCDebug(lcGui) << "MainWindow: switching to band" << bandName
                 << "freq:" << freqMhz << "mode:" << mode;

        // Radio-authoritative band change: the radio manages its own band
        // stack (frequency, mode, filters, pan center, bandwidth, antennas).
        // One command handles everything.
        m_mw.m_bandSettings.setCurrentBand(bandName);

        // Translate the UI band label to the radio's band-stack key. Mapping
        // determined from SmartSDR pcap captures (#1540/#1211):
        //   - Standard HF names (160, 80, ..., 6, 2200, 630): pass through
        //   - XVTR display names ("2m"): send band=X<idx> (xvtr index)
        //   - WWV / GEN: send numeric band-stack slot 33 / 34 (per pcap)
        //   - Anything else: fall back to tuning the slice directly
        static const QSet<QString> kPassThroughBands = {
            "160", "80", "60", "40", "30", "20", "17", "15", "12", "10", "6",
            "2200", "630"
        };
        static const QMap<QString, int> kNumericBandSlots = {
            { QStringLiteral("WWV"), 33 },
            { QStringLiteral("GEN"), 34 },
        };

        QString stackKey;
        if (kPassThroughBands.contains(bandName)) {
            stackKey = bandName;
        } else if (kNumericBandSlots.contains(bandName)) {
            stackKey = QString::number(kNumericBandSlots.value(bandName));
            qCDebug(lcGui) << "  ↳ numeric band slot:" << bandName << "->" << stackKey;
        } else {
            for (auto it = m_mw.m_radioModel.xvtrList().constBegin();
                 it != m_mw.m_radioModel.xvtrList().constEnd(); ++it) {
                if (it.value().isValid && it.value().name == bandName) {
                    stackKey = QString("X%1").arg(it.key());
                    qCDebug(lcGui) << "  ↳ XVTR match:" << bandName << "->" << stackKey;
                    break;
                }
            }
        }

        if (stackKey.isEmpty()) {
            // Unrecognized band — tune slice directly as a safety net.
            qCDebug(lcGui) << "  ↳ unrecognized band" << bandName << "— tuning slice directly";
            SliceModel* s = nullptr;
            for (auto* sl : m_mw.m_radioModel.slices()) {
                if (sl->panId() == applet->panId()) { s = sl; break; }
            }
            if (!s) s = m_mw.activeSlice();
            if (s) {
                if (!mode.isEmpty() && s->mode() != mode) s->setMode(mode);
                s->tuneAndRecenter(freqMhz);
            }
        } else {
            m_mw.m_radioModel.sendCommand(
                QString("display pan set %1 band=%2").arg(applet->panId()).arg(stackKey));
        }
    });

    // XVTR button → open Radio Setup XVTR tab (#571)
    connect(menu, &SpectrumOverlayMenu::xvtrSetupRequested,
            this, [this]() {
        m_mw.showRadioSetupDialog(QStringLiteral("XVTR"));
    });

    // ── WNB / RF Gain ────────────────────────────────────────────────────
    connect(menu, &SpectrumOverlayMenu::wnbToggled,
            this, [this, sw, applet](bool on) {
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 wnb=%2").arg(applet->panId()).arg(on ? 1 : 0));
        sw->setWnbActive(on);
    });
    connect(menu, &SpectrumOverlayMenu::wnbLevelChanged,
            this, [this, applet](int level) {
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 wnb_level=%2").arg(applet->panId()).arg(level));
    });
    connect(menu, &SpectrumOverlayMenu::rfGainChanged,
            this, [this, sw, applet](int gain) {
        m_mw.m_radioModel.sendCommand(
            QString("display pan set %1 rfgain=%2").arg(applet->panId()).arg(gain));
        sw->setRfGain(gain);
    });

    // ── NR2/RN2 overlay toggle → AudioEngine ───────────────────────────
    // Button sync back to overlay + VFO handled by nr2/rn2EnabledChanged above.
    connect(menu, &SpectrumOverlayMenu::nr2Toggled,
            this, [this, menu](bool on) {
        if (on) {
            // NR2 amplifies Opus codec artifacts → disable when compressed audio active (#1597)
            if (m_mw.m_radioModel.audioCompressionParam() == "opus") {
                QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(false); });
                m_mw.statusBar()->showMessage("NR2 is not available with compressed (SmartLink) audio", 4000);
                return;
            }
            QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setRn2Enabled(false); });
            m_mw.m_dspCtl->enableNr2WithWisdom();
        } else {
            QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(false); });
        }
        // VFO button sync happens via AudioEngine::nr2EnabledChanged signal
    });
    connect(menu, &SpectrumOverlayMenu::nr2RightClicked,
            this, [this](const QPoint& pos) { m_mw.m_dspCtl->showNr2ParamPopup(pos); });
    connect(menu, &SpectrumOverlayMenu::rn2Toggled,
            this, [this](bool on) {
        if (on) {
            QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setNr2Enabled(false); });
            QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setRn2Enabled(true); });
        } else {
            QMetaObject::invokeMethod(m_mw.m_audio, [this]() { m_mw.m_audio->setRn2Enabled(false); });
        }
        // VFO button sync happens via AudioEngine::rn2EnabledChanged signal
    });
    connect(menu, &SpectrumOverlayMenu::bnrToggled,
            this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setBnrEnabled(on); });
        // VFO button sync happens via AudioEngine::bnrEnabledChanged signal
    });
    connect(menu, &SpectrumOverlayMenu::bnrIntensityChanged,
            this, [this](float ratio) {
        // m_bnr is created/reset on the audio thread — don't touch it from here.
        QMetaObject::invokeMethod(m_mw.m_audio, [this, ratio]() { m_mw.m_audio->setBnrIntensity(ratio); });
    });
    connect(menu, &SpectrumOverlayMenu::nr4Toggled,
            this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setNr4Enabled(on); });
        // VFO button sync happens via AudioEngine::nr4EnabledChanged signal
    });
    connect(menu, &SpectrumOverlayMenu::nr4RightClicked,
            this, [this](const QPoint& pos) { m_mw.m_dspCtl->showNr4ParamPopup(pos); });
    connect(menu, &SpectrumOverlayMenu::mnrToggled,
            this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setMnrEnabled(on); });
        // VFO button sync happens via AudioEngine::mnrEnabledChanged signal
    });
    connect(menu, &SpectrumOverlayMenu::mnrRightClicked,
            this, [this](const QPoint&) { m_mw.m_dspCtl->showMnrSettings(); });
    connect(menu, &SpectrumOverlayMenu::dfnrToggled,
            this, [this](bool on) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, on]() { m_mw.m_audio->setDfnrEnabled(on); });
    });
    connect(menu, &SpectrumOverlayMenu::dfnrRightClicked,
            this, [this](const QPoint& pos) { m_mw.m_dspCtl->showDfnrParamPopup(pos); });
}

void PanadapterController::applyPanLayout(const QString& layoutId)
{
    if (!m_mw.m_radioModel.isConnected()) return;

    static const QMap<QString, int> kPanCounts = {
        {"1", 1}, {"2v", 2}, {"2h", 2}, {"2h1", 3}, {"12h", 3}, {"3v", 3},
        {"2x2", 4}, {"4v", 4}, {"3h2", 5}, {"2x3", 6}, {"4h3", 7}, {"2x4", 8}
    };
    const int needed = kPanCounts.value(layoutId, 1);
    const int existing = m_mw.m_panStack->count();

    if (needed < existing) {
        qCDebug(lcGui) << "applyPanLayout: reducing from" << existing << "to" << needed << "pans";

        // Close extra pans from the end (keep the first N)
        auto allApplets = m_mw.m_panStack->allApplets();
        int toRemove = existing - needed;
        for (int i = allApplets.size() - 1; i >= 0 && toRemove > 0; --i) {
            auto* applet = allApplets[i];
            QString panId = applet->panId();
            if (panId == "default") continue;
            qCDebug(lcGui) << "applyPanLayout: closing pan" << panId;
            m_mw.m_radioModel.sendCommand(
                QString("display pan remove %1").arg(panId));
            // Radio will send "removed" status → panadapterRemoved signal
            // → PanadapterStack::removePanadapter()
            --toRemove;
        }

        // Rearrange remaining pans after a short delay for radio to process
        QTimer::singleShot(500, this, [this, layoutId]() {
            m_mw.m_panStack->rearrangeLayout(layoutId);
        });
        return;
    }
    if (needed == existing) {
        // Same count, just rearrange
        m_mw.m_panStack->rearrangeLayout(layoutId);
        return;
    }

    // Create additional pans to reach the needed count.
    // Keep existing pan(s) alive — no tear-down, no dangling signals.
    const int toCreate = needed - existing;
    auto panIds = std::make_shared<QStringList>();

    // Collect existing pan IDs first (they'll be part of the layout)
    for (auto* applet : m_mw.m_panStack->allApplets())
        panIds->append(applet->panId());

    qCDebug(lcGui) << "applyPanLayout: have" << existing << "pans, creating"
             << toCreate << "more for layout" << layoutId;

    createPansSequentially(layoutId, toCreate, panIds, 0);
}

void PanadapterController::createPansSequentially(const QString& layoutId, int total,
                                        std::shared_ptr<QStringList> panIds, int created)
{
    if (created >= total) {
        // All new pans created — wait for radio status to establish PanadapterModels
        qCDebug(lcGui) << "applyPanLayout: all" << total << "new pans created:" << *panIds;
        QTimer::singleShot(800, this, [this, panIds, layoutId]() {
            // The new pans were added to the stack via panadapterAdded handler.
            // Wire any that aren't already wired.
            for (auto* applet : m_mw.m_panStack->allApplets()) {
                const QString pid = applet->panId();
                auto* pan = m_mw.m_radioModel.panadapter(pid);
                if (pan) {
                    // Push current state to the spectrum widget
                    applet->spectrumWidget()->setDbmRange(pan->minDbm(), pan->maxDbm());
                    applet->spectrumWidget()->setFrequencyRange(
                        pan->centerMhz(), pan->bandwidthMhz());
                }
            }

            // Rearrange splitter structure for the selected layout
            m_mw.m_panStack->rearrangeLayout(layoutId);

            m_mw.m_panApplet = m_mw.m_panStack->activeApplet();

            qCDebug(lcGui) << "applyPanLayout: layout" << layoutId
                     << "complete, total pans:" << m_mw.m_panStack->count();
        });
        return;
    }

    m_mw.m_radioModel.sendCmdPublic(
        "display panafall create x=100 y=100",
        [this, panIds, layoutId, total, created](int code, const QString& body) {
            if (code != 0) {
                qCWarning(lcGui) << "applyPanLayout: panafall create failed, code"
                           << Qt::hex << code << body;
                return;
            }
            const auto kvs = CommandParser::parseKVs(body);
            QString panId;
            if (kvs.contains("pan"))       panId = kvs["pan"];
            else if (kvs.contains("id"))   panId = kvs["id"];
            else                           panId = body.trimmed();

            qCDebug(lcGui) << "applyPanLayout: created pan" << (created + 1) << "of" << total
                     << "id:" << panId;

            if (!panId.isEmpty()) {
                panIds->append(panId);
                // Configure the pan
                m_mw.m_radioModel.sendCommand(
                    QString("display pan set %1 xpixels=1024 ypixels=700").arg(panId));
                m_mw.m_radioModel.sendCommand(
                    QString("display pan set %1 fps=25").arg(panId));
            }

            // Create next pan after a brief delay
            QTimer::singleShot(200, this, [this, layoutId, total, panIds, created]() {
                createPansSequentially(layoutId, total, panIds, created + 1);
            });
        });
}

} // namespace AetherSDR
