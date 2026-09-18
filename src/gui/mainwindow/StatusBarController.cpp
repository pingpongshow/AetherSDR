#include "gui/mainwindow/StatusBarController.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/ConnectionController.h"
#include "gui/mainwindow/PanadapterController.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "gui/PanLayoutDialog.h"
#include "core/LogManager.h"
#include "gui/SpectrumWidget.h"
#include "gui/AppletPanel.h"
#include "gui/CwxPanel.h"
#include "gui/DvkPanel.h"
#include "models/SliceModel.h"
#include "models/MeterModel.h"
#include "gui/BandStackPanel.h"
#include "models/TunerModel.h"
#include <QTimer>
#include <QDateTime>
#include <QKeyEvent>
#include <QPixmap>
#include <QPainter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialog>
#include <QLabel>
#include <QThread>
#include "core/AppSettings.h"
#include <QLocale>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace AetherSDR {

static QString formatNetworkMs(int ms)
{
    return ms < 1 ? "< 1 ms" : QString("%1 ms").arg(ms);
}

static QString formatNetworkSeqErrors(int errors, int packets)
{
    if (packets == 0) {
        return "0 / 0 packets";
    }

    const double pct = (errors * 100.0) / packets;
    return QString("%1 / %2 packets (%3%)")
        .arg(errors)
        .arg(packets)
        .arg(pct, 0, 'f', 2);
}

static QString formatNetworkSeqErrors(const PanadapterStream::CategoryStats& stats)
{
    return formatNetworkSeqErrors(stats.errors, stats.packets);
}

static QString buildNetworkTooltip(const RadioModel& model)
{
    const PanadapterStream::CategoryStats audioStats =
        model.categoryStats(PanadapterStream::CatAudio);
    const PanadapterStream::CategoryStats fftStats =
        model.categoryStats(PanadapterStream::CatFFT);
    const PanadapterStream::CategoryStats waterfallStats =
        model.categoryStats(PanadapterStream::CatWaterfall);
    const PanadapterStream::CategoryStats meterStats =
        model.categoryStats(PanadapterStream::CatMeter);
    const PanadapterStream::CategoryStats daxStats =
        model.categoryStats(PanadapterStream::CatDAX);

    QStringList lines;
    lines
        << QString("Network: %1").arg(model.networkQuality())
        << QString("Latency (RTT): %1").arg(formatNetworkMs(model.lastPingRtt()))
        << QString("Max RTT (session): %1").arg(formatNetworkMs(model.maxPingRtt()))
        << QString("Total sequence gaps: %1")
               .arg(formatNetworkSeqErrors(model.packetDropCount(), model.packetTotalCount()))
        << QString("Audio: %1").arg(formatNetworkSeqErrors(audioStats))
        << QString("FFT: %1").arg(formatNetworkSeqErrors(fftStats))
        << QString("Waterfall: %1").arg(formatNetworkSeqErrors(waterfallStats))
        << QString("Meters: %1").arg(formatNetworkSeqErrors(meterStats))
        << QString("DAX: %1").arg(formatNetworkSeqErrors(daxStats))
        << QString("UDP RX bytes: %1").arg(QLocale().formattedDataSize(model.rxBytes()))
        << QString("UDP TX bytes: %1").arg(QLocale().formattedDataSize(model.txBytes()))
        << "Double-click for full diagnostics";
    return lines.join('\n');
}

static constexpr const char* kPaTempUnitSettingKey = "PaTempDisplayUnit";

static QPixmap buildBandStackIndicatorPixmap(bool active)
{
    QPixmap pixmap(10, 22);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(active ? QColor(0x00, 0xb4, 0xd8) : QColor(0x40, 0x48, 0x58));
    painter.drawEllipse(2, 1, 6, 6);
    painter.drawEllipse(2, 8, 6, 6);
    painter.drawEllipse(2, 15, 6, 6);
    return pixmap;
}

StatusBarController::StatusBarController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void StatusBarController::loadPaTempUnit()
{
    m_paTempUseFahrenheit =
        AppSettings::instance().value(kPaTempUnitSettingKey, "Fahrenheit").toString() != "Celsius";
    updatePaTempLabel();
}

