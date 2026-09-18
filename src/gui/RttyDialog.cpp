#include "RttyDialog.h"

#include "DigitalModeStyles.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QHideEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kTxChunkFrames = 480;

struct RttyFrequencyPreset {
    const char* label;
    double khz;
};

struct RttyTonePreset {
    const char* label;
    double baud;
    int shift;
};

constexpr RttyFrequencyPreset kFrequencyPresets[] = {
    {"160m (1800-1840)", 1820.0},
    {"80m (3580-3650)", 3585.0},
    {"40m (7080-7100)", 7085.0},
    {"30m (10110-10150)", 10135.0},
    {"20m (14080-14099)", 14085.0},
    {"15m (21080-21100)", 21085.0},
    {"10m (28080-28100)", 28085.0},
    {"DDH 47 Weather (147.3)", 147.3},
    {"DDH 9 Weather (11039)", 11039.0},
    {"DDH 8 Weather (14467.3)", 14467.3},
    {"DDK 2 Weather (4583)", 4583.0},
    {"DDH 7 Weather (7646)", 7646.0},
    {"DDK 9 Weather (10100.8)", 10100.8},
};

constexpr RttyTonePreset kTonePresets[] = {
    {"45.45 / 170", 45.45, 170},
    {"50 / 270", 50.0, 270},
    {"75 / 370", 75.0, 370},
    {"50 / 85 (Weather)", 50.0, 85},
    {"50 / 450 (Weather)", 50.0, 450},
};

QString boxedStyle(const QString& bg)
{
    return QString(
        "QLabel { background: %1; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; min-width: 44px; }").arg(bg);
}

} // namespace

RttyDialog::RttyDialog(RadioModel* radioModel, AudioEngine* audio, QWidget* parent)
    : QDialog(parent)
    , m_radioModel(radioModel)
    , m_audio(audio)
{
    setWindowTitle(QStringLiteral("RTTY"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1180, 760);

    buildUi();
    loadSettings();
    applyDecoderSettings();
    updateSliceSummary();
    updateTxControls();

    connect(&m_decoder, &RttyDecoder::textDecoded,
            this, &RttyDialog::appendDecodedText);
    connect(&m_decoder, &RttyDecoder::metricsUpdated,
            this, &RttyDialog::updateMetrics);
    connect(this, &RttyDialog::txAudioReady,
            m_audio, &AudioEngine::feedDaxTxAudio,
            Qt::QueuedConnection);

    connect(m_radioModel, &RadioModel::radioTransmittingChanged,
            this, &RttyDialog::onRadioTransmittingChanged);

    m_txTimer = new QTimer(this);
    m_txTimer->setInterval(20);
    m_txTimer->setTimerType(Qt::PreciseTimer);
    connect(m_txTimer, &QTimer::timeout, this, &RttyDialog::pumpTxAudio);

    auto* sendShortcut1 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    auto* sendShortcut2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), this);
    connect(sendShortcut1, &QShortcut::activated, this, &RttyDialog::onSendClicked);
    connect(sendShortcut2, &QShortcut::activated, this, &RttyDialog::onSendClicked);
}

RttyDialog::~RttyDialog()
{
    // PTT is released in hideEvent(): the dialog is WA_DeleteOnClose, and at
    // application shutdown RadioModel is already destroyed when we get here.
    saveSettings();
}

void RttyDialog::hideEvent(QHideEvent* event)
{
    // A spontaneous hide (minimise) keeps transmitting; closing the window
    // must not leave the radio keyed with nothing feeding DAX TX audio.
    if (!event->spontaneous() && (m_txKeyRequested || m_txStartPending || m_txTimer->isActive())) {
        stopTransmissionNow();
    }
    QDialog::hideEvent(event);
}

void RttyDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* controls = new QWidget(this);
    controls->setStyleSheet(DigitalModeStyles::rxPanelStyle());
    auto* grid = new QGridLayout(controls);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    auto* rxBadge = new QLabel(QStringLiteral("RX"), controls);
    rxBadge->setStyleSheet(boxedStyle("#646464"));
    grid->addWidget(rxBadge, 0, 0, 2, 1);

    auto* freqLabel = new QLabel(QStringLiteral("Freq:"), controls);
    grid->addWidget(freqLabel, 0, 1);

    m_frequencyPresetCombo = new QComboBox(controls);
    m_frequencyPresetCombo->addItem(QStringLiteral("Manual"));
    for (const auto& preset : kFrequencyPresets)
        m_frequencyPresetCombo->addItem(QString::fromLatin1(preset.label));
    grid->addWidget(m_frequencyPresetCombo, 0, 2);

    m_frequencySpin = new QDoubleSpinBox(controls);
    m_frequencySpin->setDecimals(1);
    m_frequencySpin->setRange(0.1, 60000.0);
    m_frequencySpin->setSuffix(QStringLiteral(" kHz"));
    m_frequencySpin->setSingleStep(0.5);
    grid->addWidget(m_frequencySpin, 0, 3);

    auto* presetLabel = new QLabel(QStringLiteral("Preset:"), controls);
    grid->addWidget(presetLabel, 1, 1);

    m_tonePresetCombo = new QComboBox(controls);
    m_tonePresetCombo->addItem(QStringLiteral("Manual"));
    for (const auto& preset : kTonePresets)
        m_tonePresetCombo->addItem(QString::fromLatin1(preset.label));
    grid->addWidget(m_tonePresetCombo, 1, 2);

    m_baudSpin = new QDoubleSpinBox(controls);
    m_baudSpin->setDecimals(2);
    m_baudSpin->setRange(10.0, 300.0);
    m_baudSpin->setSingleStep(0.05);
    m_baudSpin->setPrefix(QStringLiteral("Baud "));
    grid->addWidget(m_baudSpin, 0, 4);

    m_markSpin = new QSpinBox(controls);
    m_markSpin->setRange(200, 4000);
    m_markSpin->setSingleStep(5);
    m_markSpin->setPrefix(QStringLiteral("Mark "));
    grid->addWidget(m_markSpin, 1, 4);

    m_shiftSpin = new QSpinBox(controls);
    m_shiftSpin->setRange(10, 1000);
    m_shiftSpin->setSingleStep(5);
    m_shiftSpin->setPrefix(QStringLiteral("Shift "));
    grid->addWidget(m_shiftSpin, 1, 5);

    m_invertCheck = new QCheckBox(QStringLiteral("Inverted"), controls);
    grid->addWidget(m_invertCheck, 0, 5);

    m_unshiftOnSpaceCheck = new QCheckBox(QStringLiteral("Unshift on space"), controls);
    grid->addWidget(m_unshiftOnSpaceCheck, 0, 6);

    auto* sqlLabel = new QLabel(QStringLiteral("SQL:"), controls);
    grid->addWidget(sqlLabel, 1, 6);

    m_sqlSlider = new QSlider(Qt::Horizontal, controls);
    m_sqlSlider->setRange(0, 100);
    grid->addWidget(m_sqlSlider, 1, 7);

    m_confidenceBar = new QProgressBar(controls);
    m_confidenceBar->setRange(0, 100);
    m_confidenceBar->setTextVisible(false);
    m_confidenceBar->setMinimumWidth(180);
    grid->addWidget(m_confidenceBar, 0, 7);

    m_metricsLabel = new QLabel(QStringLiteral("Mark -120 dB | Space -120 dB"), controls);
    grid->addWidget(m_metricsLabel, 0, 8);

    m_eraseButton = new QPushButton(QStringLiteral("Erase"), controls);
    m_eraseButton->setStyleSheet(
        "QPushButton { background: #0d3b84; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; }");
    grid->addWidget(m_eraseButton, 0, 9, 2, 1);

    root->addWidget(controls);

    m_sliceSummaryLabel = new QLabel(this);
    root->addWidget(m_sliceSummaryLabel);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);

    m_rxText = new QPlainTextEdit(split);
    m_rxText->setReadOnly(true);
    m_rxText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_rxText->setStyleSheet(
        "QPlainTextEdit { background: #202020; color: #f5f7fa; "
        "border: 1px solid #303030; padding: 10px; font-family: Menlo, monospace; "
        "font-size: 16px; }");

    m_txEdit = new QPlainTextEdit(split);
    m_txEdit->setPlaceholderText(QStringLiteral("Compose outgoing RTTY text here. Ctrl+Enter sends."));
    m_txEdit->setStyleSheet(
        "QPlainTextEdit { background: #2a2a2a; color: #f5f7fa; "
        "border: 1px solid #404040; padding: 10px; font-family: Menlo, monospace; "
        "font-size: 15px; }");

    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 2);
    root->addWidget(split, 1);

    auto* txPanel = new QWidget(this);
    txPanel->setStyleSheet(DigitalModeStyles::txPanelStyle());
    auto* txGrid = new QGridLayout(txPanel);
    txGrid->setContentsMargins(12, 12, 12, 12);
    txGrid->setHorizontalSpacing(10);
    txGrid->setVerticalSpacing(8);

    auto* txBadge = new QLabel(QStringLiteral("TX"), txPanel);
    txBadge->setStyleSheet(boxedStyle("#646464"));
    txGrid->addWidget(txBadge, 0, 0, 2, 1);

    m_txButton = new QPushButton(QStringLiteral("TX"), txPanel);
    m_txButton->setCheckable(true);
    m_txButton->setStyleSheet(
        "QPushButton { background: #0d3b84; color: white; font-weight: bold; "
        "padding: 10px 20px; border-radius: 4px; }"
        "QPushButton:checked { background: #9e1c26; }");
    txGrid->addWidget(m_txButton, 0, 1);

    m_sendButton = new QPushButton(QStringLiteral("Send"), txPanel);
    txGrid->addWidget(m_sendButton, 0, 2);

    m_autoTxCheck = new QCheckBox(QStringLiteral("Auto TX"), txPanel);
    txGrid->addWidget(m_autoTxCheck, 0, 3);

    m_spotCheck = new QCheckBox(QStringLiteral("Spot"), txPanel);
    txGrid->addWidget(m_spotCheck, 0, 4);

    m_pskReporterCheck = new QCheckBox(QStringLiteral("PSK Reporter"), txPanel);
    txGrid->addWidget(m_pskReporterCheck, 0, 5);

    m_pendingLabel = new QLabel(QStringLiteral("Queue: 0 bits"), txPanel);
    txGrid->addWidget(m_pendingLabel, 0, 6);

    auto* dxLabel = new QLabel(QStringLiteral("DX:"), txPanel);
    txGrid->addWidget(dxLabel, 0, 7);
    m_dxEdit = new QLineEdit(txPanel);
    txGrid->addWidget(m_dxEdit, 0, 8);

    auto* rstLabel = new QLabel(QStringLiteral("RST Received:"), txPanel);
    txGrid->addWidget(rstLabel, 0, 9);
    m_rstEdit = new QLineEdit(txPanel);
    txGrid->addWidget(m_rstEdit, 0, 10);

    m_logButton = new QPushButton(QStringLiteral("LOG"), txPanel);
    m_logButton->setStyleSheet(
        "QPushButton { background: #0d3b84; color: white; font-weight: bold; "
        "padding: 10px 16px; border-radius: 4px; }");
    txGrid->addWidget(m_logButton, 0, 11);

    m_statusLabel = new QLabel(txPanel);
    m_statusLabel->setWordWrap(true);
    txGrid->addWidget(m_statusLabel, 1, 1, 1, 11);

    root->addWidget(txPanel);

    connect(m_frequencyPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &RttyDialog::applyFrequencyPreset);
    connect(m_frequencySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &RttyDialog::onFrequencyEdited);
    connect(m_tonePresetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &RttyDialog::applyTonePreset);
    connect(m_baudSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &RttyDialog::applyDecoderSettings);
    connect(m_markSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &RttyDialog::applyDecoderSettings);
    connect(m_shiftSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &RttyDialog::applyDecoderSettings);
    connect(m_invertCheck, &QCheckBox::toggled,
            this, &RttyDialog::applyDecoderSettings);
    connect(m_unshiftOnSpaceCheck, &QCheckBox::toggled,
            this, &RttyDialog::applyDecoderSettings);
    // Squelch is decoder-only: it must not retune or re-mode the slice.
    connect(m_sqlSlider, &QSlider::valueChanged, this, [this](int value) {
        m_decoder.setSquelch(static_cast<float>(value) / 100.0f);
    });
    connect(m_eraseButton, &QPushButton::clicked,
            this, &RttyDialog::clearRxText);
    connect(m_sendButton, &QPushButton::clicked,
            this, &RttyDialog::onSendClicked);
    connect(m_txButton, &QPushButton::toggled,
            this, &RttyDialog::onTxToggled);
    connect(m_logButton, &QPushButton::clicked,
            this, &RttyDialog::logContact);
    connect(m_spotCheck, &QCheckBox::toggled, this, [](bool on) {
        AppSettings::instance().setValue(spotEnabledSettingKey(QStringLiteral("RTTY")),
                                         on ? QStringLiteral("True") : QStringLiteral("False"));
        AppSettings::instance().save();
    });
    connect(m_pskReporterCheck, &QCheckBox::toggled, this, [](bool on) {
        AppSettings::instance().setValue(reporterEnabledSettingKey(QStringLiteral("RTTY")),
                                         on ? QStringLiteral("True") : QStringLiteral("False"));
        AppSettings::instance().save();
    });
    connect(m_txEdit, &QPlainTextEdit::textChanged,
            this, &RttyDialog::updateTxControls);

    updateStatus(QStringLiteral("RTTY decoder ready."));
}

