#include "PskDialog.h"

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
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kTxChunkFrames = 480;

struct PskBandPreset {
    const char* label;
    double mhz;
};

struct PskModePreset {
    const char* label;
    double baud;
    bool qpsk;
};

constexpr std::array<PskBandPreset, 16> kBandPresets{{
    {"160m (EU)", 1.838},
    {"160m (US)", 1.807},
    {"80m", 3.580},
    {"40m", 7.070},
    {"40m (EU)", 7.035},
    {"40m (JP)", 7.040},
    {"30m", 10.142},
    {"20m", 14.070},
    {"17m", 18.100},
    {"15m", 21.070},
    {"12m", 24.920},
    {"10m", 28.120},
    {"6m (EU)", 50.290},
    {"6m (US)", 50.290},
    {"2m", 144.144},
    {"70cm", 432.200},
}};

constexpr std::array<PskModePreset, 6> kModePresets{{
    {"BPSK-31", 31.25, false},
    {"BPSK-63", 63.0, false},
    {"QPSK-31", 31.25, true},
    {"QPSK-63", 63.0, true},
    {"BPSK-125", 125.0, false},
    {"QPSK-125", 125.0, true},
}};

QString boxedStyle(const QString& bg)
{
    return QString(
        "QLabel { background: %1; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; min-width: 44px; }").arg(bg);
}

QString formatBaudValue(double baud)
{
    QString text = QString::number(baud, 'f', 2);
    while (text.contains('.') && text.endsWith('0'))
        text.chop(1);
    if (text.endsWith('.'))
        text.chop(1);
    return text;
}

} // namespace

PskDialog::PskDialog(RadioModel* radioModel, AudioEngine* audio, QWidget* parent)
    : QDialog(parent)
    , m_radioModel(radioModel)
    , m_audio(audio)
{
    setWindowTitle(QStringLiteral("PSK"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1220, 760);

    buildUi();
    loadSettings();
    applySettings();
    updateSliceSummary();
    updateTxControls();

    connect(&m_decoder, &PskDecoder::textDecoded,
            this, &PskDialog::appendDecodedText);
    connect(&m_decoder, &PskDecoder::metricsUpdated,
            this, &PskDialog::updateMetrics);
    connect(this, &PskDialog::txAudioReady,
            m_audio, &AudioEngine::feedDaxTxAudio,
            Qt::QueuedConnection);
    connect(m_radioModel, &RadioModel::radioTransmittingChanged,
            this, &PskDialog::onRadioTransmittingChanged);

    m_txTimer = new QTimer(this);
    m_txTimer->setInterval(20);
    m_txTimer->setTimerType(Qt::PreciseTimer);
    connect(m_txTimer, &QTimer::timeout, this, &PskDialog::pumpTxAudio);

    auto* sendShortcut1 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    auto* sendShortcut2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), this);
    connect(sendShortcut1, &QShortcut::activated, this, &PskDialog::onSendClicked);
    connect(sendShortcut2, &QShortcut::activated, this, &PskDialog::onSendClicked);
}

PskDialog::~PskDialog()
{
    // PTT is released in hideEvent(): the dialog is WA_DeleteOnClose, and at
    // application shutdown RadioModel is already destroyed when we get here.
    saveSettings();
}

void PskDialog::hideEvent(QHideEvent* event)
{
    // A spontaneous hide (minimise) keeps transmitting; closing the window
    // must not leave the radio keyed with nothing feeding DAX TX audio.
    if (!event->spontaneous() && (m_txKeyRequested || m_txStartPending || m_txTimer->isActive())) {
        stopTransmissionNow();
    }
    QDialog::hideEvent(event);
}