void StatusBarController::wireTelemetry()
{
    // ── Status bar telemetry ──────────────────────────────────────────────────
    connect(&m_mw.m_radioModel, &RadioModel::networkQualityChanged,
            this, [this](const QString& quality, int pingMs) {
        // Color code: Excellent/VeryGood=green, Good=cyan, Fair=amber, Poor=red
        QString color = "#00cc66";
        if (quality == "Fair") color = "#cc9900";
        else if (quality == "Poor") color = "#cc3333";
        else if (quality == "Good") color = "#00b4d8";
        m_networkLabel->setText(QString("[<span style='color:%1'>%2</span>]")
            .arg(color, quality));
        Q_UNUSED(pingMs);
        m_networkLabel->setToolTip(buildNetworkTooltip(m_mw.m_radioModel));
    });

    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::hwTelemetryChanged,
            this, [this](float paTemp, float supplyVolts) {
        m_lastPaTempC = paTemp;
        m_hasPaTempTelemetry = true;
        updatePaTempLabel();
        m_supplyVoltLabel->setText(QString("%1 V").arg(supplyVolts, 0, 'f', 2));

        // Update station label (nickname arrives via status after connect)
        const QString nick = m_mw.m_radioModel.nickname();
        if (!nick.isEmpty())
            m_mw.m_stationLabel->setText(nick);
    });

    // Frequency reference label from oscillator status (#478)
    // Show what the radio is actually locked to, not GPS satellite state.
    connect(&m_mw.m_radioModel, &RadioModel::infoChanged, this, [this]() {
        const QString& state = m_mw.m_radioModel.oscState();
        const bool locked = m_mw.m_radioModel.oscLocked();
        if (state == "gpsdo" && locked) {
            // Don't override — GPS status handler shows satellite count
        } else if (m_mw.m_radioModel.extPresent() && (state == "ext" || state == "external")) {
            m_gpsLabel->setText("Ref: Ext 10M");
            m_gpsStatusLabel->setText(QString("[%1]").arg(locked ? "Locked" : "Unlocked"));
        } else {
            m_gpsLabel->setText("Ref: TCXO");
            m_gpsStatusLabel->setText("[Locked]");
        }
    });

    connect(&m_mw.m_radioModel, &RadioModel::gpsStatusChanged,
            this, [this](const QString& status, int tracked, int visible,
                         const QString& grid, const QString& /*alt*/,
                         const QString& /*lat*/, const QString& /*lon*/,
                         const QString& utcTime) {
        // Only show satellite count + lock status when GPSDO is active
        if (m_mw.m_radioModel.oscState() == "gpsdo" && m_mw.m_radioModel.oscLocked()) {
            m_gpsLabel->setText(QString("GPS: %1/%2").arg(tracked).arg(visible));
            m_gpsStatusLabel->setText(QString("[%1]").arg(status));
        }

        if (!grid.isEmpty())
            m_gridLabel->setText(grid);

        // Use GPS UTC time only when GPSDO is installed and locked.
        // GPS with no antenna/lock sends stale "00:00:00Z" — fall back to system clock.
        if (!utcTime.isEmpty()
            && m_mw.m_radioModel.oscState() == "gpsdo"
            && m_mw.m_radioModel.oscLocked()) {
            m_gpsTimeLabel->setText(utcTime);
            m_useSystemClock = false;
        } else {
            m_useSystemClock = true;
        }
    });

    // System clock fallback when no GPS is installed
    auto* clockTimer = new QTimer(&m_mw);
    connect(clockTimer, &QTimer::timeout, this, [this] {
        auto utc = QDateTime::currentDateTimeUtc();
        m_gpsDateLabel->setText(utc.toString("yyyy-MM-dd"));
        if (m_useSystemClock)
            m_gpsTimeLabel->setText(utc.toString("HH:mm:ssZ"));
    });
    clockTimer->start(1000);
}