void RttyDialog::setActiveSlice(SliceModel* slice)
{
    if (slice == m_activeSlice)
        return;

    m_activeSlice = slice;
    updateSliceConnections(slice);
    syncControlsFromSlice();
    updateSliceSummary();
}

void RttyDialog::feedAudio(const QByteArray& pcm24kStereo)
{
    m_decoder.feedAudio(pcm24kStereo);
}

void RttyDialog::appendDecodedText(const QString& text)
{
    auto cursor = m_rxText->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    m_rxText->setTextCursor(cursor);
    m_rxText->ensureCursorVisible();

    m_recentDecodedText += text;
    if (m_recentDecodedText.size() > 512)
        m_recentDecodedText = m_recentDecodedText.right(512);
    maybeEmitReceptionReport(m_recentDecodedText);
}

void RttyDialog::applyFrequencyPreset(int index)
{
    if (m_syncingControls || index <= 0)
        return;

    const auto& preset = kFrequencyPresets[index - 1];
    m_frequencySpin->setValue(preset.khz);
    applySliceTuning();
}

void RttyDialog::applyTonePreset(int index)
{
    if (m_syncingControls || index <= 0)
        return;

    const auto& preset = kTonePresets[index - 1];
    m_baudSpin->setValue(preset.baud);
    m_shiftSpin->setValue(preset.shift);
    applyDecoderSettings();
}