void PskDialog::buildUi()
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

    grid->addWidget(new QLabel(QStringLiteral("Freq:"), controls), 0, 1);
    m_bandCombo = new QComboBox(controls);
    m_bandCombo->addItem(QStringLiteral("Manual"));
    for (const auto& preset : kBandPresets)
        m_bandCombo->addItem(QString::fromLatin1(preset.label));
    grid->addWidget(m_bandCombo, 0, 2);

    m_frequencySpin = new QDoubleSpinBox(controls);
    m_frequencySpin->setDecimals(3);
    m_frequencySpin->setRange(0.1, 5000.0);
    m_frequencySpin->setSuffix(QStringLiteral(" MHz"));
    m_frequencySpin->setSingleStep(0.001);
    grid->addWidget(m_frequencySpin, 0, 3);

    grid->addWidget(new QLabel(QStringLiteral("Mode:"), controls), 1, 1);
    m_modeCombo = new QComboBox(controls);
    m_modeCombo->addItem(QStringLiteral("Manual"));
    for (const auto& preset : kModePresets)
        m_modeCombo->addItem(QString::fromLatin1(preset.label));
    grid->addWidget(m_modeCombo, 1, 2);

    m_manualConstellationCombo = new QComboBox(controls);
    m_manualConstellationCombo->addItem(QStringLiteral("BPSK"));
    m_manualConstellationCombo->addItem(QStringLiteral("QPSK"));
    grid->addWidget(m_manualConstellationCombo, 1, 3);

    m_manualBaudSpin = new QDoubleSpinBox(controls);
    m_manualBaudSpin->setDecimals(2);
    m_manualBaudSpin->setRange(10.0, 250.0);
    m_manualBaudSpin->setSingleStep(0.25);
    m_manualBaudSpin->setSuffix(QStringLiteral(" baud"));
    grid->addWidget(m_manualBaudSpin, 1, 4);

    grid->addWidget(new QLabel(QStringLiteral("SQL:"), controls), 0, 4);
    m_sqlSlider = new QSlider(Qt::Horizontal, controls);
    m_sqlSlider->setRange(0, 100);
    grid->addWidget(m_sqlSlider, 0, 5, 1, 2);

    m_confidenceBar = new QProgressBar(controls);
    m_confidenceBar->setRange(0, 100);
    m_confidenceBar->setTextVisible(false);
    grid->addWidget(m_confidenceBar, 1, 5, 1, 2);

    grid->addWidget(new QLabel(QStringLiteral("RX:"), controls), 1, 3);
    m_rxOffsetSpin = new QSpinBox(controls);
    m_rxOffsetSpin->setRange(95, 4000);
    m_rxOffsetSpin->setSuffix(QStringLiteral(" Hz"));
    grid->addWidget(m_rxOffsetSpin, 1, 4);

    grid->addWidget(new QLabel(QStringLiteral("TX:"), controls), 1, 7);
    m_txOffsetSpin = new QSpinBox(controls);
    m_txOffsetSpin->setRange(95, 4000);
    m_txOffsetSpin->setSuffix(QStringLiteral(" Hz"));
    grid->addWidget(m_txOffsetSpin, 1, 8);

    m_copyTxToRxButton = new QPushButton(QStringLiteral("=TX"), controls);
    grid->addWidget(m_copyTxToRxButton, 1, 9);
    m_copyRxToTxButton = new QPushButton(QStringLiteral("=RX"), controls);
    grid->addWidget(m_copyRxToTxButton, 1, 10);

    m_lockOffsetsCheck = new QCheckBox(QStringLiteral("Lock"), controls);
    grid->addWidget(m_lockOffsetsCheck, 0, 8);

    m_metricsLabel = new QLabel(QStringLiteral("Signal -120 dB | Timing 0%"), controls);
    grid->addWidget(m_metricsLabel, 0, 9, 1, 2);

    m_eraseButton = new QPushButton(QStringLiteral("Erase"), controls);
    m_eraseButton->setStyleSheet(
        "QPushButton { background: #0d3b84; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; }");
    grid->addWidget(m_eraseButton, 0, 11, 2, 1);

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
    m_txEdit->setPlaceholderText(QStringLiteral("Compose outgoing PSK text here. Ctrl+Enter sends."));
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
    txBadge->setStyleSheet(boxedStyle("#0d3b84"));
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
    m_spotCheck->setToolTip(QStringLiteral("Create native spots for the current PSK mode."));
    txGrid->addWidget(m_spotCheck, 0, 4);

    m_pskReporterCheck = new QCheckBox(QStringLiteral("PSK Reporter"), txPanel);
    m_pskReporterCheck->setToolTip(QStringLiteral("Upload PSK reception reports for the current mode."));
    txGrid->addWidget(m_pskReporterCheck, 0, 5);

    m_pendingLabel = new QLabel(QStringLiteral("Queue: 0 symbols"), txPanel);
    txGrid->addWidget(m_pendingLabel, 0, 6);

    txGrid->addWidget(new QLabel(QStringLiteral("DX:"), txPanel), 0, 7);
    m_dxEdit = new QLineEdit(txPanel);
    txGrid->addWidget(m_dxEdit, 0, 8);
    txGrid->addWidget(new QLabel(QStringLiteral("RST Received:"), txPanel), 0, 9);
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

    connect(m_bandCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &PskDialog::applyBandPreset);
    connect(m_frequencySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &PskDialog::onFrequencyEdited);
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &PskDialog::applyModePreset);
    connect(m_manualConstellationCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &PskDialog::applySettings);
    connect(m_manualBaudSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &PskDialog::applySettings);
    // Squelch is decoder-only: it must not retune or re-mode the slice.
    connect(m_sqlSlider, &QSlider::valueChanged, this, [this](int value) {
        m_decoder.setSquelch(static_cast<float>(value) / 100.0f);
    });
    connect(m_rxOffsetSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &PskDialog::onOffsetChanged);
    connect(m_txOffsetSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &PskDialog::onOffsetChanged);
    connect(m_lockOffsetsCheck, &QCheckBox::toggled,
            this, &PskDialog::applySettings);
    connect(m_copyTxToRxButton, &QPushButton::clicked,
            this, &PskDialog::copyTxToRxOffset);
    connect(m_copyRxToTxButton, &QPushButton::clicked,
            this, &PskDialog::copyRxToTxOffset);
    connect(m_eraseButton, &QPushButton::clicked,
            this, &PskDialog::clearRxText);
    connect(m_sendButton, &QPushButton::clicked,
            this, &PskDialog::onSendClicked);
    connect(m_txButton, &QPushButton::toggled,
            this, &PskDialog::onTxToggled);
    connect(m_logButton, &QPushButton::clicked,
            this, &PskDialog::logContact);
    connect(m_txEdit, &QPlainTextEdit::textChanged,
            this, &PskDialog::updateTxControls);
    connect(m_spotCheck, &QCheckBox::toggled, this, [this](bool on) {
        AppSettings::instance().setValue(spotEnabledSettingKey(effectiveReportMode()),
                                         on ? QStringLiteral("True") : QStringLiteral("False"));
        AppSettings::instance().save();
    });
    connect(m_pskReporterCheck, &QCheckBox::toggled, this, [this](bool on) {
        AppSettings::instance().setValue(reporterEnabledSettingKey(effectiveReportMode()),
                                         on ? QStringLiteral("True") : QStringLiteral("False"));
        AppSettings::instance().save();
    });

    updateStatus(QStringLiteral("PSK decoder ready."));
}