bool StatusBarController::handleIndicatorEvent(QObject* obj, QEvent* event)
{
    if (obj == m_paTempLabel && event->type() == QEvent::MouseButtonPress) {
        setPaTempDisplayUnit(!m_paTempUseFahrenheit);
        return true;
    }
    if (obj == m_networkLabel && event->type() == QEvent::MouseButtonDblClick) {
        m_mw.showNetworkDiagnosticsDialog();
        return true;
    }
    if (obj == m_mw.m_stationNickLabel && event->type() == QEvent::MouseButtonDblClick) {
        m_mw.m_connectionCtl->toggleConnectionDialog();
        return true;
    }
    if (obj == m_cwxIndicator && event->type() == QEvent::MouseButtonPress) {
        if (!m_cwxIndicator->isEnabled()) return true;
        bool show = !m_mw.m_cwxPanel->isVisible();
        // Close DVK (mutual exclusion)
        if (show && m_mw.m_dvkPanel->isVisible()) {
            m_mw.m_dvkPanel->hide();
            auto* sl = m_mw.activeSlice();
            updateKeyerAvailability(sl ? sl->mode() : QString());
        }
        m_mw.m_cwxPanel->setVisible(show);
        m_cwxIndicator->setStyleSheet(show
            ? "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }"
            : "QLabel { color: rgba(255,255,255,40); font-weight: bold; font-size: 24px; }");
        if (show) {
            auto sizes = m_mw.m_splitter->sizes();
            if (sizes.size() >= 4) {
                int cwxW = 250;
                int total = sizes[0] + sizes[1] + sizes[2];
                sizes[0] = cwxW;
                sizes[1] = 0;
                sizes[2] = total - cwxW;
                m_mw.m_splitter->setSizes(sizes);
            }
        }
        return true;
    }
    if (obj == m_dvkIndicator && event->type() == QEvent::MouseButtonPress) {
        if (!m_dvkIndicator->isEnabled()) return true;
        bool show = !m_mw.m_dvkPanel->isVisible();
        // Close CWX (mutual exclusion)
        if (show && m_mw.m_cwxPanel->isVisible()) {
            m_mw.m_cwxPanel->hide();
            auto* sl = m_mw.activeSlice();
            updateKeyerAvailability(sl ? sl->mode() : QString());
        }
        m_mw.m_dvkPanel->setVisible(show);
        m_dvkIndicator->setStyleSheet(show
            ? "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }"
            : "QLabel { color: rgba(255,255,255,40); font-weight: bold; font-size: 24px; }");
        if (show) {
            auto sizes = m_mw.m_splitter->sizes();
            if (sizes.size() >= 4) {
                int dvkW = 250;
                int total = sizes[0] + sizes[1] + sizes[2];
                sizes[0] = 0;
                sizes[1] = dvkW;
                sizes[2] = total - dvkW;
                m_mw.m_splitter->setSizes(sizes);
            }
        }
        return true;
    }
    if (obj == m_mw.m_tnfIndicator && event->type() == QEvent::MouseButtonPress) {
        m_mw.m_radioModel.tnfModel().requestGlobalTnfEnabled(!m_mw.m_radioModel.tnfModel().globalEnabled());
        return true;
    }
    if (obj == m_mw.m_fdxIndicator && event->type() == QEvent::MouseButtonPress) {
        bool on = !m_mw.m_radioModel.fullDuplexEnabled();
        m_mw.m_radioModel.sendCommand(QString("radio set full_duplex_enabled=%1").arg(on ? 1 : 0));
        // Optimistic update — radio accepts this command (R|0) but doesn't
        // echo back a status update with the new value.
        m_mw.m_radioModel.setFullDuplex(on);
        return true;
    }
    if (obj == m_bandStackIndicator && event->type() == QEvent::MouseButtonPress) {
        bool show = !m_mw.m_panStack->bandStackPanel()->isVisible();
        m_mw.m_panStack->setBandStackVisible(show);
        updateBandStackIndicator();
        return true;
    }
    if (obj == m_mw.m_tgxlIndicator && event->type() == QEvent::MouseButtonPress) {
        auto& t = m_mw.m_radioModel.tunerModel();
        // Cycle: OPERATE → BYPASS → STANDBY → OPERATE
        if (t.isOperate() && !t.isBypass())
            t.setBypass(true);
        else if (t.isOperate() && t.isBypass())
            t.setOperate(false);
        else {
            t.setBypass(false);
            t.setOperate(true);
        }
        return true;
    }
    if (obj == m_mw.m_pgxlIndicator && event->type() == QEvent::MouseButtonPress) {
        // Simple toggle: OPERATE ↔ STANDBY (PGXL has no BYPASS)
        m_mw.m_radioModel.setAmpOperate(!m_mw.m_radioModel.ampOperate());
        return true;
    }
    if (obj == m_mw.m_panelToggle && event->type() == QEvent::MouseButtonPress) {
        bool visible = m_mw.m_appletPanel->isVisible();
        m_mw.m_appletPanel->setVisible(!visible);
        m_mw.m_panelToggle->setStyleSheet(!visible
            ? "QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 22px; }"
            : "QLabel { color: #404858; font-weight: bold; font-size: 22px; }");
        AppSettings::instance().setValue("AppletPanelVisible", !visible ? "True" : "False");
        AppSettings::instance().save();
        if (m_mw.m_panelVisAction) {
            QSignalBlocker sb(m_mw.m_panelVisAction);
            m_mw.m_panelVisAction->setChecked(!visible);
        }
        return true;
    }
    if (obj == m_addPanLabel && event->type() == QEvent::MouseButtonPress) {
        if (!m_mw.m_radioModel.isConnected()) return true;
        int maxPans = m_mw.m_radioModel.maxPanadapters();
        // Determine current layout from actual pan count, not saved setting
        int activePanCount = m_mw.m_panStack ? m_mw.m_panStack->count() : 1;
        QString currentLayout = "1";
        if (activePanCount >= 2)
            currentLayout = AppSettings::instance()
                .value("PanadapterLayout", "1").toString();
        PanLayoutDialog dlg(maxPans, currentLayout, &m_mw);
        if (dlg.exec() == QDialog::Accepted && !dlg.selectedLayout().isEmpty()) {
            const QString layoutId = dlg.selectedLayout();
            auto& s = AppSettings::instance();
            s.setValue("PanadapterLayout", layoutId);
            s.save();
            m_mw.m_panCtl->applyPanLayout(layoutId);
        }
        return true;
    }
    return false;
}

