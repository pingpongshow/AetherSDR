#include "gui/mainwindow/DigitalModesController.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/DigitalVoiceController.h"
#include "gui/mainwindow/SpotController.h"
#include "gui/PanadapterApplet.h"
#include "core/LogManager.h"
#include "gui/HfFaxDialog.h"
#include "gui/RttyDialog.h"
#include "gui/PskDialog.h"
#include "gui/SstvDialog.h"
#include "gui/WeakSignalDialog.h"
#include "gui/WwvDialog.h"
#include "gui/Ale2gDialog.h"
#include "gui/HelpDialog.h"
#include "models/SliceModel.h"
#include <QMenuBar>
#include <QDialog>
#include <QAction>
#include <QActionGroup>
#include <QPointer>
#include "core/AppSettings.h"

namespace AetherSDR {

DigitalModesController::DigitalModesController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void DigitalModesController::closeAllTools()
{
    // Close digital-mode tools first: they unkey any transmission they own
    // in their close handlers, which must run while RadioModel and the audio
    // engine still exist (ALE/PSK/RTTY/SSTV/FT8 could otherwise leave the
    // radio keyed or touch destroyed objects during shutdown).
    const QList<QPointer<QDialog>> digitalTools{
        m_ale2gDialog.data(), m_pskDialog.data(), m_rttyDialog.data(), m_sstvDialog.data(),
        m_weakSignalDialog.data(), m_hfFaxDialog.data(), m_wwvDialog.data()};
    for (const QPointer<QDialog>& tool : digitalTools) {
        if (tool) {
            tool->close();
        }
    }
}

HfFaxDialog* DigitalModesController::showHfFaxDialog()
{
    if (m_hfFaxDialog) {
        if (auto* slice = m_mw.activeSlice())
            m_hfFaxDialog->setActiveSlice(slice);
        m_hfFaxDialog->raise();
        m_hfFaxDialog->activateWindow();
        return m_hfFaxDialog;
    }

    auto* dlg = new HfFaxDialog(&m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_hfFaxDialog = dlg;
    connect(dlg, &QDialog::finished, this, [this](int) {
        setExclusiveDigitalModeChecked(m_modeHfFaxAction, false);
    });

    if (auto* slice = m_mw.activeSlice())
        dlg->setActiveSlice(slice);

    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            dlg, &HfFaxDialog::feedAudio);

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

PskDialog* DigitalModesController::showPskDialog()
{
    if (m_pskDialog) {
        if (auto* slice = m_mw.activeSlice())
            m_pskDialog->setActiveSlice(slice);
        m_pskDialog->raise();
        m_pskDialog->activateWindow();
        return m_pskDialog;
    }

    auto* dlg = new PskDialog(&m_mw.m_radioModel, m_mw.m_audio, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_pskDialog = dlg;
    connect(dlg, &QDialog::finished, this, [this](int) {
        setExclusiveDigitalModeChecked(m_modePskAction, false);
    });

    if (auto* slice = m_mw.activeSlice())
        dlg->setActiveSlice(slice);

    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            dlg, &PskDialog::feedAudio);
    connect(dlg, &PskDialog::receptionReportReady,
            m_mw.m_spotCtl, &SpotController::submitDigitalReceptionReport);

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

RttyDialog* DigitalModesController::showRttyDialog()
{
    if (m_rttyDialog) {
        if (auto* slice = m_mw.activeSlice())
            m_rttyDialog->setActiveSlice(slice);
        m_rttyDialog->raise();
        m_rttyDialog->activateWindow();
        return m_rttyDialog;
    }

    auto* dlg = new RttyDialog(&m_mw.m_radioModel, m_mw.m_audio, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_rttyDialog = dlg;
    connect(dlg, &QDialog::finished, this, [this](int) {
        setExclusiveDigitalModeChecked(m_modeRttyAction, false);
    });

    if (auto* slice = m_mw.activeSlice())
        dlg->setActiveSlice(slice);

    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            dlg, &RttyDialog::feedAudio);
    connect(dlg, &RttyDialog::receptionReportReady,
            m_mw.m_spotCtl, &SpotController::submitDigitalReceptionReport);

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

SstvDialog* DigitalModesController::showSstvDialog()
{
    if (m_sstvDialog) {
        if (auto* slice = m_mw.activeSlice())
            m_sstvDialog->setActiveSlice(slice);
        m_sstvDialog->raise();
        m_sstvDialog->activateWindow();
        return m_sstvDialog;
    }

    auto* dlg = new SstvDialog(&m_mw.m_radioModel, m_mw.m_audio, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_sstvDialog = dlg;
    connect(dlg, &QDialog::finished, this, [this](int) {
        setExclusiveDigitalModeChecked(m_modeSstvAction, false);
    });

    if (auto* slice = m_mw.activeSlice())
        dlg->setActiveSlice(slice);

    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            dlg, &SstvDialog::feedAudio);

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

WeakSignalDialog* DigitalModesController::showWeakSignalDialog()
{
    if (m_weakSignalDialog) {
        if (auto* slice = m_mw.activeSlice())
            m_weakSignalDialog->setActiveSlice(slice);
        m_weakSignalDialog->raise();
        m_weakSignalDialog->activateWindow();
        return m_weakSignalDialog;
    }

    auto* dlg = new WeakSignalDialog(&m_mw.m_radioModel, m_mw.m_weakSignalDecoder, m_mw.m_audio, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_weakSignalDialog = dlg;
    connect(dlg, &QDialog::finished, this, [this](int) {
        m_mw.m_weakSignalDecoder->stopListening();
        setExclusiveDigitalModeChecked(m_modeWeakSignalAction, false);
    });

    if (auto* slice = m_mw.activeSlice())
        dlg->setActiveSlice(slice);

    if (!m_mw.m_weakSignalDecoder->isListening())
        m_mw.m_weakSignalDecoder->startListening();

    connect(dlg, &WeakSignalDialog::receptionReportReady,
            m_mw.m_spotCtl, &SpotController::submitDigitalReceptionReport);

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

WwvDialog* DigitalModesController::showWwvDialog()
{
    if (m_wwvDialog) {
        if (auto* slice = m_mw.activeSlice())
            m_wwvDialog->setActiveSlice(slice);
        m_wwvDialog->raise();
        m_wwvDialog->activateWindow();
        return m_wwvDialog;
    }

    auto* dlg = new WwvDialog(&m_mw.m_radioModel, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_wwvDialog = dlg;
    connect(dlg, &QDialog::finished, this, [this](int) {
        setExclusiveDigitalModeChecked(m_modeWwvAction, false);
    });

    if (auto* slice = m_mw.activeSlice())
        dlg->setActiveSlice(slice);

    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            dlg, &WwvDialog::feedAudio);

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

Ale2gDialog* DigitalModesController::showAle2gDialog()
{
    if (m_ale2gDialog) {
        if (auto* slice = m_mw.activeSlice())
            m_ale2gDialog->setActiveSlice(slice);
        m_ale2gDialog->raise();
        m_ale2gDialog->activateWindow();
        return m_ale2gDialog;
    }

    auto* dlg = new Ale2gDialog(&m_mw.m_radioModel, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_ale2gDialog = dlg;
    connect(dlg, &QDialog::finished, this, [this](int) {
        setExclusiveDigitalModeChecked(m_modeAle2gAction, false);
    });

    if (auto* slice = m_mw.activeSlice())
        dlg->setActiveSlice(slice);

    connect(m_mw.m_radioModel.panStream(), &PanadapterStream::audioDataReady,
            dlg, &Ale2gDialog::feedAudio);

    // 2G ALE transmit: PTT through the normal MOX path, modem audio through the
    // DAX TX stream (DIGU slice; updateDaxTxMode() sets dax=1 when the ALE
    // slice is the TX slice). The dialog only requests TX when its Transmit
    // master switch is on (default off, never persisted).
    connect(dlg, &Ale2gDialog::aleTransmitRequested, this, [this](bool on) {
        m_mw.m_radioModel.setTransmit(on);
    });
    connect(dlg, &Ale2gDialog::aleTxAudioReady, this, [this](const QByteArray& pcm) {
        QMetaObject::invokeMethod(m_mw.m_audio, [this, pcm]() { m_mw.m_audio->feedDaxTxAudio(pcm); });
    });
    connect(&m_mw.m_radioModel, &RadioModel::radioTransmittingChanged,
            dlg, &Ale2gDialog::setRadioTransmitting);

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

void DigitalModesController::setExclusiveDigitalModeChecked(QAction* action, bool checked)
{
    if (!action)
        return;

    const QSignalBlocker blocker(action);
    action->setChecked(checked);
}

void DigitalModesController::closeExclusiveDigitalMode(QAction* action)
{
    if (!action)
        return;

    if (action == m_mw.m_modeCwDecodePanelAction) {
        if (m_mw.m_cwDecoderApplet)
            m_mw.m_cwDecoderApplet->setCwPanelVisible(false);
        return;
    }

    if (action == m_modeRttyAction) {
        if (m_rttyDialog && m_rttyDialog->isVisible())
            m_rttyDialog->close();
        return;
    }

    if (action == m_modePskAction) {
        if (m_pskDialog && m_pskDialog->isVisible())
            m_pskDialog->close();
        return;
    }

    if (action == m_modeHfFaxAction) {
        if (m_hfFaxDialog && m_hfFaxDialog->isVisible())
            m_hfFaxDialog->close();
        return;
    }

    if (action == m_modeSstvAction) {
        if (m_sstvDialog && m_sstvDialog->isVisible())
            m_sstvDialog->close();
        return;
    }

    if (action == m_modeWeakSignalAction) {
        if (m_weakSignalDialog && m_weakSignalDialog->isVisible())
            m_weakSignalDialog->close();
        return;
    }

    if (action == m_modeWwvAction) {
        if (m_wwvDialog && m_wwvDialog->isVisible())
            m_wwvDialog->close();
        return;
    }

    if (action == m_modeAle2gAction) {
        if (m_ale2gDialog && m_ale2gDialog->isVisible())
            m_ale2gDialog->close();
    }
}

void DigitalModesController::closeExclusiveDigitalModesExcept(QAction* keepAction)
{
    QAction* actions[] = {
        m_mw.m_modeCwDecodePanelAction,
        m_modeRttyAction,
        m_modePskAction,
        m_modeHfFaxAction,
        m_modeSstvAction,
        m_modeWeakSignalAction,
        m_modeWwvAction,
        m_modeAle2gAction,
    };

    for (QAction* action : actions) {
        if (action && action != keepAction)
            closeExclusiveDigitalMode(action);
    }
}

void DigitalModesController::openExclusiveDigitalMode(QAction* action)
{
    if (!action)
        return;

    if (action == m_mw.m_modeCwDecodePanelAction) {
        auto* sl = m_mw.activeSlice();
        const bool isCw = sl && (sl->mode() == "CW" || sl->mode() == "CWL");
        if (m_mw.m_cwDecoderApplet)
            m_mw.m_cwDecoderApplet->setCwPanelVisible(isCw);

        if (isCw)
            m_mw.statusBar()->showMessage("CW decode panel enabled", 2500);
        else
            m_mw.statusBar()->showMessage(
                "CW decode panel enabled. Switch the active slice to CW/CWL to view it.", 3500);
        return;
    }

    if (action == m_modeRttyAction) {
        showRttyDialog();
        return;
    }

    if (action == m_modePskAction) {
        showPskDialog();
        return;
    }

    if (action == m_modeHfFaxAction) {
        showHfFaxDialog();
        return;
    }

    if (action == m_modeSstvAction) {
        showSstvDialog();
        return;
    }

    if (action == m_modeWeakSignalAction) {
        showWeakSignalDialog();
        return;
    }

    if (action == m_modeWwvAction) {
        showWwvDialog();
        return;
    }

    if (action == m_modeAle2gAction)
        showAle2gDialog();
}

void DigitalModesController::buildModeMenu()
{
    // ── Mode menu ──────────────────────────────────────────────────────────
    auto* modeMenu = m_mw.menuBar()->addMenu("&Mode");
    m_exclusiveDigitalModeGroup = new QActionGroup(modeMenu);
    m_exclusiveDigitalModeGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);

    m_mw.m_modeCwDecodePanelAction = modeMenu->addAction("CW");
    m_mw.m_modeCwDecodePanelAction->setCheckable(true);
    m_mw.m_modeCwDecodePanelAction->setChecked(
        AppSettings::instance().value("CwDecodeOverlay", "True").toString() == "True");
    connect(m_mw.m_modeCwDecodePanelAction, &QAction::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("CwDecodeOverlay", on ? "True" : "False");
        s.save();
        if (on) {
            closeExclusiveDigitalModesExcept(m_mw.m_modeCwDecodePanelAction);
            openExclusiveDigitalMode(m_mw.m_modeCwDecodePanelAction);
        } else {
            closeExclusiveDigitalMode(m_mw.m_modeCwDecodePanelAction);
            m_mw.statusBar()->showMessage("CW decode panel hidden", 2500);
        }
    });
    m_exclusiveDigitalModeGroup->addAction(m_mw.m_modeCwDecodePanelAction);

    auto registerExclusiveDigitalModeAction = [this](QAction* action) {
        action->setCheckable(true);
        m_exclusiveDigitalModeGroup->addAction(action);
        connect(action, &QAction::toggled, this, [this, action](bool on) {
            if (on) {
                closeExclusiveDigitalModesExcept(action);
                openExclusiveDigitalMode(action);
            } else {
                closeExclusiveDigitalMode(action);
            }
        });
    };

    m_modeRttyAction = modeMenu->addAction("RTTY");
    registerExclusiveDigitalModeAction(m_modeRttyAction);

    m_modePskAction = modeMenu->addAction("PSK");
    registerExclusiveDigitalModeAction(m_modePskAction);

    m_modeHfFaxAction = modeMenu->addAction("HF-Fax / Weather Fax");
    registerExclusiveDigitalModeAction(m_modeHfFaxAction);

    m_modeSstvAction = modeMenu->addAction("SSTV");
    registerExclusiveDigitalModeAction(m_modeSstvAction);

    m_modeWeakSignalAction = modeMenu->addAction("FT8 / FT4 / JT65 / WSPR");
    registerExclusiveDigitalModeAction(m_modeWeakSignalAction);

    m_modeWwvAction = modeMenu->addAction("Time Stations");
    registerExclusiveDigitalModeAction(m_modeWwvAction);

    m_modeAle2gAction = modeMenu->addAction("2G ALE");
    registerExclusiveDigitalModeAction(m_modeAle2gAction);

    auto* freeDvDialogAct = modeMenu->addAction("FreeDV / Digital Voice");
    connect(freeDvDialogAct, &QAction::triggered, this, [this] {
        m_mw.m_digitalVoiceCtl->showDigitalVoiceDialog();
    });
#ifdef HAVE_WEBSOCKETS
    auto* freeDvSpotsAct = modeMenu->addAction("FreeDV Reporter spots");
    connect(freeDvSpotsAct, &QAction::triggered, this, [this] {
        m_mw.m_spotCtl->showSpotHubDialog("FreeDV");
    });
    m_mw.m_modeFreeDvReporterAction = modeMenu->addAction("FreeDV Reporter");
    m_mw.m_modeFreeDvReporterAction->setCheckable(true);
    m_mw.m_modeFreeDvReporterAction->setChecked(false);
    connect(m_mw.m_modeFreeDvReporterAction, &QAction::toggled, this, [this](bool on) {
        m_mw.m_spotCtl->setFreeDvReporterEnabled(on);
    });
#endif

    modeMenu->addSeparator();
    auto* dataModesAct = modeMenu->addAction("RTTY / PSK / Olivia / JS8 / fldigi...");
    connect(dataModesAct, &QAction::triggered, this, [this]() {
        auto* dlg = new HelpDialog("Configuring Data Modes", ":/help/understanding-data-modes.md", &m_mw);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
}

void DigitalModesController::setActiveSliceOnTools(SliceModel* s)
{
    if (m_hfFaxDialog)
        m_hfFaxDialog->setActiveSlice(s);
    if (m_pskDialog)
        m_pskDialog->setActiveSlice(s);
    if (m_rttyDialog)
        m_rttyDialog->setActiveSlice(s);
    if (m_sstvDialog)
        m_sstvDialog->setActiveSlice(s);
    if (m_weakSignalDialog)
        m_weakSignalDialog->setActiveSlice(s);
}

} // namespace AetherSDR