void PskDialog::setActiveSlice(SliceModel* slice)
{
    if (slice == m_activeSlice)
        return;
    m_activeSlice = slice;
    updateSliceConnections(slice);
    syncControlsFromSlice();
    updateSliceSummary();
}

void PskDialog::feedAudio(const QByteArray& pcm24kStereo)
{
    m_decoder.feedAudio(pcm24kStereo);
}

void PskDialog::appendDecodedText(const QString& text)
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

void PskDialog::applyBandPreset(int index)
{
    if (m_syncingControls || index <= 0)
        return;
    m_frequencySpin->setValue(kBandPresets[static_cast<size_t>(index - 1)].mhz);
    applySettings();
}

void PskDialog::applyModePreset(int)
{
    if (m_syncingControls)
        return;

    if (m_modeCombo->currentIndex() > 0) {
        const auto& preset =
            kModePresets[static_cast<size_t>(m_modeCombo->currentIndex() - 1)];
        {
            QSignalBlocker blocker1(m_manualConstellationCombo);
            QSignalBlocker blocker2(m_manualBaudSpin);
            m_manualConstellationCombo->setCurrentIndex(preset.qpsk ? 1 : 0);
            m_manualBaudSpin->setValue(preset.baud);
        }
    }

    applySettings();
}

void PskDialog::applySettings()
{
    const double baud = effectiveBaud();
    const bool qpsk = effectiveQpsk();
    updateModeControls();
    refreshReportingControls();

    if (m_lockOffsetsCheck->isChecked() && m_rxOffsetSpin->value() != m_txOffsetSpin->value()) {
        QSignalBlocker blocker(m_txOffsetSpin);
        m_txOffsetSpin->setValue(m_rxOffsetSpin->value());
    }

    m_decoder.setBaud(baud);
    m_decoder.setQpsk(qpsk);
    m_decoder.setRxOffsetHz(m_rxOffsetSpin->value());
    m_decoder.setSquelch(static_cast<float>(m_sqlSlider->value()) / 100.0f);

    m_modulator.setBaud(baud);
    m_modulator.setQpsk(qpsk);
    m_modulator.setTxOffsetHz(m_txOffsetSpin->value());

    syncPresetSelections();

    if (!m_activeSlice)
        return;

    const int lo = std::max(95, std::min(m_rxOffsetSpin->value(), m_txOffsetSpin->value()) - 180);
    const int hi = std::max(lo + 60, std::max(m_rxOffsetSpin->value(), m_txOffsetSpin->value()) + 180);
    const int midpoint = (lo + hi) / 2;

    m_activeSlice->setFrequency(m_frequencySpin->value());
    if (m_activeSlice->mode() != QStringLiteral("DIGU"))
        m_activeSlice->setMode(QStringLiteral("DIGU"));
    m_activeSlice->setDiguOffset(midpoint);
    m_activeSlice->setFilterWidth(lo, hi);
    updateSliceSummary();
    updateTxControls();
}