void RttyDialog::applyDecoderSettings()
{
    const double baud = m_baudSpin->value();
    const int mark = m_markSpin->value();
    const int shift = m_shiftSpin->value();
    const bool inverted = m_invertCheck->isChecked();
    const bool unshift = m_unshiftOnSpaceCheck->isChecked();
    const float sql = static_cast<float>(m_sqlSlider->value()) / 100.0f;

    m_decoder.setBaud(baud);
    m_decoder.setMarkHz(mark);
    m_decoder.setShiftHz(shift);
    m_decoder.setInverted(inverted);
    m_decoder.setUnshiftOnSpace(unshift);
    m_decoder.setSquelch(sql);

    m_modulator.setBaud(baud);
    m_modulator.setMarkHz(mark);
    m_modulator.setShiftHz(shift);
    m_modulator.setInverted(inverted);
    m_modulator.setUnshiftOnSpace(unshift);

    syncPresetSelections();
    applySliceTuning();
    updateTxControls();
}

void RttyDialog::applySliceTuning()
{
    if (m_syncingControls || !m_activeSlice)
        return;

    const int shift = m_shiftSpin->value();
    const int width = std::clamp(shift + 280, 350, 1200);
    const int midpoint = -(shift / 2);

    m_activeSlice->setFrequency(m_frequencySpin->value() / 1000.0);
    if (m_activeSlice->mode() != QStringLiteral("RTTY"))
        m_activeSlice->setMode(QStringLiteral("RTTY"));
    m_activeSlice->setRttyMark(m_markSpin->value());
    m_activeSlice->setRttyShift(shift);
    m_activeSlice->setFilterWidth(midpoint - (width / 2), midpoint + (width / 2));
    updateSliceSummary();
}

