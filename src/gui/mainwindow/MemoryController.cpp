#include "gui/mainwindow/MemoryController.h"

#include "core/AudioEngine.h"
#include "gui/ScannerPanel.h"

#include "gui/MainWindow.h"
#include "gui/mainwindow/SliceController.h"
#include "gui/PanadapterApplet.h"
#include "gui/PanadapterStack.h"
#include "core/LogManager.h"
#include "models/PanadapterModel.h"
#include "gui/SpectrumWidget.h"
#include "gui/SpectrumOverlayMenu.h"
#include "gui/MemoryCommands.h"
#include "gui/MemoryDialog.h"
#include "models/SliceModel.h"
#include "models/BandPlanManager.h"
#include "core/BandStackSettings.h"
#include "gui/BandStackPanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include "core/AppSettings.h"
#include <QPushButton>

namespace AetherSDR {

static constexpr int kMemorySpotIdBase = 1000000;

static int memorySpotId(int memoryIndex)
{
    return -(kMemorySpotIdBase + memoryIndex);
}

int MemoryController::memoryIndexFromSpotId(int spotIndex)
{
    if (spotIndex > -kMemorySpotIdBase)
        return -1;
    return -spotIndex - kMemorySpotIdBase;
}

static QString memorySpotLabel(const MemoryEntry& memory)
{
    if (!memory.name.trimmed().isEmpty())
        return memory.name.trimmed();
    if (!memory.group.trimmed().isEmpty())
        return memory.group.trimmed();
    return QString("Memory %1").arg(memory.index);
}

static QString memorySpotComment(const MemoryEntry& memory)
{
    QStringList parts;
    if (!memory.group.trimmed().isEmpty())
        parts << QString("Group: %1").arg(memory.group.trimmed());
    if (!memory.owner.trimmed().isEmpty())
        parts << QString("Owner: %1").arg(memory.owner.trimmed());
    if (!memory.mode.trimmed().isEmpty())
        parts << QString("Mode: %1").arg(memory.mode.trimmed());
    if (memory.rxFilterLow != 0 || memory.rxFilterHigh != 0) {
        parts << QString("Filter: %1..%2 Hz")
                    .arg(memory.rxFilterLow)
                    .arg(memory.rxFilterHigh);
    }
    return parts.join(" | ");
}

MemoryController::MemoryController(MainWindow& mw)
    : QObject(&mw)
    , m_mw(mw)
{
}

void MemoryController::showMemoryDialog()
{
    if (m_mw.m_memoryDialog) {
        m_mw.m_memoryDialog->show();
        m_mw.m_memoryDialog->raise();
        m_mw.m_memoryDialog->activateWindow();
        return;
    }

    auto* dlg = new MemoryDialog(&m_mw.m_radioModel, &m_mw);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QObject::destroyed, this, [this] {
        if (m_mw.m_shuttingDown)
            return;
        auto& s = AppSettings::instance();
        s.setValue("MemoryDialogOpen", "False");
        s.save();
    });
    m_mw.m_memoryDialog = dlg;
    dlg->show();
}

SliceModel* MemoryController::preferredMemorySlice(const QString& preferredPanId) const
{
    if (preferredPanId.isEmpty())
        return m_mw.activeSlice();

    if (auto* slice = m_mw.activeSlice(); slice && slice->panId() == preferredPanId)
        return slice;

    for (auto* slice : m_mw.m_radioModel.slices()) {
        if (slice && slice->panId() == preferredPanId)
            return slice;
    }

    return nullptr;
}