void StatusBarController::updatePaTempLabel()
{
    const QString unit = m_paTempUseFahrenheit ? "F" : "C";
    if (!m_hasPaTempTelemetry) {
        m_paTempLabel->setText(QString("PA --\u00B0%1").arg(unit));
    } else if (m_paTempUseFahrenheit) {
        const float paTempF = (m_lastPaTempC * 9.0f / 5.0f) + 32.0f;
        m_paTempLabel->setText(QString("PA %1\u00B0F").arg(paTempF, 0, 'f', 1));
    } else {
        m_paTempLabel->setText(QString("PA %1\u00B0C").arg(m_lastPaTempC, 0, 'f', 1));
    }

    m_paTempLabel->setToolTip(
        QString("PA temperature\nClick to switch to %1")
            .arg(m_paTempUseFahrenheit ? "Celsius (\u00B0C)" : "Fahrenheit (\u00B0F)"));
}

void StatusBarController::setPaTempDisplayUnit(bool useFahrenheit)
{
    if (m_paTempUseFahrenheit == useFahrenheit)
        return;

    m_paTempUseFahrenheit = useFahrenheit;
    auto& settings = AppSettings::instance();
    settings.setValue(kPaTempUnitSettingKey, useFahrenheit ? "Fahrenheit" : "Celsius");
    settings.save();
    updatePaTempLabel();
}

