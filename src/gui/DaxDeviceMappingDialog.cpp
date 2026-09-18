#include "DaxDeviceMappingDialog.h"
#include "core/AppSettings.h"
#include "core/DaxDevicePipeline.h"
#include "core/DeviceMappedDax.h"
#include "core/VirtualAudioBridge.h"

#include <QAudioDevice>
#include <QBoxLayout>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardItemModel>

namespace AetherSDR {

namespace {

// Same palette as the other AetherSDR dialogs (MidiMappingDialog,
// AetherDspDialog).  Visual design is maintainer-owned; keep in sync.
const char* kDialogStyle =
    "QDialog { background: #0f0f1a; color: #c8d8e8; }"
    "QLabel { color: #c8d8e8; }"
    "QGroupBox { border: 1px solid #304050; border-radius: 4px; "
    "  margin-top: 8px; padding-top: 12px; font-weight: bold; color: #8aa8c0; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
    "QComboBox { background: #1a2a3a; border: 1px solid #304050; "
    "  border-radius: 3px; color: #c8d8e8; padding: 2px 6px; }"
    "QComboBox::drop-down { border: none; }"
    "QComboBox QAbstractItemView { background: #1a2a3a; color: #c8d8e8; "
    "  selection-background-color: #00b4d8; }"
    "QComboBox:disabled { color: #607080; }"
    "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050;"
    "  border-radius: 3px; padding: 4px 12px; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); border: 1px solid #0090e0; }";

const char* kHintStyle = "QLabel { color: #8090a0; font-size: 11px; }";
const char* kNotesStyle = "QLabel { color: #d0a040; font-size: 11px; }";

QString nativeDescription()
{
#if defined(Q_OS_MAC)
    return QStringLiteral("Built-in AetherSDR DAX driver");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Built-in PipeWire / PulseAudio DAX devices");
#else
    return QStringLiteral("Built-in DAX driver (not available on this platform)");
#endif
}

// Other end of the cable `name` belongs to, if it is a virtual cable.
QString partnerEndpoint(const QString& name, const QStringList& candidates)
{
    if (name.isEmpty() || daxClassifyDevice(name) == DaxCableFamily::NotVirtual) {
        return {};
    }
    const QString key = daxCableKey(name);
    for (const QString& c : candidates) {
        if (daxCableKey(c) == key) {
            return c;
        }
    }
    return {};
}

} // namespace

DaxDeviceMappingDialog::DaxDeviceMappingDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("DAX Audio Devices"));
    setStyleSheet(QLatin1String(kDialogStyle));
    setMinimumWidth(640);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    // ── Backend ─────────────────────────────────────────────────────────
    auto* backendRow = new QHBoxLayout;
    backendRow->addWidget(new QLabel(QStringLiteral("DAX audio backend:")));
    m_backendCombo = new QComboBox;
    m_backendCombo->addItem(nativeDescription(),
                            VirtualAudioBridge::backendName(VirtualAudioBridge::Backend::Native));
    m_backendCombo->addItem(QStringLiteral("Map to audio devices (virtual cables)"),
                            VirtualAudioBridge::backendName(VirtualAudioBridge::Backend::DeviceMapped));
    if (!VirtualAudioBridge::nativeBackendAvailable()) {
        // Disable the Native entry: there is no bundled driver on Windows.
        if (auto* model = qobject_cast<QStandardItemModel*>(m_backendCombo->model())) {
            if (QStandardItem* item = model->item(0)) {
                item->setEnabled(false);
            }
        }
    }
    selectByData(m_backendCombo,
                 VirtualAudioBridge::backendName(VirtualAudioBridge::configuredBackend()));
    backendRow->addWidget(m_backendCombo, 1);
    root->addLayout(backendRow);

    // ── Mapping grid ────────────────────────────────────────────────────
    m_mappingGroup = new QGroupBox(QStringLiteral("Device mapping"));
    auto* grid = new QGridLayout(m_mappingGroup);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    auto* h1 = new QLabel(QStringLiteral("AetherSDR uses"));
    auto* h2 = new QLabel(QStringLiteral("Select in WSJT-X / VARA / fldigi"));
    h1->setStyleSheet(QLatin1String(kHintStyle));
    h2->setStyleSheet(QLatin1String(kHintStyle));
    grid->addWidget(h1, 0, 1);
    grid->addWidget(h2, 0, 2);

    for (int i = 0; i < 4; ++i) {
        grid->addWidget(new QLabel(QStringLiteral("DAX RX %1 →").arg(i + 1)), i + 1, 0);
        m_rxCombo[static_cast<size_t>(i)] = new QComboBox;
        m_rxCombo[static_cast<size_t>(i)]->setToolTip(
            QStringLiteral("Output device AetherSDR plays DAX RX %1 audio to "
                           "(e.g. \"CABLE Input\").").arg(i + 1));
        grid->addWidget(m_rxCombo[static_cast<size_t>(i)], i + 1, 1);
        m_rxHint[static_cast<size_t>(i)] = new QLabel;
        m_rxHint[static_cast<size_t>(i)]->setStyleSheet(QLatin1String(kHintStyle));
        m_rxHint[static_cast<size_t>(i)]->setWordWrap(true);
        grid->addWidget(m_rxHint[static_cast<size_t>(i)], i + 1, 2);
    }
    grid->addWidget(new QLabel(QStringLiteral("DAX TX ←")), 5, 0);
    m_txCombo = new QComboBox;
    m_txCombo->setToolTip(QStringLiteral(
        "Input device AetherSDR records DAX TX audio from (e.g. \"CABLE-B Output\"). "
        "Must be a different cable from every DAX RX device."));
    grid->addWidget(m_txCombo, 5, 1);
    m_txHint = new QLabel;
    m_txHint->setStyleSheet(QLatin1String(kHintStyle));
    m_txHint->setWordWrap(true);
    grid->addWidget(m_txHint, 5, 2);
    grid->setColumnStretch(1, 3);
    grid->setColumnStretch(2, 2);
    root->addWidget(m_mappingGroup);

    auto* toolRow = new QHBoxLayout;
    auto* detectBtn = new QPushButton(QStringLiteral("Auto-detect"));
    detectBtn->setToolTip(QStringLiteral(
        "Suggest a mapping from installed virtual cables (VB-Audio Virtual Cable, "
        "VB-Cable A/B, VAC, Voicemeeter, BlackHole, Loopback)."));
    auto* refreshBtn = new QPushButton(QStringLiteral("Refresh devices"));
    toolRow->addWidget(detectBtn);
    toolRow->addWidget(refreshBtn);
    toolRow->addStretch();
    root->addLayout(toolRow);

    m_notes = new QLabel;
    m_notes->setStyleSheet(QLatin1String(kNotesStyle));
    m_notes->setWordWrap(true);
    root->addWidget(m_notes);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &DaxDeviceMappingDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &DaxDeviceMappingDialog::reject);
    connect(detectBtn, &QPushButton::clicked, this, &DaxDeviceMappingDialog::autoDetect);
    connect(refreshBtn, &QPushButton::clicked, this, &DaxDeviceMappingDialog::populateDevices);
    connect(m_backendCombo, &QComboBox::currentIndexChanged,
            this, &DaxDeviceMappingDialog::updateEnabled);
    for (QComboBox* c : m_rxCombo) {
        connect(c, &QComboBox::currentIndexChanged, this, &DaxDeviceMappingDialog::updateHints);
    }
    connect(m_txCombo, &QComboBox::currentIndexChanged, this, &DaxDeviceMappingDialog::updateHints);

    populateDevices();
    // First use of device mapping pre-fills a suggestion (saved only on OK).
    updateEnabled();
}

