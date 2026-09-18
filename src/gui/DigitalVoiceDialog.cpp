#include "DigitalVoiceDialog.h"
#include "ComboStyle.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCheckBox>
#include <QDateTime>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// Same dark palette as ProfileManagerDialog / DxClusterDialog.
const QString kDialogStyle = QStringLiteral(
    "QDialog { background: #0f0f1a; color: #c8d8e8; }"
    "QGroupBox { border: 1px solid #203040; border-radius: 4px; margin-top: 10px;"
    "  padding-top: 8px; color: #8898a8; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
    "QLabel { color: #c8d8e8; }"
    "QLineEdit { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  padding: 4px 6px; color: #c8d8e8; }"
    "QDoubleSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #203040;"
    "  border-radius: 3px; padding: 4px 12px; color: #c8d8e8; }"
    "QPushButton:hover { background: #2a3a4a; }"
    "QPushButton:disabled { color: #506070; }"
    "QCheckBox { color: #c8d8e8; }"
    "QCheckBox::indicator { width: 16px; height: 16px;"
    "  border: 1px solid #406080; border-radius: 3px; background: #0a0a18; }"
    "QCheckBox::indicator:checked { background: #00b4d8; }");

const QString kNoteStyle = QStringLiteral("QLabel { color: #6888a0; font-size: 11px; }");
const QString kWarnStyle = QStringLiteral("QLabel { color: #e0a040; font-size: 11px; }");

} // namespace