void RttyDialog::clearRxText()
{
    m_rxText->clear();
    updateStatus(QStringLiteral("RTTY receive text cleared."));
}

QString RttyDialog::csvEscape(const QString& text) const
{
    QString escaped = text;
    escaped.replace('"', QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString RttyDialog::logFilePath() const
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(base);
    dir.mkpath(QStringLiteral("AetherSDR"));
    return dir.filePath(QStringLiteral("AetherSDR/rtty-log.csv"));
}

void RttyDialog::logContact()
{
    QFile file(logFilePath());
    const bool newFile = !file.exists();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        updateStatus(QStringLiteral("Unable to open RTTY log file for writing."));
        return;
    }

    QTextStream ts(&file);
    if (newFile)
        ts << "timestamp_local,frequency_khz,dx,rst,baud,mark_hz,shift_hz,rx_excerpt\n";

    QString rxExcerpt = m_rxText->toPlainText();
    rxExcerpt = rxExcerpt.right(240);
    rxExcerpt.replace('\n', ' ');

    ts << csvEscape(QDateTime::currentDateTime().toString(Qt::ISODate)) << ','
       << csvEscape(QString::number(m_frequencySpin->value(), 'f', 1)) << ','
       << csvEscape(m_dxEdit->text().trimmed().toUpper()) << ','
       << csvEscape(m_rstEdit->text().trimmed()) << ','
       << csvEscape(QString::number(m_baudSpin->value(), 'f', 2)) << ','
       << csvEscape(QString::number(m_markSpin->value())) << ','
       << csvEscape(QString::number(m_shiftSpin->value())) << ','
       << csvEscape(rxExcerpt)
       << '\n';

    updateStatus(QStringLiteral("Logged RTTY contact to %1").arg(file.fileName()));
}

void RttyDialog::onFrequencyEdited(double)
{
    if (m_syncingControls)
        return;
    syncPresetSelections();
    applySliceTuning();
}

void RttyDialog::onSendClicked()
{
    queueEditorText();
}

void RttyDialog::queueEditorText()
{
    const QString text = m_txEdit->toPlainText();
    if (text.trimmed().isEmpty()) {
        updateStatus(QStringLiteral("Nothing queued for RTTY transmit."));
        return;
    }

    m_modulator.queueText(text);
    m_txEdit->clear();
    {
        auto cursor = m_rxText->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QStringLiteral("\nTX> %1\n").arg(text));
        m_rxText->setTextCursor(cursor);
        m_rxText->ensureCursorVisible();
    }
    updateTxControls();

    if (m_autoTxCheck->isChecked() && !m_radioModel->transmitModel().isMox()) {
        m_stopAfterDrain = true;
        m_manualTxHold = false;
        {
            QSignalBlocker blocker(m_txButton);
            m_txButton->setChecked(true);
        }
        startTransmission();
        updateTxControls();
    } else {
        updateStatus(QStringLiteral("Queued RTTY text for transmit."));
    }
}