void MemoryController::showQuickAddMemoryDialog(const QString& preferredPanId)
{
    auto* slice = preferredMemorySlice(preferredPanId);
    if (!slice) {
        m_mw.statusBar()->showMessage(
            preferredPanId.isEmpty()
                ? "Open a slice before saving a memory."
                : "Open a slice on this pan before saving a memory.",
            3000);
        return;
    }

    const int sliceId = slice->sliceId();
    const QString frequencyText = QString::number(slice->frequency(), 'f', 6);
    const QString summaryText = QString("%1  |  Filter %2 to %3 Hz")
        .arg(slice->mode())
        .arg(slice->filterLow())
        .arg(slice->filterHigh());

    QDialog dialog(&m_mw);
    dialog.setWindowTitle("Save Memory");
    dialog.setModal(true);
    dialog.setMinimumWidth(360);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* nameLabel = new QLabel("Name:", &dialog);
    root->addWidget(nameLabel);

    auto* nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText("Enter a memory name");
    root->addWidget(nameEdit);

    auto* freqLabel = new QLabel(QString("Current Frequency: %1 MHz").arg(frequencyText), &dialog);
    freqLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 12px; }");
    root->addWidget(freqLabel);

    auto* summaryLabel = new QLabel(summaryText, &dialog);
    summaryLabel->setStyleSheet("QLabel { color: #70879b; font-size: 11px; }");
    root->addWidget(summaryLabel);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);

    auto* cancelButton = new QPushButton("Cancel", &dialog);
    cancelButton->setAutoDefault(false);
    buttonRow->addWidget(cancelButton);

    auto* saveButton = new QPushButton("Save", &dialog);
    saveButton->setDefault(true);
    saveButton->setEnabled(false);
    buttonRow->addWidget(saveButton);
    root->addLayout(buttonRow);

    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(nameEdit, &QLineEdit::textChanged, &dialog, [saveButton](const QString& text) {
        saveButton->setEnabled(!text.trimmed().isEmpty());
    });

    const QPointer<QDialog> dialogGuard(&dialog);
    connect(saveButton, &QPushButton::clicked, &dialog, [this,
                                                         sliceId,
                                                         dialogGuard,
                                                         nameEdit,
                                                         saveButton,
                                                         cancelButton]() {
        auto* currentSlice = m_mw.m_radioModel.slice(sliceId);
        if (!currentSlice) {
            QMessageBox::warning(&m_mw, "Save Memory",
                                 "That slice is no longer available.");
            return;
        }

        saveButton->setEnabled(false);
        cancelButton->setEnabled(false);
        nameEdit->setEnabled(false);

        const QString name = nameEdit->text().trimmed();
        createMemoryFromSlice(&m_mw.m_radioModel, currentSlice, name, dialogGuard.data(),
            [this, dialogGuard, nameEdit, saveButton, cancelButton, name](int code, const QString& body, int) {
            if (!dialogGuard)
                return;

            if (code == 0) {
                m_mw.statusBar()->showMessage(
                    QString("Saved \"%1\" to memories.").arg(name),
                    3000);
                dialogGuard->accept();
                return;
            }

            QMessageBox::warning(dialogGuard, "Save Memory",
                                 body.isEmpty()
                                     ? "Couldn't save the current slice to memory."
                                     : body);
            nameEdit->setEnabled(true);
            saveButton->setEnabled(!nameEdit->text().trimmed().isEmpty());
            cancelButton->setEnabled(true);
            nameEdit->setFocus(Qt::OtherFocusReason);
            nameEdit->selectAll();
        });
    });

    nameEdit->setFocus(Qt::OtherFocusReason);
    dialog.exec();
}