DigitalVoiceDialog::DigitalVoiceDialog(RadioModel* model, QWidget* parent)
    : QDialog(parent)
    , m_model(model)
{
    setWindowTitle(QStringLiteral("FreeDV / Digital Voice"));
    setMinimumWidth(420);
    setStyleSheet(kDialogStyle);

    auto& settings = AppSettings::instance();
    auto* root = new QVBoxLayout(this);

    // ── Mode / slice ────────────────────────────────────────────────────
    auto* modeBox = new QGroupBox(QStringLiteral("Digital voice"));
    auto* modeForm = new QFormLayout(modeBox);

    m_sliceCombo = new QComboBox;
    applyComboStyle(m_sliceCombo);
    modeForm->addRow(QStringLiteral("Slice:"), m_sliceCombo);

    m_modeCombo = new QComboBox;
    applyComboStyle(m_modeCombo);
    const DigitalVoiceMode lastMode = digitalVoiceModeFromKey(
        settings.value(QStringLiteral("DigitalVoiceMode"), QString()).toString());
    for (DigitalVoiceMode m : availableDigitalVoiceModes()) {
        m_modeCombo->addItem(digitalVoiceModeName(m), static_cast<int>(m));
        if (m == lastMode) {
            m_modeCombo->setCurrentIndex(m_modeCombo->count() - 1);
        }
    }
    modeForm->addRow(QStringLiteral("Mode:"), m_modeCombo);

    auto* btnRow = new QHBoxLayout;
    m_activateBtn = new QPushButton(QStringLiteral("Activate"));
    m_activeLabel = new QLabel;
    btnRow->addWidget(m_activateBtn);
    btnRow->addWidget(m_activeLabel, 1);
    modeForm->addRow(btnRow);

    auto* modeNote = new QLabel(QStringLiteral(
        "Activating sets the slice to DIGU/DIGL, makes it the TX slice and "
        "routes its DAX audio through the decoder."));
    modeNote->setWordWrap(true);
    modeNote->setStyleSheet(kNoteStyle);
    modeForm->addRow(modeNote);
    root->addWidget(modeBox);

    // ── Live status ─────────────────────────────────────────────────────
    auto* statusBox = new QGroupBox(QStringLiteral("Status"));
    auto* statusForm = new QFormLayout(statusBox);
    m_syncLabel = new QLabel;
    m_syncLabel->setTextFormat(Qt::RichText);
    m_snrLabel = new QLabel;
    m_foffLabel = new QLabel;
    m_rxTextLabel = new QLabel;
    m_rxTextLabel->setTextFormat(Qt::PlainText);
    m_rxTextLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusForm->addRow(QStringLiteral("Sync:"), m_syncLabel);
    statusForm->addRow(QStringLiteral("SNR:"), m_snrLabel);
    statusForm->addRow(QStringLiteral("Freq offset:"), m_foffLabel);
    statusForm->addRow(QStringLiteral("Received text:"), m_rxTextLabel);
    m_rxTextCaption = qobject_cast<QLabel*>(statusForm->labelForField(m_rxTextLabel));
    root->addWidget(statusBox);

    // ── TX / RX options ─────────────────────────────────────────────────
    auto* optBox = new QGroupBox(QStringLiteral("Options"));
    auto* optForm = new QFormLayout(optBox);
    m_callsignEdit = new QLineEdit;
    m_callsignEdit->setMaxLength(16);
    m_callsignEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9/]*")), m_callsignEdit));
    QString callsign = settings.value(QStringLiteral("FreeDvTxCallsign"), QString()).toString();
    if (callsign.isEmpty() && m_model) {
        callsign = m_model->callsign();
    }
    m_callsignEdit->setText(callsign.toUpper());
    m_callsignEdit->setPlaceholderText(QStringLiteral("Callsign sent in text channel"));
    m_callsignEdit->setToolTip(QStringLiteral(
        "700D/700E/1600/700C: sent continuously in the modem text channel.\n"
        "RADE V1: sent once in the end-of-over frame when you unkey "
        "(first 8 characters, A-Z 0-9 /)."));
    optForm->addRow(QStringLiteral("TX callsign:"), m_callsignEdit);

    auto* sqRow = new QHBoxLayout;
    m_squelchCheck = new QCheckBox(QStringLiteral("Squelch"));
    m_squelchCheck->setChecked(
        settings.value(QStringLiteral("FreeDvSquelchEnabled"), QStringLiteral("True")).toString()
        == QStringLiteral("True"));
    m_squelchSpin = new QDoubleSpinBox;
    m_squelchSpin->setRange(-5.0, 20.0);
    m_squelchSpin->setSingleStep(0.5);
    m_squelchSpin->setDecimals(1);
    m_squelchSpin->setSuffix(QStringLiteral(" dB SNR"));
    m_squelchSpin->setValue(
        settings.value(QStringLiteral("FreeDvSquelchSnrDb"), QStringLiteral("0.0")).toString().toDouble());
    sqRow->addWidget(m_squelchCheck);
    sqRow->addWidget(m_squelchSpin, 1);
    optForm->addRow(QStringLiteral("700D/700E/1600:"), sqRow);
    root->addWidget(optBox);

    // ── FreeDV Reporter ─────────────────────────────────────────────────
    auto* repBox = new QGroupBox(QStringLiteral("FreeDV Reporter (qso.freedv.org)"));
    auto* repLayout = new QVBoxLayout(repBox);
    m_reportCheck = new QCheckBox(QStringLiteral("Report my station"));
    m_reportCheck->setChecked(false);  // never on by default, not persisted
    repLayout->addWidget(m_reportCheck);
    m_reportNote = new QLabel(QStringLiteral(
        "When enabled, your callsign, grid square (Radio Setup), slice frequency, "
        "transmit state and the callsigns you decode are published PUBLICLY on "
        "qso.freedv.org."));
    m_reportNote->setWordWrap(true);
    m_reportNote->setStyleSheet(kWarnStyle);
    repLayout->addWidget(m_reportNote);
    root->addWidget(repBox);

    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch();
    auto* closeBtn = new QPushButton(QStringLiteral("Close"));
    closeRow->addWidget(closeBtn);
    root->addLayout(closeRow);

    // ── Wiring ──────────────────────────────────────────────────────────
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(m_activateBtn, &QPushButton::clicked, this, [this] {
        const int sliceId = selectedSliceId();
        const DigitalVoiceMode mode = selectedMode();
        const bool sameAsActive = m_active && sliceId == m_activeSliceId && mode == m_activeMode;
        if (sameAsActive) {
            emit deactivateRequested();
            return;
        }
        if (sliceId < 0 || mode == DigitalVoiceMode::None) {
            return;
        }
        auto& s = AppSettings::instance();
        s.setValue(QStringLiteral("DigitalVoiceMode"), digitalVoiceModeKey(mode));
        s.save();
        emit activateRequested(sliceId, mode);
    });
    connect(m_sliceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateControls(); });
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateControls(); });
    connect(m_callsignEdit, &QLineEdit::editingFinished, this, [this] {
        const QString cs = m_callsignEdit->text().trimmed().toUpper();
        m_callsignEdit->setText(cs);
        auto& s = AppSettings::instance();
        s.setValue(QStringLiteral("FreeDvTxCallsign"), cs);
        s.save();
        emit txCallsignChanged(cs);
    });
    const auto emitSquelch = [this] {
        auto& s = AppSettings::instance();
        s.setValue(QStringLiteral("FreeDvSquelchEnabled"),
                   m_squelchCheck->isChecked() ? QStringLiteral("True") : QStringLiteral("False"));
        s.setValue(QStringLiteral("FreeDvSquelchSnrDb"), QString::number(m_squelchSpin->value(), 'f', 1));
        s.save();
        m_squelchSpin->setEnabled(m_squelchCheck->isChecked());
        emit squelchChanged(m_squelchCheck->isChecked(), static_cast<float>(m_squelchSpin->value()));
    };
    connect(m_squelchCheck, &QCheckBox::toggled, this, emitSquelch);
    connect(m_squelchSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, emitSquelch);
    connect(m_reportCheck, &QCheckBox::toggled, this, &DigitalVoiceDialog::reportingToggled);

    if (m_model) {
        connect(m_model, &RadioModel::sliceAdded, this, [this](SliceModel*) { refreshSlices(); });
        connect(m_model, &RadioModel::sliceRemoved, this, [this](int) { refreshSlices(); });
    }

    m_squelchSpin->setEnabled(m_squelchCheck->isChecked());
    refreshSlices();
    resetStatus();
    updateControls();
}