void RttyDialog::onTxToggled(bool on)
{
    if (on) {
        if (m_autoTxCheck->isChecked() && !m_modulator.hasPendingData()) {
            updateStatus(QStringLiteral("Queue text with Send before starting Auto TX."));
            QSignalBlocker blocker(m_txButton);
            m_txButton->setChecked(false);
            return;
        }

        m_manualTxHold = !m_autoTxCheck->isChecked();
        m_stopAfterDrain = m_autoTxCheck->isChecked();
        startTransmission();
        return;
    }

    m_manualTxHold = false;
    m_stopAfterDrain = true;
    if (!m_modulator.hasPendingData())
        stopTransmissionNow();
    else
        updateStatus(QStringLiteral("RTTY TX will stop after queued text drains."));
}

void RttyDialog::startTransmission()
{
    if (!m_activeSlice) {
        updateStatus(QStringLiteral("Select an active slice before transmitting RTTY."));
        QSignalBlocker blocker(m_txButton);
        m_txButton->setChecked(false);
        return;
    }

    applySliceTuning();
    if (!m_activeSlice->isTxSlice())
        m_activeSlice->setTxSlice(true);

    if (m_activeSlice->mode() != QStringLiteral("RTTY"))
        m_activeSlice->setMode(QStringLiteral("RTTY"));

    m_txStartPending = true;
    m_drainGuardChunks = 0;
    m_txKeyRequested = true;
    m_radioModel->setTransmit(true);
    updateStatus(QStringLiteral("Keying RTTY transmitter..."));
    updateTxControls();
}

void RttyDialog::stopTransmissionNow()
{
    m_txTimer->stop();
    m_txStartPending = false;
    m_manualTxHold = false;
    m_stopAfterDrain = false;
    m_drainGuardChunks = 0;
    m_modulator.clearQueue();
    m_modulator.resetStreamState();

    // Unkey if we requested TX even when mox= has not been echoed yet
    // (stopping during PTT_REQUESTED otherwise left the radio keyed).
    const bool unkey = m_txKeyRequested || m_radioModel->transmitModel().isMox();
    m_txKeyRequested = false;
    if (unkey)
        m_radioModel->setTransmit(false);

    {
        QSignalBlocker blocker(m_txButton);
        m_txButton->setChecked(false);
    }

    updateStatus(QStringLiteral("RTTY TX stopped."));
    updateTxControls();
}

void RttyDialog::onRadioTransmittingChanged(bool on)
{
    if (on) {
        if (m_txStartPending || m_txButton->isChecked()) {
            m_txStartPending = false;
            if (!m_txTimer->isActive())
                m_txTimer->start();
            updateStatus(QStringLiteral("RTTY transmitter active."));
        }
    } else {
        m_txTimer->stop();
        if (!m_txStartPending) {
            QSignalBlocker blocker(m_txButton);
            m_txButton->setChecked(false);
        }
    }

    updateTxControls();
}

void RttyDialog::pumpTxAudio()
{
    if (!m_radioModel->transmitModel().isMox() && !m_txStartPending)
        return;

    emit txAudioReady(m_modulator.renderAudio(kTxChunkFrames));
    updateTxControls();

    if (m_stopAfterDrain && !m_manualTxHold && !m_modulator.hasPendingData()) {
        ++m_drainGuardChunks;
        if (m_drainGuardChunks >= 2)
            stopTransmissionNow();
    } else {
        m_drainGuardChunks = 0;
    }
}