void MemoryController::wireBandStackPanel()
{
    // Band stack panel signal wiring
    connect(m_mw.m_panStack->bandStackPanel(), &BandStackPanel::addRequested, this, [this]() {
        auto* slice = m_mw.activeSlice();
        if (!slice) return;
        BandStackEntry entry;
        entry.frequencyMhz = slice->frequency();
        entry.mode = slice->mode();
        entry.filterLow = slice->filterLow();
        entry.filterHigh = slice->filterHigh();
        entry.rxAntenna = slice->rxAntenna();
        entry.txAntenna = slice->txAntenna();
        entry.agcMode = slice->agcMode();
        entry.agcThreshold = slice->agcThreshold();
        entry.audioGain = static_cast<int>(slice->audioGain());
        entry.nbOn = slice->nbOn();
        entry.nbLevel = slice->nbLevel();
        entry.nrOn = slice->nrOn();
        entry.nrLevel = slice->nrLevel();
        if (auto* pan = m_mw.m_radioModel.activePanadapter()) {
            entry.wnbOn = pan->wnbActive();
            entry.wnbLevel = pan->wnbLevel();
        }

        QColor color = BandStackPanel::colorForFrequency(entry.frequencyMhz, m_mw.m_bandPlanMgr);
        BandStackSettings::instance().addEntry(m_mw.m_radioModel.serial(), entry);
        BandStackSettings::instance().save();
        m_mw.m_panStack->bandStackPanel()->addBookmark(entry, color);
    });
    connect(m_mw.m_panStack->bandStackPanel(), &BandStackPanel::recallRequested, this,
            [this](const BandStackEntry& e) {
        auto* slice = m_mw.activeSlice();
        if (!slice) return;
        int id = slice->sliceId();

        // Mode first (affects filter ranges)
        if (slice->mode() != e.mode) {
            slice->setMode(e.mode);
        }
        // Frequency + recenter pan
        slice->tuneAndRecenter(e.frequencyMhz);
        // Filter
        if (e.filterLow != 0 || e.filterHigh != 0) {
            slice->setFilterWidth(e.filterLow, e.filterHigh);
        }
        // Antennas
        if (!e.rxAntenna.isEmpty() && e.rxAntenna != slice->rxAntenna()) {
            m_mw.m_radioModel.sendCommand(QString("slice set %1 rxant=%2").arg(id).arg(e.rxAntenna));
        }
        if (!e.txAntenna.isEmpty() && e.txAntenna != slice->txAntenna()) {
            m_mw.m_radioModel.sendCommand(QString("slice set %1 txant=%2").arg(id).arg(e.txAntenna));
        }
        // AGC
        if (!e.agcMode.isEmpty() && e.agcMode != slice->agcMode()) {
            m_mw.m_radioModel.sendCommand(QString("slice set %1 agc_mode=%2").arg(id).arg(e.agcMode));
        }
        if (e.agcThreshold != slice->agcThreshold()) {
            m_mw.m_radioModel.sendCommand(QString("slice set %1 agc_threshold=%2").arg(id).arg(e.agcThreshold));
        }
        // Volume
        if (static_cast<int>(slice->audioGain()) != e.audioGain) {
            slice->setAudioGain(static_cast<float>(e.audioGain));
        }
        // NB
        if (e.nbOn != slice->nbOn()) {
            m_mw.m_radioModel.sendCommand(QString("slice set %1 nb=%2").arg(id).arg(e.nbOn ? 1 : 0));
        }
        if (e.nbLevel != slice->nbLevel()) {
            m_mw.m_radioModel.sendCommand(QString("slice set %1 nb_level=%2").arg(id).arg(e.nbLevel));
        }
        // NR
        if (e.nrOn != slice->nrOn()) {
            m_mw.m_radioModel.sendCommand(QString("slice set %1 nr=%2").arg(id).arg(e.nrOn ? 1 : 0));
        }
        if (e.nrLevel != slice->nrLevel()) {
            m_mw.m_radioModel.sendCommand(QString("slice set %1 nr_level=%2").arg(id).arg(e.nrLevel));
        }
        // WNB (panadapter-level, not slice)
        if (auto* pan = m_mw.m_radioModel.activePanadapter()) {
            if (e.wnbOn != pan->wnbActive()) {
                m_mw.m_radioModel.sendCommand(
                    QString("display pan set %1 wnb=%2").arg(pan->panId()).arg(e.wnbOn ? 1 : 0));
            }
            if (e.wnbLevel != pan->wnbLevel()) {
                m_mw.m_radioModel.sendCommand(
                    QString("display pan set %1 wnb_level=%2").arg(pan->panId()).arg(e.wnbLevel));
            }
        }
    });
    connect(m_mw.m_panStack->bandStackPanel(), &BandStackPanel::removeRequested, this,
            [this](int index) {
        BandStackSettings::instance().removeEntry(m_mw.m_radioModel.serial(), index);
        BandStackSettings::instance().save();
        m_mw.m_panStack->bandStackPanel()->removeBookmark(index);
    });
}

void MemoryController::syncMemorySpot(int memoryIndex)
{
    auto it = m_mw.m_radioModel.memories().constFind(memoryIndex);
    if (it == m_mw.m_radioModel.memories().constEnd()) {
        removeMemorySpot(memoryIndex);
        return;
    }

    const MemoryEntry& memory = it.value();
    if (memory.freq <= 0.0) {
        removeMemorySpot(memoryIndex);
        return;
    }

    QMap<QString, QString> kvs;
    kvs["callsign"] = memorySpotLabel(memory).replace(' ', QChar(0x7f));
    kvs["rx_freq"] = QString::number(memory.freq, 'f', 6);
    kvs["tx_freq"] = QString::number(memory.freq, 'f', 6);
    kvs["source"] = "Memory";
    kvs["mode"] = memory.mode;
    kvs["color"] = "#FFFFC857";
    const QString comment = memorySpotComment(memory);
    if (!comment.isEmpty())
        kvs["comment"] = QString(comment).replace(' ', QChar(0x7f));

    m_mw.m_radioModel.spotModel().applySpotStatus(memorySpotId(memoryIndex), kvs);
}

