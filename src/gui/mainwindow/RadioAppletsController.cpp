#include "gui/mainwindow/RadioAppletsController.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/DaxController.h"
#include "gui/mainwindow/DigitalVoiceController.h"
#include "gui/mainwindow/ExternalControlsController.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "gui/SpectrumWidget.h"
#include "gui/AppletPanel.h"
#include "gui/RxApplet.h"
#include "gui/SMeterWidget.h"
#include "gui/TunerApplet.h"
#include "gui/TxApplet.h"
#include "gui/PhoneCwApplet.h"
#include "gui/PhoneApplet.h"
#include "gui/EqApplet.h"
#include "gui/AmpApplet.h"
#include "gui/MeterApplet.h"
#include "models/SliceModel.h"
#include "models/MeterModel.h"
#include "models/TunerModel.h"
#include "models/TransmitModel.h"
#include "models/EqualizerModel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <QLabel>
#include "core/AppSettings.h"

namespace AetherSDR {

RadioAppletsController::RadioAppletsController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void RadioAppletsController::wireTransmitState()
{
    // TX/RX transition → waterfall tile source switching
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            this, [this](bool tx) {
        // Set transmitting on ALL spectrums (multi-pan aware).
        // Each spectrum's m_hasTxSlice flag determines whether it freezes.
        for (auto* pan : m_mw.m_radioModel.panadapters()) {
            if (auto* sw = m_mw.m_panStack ? m_mw.m_panStack->spectrum(pan->panId()) : nullptr)
                sw->setTransmitting(tx);
        }
        if (!m_mw.m_panStack && m_mw.m_panApplet)
            m_mw.m_panApplet->spectrumWidget()->setTransmitting(tx);
        // Keep TX audio source strictly aligned with the local MOX edge for all
        // modes (SSB + DAX). Waiting for interlock introduces audible lag.
        m_mw.m_audio->setTransmitting(tx);  // atomic flag now; buffer flush hops to audio thread
#if defined(HAVE_DAX_BRIDGE)
        m_mw.m_daxCtl->setBridgeTransmitting(tx);
#endif

        // TX indicator is driven by radioTransmittingChanged (see below) so that
        // it lights up for external PTT and Multi-Flex TX, not just local MOX.
        // On RX resume, native tiles will restart and m_hasNativeWaterfall
        // will be set again by the first arriving tile.
#ifdef HAVE_DIGITAL_VOICE
        m_mw.m_digitalVoiceCtl->updateForMox(tx);
#endif
#ifdef HAVE_SERIALPORT
        m_mw.m_extCtl->setSerialTransmitting(tx);
#endif
    });

    // Interlock fallback gate:
    // we only consume TX-off here, as a safety net if local edge updates
    // are missed while interlock transitions.
    connect(&m_mw.m_radioModel, &RadioModel::txAudioGateChanged,
            this, [this](bool tx) {
        if (!tx) {
            m_mw.m_audio->setTransmitting(false);
#if defined(HAVE_DAX_BRIDGE)
            m_mw.m_daxCtl->setBridgeTransmitting(false);
#endif
        }
    });

    // Raw radio TX state: fired for every interlock state=TRANSMITTING regardless
    // of TX ownership. Used for DAX passthrough (#752) and the TX status bar
    // indicator — moxChanged is ownership-gated so it misses external PTT and
    // Multi-Flex TX from other clients.
    connect(&m_mw.m_radioModel, &RadioModel::radioTransmittingChanged,
            this, [this](bool tx) {
        m_mw.m_audio->setRadioTransmitting(tx);
        // S-Meter: use raw interlock state so Level/Compression modes work
        // during VOX/hardware CW without the effectiveTx power threshold (#877)
        m_mw.m_appletPanel->sMeterWidget()->setTransmitting(tx);
        if (tx) {
            m_mw.m_txIndicator->setStyleSheet(
                "QLabel { color: white; background: #c03030; font-weight: bold; "
                "font-size: 21px; border-radius: 4px; padding: 0px 1px; }");
        } else {
            m_mw.m_txIndicator->setStyleSheet(
                "QLabel { color: rgba(255,255,255,128); font-weight: bold; "
                "font-size: 21px; }");
        }
    });