void StatusBarController::buildStatusBar()
{
    // ── Status bar (SmartSDR-style, double height) ─────────────────────
    m_mw.statusBar()->setFixedHeight(46);
    m_mw.statusBar()->setSizeGripEnabled(false);
    m_mw.statusBar()->setStyleSheet(
        "QStatusBar { background: #0a0a14; border-top: 1px solid #203040; }"
        "QStatusBar::item { border: none; }"
        "QLabel { font-size: 21px; background: transparent; }");

    const QString valStyle  = "QLabel { color: #8aa8c0; font-size: 21px; }";
    const QString sepStyle  = "QLabel { color: #304050; font-size: 21px; }";
    const QString greyInd   = "QLabel { color: #404858; font-weight: bold; font-size: 21px; }";
    const QString greenInd  = "QLabel { color: #00e060; font-weight: bold; font-size: 21px; }";
    const QString redInd    = "QLabel { color: #e04040; font-weight: bold; font-size: 21px; }";
    const QString greyIndLg = "QLabel { color: #404858; font-weight: bold; font-size: 24px; }";
    const QString greenIndLg= "QLabel { color: #00e060; font-weight: bold; font-size: 24px; }";

    // Use a container with HBoxLayout for 3-section layout:
    // [left items] → stretch → [STATION centered] → stretch → [right items]
    auto* container = new QWidget(&m_mw);
    auto* hbox = new QHBoxLayout(container);
    hbox->setContentsMargins(6, 0, 6, 0);
    hbox->setSpacing(6);

    auto addSep = [&]() {
        auto* sep = new QLabel(" · ");
        sep->setStyleSheet(sepStyle);
        hbox->addWidget(sep);
    };

    // Hidden connection state label (used by connect/disconnect logic)
    m_mw.m_connStatusLabel = new QLabel("");
    m_mw.m_connStatusLabel->hide();

    // ── Left section ─────────────────────────────────────────────────────
    // +PAN icon: mini spectrum with + overlay
    {
        QPixmap pm(36, 28);
        pm.fill(Qt::transparent);
        QPainter pp(&pm);
        pp.setRenderHint(QPainter::Antialiasing);
        // Spectrum line — flat noise floor with two signal peaks
        pp.setPen(QPen(QColor(255, 255, 255, 128), 1.8));
        const QPointF pts[] = {
            {0,22}, {4,21}, {7,20}, {9,14}, {10,7}, {11,14}, {13,20},
            {16,21}, {18,20}, {20,10}, {21,4}, {22,10}, {24,20},
            {27,21}, {30,22}
        };
        pp.drawPolyline(pts, 15);
        // + sign in upper-right
        pp.setPen(QPen(QColor(255, 255, 255, 200), 2.2));
        pp.drawLine(30, 4, 30, 14);   // vertical
        pp.drawLine(25, 9, 35, 9);    // horizontal
        pp.end();
        // Band stack toggle: 3 vertically stacked circles
        {
            m_bandStackIndicator = new QLabel;
            m_bandStackIndicator->setPixmap(buildBandStackIndicatorPixmap(false));
            m_bandStackIndicator->setCursor(Qt::PointingHandCursor);
            m_bandStackIndicator->setToolTip("Open band stack panel");
            m_bandStackIndicator->installEventFilter(&m_mw);
            hbox->addWidget(m_bandStackIndicator);
        }

        hbox->addSpacing(8);

        auto* addPanBtn = new QLabel;
        addPanBtn->setPixmap(pm);
        addPanBtn->setCursor(Qt::PointingHandCursor);
        addPanBtn->setToolTip("Add Panadapter");
        addPanBtn->installEventFilter(&m_mw);
        hbox->addWidget(addPanBtn);
        m_addPanLabel = addPanBtn;
    }

    hbox->addSpacing(8);

    bool panelVis = AppSettings::instance().value("AppletPanelVisible", "True").toString() == "True";
    m_mw.m_panelToggle = new QLabel(QString::fromUtf8("\xe2\x98\xb0"));  // ☰ hamburger
    m_mw.m_panelToggle->setStyleSheet(panelVis
        ? "QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 22px; }"
        : "QLabel { color: #404858; font-weight: bold; font-size: 22px; }");
    m_mw.m_panelToggle->setAlignment(Qt::AlignBottom);
    m_mw.m_panelToggle->setCursor(Qt::PointingHandCursor);
    m_mw.m_panelToggle->setToolTip("Toggle applet panel");
    m_mw.m_panelToggle->installEventFilter(&m_mw);
    hbox->addWidget(m_mw.m_panelToggle);

    hbox->addSpacing(8);

    m_mw.m_tnfIndicator = new QLabel("TNF");
    m_mw.m_tnfIndicator->setStyleSheet("QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 24px; }");
    m_mw.m_tnfIndicator->setCursor(Qt::PointingHandCursor);
    m_mw.m_tnfIndicator->installEventFilter(&m_mw);
    hbox->addWidget(m_mw.m_tnfIndicator);

    m_cwxIndicator = new QLabel("CWX");
    m_cwxIndicator->setStyleSheet(greyIndLg);
    m_cwxIndicator->setCursor(Qt::PointingHandCursor);
    m_cwxIndicator->setToolTip("CW Keyer — click to toggle");
    m_cwxIndicator->installEventFilter(&m_mw);
    hbox->addWidget(m_cwxIndicator);

    m_dvkIndicator = new QLabel("DVK");
    m_dvkIndicator->setStyleSheet(greyIndLg);
    m_dvkIndicator->setCursor(Qt::PointingHandCursor);
    m_dvkIndicator->setToolTip("Digital Voice Keyer — click to toggle");
    m_dvkIndicator->installEventFilter(&m_mw);
    hbox->addWidget(m_dvkIndicator);

    m_mw.m_fdxIndicator = new QLabel("FDX");
    m_mw.m_fdxIndicator->setStyleSheet(greyIndLg);
    m_mw.m_fdxIndicator->setCursor(Qt::PointingHandCursor);
    m_mw.m_fdxIndicator->setToolTip("Full Duplex — RX stays active during TX (click to toggle)");
    m_mw.m_fdxIndicator->installEventFilter(&m_mw);
    hbox->addWidget(m_mw.m_fdxIndicator);

    addSep();

    // Radio model (top) + version (bottom) stacked.
    // NOTE: m_radioInfoLabel used to be assigned twice here — the first QLabel
    // had no parent and was never added to a layout, so it leaked.
    auto* radioStack = new QWidget;
    auto* radioVbox = new QVBoxLayout(radioStack);
    radioVbox->setContentsMargins(0, 0, 0, 0);
    radioVbox->setSpacing(0);
    m_mw.m_radioInfoLabel = new QLabel("");
    m_mw.m_radioInfoLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    m_mw.m_radioInfoLabel->setAlignment(Qt::AlignCenter);
    m_mw.m_radioInfoLabel->setAccessibleName("Radio model");
    m_mw.m_radioVersionLabel = new QLabel("");
    m_mw.m_radioVersionLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    m_mw.m_radioVersionLabel->setAlignment(Qt::AlignCenter);
    m_mw.m_radioVersionLabel->setAccessibleName("Radio firmware version");
    radioVbox->addWidget(m_mw.m_radioInfoLabel);
    radioVbox->addWidget(m_mw.m_radioVersionLabel);
    hbox->addWidget(radioStack);

    // ── Center stretch → STATION → stretch ───────────────────────────────
    hbox->addStretch(1);

    m_mw.m_stationNickLabel = new QLabel("N0CALL");
    m_mw.m_stationNickLabel->setStyleSheet(
        "QLabel { color: #c8d8e8; font-size: 21px; background: #0a0a14; "
        "border: 1px solid rgba(255,255,255,128); padding: 2px 12px; }");
    m_mw.m_stationNickLabel->setAlignment(Qt::AlignCenter);
    m_mw.m_stationNickLabel->setCursor(Qt::PointingHandCursor);
    m_mw.m_stationNickLabel->setToolTip("Double-click to connect/disconnect");
    m_mw.m_stationNickLabel->setAccessibleName("Station nickname");
    m_mw.m_stationNickLabel->setAccessibleDescription(
        "Shows the connected radio's station nickname. Double-click to connect or disconnect.");
    m_mw.m_stationNickLabel->installEventFilter(&m_mw);
    hbox->addWidget(m_mw.m_stationNickLabel);
    m_mw.m_stationLabel = m_mw.m_stationNickLabel;  // alias for existing references

    hbox->addStretch(1);

    // ── Right section ────────────────────────────────────────────────────
    // Reserve consistent width for the compact telemetry stacks so updates
    // do not cause the status bar to reshuffle as values change.
    constexpr int kTelemetryStackMinWidth = 84;

    // GPS satellites (top) + lock status (bottom) stacked
    auto* gpsStack = new QWidget;
    gpsStack->setMinimumWidth(kTelemetryStackMinWidth);
    auto* gpsVbox = new QVBoxLayout(gpsStack);
    gpsVbox->setContentsMargins(0, 0, 0, 0);
    gpsVbox->setSpacing(0);
    m_gpsLabel = new QLabel("");
    m_gpsLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    m_gpsLabel->setAlignment(Qt::AlignCenter);
    m_gpsStatusLabel = new QLabel("");
    m_gpsStatusLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    m_gpsStatusLabel->setAlignment(Qt::AlignCenter);
    gpsVbox->addWidget(m_gpsLabel);
    gpsVbox->addWidget(m_gpsStatusLabel);
    hbox->addWidget(gpsStack);

    addSep();

    // CPU (top) + Memory (bottom) stacked
    {
        auto* cpuStack = new QWidget;
        cpuStack->setMinimumWidth(kTelemetryStackMinWidth);
        auto* cpuVbox = new QVBoxLayout(cpuStack);
        cpuVbox->setContentsMargins(0, 0, 0, 0);
        cpuVbox->setSpacing(0);
        m_cpuLabel = new QLabel("CPU: \u2014");
        m_cpuLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
        m_cpuLabel->setAlignment(Qt::AlignCenter);
        m_cpuLabel->setToolTip("AetherSDR process CPU usage");
        m_memLabel = new QLabel("Mem: \u2014");
        m_memLabel->setStyleSheet("QLabel { color: #607080; font-size: 12px; }");
        m_memLabel->setAlignment(Qt::AlignCenter);
        m_memLabel->setToolTip("AetherSDR process memory (RSS)");
        cpuVbox->addWidget(m_cpuLabel);
        cpuVbox->addWidget(m_memLabel);
        hbox->addWidget(cpuStack);

        m_cpuTimer = new QTimer(&m_mw);
        m_cpuTimer->setInterval(1500);
        connect(m_cpuTimer, &QTimer::timeout, this, [this]() {
            double cpuPct = -1.0;
#ifdef Q_OS_WIN
            static FILETIME prevKernel{}, prevUser{};
            static qint64 prevWall = 0;
            FILETIME creation, exit, kernel, user;
            if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
                auto toUs = [](const FILETIME& ft) -> qint64 {
                    return (static_cast<qint64>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10;
                };
                qint64 now = QDateTime::currentMSecsSinceEpoch() * 1000;
                qint64 cpuUs = toUs(kernel) + toUs(user);
                qint64 prevCpuUs = toUs(prevKernel) + toUs(prevUser);
                if (prevWall > 0) {
                    qint64 wallDelta = now - prevWall;
                    qint64 cpuDelta = cpuUs - prevCpuUs;
                    if (wallDelta > 0)
                        cpuPct = 100.0 * cpuDelta / wallDelta / QThread::idealThreadCount();
                }
                prevKernel = kernel;
                prevUser = user;
                prevWall = now;
            }
#else
            // POSIX (Linux + macOS): getrusage
            static qint64 prevUserUs = 0, prevSysUs = 0, prevWallMs = 0;
            struct rusage ru;
            if (getrusage(RUSAGE_SELF, &ru) == 0) {
                qint64 userUs = ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec;
                qint64 sysUs  = ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec;
                qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
                if (prevWallMs > 0) {
                    qint64 wallDelta = (nowMs - prevWallMs) * 1000; // to microseconds
                    qint64 cpuDelta  = (userUs - prevUserUs) + (sysUs - prevSysUs);
                    if (wallDelta > 0)
                        cpuPct = 100.0 * cpuDelta / wallDelta / QThread::idealThreadCount();
                }
                prevUserUs = userUs;
                prevSysUs  = sysUs;
                prevWallMs = nowMs;
            }
#endif
            if (cpuPct >= 0.0) {
                QString color = "#8aa8c0";
                if (cpuPct >= 80.0) color = "#e05050";
                else if (cpuPct >= 50.0) color = "#f0c040";
                m_cpuLabel->setText(QString("CPU: %1%").arg(cpuPct, 0, 'f', 1));
                m_cpuLabel->setStyleSheet(QString("QLabel { color: %1; font-size: 12px; }").arg(color));
            }

            // Memory (RSS)
#ifdef Q_OS_WIN
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                double mb = pmc.WorkingSetSize / (1024.0 * 1024.0);
                m_memLabel->setText(QString("Mem: %1 MB").arg(static_cast<int>(mb)));
            }
#else
            // getrusage ru_maxrss is in KB on Linux, bytes on macOS
            struct rusage ruMem;
            if (getrusage(RUSAGE_SELF, &ruMem) == 0) {
#ifdef Q_OS_MAC
                double mb = ruMem.ru_maxrss / (1024.0 * 1024.0);
#else
                double mb = ruMem.ru_maxrss / 1024.0;
#endif
                m_memLabel->setText(QString("Mem: %1 MB").arg(static_cast<int>(mb)));
            }
#endif
        });
        m_cpuTimer->start();
    }

    addSep();

    // PA temp (top) + supply voltage (bottom) stacked
    auto* paStack = new QWidget;
    paStack->setMinimumWidth(kTelemetryStackMinWidth);
    auto* paVbox = new QVBoxLayout(paStack);
    paVbox->setContentsMargins(0, 0, 0, 0);
    paVbox->setSpacing(0);
    m_paTempLabel = new QLabel("");
    m_paTempLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    m_paTempLabel->setAlignment(Qt::AlignCenter);
    m_paTempLabel->setCursor(Qt::PointingHandCursor);
    m_paTempLabel->installEventFilter(&m_mw);
    updatePaTempLabel();
    m_supplyVoltLabel = new QLabel("");
    m_supplyVoltLabel->setStyleSheet("QLabel { color: #607080; font-size: 12px; }");
    m_supplyVoltLabel->setAlignment(Qt::AlignCenter);
    paVbox->addWidget(m_paTempLabel);
    paVbox->addWidget(m_supplyVoltLabel);
    hbox->addWidget(paStack);

    addSep();

    // Network label (top) + quality (bottom) stacked
    auto* netStack = new QWidget;
    netStack->setMinimumWidth(kTelemetryStackMinWidth);
    netStack->setCursor(Qt::PointingHandCursor);
    auto* netVbox = new QVBoxLayout(netStack);
    netVbox->setContentsMargins(0, 0, 0, 0);
    netVbox->setSpacing(0);
    auto* netTitle = new QLabel("Network:");
    netTitle->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    netTitle->setAlignment(Qt::AlignCenter);
    netVbox->addWidget(netTitle);
    m_networkLabel = new QLabel("");
    m_networkLabel->setStyleSheet("QLabel { color: #607080; font-size: 12px; }");
    m_networkLabel->setTextFormat(Qt::RichText);
    m_networkLabel->setAlignment(Qt::AlignCenter);
    m_networkLabel->setToolTip(buildNetworkTooltip(m_mw.m_radioModel));
    m_networkLabel->installEventFilter(&m_mw);
    netVbox->addWidget(m_networkLabel);
    hbox->addWidget(netStack);

    addSep();

    m_mw.m_tgxlIndicator = new QLabel;
    m_mw.m_tgxlIndicator->setTextFormat(Qt::RichText);
    m_mw.m_tgxlIndicator->setAlignment(Qt::AlignCenter);
    m_mw.m_tgxlIndicator->setCursor(Qt::PointingHandCursor);
    m_mw.m_tgxlIndicator->setToolTip("Tuner Genius XL — click to cycle OPERATE/BYPASS/STANDBY");
    m_mw.m_tgxlIndicator->installEventFilter(&m_mw);
    m_mw.m_tgxlIndicator->setVisible(false);
    hbox->addWidget(m_mw.m_tgxlIndicator);

    addSep();

    m_mw.m_pgxlIndicator = new QLabel;
    m_mw.m_pgxlIndicator->setTextFormat(Qt::RichText);
    m_mw.m_pgxlIndicator->setAlignment(Qt::AlignCenter);
    m_mw.m_pgxlIndicator->setCursor(Qt::PointingHandCursor);
    m_mw.m_pgxlIndicator->setToolTip("Power Genius XL — click to toggle OPERATE/STANDBY");
    m_mw.m_pgxlIndicator->installEventFilter(&m_mw);
    m_mw.m_pgxlIndicator->setVisible(false);
    hbox->addWidget(m_mw.m_pgxlIndicator);

    addSep();

    m_mw.m_txIndicator = new QLabel("TX");
    m_mw.m_txIndicator->setFixedSize(36, 36);
    m_mw.m_txIndicator->setAlignment(Qt::AlignCenter);
    m_mw.m_txIndicator->setStyleSheet("QLabel { color: rgba(255,255,255,128); font-weight: bold; font-size: 21px; }");
    hbox->addWidget(m_mw.m_txIndicator);

    addSep();

    // Grid square (top) + UTC time (bottom) stacked, right-aligned
    auto* timeStack = new QWidget;
    timeStack->setMinimumWidth(kTelemetryStackMinWidth);
    auto* timeVbox = new QVBoxLayout(timeStack);
    timeVbox->setContentsMargins(0, 0, 0, 0);
    timeVbox->setSpacing(0);
    m_gridLabel = new QLabel("");
    m_gridLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    m_gridLabel->setAlignment(Qt::AlignCenter);
    m_gridLabel->setMinimumWidth(kTelemetryStackMinWidth);
    m_gpsDateLabel = new QLabel("");
    m_gpsDateLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    m_gpsDateLabel->setAlignment(Qt::AlignCenter);
    m_gpsDateLabel->setMinimumWidth(kTelemetryStackMinWidth);
    m_gpsTimeLabel = new QLabel("");
    m_gpsTimeLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 12px; }");
    m_gpsTimeLabel->setAlignment(Qt::AlignCenter);
    m_gpsTimeLabel->setMinimumWidth(kTelemetryStackMinWidth);
    timeVbox->addWidget(m_gridLabel);
    timeVbox->addWidget(m_gpsDateLabel);
    timeVbox->addWidget(m_gpsTimeLabel);
    hbox->addWidget(timeStack);

    m_mw.statusBar()->addWidget(container, 1);
    updateBandStackIndicator();
}