QString DigitalVoiceDialog::txCallsign() const
{
    return m_callsignEdit->text().trimmed().toUpper();
}

bool DigitalVoiceDialog::squelchEnabled() const
{
    return m_squelchCheck->isChecked();
}

float DigitalVoiceDialog::squelchThresholdDb() const
{
    return static_cast<float>(m_squelchSpin->value());
}

bool DigitalVoiceDialog::reportingEnabled() const
{
    return m_reportCheck->isChecked();
}

void DigitalVoiceDialog::setReportingAvailable(bool available)
{
    m_reportCheck->setEnabled(available);
    if (!available) {
        m_reportNote->setText(QStringLiteral("FreeDV Reporter requires Qt WebSockets (not in this build)."));
    }
}

void DigitalVoiceDialog::setReportingState(bool enabled)
{
    QSignalBlocker b(m_reportCheck);
    m_reportCheck->setChecked(enabled);
}

void DigitalVoiceDialog::showEvent(QShowEvent* ev)
{
    refreshSlices();
    QDialog::showEvent(ev);
}

void DigitalVoiceDialog::refreshSlices()
{
    const int prev = m_sliceCombo->currentData().isValid() ? m_sliceCombo->currentData().toInt() : -1;
    QSignalBlocker b(m_sliceCombo);
    m_sliceCombo->clear();
    if (m_model) {
        for (SliceModel* s : m_model->slices()) {
            if (!s) {
                continue;
            }
            const QChar letter(QLatin1Char(static_cast<char>('A' + (s->sliceId() % 26))));
            QString label = QStringLiteral("Slice %1  %2 MHz %3")
                                .arg(letter)
                                .arg(s->frequency(), 0, 'f', 3)
                                .arg(s->isTxSlice() ? QStringLiteral("(TX)") : QString());
            m_sliceCombo->addItem(label.trimmed(), s->sliceId());
        }
    }
    int want = m_active ? m_activeSliceId : prev;
    int idx = m_sliceCombo->findData(want);
    if (idx < 0 && m_model) {
        for (SliceModel* s : m_model->slices()) {
            if (s && s->isTxSlice()) {
                idx = m_sliceCombo->findData(s->sliceId());
                break;
            }
        }
    }
    if (idx >= 0) {
        m_sliceCombo->setCurrentIndex(idx);
    }
    updateControls();
}

int DigitalVoiceDialog::selectedSliceId() const
{
    const QVariant v = m_sliceCombo->currentData();
    return v.isValid() ? v.toInt() : -1;
}

DigitalVoiceMode DigitalVoiceDialog::selectedMode() const
{
    const QVariant v = m_modeCombo->currentData();
    return v.isValid() ? static_cast<DigitalVoiceMode>(v.toInt()) : DigitalVoiceMode::None;
}