    // Sync show-TX-in-waterfall setting to all spectrum widgets
    auto syncShowTxWf = [this]() {
        bool show = m_mw.m_radioModel.transmitModel().showTxInWaterfall();
        for (auto* pan : m_mw.m_radioModel.panadapters()) {
            if (auto* sw = m_mw.m_panStack ? m_mw.m_panStack->spectrum(pan->panId()) : nullptr)
                sw->setShowTxInWaterfall(show);
        }
        if (!m_mw.m_panStack && m_mw.m_panApplet)
            m_mw.m_panApplet->spectrumWidget()->setShowTxInWaterfall(show);
    };
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::stateChanged,
            this, syncShowTxWf);
}

void RadioAppletsController::wireMetersAndAmplifiers()
{
    // ── S-Meter: MeterModel → SMeterWidget (active slice only) ─────────────
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::sLevelChanged,
            this, [this](int sliceIndex, float dbm) {
        if (sliceIndex == m_mw.m_activeSliceId)
            m_mw.m_appletPanel->sMeterWidget()->setLevel(dbm);
    });
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            m_mw.m_appletPanel->sMeterWidget(), &SMeterWidget::setTxMeters);
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::micMetersChanged,
            m_mw.m_appletPanel->sMeterWidget(), &SMeterWidget::setMicMeters);
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_mw.m_appletPanel->sMeterWidget(), &SMeterWidget::setTransmitting);

    // ── Tuner: MeterModel TX meters → TunerApplet gauges ────────────────
    // Use TGXL-specific meters when available (disambiguated from PGXL by handle)
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::tgxlMetersChanged,
            m_mw.m_appletPanel->tunerApplet(), &TunerApplet::updateMeters);
    // Note: txMetersChanged NOT connected to TunerApplet — exciter power
    // would overwrite TGXL readings. TGXL meters come from TunerModel
    // via the direct TCP connection (port 9010). (#625)
    m_mw.m_appletPanel->tunerApplet()->setTunerModel(&m_mw.m_radioModel.tunerModel());
    m_mw.m_appletPanel->tunerApplet()->setMeterModel(&m_mw.m_radioModel.meterModel());

    // Show/hide TUNE button + applet based on TGXL presence
    connect(&m_mw.m_radioModel.tunerModel(), &TunerModel::presenceChanged,
            m_mw.m_appletPanel, &AppletPanel::setTunerVisible);
    connect(&m_mw.m_radioModel.tunerModel(), &TunerModel::presenceChanged,
            this, [this](bool present) {
        m_mw.m_tgxlIndicator->setVisible(present);
        // Auto-connect/disconnect direct TGXL connection for manual relay control (#469)
        if (present) {
            QString ip = m_mw.m_radioModel.tunerModel().tgxlIp();
            if (!ip.isEmpty() && !m_mw.m_tgxlConn.isConnected()) {
                m_mw.m_tgxlConn.connectToTgxl(ip);
            }
        } else {
            m_mw.m_tgxlConn.disconnect();
        }
    });
    // Wire TgxlConnection to TunerModel
    m_mw.m_radioModel.tunerModel().setDirectConnection(&m_mw.m_tgxlConn);
    // Also attempt connection when TGXL IP arrives (may come after presence)
    connect(&m_mw.m_radioModel.tunerModel(), &TunerModel::stateChanged, this, [this]() {
        auto* tuner = &m_mw.m_radioModel.tunerModel();
        if (tuner->isPresent() && !tuner->tgxlIp().isEmpty() && !m_mw.m_tgxlConn.isConnected()) {
            m_mw.m_tgxlConn.connectToTgxl(tuner->tgxlIp());
        }
    });

    // Auto-connect to PGXL when detected
    connect(&m_mw.m_radioModel, &RadioModel::amplifierChanged, this, [this](bool present) {
        if (present && !m_mw.m_radioModel.ampIp().isEmpty() && !m_mw.m_pgxlConn.isConnected()) {
            m_mw.m_pgxlConn.connectToPgxl(m_mw.m_radioModel.ampIp());
        } else if (!present) {
            m_mw.m_pgxlConn.disconnect();
        }
    });
    // PGXL status → AmpApplet (direct telemetry: vac, id, temp, state, etc.)
    connect(&m_mw.m_pgxlConn, &PgxlConnection::statusUpdated, this, [this](const QMap<QString, QString>& kvs) {
        qCDebug(lcTuner) << "PGXL status:" << kvs;
        auto* amp = m_mw.m_appletPanel->ampApplet();
        if (kvs.contains("temp"))
            amp->setTemp(kvs["temp"].toFloat());
        if (kvs.contains("id"))
            amp->setDrainCurrent(kvs["id"].toFloat());
        if (kvs.contains("vac"))
            amp->setMainsVoltage(kvs["vac"].toInt());
        if (kvs.contains("state"))
            amp->setState(kvs["state"]);
        if (kvs.contains("meffa"))
            amp->setMeff(kvs["meffa"]);
        // Convert PGXL dBm to watts and feed S-Meter alongside radio meters.
        // Use peakfwd (actual peak power) not fwd (floor/minimum).
        // Skip when amp is STANDBY — peakfwd reads ~0 dBm in standby and would
        // stomp on the exciter feed that should drive the barefoot scale.
        if (kvs.contains("peakfwd") && m_mw.m_radioModel.hasAmplifier()
                && m_mw.m_radioModel.ampOperate()) {
            float dbm = kvs["peakfwd"].toFloat();
            float watts = std::pow(10.0f, (dbm - 30.0f) / 10.0f);
            qCDebug(lcTuner) << "PGXL→SMeter: peakfwd=" << dbm << "dBm =" << watts << "W";
            float swr = 1.0f;
            if (kvs.contains("swr")) {
                float rl = std::abs(kvs["swr"].toFloat());
                float rho = std::pow(10.0f, -rl / 20.0f);
                swr = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.0f;
            }
            // Ensure S-Meter is in TX mode when PGXL reports transmitting
            if (kvs.value("state").startsWith("TRANSMIT"))
                m_mw.m_appletPanel->sMeterWidget()->setTransmitting(true);
            else if (kvs.contains("state") && !kvs.value("state").startsWith("TRANSMIT"))
                m_mw.m_appletPanel->sMeterWidget()->setTransmitting(false);
            m_mw.m_appletPanel->sMeterWidget()->setTxMeters(watts, swr);
        }
    });
    connect(&m_mw.m_pgxlConn, &PgxlConnection::connected, this, [this]() {
        qCDebug(lcGui) << "PGXL direct connection established, version:" << m_mw.m_pgxlConn.version();
    });
    // OPERATE button → PGXL standby/operate command via radio amplifier API
    connect(m_mw.m_appletPanel->ampApplet(), &AmpApplet::operateToggled, this, [this](bool on) {
        if (!m_mw.m_radioModel.ampHandle().isEmpty())
            m_mw.m_radioModel.sendCommand(
                QString("amplifier set %1 operate=%2").arg(m_mw.m_radioModel.ampHandle()).arg(on ? 1 : 0));
    });

    // Switch Fwd Power gauge scale based on radio max power and amplifier presence.
    // All three power gauges (TxApplet, TunerApplet, SMeterWidget) update together.
    // When the PGXL is in STANDBY we fall back to the barefoot scale — only the
    // radio's forward power is reaching the meter, so the 2kW arc would make
    // every reading look tiny and useless.
    auto updatePowerScale = [this]() {
        int maxW = m_mw.m_radioModel.transmitModel().maxPowerLevel();
        // Aurora (AU-) radios have an integrated 600W PA (Overlord) but
        // max_power_level only reports the exciter limit (100W). Use model
        // name to detect the true PA capability. (#484)
        const QString& model = m_mw.m_radioModel.model();
        if (model.startsWith("AU-") && maxW <= 100) {
            maxW = 500;
        }
        const bool ampActive = m_mw.m_radioModel.hasAmplifier()
                            && m_mw.m_radioModel.ampOperate();
        m_mw.m_appletPanel->txApplet()->setPowerScale(maxW, ampActive);
        m_mw.m_appletPanel->tunerApplet()->setPowerScale(maxW, ampActive);
        m_mw.m_appletPanel->sMeterWidget()->setPowerScale(maxW, ampActive);
    };
    connect(&m_mw.m_radioModel, &RadioModel::amplifierChanged, this, updatePowerScale);
    connect(&m_mw.m_radioModel, &RadioModel::ampStateChanged, this, updatePowerScale);

    // TGXL indicator: two-line rich text — label on top, state smaller below.
    // Green = OPERATE, amber = BYPASS, grey = STANDBY (matches SmartSDR)
    auto setIndicatorHtml = [](QLabel* lbl, const QString& name,
                               const QString& state, const QString& color) {
        lbl->setText(QString("<span style='color:%1; font-size:18px; font-weight:bold;'>%2</span><br>"
                             "<span style='color:%1; font-size:11px;'>%3</span>")
                     .arg(color, name, state));
    };

    auto updateTgxlStyle = [this, setIndicatorHtml]() {
        auto& t = m_mw.m_radioModel.tunerModel();
        if (t.isOperate() && !t.isBypass())
            setIndicatorHtml(m_mw.m_tgxlIndicator, "TUN", "OPERATE", "#00e060");
        else if (t.isOperate() && t.isBypass())
            setIndicatorHtml(m_mw.m_tgxlIndicator, "TUN", "BYPASS", "#e0a000");
        else
            setIndicatorHtml(m_mw.m_tgxlIndicator, "TUN", "STANDBY", "#404858");
    };
    connect(&m_mw.m_radioModel.tunerModel(), &TunerModel::stateChanged, this, updateTgxlStyle);

    // PGXL indicator: OPERATE (green) or STANDBY (grey) — no bypass for PGXL
    auto updatePgxlStyle = [this, setIndicatorHtml]() {
        if (m_mw.m_radioModel.ampOperate())
            setIndicatorHtml(m_mw.m_pgxlIndicator, "AMP", "OPERATE", "#00e060");
        else
            setIndicatorHtml(m_mw.m_pgxlIndicator, "AMP", "STANDBY", "#404858");
    };
    connect(&m_mw.m_radioModel, &RadioModel::ampStateChanged, this, updatePgxlStyle);

    connect(&m_mw.m_radioModel, &RadioModel::amplifierChanged, this, [this, updatePgxlStyle](bool present) {
        m_mw.m_pgxlIndicator->setVisible(present);
        m_mw.m_appletPanel->setAmpVisible(present);
        if (present) updatePgxlStyle();
    });
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::ampMetersChanged,
            this, [this](float fwdPwr, float swr, float temp) {
        m_mw.m_appletPanel->ampApplet()->setFwdPower(fwdPwr);
        m_mw.m_appletPanel->ampApplet()->setSwr(swr);
        m_mw.m_appletPanel->ampApplet()->setTemp(temp);
        // S-Meter TX power follows the scale: amp output when PGXL is OPERATE,
        // exciter output when it's STANDBY (txMetersChanged already handles that
        // path, so we just stop overriding it here).
        if (m_mw.m_radioModel.hasAmplifier() && m_mw.m_radioModel.ampOperate()) {
            m_mw.m_appletPanel->sMeterWidget()->setTxMeters(fwdPwr, swr);
            static int ampDbg = 0;
            if (++ampDbg % 50 == 1)
                qCDebug(lcTuner) << "AMP→SMeter: fwd=" << fwdPwr << "W swr=" << swr;
        }
    });
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::maxPowerLevelChanged,
            this, updatePowerScale);

    // ── Meter applet: all meters consolidated ──────────────────────────────
    m_mw.m_appletPanel->meterApplet()->setMeterModel(&m_mw.m_radioModel.meterModel());

    // ── TX applet: meters + model ───────────────────────────────────────────
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::txMetersChanged,
            m_mw.m_appletPanel->txApplet(), &TxApplet::updateMeters);
    m_mw.m_appletPanel->txApplet()->setTransmitModel(&m_mw.m_radioModel.transmitModel());
    m_mw.m_appletPanel->txApplet()->setTunerModel(&m_mw.m_radioModel.tunerModel());
    m_mw.m_appletPanel->rxApplet()->setTransmitModel(&m_mw.m_radioModel.transmitModel());

    // Hide APD row on radios that don't support it
    connect(&m_mw.m_radioModel.transmitModel(), &TransmitModel::apdStateChanged, this, [this]() {
        m_mw.m_appletPanel->txApplet()->setApdVisible(
            m_mw.m_radioModel.transmitModel().apdConfigurable());
    });
}