void DaxDeviceMappingDialog::selectByData(QComboBox* combo, const QString& name)
{
    const int idx = combo->findData(name);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void DaxDeviceMappingDialog::populateDevices()
{
    // Remember the current choice (or the saved one on first population).
    const auto& s = AppSettings::instance();
    std::array<QString, 4> rxWant;
    QString txWant;
    for (int i = 0; i < 4; ++i) {
        QComboBox* c = m_rxCombo[static_cast<size_t>(i)];
        rxWant[static_cast<size_t>(i)] = c->count() > 0
            ? c->currentData().toString()
            : s.value(DeviceMappedDax::rxDeviceSettingKey(i + 1), QString()).toString();
    }
    txWant = m_txCombo->count() > 0
        ? m_txCombo->currentData().toString()
        : s.value(DeviceMappedDax::txDeviceSettingKey(), QString()).toString();

    m_outputs.clear();
    m_inputs.clear();
    for (const QAudioDevice& d : QMediaDevices::audioOutputs()) {
        m_outputs << d.description();
    }
    for (const QAudioDevice& d : QMediaDevices::audioInputs()) {
        m_inputs << d.description();
    }

    auto fill = [](QComboBox* combo, const QStringList& devices, const QString& want) {
        const QSignalBlocker block(combo);
        combo->clear();
        combo->addItem(QStringLiteral("None"), QString());
        for (const QString& d : devices) {
            // Never offer our own HAL devices as mapping targets.
            if (d.startsWith(QLatin1String("AetherSDR DAX"))
                || d.startsWith(QLatin1String("AetherSDR TX"))) {
                continue;
            }
            combo->addItem(d, d);
            if (daxClassifyDevice(d) != DaxCableFamily::NotVirtual) {
                combo->setItemData(combo->count() - 1,
                                   QStringLiteral("Virtual audio cable"), Qt::ToolTipRole);
            }
        }
        if (!want.isEmpty() && combo->findData(want) < 0) {
            combo->addItem(QStringLiteral("%1 (not connected)").arg(want), want);
        }
        selectByData(combo, want);
    };
    for (int i = 0; i < 4; ++i) {
        fill(m_rxCombo[static_cast<size_t>(i)], m_outputs, rxWant[static_cast<size_t>(i)]);
    }
    fill(m_txCombo, m_inputs, txWant);
    updateHints();
}

void DaxDeviceMappingDialog::autoDetect()
{
    const DaxDeviceSuggestion sug = suggestDaxDeviceMapping(m_outputs, m_inputs);
    for (int i = 0; i < 4; ++i) {
        selectByData(m_rxCombo[static_cast<size_t>(i)], sug.rx[static_cast<size_t>(i)]);
    }
    selectByData(m_txCombo, sug.tx);
    m_suggestionNotes = sug.notes.join(QStringLiteral("\n\n"));
    updateHints();
}

void DaxDeviceMappingDialog::updateHints()
{
    for (int i = 0; i < 4; ++i) {
        const QString dev = m_rxCombo[static_cast<size_t>(i)]->currentData().toString();
        const QString partner = partnerEndpoint(dev, m_inputs);
        QString text;
        if (dev.isEmpty()) {
            text = QStringLiteral("—");
        } else if (!partner.isEmpty()) {
            text = QStringLiteral("Input: %1").arg(partner);
        } else {
            text = QStringLiteral("Not a recognised virtual cable");
        }
        m_rxHint[static_cast<size_t>(i)]->setText(text);
    }
    const QString tx = m_txCombo->currentData().toString();
    const QString txPartner = partnerEndpoint(tx, m_outputs);
    if (tx.isEmpty()) {
        m_txHint->setText(QStringLiteral("—"));
    } else if (!txPartner.isEmpty()) {
        m_txHint->setText(QStringLiteral("Output: %1").arg(txPartner));
    } else {
        m_txHint->setText(QStringLiteral("Not a recognised virtual cable"));
    }

    // Live warning for the dangerous case.
    QStringList warn;
    for (int i = 0; i < 4; ++i) {
        const QString rx = m_rxCombo[static_cast<size_t>(i)]->currentData().toString();
        if (daxDevicesShareCable(rx, tx)) {
            warn << QStringLiteral("DAX TX uses the same cable as DAX RX %1 — received "
                                   "audio would be transmitted. Pick a different cable.")
                        .arg(i + 1);
        }
    }
    m_notes->setText(warn.isEmpty() ? m_suggestionNotes : warn.join(QLatin1Char('\n')));
}

void DaxDeviceMappingDialog::updateEnabled()
{
    const bool mapped = m_backendCombo->currentData().toString()
        == VirtualAudioBridge::backendName(VirtualAudioBridge::Backend::DeviceMapped);
    m_mappingGroup->setEnabled(mapped);

    // Switching to device mapping with nothing chosen yet: suggest one.
    if (mapped) {
        bool any = !m_txCombo->currentData().toString().isEmpty();
        for (QComboBox* c : m_rxCombo) {
            any = any || !c->currentData().toString().isEmpty();
        }
        if (!any) {
            autoDetect();
        }
    }
}

void DaxDeviceMappingDialog::accept()
{
    const QString tx = m_txCombo->currentData().toString();
    QSet<QString> used;
    for (int i = 0; i < 4; ++i) {
        const QString rx = m_rxCombo[static_cast<size_t>(i)]->currentData().toString();
        if (rx.isEmpty()) {
            continue;
        }
        if (daxDevicesShareCable(rx, tx)) {
            QMessageBox::warning(this, windowTitle(),
                QStringLiteral("DAX TX (%1) is the other end of the cable used by DAX RX %2 "
                               "(%3).\n\nThat would transmit the received audio. Use a "
                               "separate virtual cable for DAX TX.").arg(tx).arg(i + 1).arg(rx));
            return;
        }
        if (used.contains(rx)) {
            QMessageBox::warning(this, windowTitle(),
                QStringLiteral("\"%1\" is mapped to more than one DAX RX channel. Each "
                               "channel needs its own cable.").arg(rx));
            return;
        }
        used.insert(rx);
    }

    auto& s = AppSettings::instance();
    const QString oldBackend =
        VirtualAudioBridge::backendName(VirtualAudioBridge::configuredBackend());
    const QString newBackend = m_backendCombo->currentData().toString();
    s.setValue(VirtualAudioBridge::backendSettingKey(), newBackend);
    for (int i = 0; i < 4; ++i) {
        s.setValue(DeviceMappedDax::rxDeviceSettingKey(i + 1),
                   m_rxCombo[static_cast<size_t>(i)]->currentData().toString());
    }
    s.setValue(DeviceMappedDax::txDeviceSettingKey(), tx);
    s.save();
    m_backendChanged = (oldBackend != newBackend);
    QDialog::accept();
}

} // namespace AetherSDR