void StatusBarController::updateBandStackIndicator()
{
    if (!m_bandStackIndicator || !m_mw.m_panStack)
        return;

    const bool visible = m_mw.m_panStack->bandStackPanel()->isVisible();
    m_bandStackIndicator->setPixmap(buildBandStackIndicatorPixmap(visible));
    m_bandStackIndicator->setToolTip(visible ? "Close band stack panel"
                                             : "Open band stack panel");
}

void StatusBarController::updateKeyerAvailability(const QString& mode)
{
    static const QString kActive   = "QLabel { color: #00b4d8; font-weight: bold; font-size: 24px; }";
    static const QString kAvail    = "QLabel { color: rgba(255,255,255,40); font-weight: bold; font-size: 24px; }";
    static const QString kDisabled = "QLabel { color: #252530; font-weight: bold; font-size: 24px; }";

    bool isCw  = (mode == "CW" || mode == "CWL");
    bool isSsb = (mode == "USB" || mode == "LSB" || mode == "AM" || mode == "SAM"
                  || mode == "FM" || mode == "NFM" || mode == "DFM");

    // CWX: available in CW modes only
    m_cwxIndicator->setEnabled(isCw);
    if (!isCw && m_mw.m_cwxPanel->isVisible()) {
        m_mw.m_cwxPanel->hide();
        m_cwxIndicator->setStyleSheet(kDisabled);
    } else if (m_mw.m_cwxPanel->isVisible()) {
        m_cwxIndicator->setStyleSheet(kActive);
    } else {
        m_cwxIndicator->setStyleSheet(isCw ? kAvail : kDisabled);
    }
    m_cwxIndicator->setCursor(isCw ? Qt::PointingHandCursor : Qt::ArrowCursor);

    // DVK: available in voice modes (SSB, AM, FM — not DIGU/DIGL)
    m_dvkIndicator->setEnabled(isSsb);
    if (!isSsb && m_mw.m_dvkPanel->isVisible()) {
        m_mw.m_dvkPanel->hide();
        m_dvkIndicator->setStyleSheet(kDisabled);
    } else if (m_mw.m_dvkPanel->isVisible()) {
        m_dvkIndicator->setStyleSheet(kActive);
    } else {
        m_dvkIndicator->setStyleSheet(isSsb ? kAvail : kDisabled);
    }
    m_dvkIndicator->setCursor(isSsb ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

} // namespace AetherSDR