void RttyDialog::syncControlsFromSlice()
{
    if (!m_activeSlice)
        return;

    m_syncingControls = true;
    m_frequencySpin->setValue(m_activeSlice->frequency() * 1000.0);
    m_markSpin->setValue(m_activeSlice->rttyMark());
    m_shiftSpin->setValue(m_activeSlice->rttyShift());
    syncPresetSelections();
    m_syncingControls = false;
}

void RttyDialog::updateMetrics(float markLevelDb, float spaceLevelDb, float confidence, bool locked)
{
    m_confidenceBar->setValue(static_cast<int>(std::clamp(confidence, 0.0f, 1.0f) * 100.0f));
    m_metricsLabel->setText(
        QStringLiteral("Mark %1 dB | Space %2 dB | %3")
            .arg(markLevelDb, 0, 'f', 1)
            .arg(spaceLevelDb, 0, 'f', 1)
            .arg(locked ? QStringLiteral("SYNC") : QStringLiteral("SEARCH")));
}

void RttyDialog::updateSliceConnections(SliceModel* slice)
{
    if (m_sliceFrequencyConnection)
        disconnect(m_sliceFrequencyConnection);
    if (m_sliceModeConnection)
        disconnect(m_sliceModeConnection);
    if (m_sliceMarkConnection)
        disconnect(m_sliceMarkConnection);
    if (m_sliceShiftConnection)
        disconnect(m_sliceShiftConnection);

    if (!slice)
        return;

    m_sliceFrequencyConnection = connect(slice, &SliceModel::frequencyChanged,
                                         this, [this](double) { syncControlsFromSlice(); });
    m_sliceModeConnection = connect(slice, &SliceModel::modeChanged,
                                    this, [this](const QString&) { updateSliceSummary(); });
    m_sliceMarkConnection = connect(slice, &SliceModel::rttyMarkChanged,
                                    this, [this](int) { syncControlsFromSlice(); });
    m_sliceShiftConnection = connect(slice, &SliceModel::rttyShiftChanged,
                                     this, [this](int) { syncControlsFromSlice(); });
}

void RttyDialog::updateSliceSummary()
{
    if (!m_activeSlice) {
        m_sliceSummaryLabel->setText(
            QStringLiteral("No active slice selected. RTTY controls follow the active slice."));
        return;
    }

    m_sliceSummaryLabel->setText(
        QStringLiteral("Active slice %1 | %2 kHz | %3 | mark %4 / shift %5")
            .arg(QChar('A' + std::clamp(m_activeSlice->sliceId(), 0, 25)))
            .arg(m_activeSlice->frequency() * 1000.0, 0, 'f', 1)
            .arg(m_activeSlice->mode())
            .arg(m_activeSlice->rttyMark())
            .arg(m_activeSlice->rttyShift()));
}

void RttyDialog::updateStatus(const QString& text)
{
    m_statusLabel->setText(text);
}

void RttyDialog::updateTxControls()
{
    const bool txActive = m_radioModel && m_radioModel->transmitModel().isMox();
    m_pendingLabel->setText(QStringLiteral("Queue: %1 bits").arg(m_modulator.pendingBits()));
    m_sendButton->setEnabled(txActive || !m_txEdit->toPlainText().trimmed().isEmpty());
}

void RttyDialog::syncPresetSelections()
{
    const double khz = m_frequencySpin->value();
    int freqIndex = 0;
    for (int i = 0; i < static_cast<int>(std::size(kFrequencyPresets)); ++i) {
        if (std::abs(kFrequencyPresets[i].khz - khz) <= 0.15) {
            freqIndex = i + 1;
            break;
        }
    }

    int toneIndex = 0;
    for (int i = 0; i < static_cast<int>(std::size(kTonePresets)); ++i) {
        if (std::abs(kTonePresets[i].baud - m_baudSpin->value()) <= 0.05
                && kTonePresets[i].shift == m_shiftSpin->value()) {
            toneIndex = i + 1;
            break;
        }
    }

    QSignalBlocker freqBlocker(m_frequencyPresetCombo);
    QSignalBlocker toneBlocker(m_tonePresetCombo);
    m_frequencyPresetCombo->setCurrentIndex(freqIndex);
    m_tonePresetCombo->setCurrentIndex(toneIndex);
}