void PskDialog::clearRxText()
{
    m_rxText->clear();
    updateStatus(QStringLiteral("PSK receive text cleared."));
}

void PskDialog::copyRxToTxOffset()
{
    m_txOffsetSpin->setValue(m_rxOffsetSpin->value());
    applySettings();
}

void PskDialog::copyTxToRxOffset()
{
    m_rxOffsetSpin->setValue(m_txOffsetSpin->value());
    applySettings();
}

QString PskDialog::csvEscape(const QString& text) const
{
    QString escaped = text;
    escaped.replace('"', QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString PskDialog::logFilePath() const
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(base);
    dir.mkpath(QStringLiteral("AetherSDR"));
    return dir.filePath(QStringLiteral("AetherSDR/psk-log.csv"));
}

void PskDialog::logContact()
{
    QFile file(logFilePath());
    const bool newFile = !file.exists();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        updateStatus(QStringLiteral("Unable to open PSK log file for writing."));
        return;
    }

    QTextStream ts(&file);
    if (newFile)
        ts << "timestamp_local,frequency_mhz,mode,dx,rst,rx_offset_hz,tx_offset_hz,rx_excerpt\n";

    QString rxExcerpt = m_rxText->toPlainText().right(240);
    rxExcerpt.replace('\n', ' ');

       ts << csvEscape(QDateTime::currentDateTime().toString(Qt::ISODate)) << ','
       << csvEscape(QString::number(m_frequencySpin->value(), 'f', 3)) << ','
       << csvEscape(effectiveModeLabel()) << ','
       << csvEscape(m_dxEdit->text().trimmed().toUpper()) << ','
       << csvEscape(m_rstEdit->text().trimmed()) << ','
       << csvEscape(QString::number(m_rxOffsetSpin->value())) << ','
       << csvEscape(QString::number(m_txOffsetSpin->value())) << ','
       << csvEscape(rxExcerpt)
       << '\n';

    updateStatus(QStringLiteral("Logged PSK contact to %1").arg(file.fileName()));
}