void RadioAppletsController::wireTxApplets()
{
    // ── P/CW applet: mic meters + ALC meter + model ────────────────────────
    // Suppress radio CODEC meters when mic_selection=PC (they just show noise).
    // Client-side metering handles PC mic display below.
    // Compression gauge: full 20fps meter rate, gated on speech_processor_enable.
    {
        connect(&m_mw.m_radioModel.meterModel(), &MeterModel::micMetersChanged,
                this, [this](float micLevel, float compLevel, float micPeak, float compPeak) {
            // Mic level: hardware mic uses radio meters, PC uses client-side
            if (m_mw.m_radioModel.transmitModel().micSelection() != "PC")
                m_mw.m_appletPanel->phoneCwApplet()->updateMeters(micLevel, compLevel, micPeak, 0.0f);

            // Compression gauge: full rate (20fps from radio), gated on PROC
            {
                float comp = m_mw.m_radioModel.transmitModel().speechProcessorEnable() ? compPeak : 0.0f;
                m_mw.m_appletPanel->phoneCwApplet()->updateCompression(comp);
            }
        });
    }
    connect(&m_mw.m_radioModel.meterModel(), &MeterModel::alcChanged,
            m_mw.m_appletPanel->phoneCwApplet(), &PhoneCwApplet::updateAlc);
    // Client-side PC mic metering — radio CODEC meters only see hardware mics.
    // Apply VU-style ballistics: fast attack, slow decay (~20 dB/sec).
    {
        auto* heldLevel = new float(-150.0f);  // persists across calls
        auto* heldPeak  = new float(-150.0f);
        connect(m_mw.m_audio, &AudioEngine::pcMicLevelChanged,
                this, [this, heldLevel, heldPeak](float peakDb, float avgDb) {
            if (m_mw.m_radioModel.transmitModel().micSelection() != "PC") return;
            constexpr float kDecayPerUpdate = 1.0f;  // ~20 dB/sec at 20 updates/sec
            // Level: fast attack, slow decay
            if (avgDb > *heldLevel)
                *heldLevel = avgDb;
            else
                *heldLevel = qMax(avgDb, *heldLevel - kDecayPerUpdate);
            // Peak: fast attack, slower decay
            if (peakDb > *heldPeak)
                *heldPeak = peakDb;
            else
                *heldPeak = qMax(*heldLevel, *heldPeak - kDecayPerUpdate * 0.5f);
            m_mw.m_appletPanel->phoneCwApplet()->updateMeters(*heldLevel, 0.0f, *heldPeak, 0.0f);
        });
    }
    m_mw.m_appletPanel->phoneCwApplet()->setTransmitModel(&m_mw.m_radioModel.transmitModel());

    // ── PHNE applet: VOX + CW controls ──────────────────────────────────────
    m_mw.m_appletPanel->phoneApplet()->setTransmitModel(&m_mw.m_radioModel.transmitModel());

    // ── EQ applet: graphic equalizer ─────────────────────────────────────────
    m_mw.m_appletPanel->eqApplet()->setEqualizerModel(&m_mw.m_radioModel.equalizerModel());
}

} // namespace AetherSDR