void DigitalVoiceDialog::setActiveState(int sliceId, DigitalVoiceMode mode, bool active)
{
    const bool changed = active != m_active || sliceId != m_activeSliceId || mode != m_activeMode;
    m_active = active;
    m_activeSliceId = active ? sliceId : -1;
    m_activeMode = active ? mode : DigitalVoiceMode::None;
    if (active) {
        int si = m_sliceCombo->findData(sliceId);
        if (si >= 0) {
            QSignalBlocker b(m_sliceCombo);
            m_sliceCombo->setCurrentIndex(si);
        }
        int mi = m_modeCombo->findData(static_cast<int>(mode));
        if (mi >= 0) {
            QSignalBlocker b(m_modeCombo);
            m_modeCombo->setCurrentIndex(mi);
        }
    }
    if (changed) {
        resetStatus();
    }
    updateControls();
}

void DigitalVoiceDialog::updateControls()
{
    const bool haveModes = m_modeCombo->count() > 0;
    const bool haveSlice = selectedSliceId() >= 0;
    const bool selectionIsActive = m_active && selectedSliceId() == m_activeSliceId
                                && selectedMode() == m_activeMode;
    m_activateBtn->setText(selectionIsActive ? QStringLiteral("Deactivate") : QStringLiteral("Activate"));
    m_activateBtn->setEnabled(selectionIsActive || (haveModes && haveSlice));

    if (!haveModes) {
        m_activeLabel->setText(QStringLiteral("No digital voice codecs in this build"));
    } else if (m_active) {
        const QChar letter(QLatin1Char(static_cast<char>('A' + (m_activeSliceId % 26))));
        m_activeLabel->setText(QStringLiteral("Active: %1 on slice %2")
                                   .arg(digitalVoiceModeName(m_activeMode), letter));
    } else {
        m_activeLabel->setText(QStringLiteral("Inactive"));
    }

    if (m_rxTextCaption) {
        // RADE V1 carries only the callsign, decoded from the end-of-over frame.
        m_rxTextCaption->setText(selectedMode() == DigitalVoiceMode::RadeV1
                                     ? QStringLiteral("Callsign (EOO):")
                                     : QStringLiteral("Received text:"));
    }

    const bool codec2Selected = isCodec2Mode(selectedMode());
    m_squelchCheck->setEnabled(codec2Selected);
    m_squelchSpin->setEnabled(codec2Selected && m_squelchCheck->isChecked());
}

void DigitalVoiceDialog::resetStatus()
{
    setSynced(false);
    m_snrLabel->setText(QStringLiteral("---"));
    m_foffLabel->setText(QStringLiteral("---"));
    m_rxTextLabel->setText(QStringLiteral("---"));
}

void DigitalVoiceDialog::setSynced(bool synced)
{
    if (!m_active) {
        m_syncLabel->setText(QStringLiteral("<font color='#505050'>○</font> inactive"));
        return;
    }
    m_syncLabel->setText(synced
        ? QStringLiteral("<font color='#00ff88'>●</font> synced")
        : QStringLiteral("<font color='#505050'>○</font> searching"));
    if (!synced) {
        m_snrLabel->setText(QStringLiteral("---"));
        m_foffLabel->setText(QStringLiteral("---"));
    }
}

void DigitalVoiceDialog::setSnr(float snrDb)
{
    m_snrLabel->setText(QStringLiteral("%1 dB").arg(static_cast<double>(snrDb), 0, 'f', 1));
}

void DigitalVoiceDialog::setFreqOffset(float hz)
{
    m_foffLabel->setText(QStringLiteral("%1%2 Hz")
                             .arg(hz >= 0.0f ? QStringLiteral("+") : QString())
                             .arg(static_cast<double>(hz), 0, 'f', 1));
}

void DigitalVoiceDialog::setReceivedText(const QString& text)
{
    // Time-stamp it: a RADE EOO callsign arrives once per over, so the label
    // otherwise gives no hint of how old it is.
    m_rxTextLabel->setText(QStringLiteral("%1   (%2 UTC)")
                               .arg(text, QDateTime::currentDateTimeUtc().toString(
                                              QStringLiteral("HH:mm:ss"))));
}

} // namespace AetherSDR