void PskDialog::onFrequencyEdited(double)
{
    if (m_syncingControls)
        return;
    syncPresetSelections();
    applySettings();
}

void PskDialog::onOffsetChanged(int)
{
    if (m_syncingControls)
        return;
    if (sender() == m_rxOffsetSpin && m_lockOffsetsCheck->isChecked()) {
        QSignalBlocker blocker(m_txOffsetSpin);
        m_txOffsetSpin->setValue(m_rxOffsetSpin->value());
    }
    applySettings();
}

void PskDialog::onSendClicked()
{
    queueEditorText();
}

void PskDialog::queueEditorText()
{
    const QString text = m_txEdit->toPlainText();
    if (text.trimmed().isEmpty()) {
        updateStatus(QStringLiteral("Nothing queued for PSK transmit."));
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
    } else {
        updateStatus(QStringLiteral("Queued PSK text for transmit."));
    }
}

void PskDialog::onTxToggled(bool on)
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
        updateStatus(QStringLiteral("PSK TX will stop after queued text drains."));
}

void PskDialog::startTransmission()
{
    if (!m_activeSlice) {
        updateStatus(QStringLiteral("Select an active slice before transmitting PSK."));
        QSignalBlocker blocker(m_txButton);
        m_txButton->setChecked(false);
        return;
    }

    applySettings();
    if (!m_activeSlice->isTxSlice())
        m_activeSlice->setTxSlice(true);
    if (m_activeSlice->mode() != QStringLiteral("DIGU"))
        m_activeSlice->setMode(QStringLiteral("DIGU"));

    m_txStartPending = true;
    m_drainGuardChunks = 0;
    m_txKeyRequested = true;
    m_radioModel->setTransmit(true);
    updateStatus(QStringLiteral("Keying PSK transmitter..."));
    updateTxControls();
}

void PskDialog::stopTransmissionNow()
{
    m_txTimer->stop();
    m_txStartPending = false;
    m_manualTxHold = false;
    m_stopAfterDrain = false;
    m_drainGuardChunks = 0;
    m_modulator.clearQueue();
    m_modulator.reset();

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

    updateStatus(QStringLiteral("PSK TX stopped."));
    updateTxControls();
}