void MemoryController::removeMemorySpot(int memoryIndex)
{
    m_mw.m_radioModel.spotModel().removeSpot(memorySpotId(memoryIndex));
}

void MemoryController::clearMemorySpotFeed()
{
    QVector<int> ids;
    const auto& spots = m_mw.m_radioModel.spotModel().spots();
    for (auto it = spots.cbegin(); it != spots.cend(); ++it) {
        if (it.value().source == "Memory")
            ids.append(it.key());
    }
    // Block signals during batch removal to avoid N marker rebuilds (#708)
    m_mw.m_radioModel.spotModel().blockSignals(true);
    for (int id : ids)
        m_mw.m_radioModel.spotModel().removeSpot(id);
    m_mw.m_radioModel.spotModel().blockSignals(false);
    if (!ids.isEmpty())
        emit m_mw.m_radioModel.spotModel().spotsRefreshed();
}

void MemoryController::rebuildMemorySpotFeed()
{
    for (auto it = m_mw.m_radioModel.memories().cbegin(); it != m_mw.m_radioModel.memories().cend(); ++it)
        syncMemorySpot(it.key());
}

void MemoryController::refreshMemoryBrowsePanel()
{
    if (!m_mw.m_panStack)
        return;
    for (auto* applet : m_mw.m_panStack->allApplets()) {
        if (!applet)
            continue;
        if (auto* menu = applet->spectrumWidget()->overlayMenu()) {
            menu->setMemories(m_mw.m_radioModel.memories());
            // Update the MEM button badge showing which slice save/recall
            // will target on this pan — makes the fallback slice visible
            // when the active slice is on a different pan (#1781).
            auto* target = preferredMemorySlice(menu->panId());
            menu->setMemoryTargetSliceLetter(
                target ? QChar('A' + (target->sliceId() & 3)) : QChar());
        }
    }
}

bool MemoryController::activateMemorySpot(int memoryIndex, const QString& preferredPanId)
{
    auto* slice = preferredMemorySlice(preferredPanId);
    if (!slice) {
        m_mw.statusBar()->showMessage(
            preferredPanId.isEmpty()
                ? "Open a slice before recalling a memory."
                : "Open a slice on this pan before recalling a memory.",
            3000);
        return false;
    }
    if (slice->isLocked()) {
        m_mw.statusBar()->showMessage("Unlock the target slice before recalling a memory.", 3000);
        return false;
    }

    const auto it = m_mw.m_radioModel.memories().constFind(memoryIndex);
    if (it == m_mw.m_radioModel.memories().constEnd())
        return false;

    const int sliceId = slice->sliceId();
    if (!m_mw.activeSlice() || m_mw.activeSlice()->sliceId() != sliceId)
        m_mw.m_sliceCtl->setActiveSlice(sliceId);

    slice = m_mw.m_radioModel.slice(sliceId);
    if (!slice)
        return false;

    m_mw.m_radioModel.sendCommand(QString("memory apply %1").arg(memoryIndex));
    m_mw.m_radioModel.setPanCenter(it->freq);

    // The radio should push the rest of the applied memory state, but keep the
    // local tuning step in sync immediately so wheel/click snap follows along.
    if (it->step > 0)
        m_mw.m_radioModel.sendCommand(QString("slice set %1 step=%2")
            .arg(slice->sliceId()).arg(it->step));
    return true;
}

ScannerPanel* MemoryController::scannerPanel() const
{
    return m_scannerPanel.data();
}

void MemoryController::showScannerPanel()
{
    if (m_scannerPanel) {
        m_scannerPanel->show();
        m_scannerPanel->raise();
        m_scannerPanel->activateWindow();
        return;
    }
    auto* panel = new ScannerPanel(&m_mw.m_radioModel, &m_mw);
    panel->setWindowFlag(Qt::Window);
    panel->setAttribute(Qt::WA_DeleteOnClose);   // close = stop + restore slice
    panel->setActiveSliceId(m_mw.m_activeSliceId);
    // Squelch-open detection input (AudioEngine lives on the audio thread → queued).
    if (m_mw.m_audio) {
        connect(m_mw.m_audio, &AudioEngine::levelChanged,
                panel, &ScannerPanel::setRxAudioLevel);
    }
    m_scannerPanel = panel;
    panel->show();
}

} // namespace AetherSDR