void RttyDialog::loadSettings()
{
    auto& s = AppSettings::instance();
    restoreGeometry(s.value("RttyDialogGeometry").toByteArray());

    m_frequencySpin->setValue(s.value("RttyFrequencyKhz", 14085.0).toDouble());
    m_baudSpin->setValue(s.value("RttyBaud", 45.45).toDouble());
    m_markSpin->setValue(s.value("RttyMarkHz", 2125).toInt());
    m_shiftSpin->setValue(s.value("RttyShiftHz", 170).toInt());
    m_invertCheck->setChecked(s.value("RttyInverted", false).toBool());
    m_unshiftOnSpaceCheck->setChecked(s.value("RttyUnshiftOnSpace", true).toBool());
    m_sqlSlider->setValue(s.value("RttySquelch", 12).toInt());
    m_autoTxCheck->setChecked(s.value("RttyAutoTx", true).toBool());
    m_dxEdit->setText(s.value("RttyLastDx").toString());
    m_rstEdit->setText(s.value("RttyLastRst").toString());
    syncPresetSelections();
    refreshReportingControls();
}

void RttyDialog::saveSettings() const
{
    auto& s = AppSettings::instance();
    s.setValue("RttyDialogGeometry", saveGeometry());
    s.setValue("RttyFrequencyKhz", m_frequencySpin->value());
    s.setValue("RttyBaud", m_baudSpin->value());
    s.setValue("RttyMarkHz", m_markSpin->value());
    s.setValue("RttyShiftHz", m_shiftSpin->value());
    s.setValue("RttyInverted", m_invertCheck->isChecked());
    s.setValue("RttyUnshiftOnSpace", m_unshiftOnSpaceCheck->isChecked());
    s.setValue("RttySquelch", m_sqlSlider->value());
    s.setValue("RttyAutoTx", m_autoTxCheck->isChecked());
    s.setValue("RttyLastDx", m_dxEdit->text());
    s.setValue("RttyLastRst", m_rstEdit->text());
    s.save();
}

QString RttyDialog::receiverGrid() const
{
    QString grid = m_radioModel->gpsGrid().trimmed().toUpper();
    if (grid.isEmpty())
        grid = AppSettings::instance().value(QStringLiteral("StationGridSquare"), QString()).toString().trimmed().toUpper();
    return grid.left(6);
}

void RttyDialog::refreshReportingControls()
{
    const auto& settings = AppSettings::instance();
    {
        QSignalBlocker blocker(m_spotCheck);
        m_spotCheck->setChecked(
            settings.value(spotEnabledSettingKey(QStringLiteral("RTTY")), QStringLiteral("False")).toString()
            == QStringLiteral("True"));
    }
    {
        QSignalBlocker blocker(m_pskReporterCheck);
        m_pskReporterCheck->setChecked(
            settings.value(reporterEnabledSettingKey(QStringLiteral("RTTY")), QStringLiteral("False")).toString()
            == QStringLiteral("True"));
    }
}

void RttyDialog::maybeEmitReceptionReport(const QString& text)
{
    const auto report = parseTextReception(text,
                                           m_radioModel->callsign().trimmed().toUpper(),
                                           receiverGrid(),
                                           static_cast<quint64>(
                                               std::llround(m_frequencySpin->value() * 1000.0 * 1000.0)
                                               + m_markSpin->value()),
                                           QStringLiteral("RTTY"));
    if (report.has_value())
        emit receptionReportReady(*report);
}

} // namespace AetherSDR