void PskDialog::onRadioTransmittingChanged(bool on)
{
    if (on) {
        if (m_txStartPending || m_txButton->isChecked()) {
            m_txStartPending = false;
            if (!m_txTimer->isActive())
                m_txTimer->start();
            updateStatus(QStringLiteral("PSK transmitter active."));
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

void PskDialog::pumpTxAudio()
{
    if (!m_radioModel->transmitModel().isMox() && !m_txStartPending)
        return;

    emit txAudioReady(m_modulator.renderAudio(kTxChunkFrames));
    updateTxControls();

    if (m_stopAfterDrain && !m_manualTxHold && !m_modulator.hasPendingData()) {
        ++m_drainGuardChunks;
        if (m_drainGuardChunks >= 3)
            stopTransmissionNow();
    } else {
        m_drainGuardChunks = 0;
    }
}

void PskDialog::syncControlsFromSlice()
{
    if (!m_activeSlice)
        return;

    m_syncingControls = true;
    m_frequencySpin->setValue(m_activeSlice->frequency());
    if (m_activeSlice->diguOffset() > 0) {
        m_rxOffsetSpin->setValue(m_activeSlice->diguOffset());
        if (m_lockOffsetsCheck->isChecked())
            m_txOffsetSpin->setValue(m_activeSlice->diguOffset());
    }
    syncPresetSelections();
    m_syncingControls = false;
}

void PskDialog::updateMetrics(float signalLevelDb, float confidence, float timingQuality, bool locked)
{
    m_confidenceBar->setValue(static_cast<int>(std::clamp(confidence, 0.0f, 1.0f) * 100.0f));
    m_metricsLabel->setText(
        QStringLiteral("Signal %1 dB | Timing %2% | %3")
            .arg(signalLevelDb, 0, 'f', 1)
            .arg(static_cast<int>(std::clamp(timingQuality, 0.0f, 1.0f) * 100.0f))
            .arg(locked ? QStringLiteral("SYNC") : QStringLiteral("SEARCH")));
}

void PskDialog::updateSliceConnections(SliceModel* slice)
{
    if (m_sliceFrequencyConnection)
        disconnect(m_sliceFrequencyConnection);
    if (m_sliceModeConnection)
        disconnect(m_sliceModeConnection);
    if (m_sliceOffsetConnection)
        disconnect(m_sliceOffsetConnection);

    if (!slice)
        return;

    m_sliceFrequencyConnection = connect(slice, &SliceModel::frequencyChanged,
                                         this, [this](double) { syncControlsFromSlice(); });
    m_sliceModeConnection = connect(slice, &SliceModel::modeChanged,
                                    this, [this](const QString&) { updateSliceSummary(); });
    m_sliceOffsetConnection = connect(slice, &SliceModel::diguOffsetChanged,
                                      this, [this](int) { syncControlsFromSlice(); });
}

void PskDialog::updateSliceSummary()
{
    if (!m_activeSlice) {
        m_sliceSummaryLabel->setText(
            QStringLiteral("No active slice selected. PSK controls follow the active slice."));
        return;
    }

    m_sliceSummaryLabel->setText(
        QStringLiteral("Active slice %1 | %2 MHz | %3 | RX %4 Hz | TX %5 Hz")
            .arg(QChar('A' + std::clamp(m_activeSlice->sliceId(), 0, 25)))
            .arg(m_activeSlice->frequency(), 0, 'f', 3)
            .arg(effectiveModeLabel())
            .arg(m_rxOffsetSpin->value())
            .arg(m_txOffsetSpin->value()));
}

void PskDialog::updateStatus(const QString& text)
{
    m_statusLabel->setText(text);
}

void PskDialog::updateTxControls()
{
    const bool hasText = !m_txEdit->toPlainText().trimmed().isEmpty();
    m_pendingLabel->setText(QStringLiteral("Queue: %1 symbols").arg(m_modulator.pendingSymbols()));
    m_sendButton->setEnabled(hasText || m_radioModel->transmitModel().isMox());
}

void PskDialog::syncPresetSelections()
{
    int bandIndex = 0;
    for (int i = 0; i < static_cast<int>(kBandPresets.size()); ++i) {
        if (std::abs(kBandPresets[static_cast<size_t>(i)].mhz - m_frequencySpin->value()) <= 0.003) {
            bandIndex = i + 1;
            break;
        }
    }

    QSignalBlocker blocker(m_bandCombo);
    m_bandCombo->setCurrentIndex(bandIndex);
}

void PskDialog::loadSettings()
{
    auto& s = AppSettings::instance();
    restoreGeometry(s.value("PskDialogGeometry").toByteArray());

    m_frequencySpin->setValue(s.value("PskFrequencyMHz", 14.070).toDouble());
    m_manualConstellationCombo->setCurrentIndex(
        std::clamp(s.value("PskManualConstellationIndex", 0).toInt(), 0, 1));
    m_manualBaudSpin->setValue(s.value("PskManualBaud", 31.25).toDouble());
    m_modeCombo->setCurrentIndex(
        std::clamp(s.value("PskModeIndex", 1).toInt(), 0, static_cast<int>(kModePresets.size())));
    m_sqlSlider->setValue(s.value("PskSquelch", 18).toInt());
    m_rxOffsetSpin->setValue(s.value("PskRxOffsetHz", 2000).toInt());
    m_txOffsetSpin->setValue(s.value("PskTxOffsetHz", 2000).toInt());
    m_lockOffsetsCheck->setChecked(s.value("PskLockOffsets", true).toBool());
    m_autoTxCheck->setChecked(s.value("PskAutoTx", true).toBool());
    m_dxEdit->setText(s.value("PskLastDx").toString());
    m_rstEdit->setText(s.value("PskLastRst").toString());
    syncPresetSelections();
    refreshReportingControls();
}

void PskDialog::saveSettings() const
{
    auto& s = AppSettings::instance();
    s.setValue("PskDialogGeometry", saveGeometry());
    s.setValue("PskFrequencyMHz", m_frequencySpin->value());
    s.setValue("PskModeIndex", m_modeCombo->currentIndex());
    s.setValue("PskManualConstellationIndex", m_manualConstellationCombo->currentIndex());
    s.setValue("PskManualBaud", m_manualBaudSpin->value());
    s.setValue("PskSquelch", m_sqlSlider->value());
    s.setValue("PskRxOffsetHz", m_rxOffsetSpin->value());
    s.setValue("PskTxOffsetHz", m_txOffsetSpin->value());
    s.setValue("PskLockOffsets", m_lockOffsetsCheck->isChecked());
    s.setValue("PskAutoTx", m_autoTxCheck->isChecked());
    s.setValue("PskLastDx", m_dxEdit->text());
    s.setValue("PskLastRst", m_rstEdit->text());
    s.save();
}

double PskDialog::effectiveBaud() const
{
    if (m_modeCombo->currentIndex() > 0)
        return kModePresets[static_cast<size_t>(m_modeCombo->currentIndex() - 1)].baud;
    return m_manualBaudSpin->value();
}

QString PskDialog::effectiveModeLabel() const
{
    return QStringLiteral("%1-%2")
        .arg(effectiveQpsk() ? QStringLiteral("QPSK") : QStringLiteral("BPSK"))
        .arg(formatBaudValue(effectiveBaud()));
}

QString PskDialog::effectiveReportMode() const
{
    const int roundedBaud = std::clamp(static_cast<int>(std::lround(effectiveBaud())), 1, 999);
    return effectiveQpsk()
               ? QStringLiteral("QPSK%1").arg(roundedBaud)
               : QStringLiteral("PSK%1").arg(roundedBaud);
}

bool PskDialog::effectiveQpsk() const
{
    if (m_modeCombo->currentIndex() > 0)
        return kModePresets[static_cast<size_t>(m_modeCombo->currentIndex() - 1)].qpsk;
    return m_manualConstellationCombo->currentIndex() == 1;
}

void PskDialog::updateModeControls()
{
    const bool manual = m_modeCombo->currentIndex() == 0;
    m_manualConstellationCombo->setEnabled(manual);
    m_manualBaudSpin->setEnabled(manual);
}

QString PskDialog::receiverGrid() const
{
    QString grid = m_radioModel->gpsGrid().trimmed().toUpper();
    if (grid.isEmpty())
        grid = AppSettings::instance().value(QStringLiteral("StationGridSquare"), QString()).toString().trimmed().toUpper();
    return grid.left(6);
}

void PskDialog::refreshReportingControls()
{
    const auto& settings = AppSettings::instance();
    const QString modeName = effectiveReportMode();
    {
        QSignalBlocker blocker(m_spotCheck);
        m_spotCheck->setChecked(
            settings.value(spotEnabledSettingKey(modeName), QStringLiteral("False")).toString()
            == QStringLiteral("True"));
    }
    {
        QSignalBlocker blocker(m_pskReporterCheck);
        m_pskReporterCheck->setChecked(
            settings.value(reporterEnabledSettingKey(modeName), QStringLiteral("False")).toString()
            == QStringLiteral("True"));
    }
}

void PskDialog::maybeEmitReceptionReport(const QString& text)
{
    const auto report = parseTextReception(text,
                                           m_radioModel->callsign().trimmed().toUpper(),
                                           receiverGrid(),
                                           static_cast<quint64>(
                                               std::llround(m_frequencySpin->value() * 1000000.0)
                                               + m_rxOffsetSpin->value()),
                                           effectiveReportMode());
    if (report.has_value())
        emit receptionReportReady(*report);
}

} // namespace AetherSDR
