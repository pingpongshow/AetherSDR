#include "WeakSignalDialog.h"

#include "DigitalModeStyles.h"
#include "WeakSignalMapDialog.h"

#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/WeakSignalTxCodec.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QDateTime>
#include <QTimeZone>

#include <array>
#include <cmath>

namespace AetherSDR {

namespace {

struct PresetDef {
    const char* label;
    double khz;
};

constexpr std::array<PresetDef, 24> kFt8Presets{{
    {"160m (1840.0 kHz)", 1840.0},
    {"80m Region 3 (3531.0 kHz)", 3531.0},
    {"80m (3573.0 kHz)", 3573.0},
    {"60m (5357.0 kHz)", 5357.0},
    {"40m Region 3 (7041.0 kHz)", 7041.0},
    {"40m (7074.0 kHz)", 7074.0},
    {"30m (10136.0 kHz)", 10136.0},
    {"20m (14074.0 kHz)", 14074.0},
    {"17m (18100.0 kHz)", 18100.0},
    {"15m (21074.0 kHz)", 21074.0},
    {"12m (24915.0 kHz)", 24915.0},
    {"10m (28074.0 kHz)", 28074.0},
    {"6m (50313.0 kHz)", 50313.0},
    {"4m Region 1 (70100.0 kHz)", 70100.0},
    {"4m (70154.0 kHz)", 70154.0},
    {"2m (144174.0 kHz)", 144174.0},
    {"2m Region 3 (144460.0 kHz)", 144460.0},
    {"1.25m (222065.0 kHz)", 222065.0},
    {"70cm Region 3 (430510.0 kHz)", 430510.0},
    {"70cm (432063.0 kHz)", 432063.0},
    {"70cm Region 1 (432174.0 kHz)", 432174.0},
    {"70cm alt (432500.0 kHz)", 432500.0},
    {"23cm (1296174.0 kHz)", 1296174.0},
    {"23cm Region 3 (1296600.0 kHz)", 1296600.0},
}};

constexpr std::array<PresetDef, 9> kFt4Presets{{
    {"80m (3575.0 kHz)", 3575.0},
    {"40m (7047.5 kHz)", 7047.5},
    {"30m (10140.0 kHz)", 10140.0},
    {"20m (14080.0 kHz)", 14080.0},
    {"17m (18104.0 kHz)", 18104.0},
    {"15m (21140.0 kHz)", 21140.0},
    {"12m (24919.0 kHz)", 24919.0},
    {"10m (28180.0 kHz)", 28180.0},
    {"6m (50318.0 kHz)", 50318.0},
}};

constexpr std::array<PresetDef, 10> kJt65Presets{{
    {"160m (1838.0 kHz)", 1838.0},
    {"80m (3570.0 kHz)", 3570.0},
    {"40m (7076.0 kHz)", 7076.0},
    {"30m (10138.0 kHz)", 10138.0},
    {"20m (14076.0 kHz)", 14076.0},
    {"17m (18102.0 kHz)", 18102.0},
    {"15m (21076.0 kHz)", 21076.0},
    {"12m (24917.0 kHz)", 24917.0},
    {"10m (28076.0 kHz)", 28076.0},
    {"6m (50310.0 kHz)", 50310.0},
}};

constexpr std::array<PresetDef, 12> kWsprPresets{{
    {"160m (1836.6 kHz)", 1836.6},
    {"80m (3568.6 kHz)", 3568.6},
    {"60m (5287.2 kHz)", 5287.2},
    {"40m (7038.6 kHz)", 7038.6},
    {"30m (10138.7 kHz)", 10138.7},
    {"20m (14095.6 kHz)", 14095.6},
    {"17m (18104.6 kHz)", 18104.6},
    {"15m (21094.6 kHz)", 21094.6},
    {"12m (24924.6 kHz)", 24924.6},
    {"10m (28124.6 kHz)", 28124.6},
    {"6m (50293.6 kHz)", 50293.6},
    {"2m (144488.5 kHz)", 144488.5},
}};

QString badgeStyle(const QString& background)
{
    return QStringLiteral(
               "QLabel { background: %1; color: white; font-weight: bold; "
               "padding: 10px 18px; border-radius: 4px; min-width: 44px; }")
        .arg(background);
}

QString rxToggleStyle()
{
    return QStringLiteral(
        "QPushButton { background: #646464; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; min-width: 44px; }"
        "QPushButton:checked { background: #0d7a2b; }"
        "QPushButton:disabled { background: #525252; color: #d0d0d0; }");
}

QString actionButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: #0d3b84; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; }"
        "QPushButton:disabled { background: #4a5b75; color: #c8d2df; }");
}

QString modeButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: #b8d3ea; color: #103250; font-weight: bold; "
        "padding: 8px 16px; border-radius: 6px; }"
        "QPushButton:checked { background: #0d3b84; color: white; }");
}

QString formatFrequencyKhz(double khz)
{
    QString text = QString::number(khz, 'f', khz < 100000.0 ? 1 : 0);
    while (text.contains('.') && text.endsWith('0'))
        text.chop(1);
    if (text.endsWith('.'))
        text.chop(1);
    return text;
}

qint64 currentUtcMs()
{
    return QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
}

QString decodeKey(const WeakSignalDialog::DisplayDecode& entry)
{
    return QStringLiteral("%1|%2|%3")
        .arg(entry.decode.utcTime.toString(QStringLiteral("HH:mm:ss.zzz")),
             entry.decode.callsign,
             entry.decode.message);
}

bool isWeakSignalMode(const QString& mode)
{
    const QString upper = mode.trimmed().toUpper();
    return upper == QLatin1String("FT8")
           || upper == QLatin1String("FT4")
           || upper == QLatin1String("JT65")
           || upper == QLatin1String("WSPR");
}

WeakSignalNativeDecoder::Mode decoderModeForUiMode(WeakSignalDialog::Mode mode)
{
    switch (mode) {
    case WeakSignalDialog::Mode::FT4:
        return WeakSignalNativeDecoder::Mode::FT4;
    case WeakSignalDialog::Mode::JT65:
        return WeakSignalNativeDecoder::Mode::JT65;
    case WeakSignalDialog::Mode::WSPR:
        return WeakSignalNativeDecoder::Mode::WSPR;
    case WeakSignalDialog::Mode::FT8:
    default:
        return WeakSignalNativeDecoder::Mode::FT8;
    }
}

} // namespace

WeakSignalDialog::WeakSignalDialog(RadioModel* radioModel,
                                   WeakSignalNativeDecoder* decoder,
                                   AudioEngine* audio,
                                   QWidget* parent)
    : QDialog(parent)
    , m_radioModel(radioModel)
    , m_decoder(decoder)
    , m_audio(audio)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1320, 820);

    m_ctyParser.loadFromResource(QStringLiteral(":/cty.dat"));
    buildUi();
    loadSettings();

    connect(m_decoder, &WeakSignalNativeDecoder::decodeReceived,
            this, &WeakSignalDialog::onDecodeReceived);
    connect(m_decoder, &WeakSignalNativeDecoder::cleared,
            this, &WeakSignalDialog::onCleared);
    connect(m_decoder, &WeakSignalNativeDecoder::listening,
            this, &WeakSignalDialog::onListenerStarted);
    connect(m_decoder, &WeakSignalNativeDecoder::stopped,
            this, &WeakSignalDialog::onListenerStopped);
    connect(m_decoder, &WeakSignalNativeDecoder::statusMessage,
            this, &WeakSignalDialog::updateStatusLine);
    connect(m_radioModel, &RadioModel::gpsStatusChanged,
            this, &WeakSignalDialog::updateHomeStation);
    connect(m_radioModel, &RadioModel::radioTransmittingChanged,
            this, &WeakSignalDialog::onRadioTransmittingChanged);
    connect(this, &WeakSignalDialog::txAudioReady,
            m_audio, &AudioEngine::feedDaxTxAudio,
            Qt::QueuedConnection);

    m_txTimer = new QTimer(this);
    m_txTimer->setInterval(20);
    m_txTimer->setTimerType(Qt::PreciseTimer);
    connect(m_txTimer, &QTimer::timeout, this, &WeakSignalDialog::pumpTxAudio);

    m_slotTimer = new QTimer(this);
    m_slotTimer->setSingleShot(true);
    connect(m_slotTimer, &QTimer::timeout, this, &WeakSignalDialog::armScheduledTransmission);

    m_decoder->setMode(decoderModeForUiMode(currentMode()));
    m_decoder->setDialFrequencyKhz(m_frequencySpin->value());
    updateHomeStation(m_radioModel->gpsStatus(),
                      m_radioModel->gpsTracked(),
                      m_radioModel->gpsVisible(),
                      m_radioModel->gpsGrid(),
                      m_radioModel->gpsAltitude(),
                      m_radioModel->gpsLat(),
                      m_radioModel->gpsLon(),
                      m_radioModel->gpsTime());

    refreshListenerSummary();
    if (m_decoder->isListening())
        updateStatusLine(QStringLiteral("Native weak-signal decoder is listening to panadapter audio."));
    else
        updateStatusLine(QStringLiteral("RX is off. Enable the native decoder to start decoding panadapter audio."));

    if (m_decoder->isListening())
        QTimer::singleShot(100, this, &WeakSignalDialog::requestReplay);
}

WeakSignalDialog::~WeakSignalDialog()
{
    // Do not call stopTransmissionNow() here: at application shutdown this
    // child is destroyed after MainWindow's RadioModel member and after the
    // shared WeakSignalNativeDecoder, both of which it dereferences. PTT and
    // queued slots are released in hideEvent() when the window is closed.
    saveSettings();
}

void WeakSignalDialog::hideEvent(QHideEvent* event)
{
    const bool txBusy = m_txKeyRequested || m_txStartPending || m_txActive
                        || !m_txWaveform.isEmpty()
                        || (m_slotTimer && m_slotTimer->isActive());
    if (!event->spontaneous() && txBusy) {
        stopTransmissionNow(QStringLiteral("%1 transmission cancelled (window closed).")
                                .arg(currentModeLabel()));
    }
    QDialog::hideEvent(event);
}

void WeakSignalDialog::setActiveSlice(SliceModel* slice)
{
    if (slice == m_activeSlice)
        return;

    m_activeSlice = slice;
    // Assigning {} only drops the handle; the old slice would stay connected.
    disconnect(m_sliceFrequencyConnection);
    disconnect(m_sliceModeConnection);

    if (slice) {
        m_sliceFrequencyConnection = connect(slice, &SliceModel::frequencyChanged,
                                            this, &WeakSignalDialog::syncControlsFromSlice);
        m_sliceModeConnection = connect(slice, &SliceModel::modeChanged,
                                        this, &WeakSignalDialog::syncControlsFromSlice);
    }

    syncControlsFromSlice();
}

void WeakSignalDialog::buildUi()
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

    m_rxToggleButton = new QPushButton(QStringLiteral("RX"), controls);
    m_rxToggleButton->setCheckable(true);
    m_rxToggleButton->setStyleSheet(rxToggleStyle());
    m_rxToggleButton->setToolTip(QStringLiteral(
        "Start or stop the native weak-signal decoder.\n"
        "This dialog decodes panadapter audio in-process and no longer depends on WSJT-X."));
    grid->addWidget(m_rxToggleButton, 0, 0, 2, 1);

    grid->addWidget(new QLabel(QStringLiteral("Freq:"), controls), 0, 1);
    m_frequencyCombo = new QComboBox(controls);
    grid->addWidget(m_frequencyCombo, 0, 2);

    m_frequencySpin = new QDoubleSpinBox(controls);
    m_frequencySpin->setDecimals(1);
    m_frequencySpin->setRange(100.0, 2500000.0);
    m_frequencySpin->setSuffix(QStringLiteral(" kHz"));
    m_frequencySpin->setSingleStep(0.1);
    grid->addWidget(m_frequencySpin, 0, 3);

    grid->addWidget(new QLabel(QStringLiteral("Mode:"), controls), 0, 4);
    m_ft8Button = new QPushButton(QStringLiteral("FT8"), controls);
    m_ft4Button = new QPushButton(QStringLiteral("FT4"), controls);
    m_jt65Button = new QPushButton(QStringLiteral("JT65"), controls);
    m_wsprButton = new QPushButton(QStringLiteral("WSPR"), controls);
    m_ft8Button->setCheckable(true);
    m_ft4Button->setCheckable(true);
    m_jt65Button->setCheckable(true);
    m_wsprButton->setCheckable(true);
    m_ft8Button->setStyleSheet(modeButtonStyle());
    m_ft4Button->setStyleSheet(modeButtonStyle());
    m_jt65Button->setStyleSheet(modeButtonStyle());
    m_wsprButton->setStyleSheet(modeButtonStyle());

    auto* modeRow = new QHBoxLayout;
    modeRow->setContentsMargins(0, 0, 0, 0);
    modeRow->setSpacing(6);
    modeRow->addWidget(m_ft8Button);
    modeRow->addWidget(m_ft4Button);
    modeRow->addWidget(m_jt65Button);
    modeRow->addWidget(m_wsprButton);
    modeRow->addStretch();
    auto* modeWrap = new QWidget(controls);
    modeWrap->setLayout(modeRow);
    grid->addWidget(modeWrap, 0, 5, 1, 5);

    m_modeGroup = new QButtonGroup(this);
    m_modeGroup->setExclusive(true);
    m_modeGroup->addButton(m_ft8Button, static_cast<int>(Mode::FT8));
    m_modeGroup->addButton(m_ft4Button, static_cast<int>(Mode::FT4));
    m_modeGroup->addButton(m_jt65Button, static_cast<int>(Mode::JT65));
    m_modeGroup->addButton(m_wsprButton, static_cast<int>(Mode::WSPR));

    m_cqOnlyCheck = new QCheckBox(QStringLiteral("Filter CQ/Beacon only"), controls);
    grid->addWidget(m_cqOnlyCheck, 1, 5);

    m_spotCheck = new QCheckBox(QStringLiteral("Spot"), controls);
    m_spotCheck->setToolTip(QStringLiteral("Create native spots for the current mode."));
    grid->addWidget(m_spotCheck, 1, 6);

    m_pskReporterCheck = new QCheckBox(QStringLiteral("PSK Reporter"), controls);
    m_pskReporterCheck->setToolTip(QStringLiteral("Upload reception reports for the current mode."));
    grid->addWidget(m_pskReporterCheck, 1, 7);

    m_listenerLabel = new QLabel(QStringLiteral("Native weak-signal decoder stopped"), controls);
    m_listenerLabel->setStyleSheet(QStringLiteral("QLabel { color: #f5f7fa; font-weight: 600; }"));
    grid->addWidget(m_listenerLabel, 1, 1, 1, 4);

    m_sliceSummaryLabel = new QLabel(QStringLiteral("No active slice selected."), controls);
    m_sliceSummaryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(m_sliceSummaryLabel, 1, 8, 1, 2);

    root->addWidget(controls);

    auto* tablesSplitter = new QSplitter(Qt::Horizontal, this);
    tablesSplitter->setChildrenCollapsible(false);

    auto tableStyle = QStringLiteral(
        "QTableWidget { background: #1f1f1f; color: #f6f9fc; border: 1px solid #303030; "
        "alternate-background-color: #252525; }"
        "QHeaderView::section { background: #121212; color: #dce7f3; padding: 6px; border: none; }"
        "QTableWidget::item:selected { background: #274b75; color: white; }");

    auto configureDecodeTable = [&](QTableWidget* table, bool showCountryColumn) {
        table->setColumnCount(showCountryColumn ? 6 : 5);
        table->setHorizontalHeaderLabels(
            showCountryColumn
                ? QStringList{QStringLiteral("UTC"), QStringLiteral("dB"), QStringLiteral("DT"),
                              QStringLiteral("Freq"), QStringLiteral("Message"), QStringLiteral("Country")}
                : QStringList{QStringLiteral("UTC"), QStringLiteral("dB"), QStringLiteral("DT"),
                              QStringLiteral("Freq"), QStringLiteral("Message")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setStretchLastSection(false);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        if (showCountryColumn)
            table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        table->setStyleSheet(tableStyle);
    };

    auto* bandActivityPage = new QWidget(this);
    auto* bandLayout = new QVBoxLayout(bandActivityPage);
    bandLayout->setContentsMargins(0, 0, 0, 0);
    bandLayout->setSpacing(6);
    auto* bandTitle = new QLabel(QStringLiteral("Band Activity"), bandActivityPage);
    bandTitle->setStyleSheet(QStringLiteral("QLabel { color: #dce7f3; font-weight: 700; font-size: 14px; }"));
    bandLayout->addWidget(bandTitle);
    m_bandActivityTable = new QTableWidget(bandActivityPage);
    configureDecodeTable(m_bandActivityTable, true);
    bandLayout->addWidget(m_bandActivityTable, 1);

    auto* rxFrequencyPage = new QWidget(this);
    auto* rxLayout = new QVBoxLayout(rxFrequencyPage);
    rxLayout->setContentsMargins(0, 0, 0, 0);
    rxLayout->setSpacing(6);
    auto* rxHeaderRow = new QHBoxLayout;
    rxHeaderRow->setContentsMargins(0, 0, 0, 0);
    rxHeaderRow->setSpacing(8);
    auto* rxTitle = new QLabel(QStringLiteral("Rx Frequency"), rxFrequencyPage);
    rxTitle->setStyleSheet(QStringLiteral("QLabel { color: #dce7f3; font-weight: 700; font-size: 14px; }"));
    rxHeaderRow->addWidget(rxTitle);
    m_rxFrequencyLabel = new QLabel(QStringLiteral("No focus frequency selected"), rxFrequencyPage);
    m_rxFrequencyLabel->setStyleSheet(QStringLiteral("QLabel { color: #9db3c7; }"));
    rxHeaderRow->addWidget(m_rxFrequencyLabel);
    rxHeaderRow->addStretch();
    rxLayout->addLayout(rxHeaderRow);
    m_rxFrequencyTable = new QTableWidget(rxFrequencyPage);
    configureDecodeTable(m_rxFrequencyTable, false);
    rxLayout->addWidget(m_rxFrequencyTable, 1);

    tablesSplitter->addWidget(bandActivityPage);
    tablesSplitter->addWidget(rxFrequencyPage);
    tablesSplitter->setStretchFactor(0, 1);
    tablesSplitter->setStretchFactor(1, 1);
    root->addWidget(tablesSplitter, 1);

    auto* actionBar = new QWidget(this);
    auto* actionLayout = new QHBoxLayout(actionBar);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(8);

    m_stopButton = new QPushButton(QStringLiteral("Stop"), actionBar);
    m_monitorButton = new QPushButton(QStringLiteral("Monitor"), actionBar);
    m_decodeButton = new QPushButton(QStringLiteral("Decode"), actionBar);
    auto* eraseButton = new QPushButton(QStringLiteral("Erase"), actionBar);
    m_tuneButton = new QPushButton(QStringLiteral("Tune Slice"), actionBar);
    auto* mapButton = new QPushButton(QStringLiteral("Map"), actionBar);
    auto* panButton = new QPushButton(QStringLiteral("Panadapter"), actionBar);
    for (QPushButton* button : {m_stopButton, m_monitorButton, m_decodeButton,
                                eraseButton, m_tuneButton, mapButton, panButton}) {
        button->setStyleSheet(actionButtonStyle());
        actionLayout->addWidget(button);
    }
    actionLayout->addStretch();
    root->addWidget(actionBar);

    auto* txPanel = new QWidget(this);
    txPanel->setStyleSheet(DigitalModeStyles::txPanelStyle());
    auto* txGrid = new QGridLayout(txPanel);
    txGrid->setContentsMargins(12, 12, 12, 12);
    txGrid->setHorizontalSpacing(10);
    txGrid->setVerticalSpacing(8);

    auto* txBadge = new QLabel(QStringLiteral("TX"), txPanel);
    txBadge->setStyleSheet(badgeStyle(QStringLiteral("#646464")));
    txGrid->addWidget(txBadge, 0, 0, 3, 1);

    m_bigDialLabel = new QLabel(QStringLiteral("14.074 000"), txPanel);
    m_bigDialLabel->setAlignment(Qt::AlignCenter);
    m_bigDialLabel->setStyleSheet(
        QStringLiteral("QLabel { background: #0a0a0a; color: #ffe24a; border: 1px solid #202020; "
                       "font-family: Menlo, Consolas, monospace; font-size: 28px; font-weight: 700; "
                       "padding: 12px 18px; min-width: 210px; }"));
    txGrid->addWidget(m_bigDialLabel, 0, 1, 1, 2);

    auto* contextLabel = new QLabel(QStringLiteral("Native weak-signal compose"), txPanel);
    contextLabel->setStyleSheet(QStringLiteral("QLabel { color: #f4f7fb; font-weight: 600; }"));
    txGrid->addWidget(contextLabel, 1, 1, 1, 2);

    txGrid->addWidget(new QLabel(QStringLiteral("DX Call:"), txPanel), 0, 3);
    m_dxEdit = new QLineEdit(txPanel);
    m_dxEdit->setPlaceholderText(QStringLiteral("Target callsign"));
    txGrid->addWidget(m_dxEdit, 0, 4);

    txGrid->addWidget(new QLabel(QStringLiteral("DX Grid:"), txPanel), 0, 5);
    m_dxGridEdit = new QLineEdit(txPanel);
    m_dxGridEdit->setReadOnly(true);
    txGrid->addWidget(m_dxGridEdit, 0, 6);

    txGrid->addWidget(new QLabel(QStringLiteral("RX DF:"), txPanel), 1, 3);
    m_rxDfEdit = new QLineEdit(txPanel);
    m_rxDfEdit->setReadOnly(true);
    txGrid->addWidget(m_rxDfEdit, 1, 4);

    txGrid->addWidget(new QLabel(QStringLiteral("TX DF:"), txPanel), 1, 5);
    m_txDfEdit = new QLineEdit(txPanel);
    m_txDfEdit->setValidator(new QIntValidator(0, 4000, m_txDfEdit));
    m_txDfEdit->setText(QStringLiteral("1500"));
    txGrid->addWidget(m_txDfEdit, 1, 6);

    txGrid->addWidget(new QLabel(QStringLiteral("Report:"), txPanel), 2, 3);
    m_reportEdit = new QLineEdit(txPanel);
    m_reportEdit->setPlaceholderText(QStringLiteral("Signal report or WSPR dBm"));
    txGrid->addWidget(m_reportEdit, 2, 4);

    txGrid->addWidget(new QLabel(QStringLiteral("Free Text:"), txPanel), 2, 5);
    m_messageEdit = new QLineEdit(txPanel);
    m_messageEdit->setPlaceholderText(QStringLiteral("Optional free-text message"));
    txGrid->addWidget(m_messageEdit, 2, 6);

    auto* generatedWrap = new QWidget(txPanel);
    auto* generatedLayout = new QGridLayout(generatedWrap);
    generatedLayout->setContentsMargins(0, 0, 0, 0);
    generatedLayout->setHorizontalSpacing(8);
    generatedLayout->setVerticalSpacing(6);

    auto* generatedTitle = new QLabel(QStringLiteral("Generated Standard Messages"), generatedWrap);
    generatedTitle->setStyleSheet(QStringLiteral("QLabel { color: #f4f7fb; font-weight: 700; }"));
    generatedLayout->addWidget(generatedTitle, 0, 0, 1, 2);

    m_txMessageGroup = new QButtonGroup(this);
    m_txMessageGroup->setExclusive(true);
    for (int i = 0; i < static_cast<int>(m_txMessageEdits.size()); ++i) {
        auto* messageEdit = new QLineEdit(generatedWrap);
        messageEdit->setReadOnly(true);
        messageEdit->setPlaceholderText(QStringLiteral("Message %1").arg(i + 1));
        auto* txButton = new QRadioButton(QStringLiteral("Tx %1").arg(i + 1), generatedWrap);
        m_txMessageEdits[static_cast<size_t>(i)] = messageEdit;
        m_txMessageButtons[static_cast<size_t>(i)] = txButton;
        m_txMessageGroup->addButton(txButton, i);
        generatedLayout->addWidget(messageEdit, i + 1, 0);
        generatedLayout->addWidget(txButton, i + 1, 1);
    }
    if (m_txMessageButtons[0])
        m_txMessageButtons[0]->setChecked(true);
    txGrid->addWidget(generatedWrap, 0, 7, 3, 2);

    m_replyButton = new QPushButton(QStringLiteral("Reply Selected"), txPanel);
    m_sendTextButton = new QPushButton(QStringLiteral("Send Selected"), txPanel);
    m_haltButton = new QPushButton(QStringLiteral("Halt Tx"), txPanel);
    m_replyButton->setStyleSheet(actionButtonStyle());
    m_sendTextButton->setStyleSheet(actionButtonStyle());
    m_haltButton->setStyleSheet(actionButtonStyle());
    txGrid->addWidget(m_replyButton, 0, 9);
    txGrid->addWidget(m_sendTextButton, 1, 9);
    txGrid->addWidget(m_haltButton, 2, 9);

    m_statusLabel = new QLabel(QStringLiteral("Native weak-signal backend idle."), txPanel);
    m_statusLabel->setWordWrap(true);
    txGrid->addWidget(m_statusLabel, 3, 1, 1, 9);

    root->addWidget(txPanel);

    connect(m_modeGroup, qOverload<int>(&QButtonGroup::idClicked),
            this, &WeakSignalDialog::onModeButtonClicked);
    connect(m_rxToggleButton, &QPushButton::toggled,
            this, &WeakSignalDialog::onListenerToggled);
    connect(m_frequencyCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &WeakSignalDialog::applyFrequencyPreset);
    connect(m_frequencySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &WeakSignalDialog::onFrequencyEdited);
    connect(m_cqOnlyCheck, &QCheckBox::toggled,
            this, &WeakSignalDialog::onCqFilterToggled);
    connect(m_spotCheck, &QCheckBox::toggled, this, [this](bool on) {
        AppSettings::instance().setValue(spotEnabledSettingKey(currentModeLabel()),
                                         on ? QStringLiteral("True") : QStringLiteral("False"));
        AppSettings::instance().save();
    });
    connect(m_pskReporterCheck, &QCheckBox::toggled, this, [this](bool on) {
        AppSettings::instance().setValue(reporterEnabledSettingKey(currentModeLabel()),
                                         on ? QStringLiteral("True") : QStringLiteral("False"));
        AppSettings::instance().save();
    });
    connect(m_stopButton, &QPushButton::clicked, this, [this] {
        if (m_rxToggleButton->isChecked())
            m_rxToggleButton->setChecked(false);
    });
    connect(m_monitorButton, &QPushButton::clicked, this, [this] {
        if (!m_rxToggleButton->isChecked())
            m_rxToggleButton->setChecked(true);
    });
    connect(m_decodeButton, &QPushButton::clicked,
            this, &WeakSignalDialog::requestReplay);
    connect(mapButton, &QPushButton::clicked,
            this, &WeakSignalDialog::openMapDialog);
    connect(panButton, &QPushButton::clicked,
            this, &WeakSignalDialog::focusPanadapter);
    connect(m_tuneButton, &QPushButton::clicked, this, [this] {
        applySliceTuning(true);
    });
    connect(eraseButton, &QPushButton::clicked,
            this, &WeakSignalDialog::wipeDecodes);
    connect(m_bandActivityTable, &QTableWidget::itemSelectionChanged,
            this, &WeakSignalDialog::onRowSelectionChanged);
    connect(m_rxFrequencyTable, &QTableWidget::itemSelectionChanged,
            this, &WeakSignalDialog::onRowSelectionChanged);
    connect(m_bandActivityTable, &QTableWidget::cellDoubleClicked,
            this, [this](int, int) { onReplyClicked(); });
    connect(m_rxFrequencyTable, &QTableWidget::cellDoubleClicked,
            this, [this](int, int) { onReplyClicked(); });
    connect(m_replyButton, &QPushButton::clicked,
            this, &WeakSignalDialog::onReplyClicked);
    connect(m_sendTextButton, &QPushButton::clicked,
            this, &WeakSignalDialog::onSendTextClicked);
    connect(m_haltButton, &QPushButton::clicked,
            this, &WeakSignalDialog::requestHaltTx);
    connect(m_txDfEdit, &QLineEdit::textChanged,
            this, [this] { updateTxControls(); });
    connect(m_dxEdit, &QLineEdit::textChanged,
            this, [this] { updateTxControls(); });
    connect(m_reportEdit, &QLineEdit::textChanged,
            this, [this] { updateTxControls(); });
    connect(m_messageEdit, &QLineEdit::textChanged,
            this, [this] { updateTxControls(); });
    connect(m_messageEdit, &QLineEdit::textChanged,
            this, [this] { refreshGeneratedMessages(); });
    connect(m_reportEdit, &QLineEdit::textChanged,
            this, [this] { refreshGeneratedMessages(); });
    connect(m_dxEdit, &QLineEdit::textChanged,
            this, [this] { refreshGeneratedMessages(); });
    connect(m_txMessageGroup, &QButtonGroup::idClicked,
            this, [this](int) { updateTxControls(); });
}

void WeakSignalDialog::applyFrequencyPreset(int index)
{
    if (m_syncingControls || index <= 0)
        return;

    const auto presets = frequencyPresetsForMode(currentMode());
    const int presetIndex = index - 1;
    if (presetIndex < 0 || presetIndex >= presets.size())
        return;

    m_frequencySpin->setValue(presets.at(presetIndex).khz);
    applySliceTuning(true);
}

void WeakSignalDialog::focusPanadapter()
{
    applySliceTuning(true);
    if (parentWidget()) {
        parentWidget()->showNormal();
        parentWidget()->raise();
        parentWidget()->activateWindow();
    }
}

void WeakSignalDialog::onCleared(quint8 window)
{
    if (window != 0 && window != 2)
        return;

    m_allDecodes.clear();
    refreshTable();
    updateStatusLine(QStringLiteral("Native band activity cleared."));
}

void WeakSignalDialog::onCqFilterToggled(bool)
{
    refreshTable();
    saveSettings();
}

void WeakSignalDialog::onDecodeReceived(const AetherSDR::WeakSignalDecode& decode)
{
    if (!isWeakSignalMode(decode.mode))
        return;

    if (!decode.callsign.isEmpty() && !decode.grid.isEmpty())
        m_gridByCallsign.insert(decode.callsign.toUpper(), decode.grid.toUpper());

    DisplayDecode entry;
    entry.decode = decode;
    entry.hasLocation = resolveLocation(decode.callsign, decode.grid,
                                        &entry.country, &entry.latitude, &entry.longitude);

    m_allDecodes.prepend(entry);
    while (m_allDecodes.size() > 600)
        m_allDecodes.removeLast();

    ReceptionReport report;
    report.receiverCallsign = m_radioModel->callsign().trimmed().toUpper();
    report.receiverLocator = currentHomeGrid().left(6).trimmed().toUpper();
    report.senderCallsign = decode.callsign.trimmed().toUpper();
    report.senderLocator = decode.grid.trimmed().toUpper();
    report.frequencyHz = decode.frequencyHz;
    report.snr = decode.snr;
    report.mode = reporterModeName(decode.mode);
    report.flowStartSeconds = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    report.message = decode.message.trimmed();
    report.lifetimeSec = 120;
    if (!report.senderCallsign.isEmpty() && report.frequencyHz > 0)
        emit receptionReportReady(report);

    refreshTable();
}

void WeakSignalDialog::onFrequencyEdited(double)
{
    if (m_syncingControls)
        return;

    syncFrequencyPresetSelection();
    m_decoder->setDialFrequencyKhz(m_frequencySpin->value());
    refreshListenerSummary();
    applySliceTuning(false);
}

void WeakSignalDialog::onListenerStarted()
{
    refreshListenerSummary();
    updateTxControls();
    if (currentMode() == Mode::FT8 || currentMode() == Mode::FT4) {
        updateStatusLine(QStringLiteral("Native %1 decoder is listening to panadapter audio.")
                             .arg(currentModeLabel()));
    } else {
        updateStatusLine(QStringLiteral("Audio capture is running for %1. Native TX is available; RX decoding is still pending.")
                             .arg(currentModeLabel()));
    }
    QTimer::singleShot(100, this, &WeakSignalDialog::requestReplay);
}

void WeakSignalDialog::onListenerStopped()
{
    refreshListenerSummary();
    updateStatusLine(QStringLiteral("RX is off. Enable the native decoder to start decoding panadapter audio."));
}

void WeakSignalDialog::onListenerToggled(bool enabled)
{
    if (enabled == m_decoder->isListening())
        return;

    if (enabled)
        m_decoder->startListening();
    else
        m_decoder->stopListening();
}

void WeakSignalDialog::onModeButtonClicked(int id)
{
    setModeSelection(static_cast<Mode>(id), true);
}

void WeakSignalDialog::onRadioTransmittingChanged(bool on)
{
    if (on) {
        if (m_txStartPending || !m_txWaveform.isEmpty()) {
            m_txStartPending = false;
            m_txActive = true;
            if (!m_txTimer->isActive())
                m_txTimer->start();
            updateStatusLine(QStringLiteral("%1 transmitter active.").arg(currentModeLabel()));
        }
    } else {
        m_txTimer->stop();
        m_txActive = false;
        if (!m_txStartPending && m_txWaveform.isEmpty())
            m_txDrainGuardChunks = 0;
    }

    updateTxControls();
}

void WeakSignalDialog::onReplyClicked()
{
    QString statusText;
    if (!buildTxWaveform(true, &statusText)) {
        updateStatusLine(statusText);
        return;
    }
    updateStatusLine(statusText);
}

void WeakSignalDialog::onRowSelectionChanged()
{
    auto* table = qobject_cast<QTableWidget*>(sender());
    if (!table)
        return;

    const int row = table->currentRow();
    if (row >= 0) {
        auto* keyItem = table->item(row, 0);
        if (keyItem) {
            const QString key = keyItem->data(Qt::UserRole).toString();
            if (!key.isEmpty())
                m_selectedDecodeKey = key;
        }
    }

    setSelectedDecodeDetailsByKey(m_selectedDecodeKey);
    refreshTable();
}

void WeakSignalDialog::onSendTextClicked()
{
    QString statusText;
    if (!buildTxWaveform(false, &statusText)) {
        updateStatusLine(statusText);
        return;
    }
    updateStatusLine(statusText);
}

void WeakSignalDialog::pumpTxAudio()
{
    constexpr int kChunkFrames = 480;
    const int chunkBytes = kChunkFrames * 2 * static_cast<int>(sizeof(float));

    if (!m_radioModel->transmitModel().isMox() && !m_txStartPending && !m_txActive)
        return;

    if (currentUtcMs() < m_txBoundaryMs) {
        emit txAudioReady(QByteArray(chunkBytes, 0));
        return;
    }

    if (m_txWaveformOffset < m_txWaveform.size()) {
        const int remaining = m_txWaveform.size() - m_txWaveformOffset;
        const int bytesToCopy = std::min(chunkBytes, remaining);
        QByteArray chunk = m_txWaveform.mid(m_txWaveformOffset, bytesToCopy);
        if (chunk.size() < chunkBytes)
            chunk.append(QByteArray(chunkBytes - chunk.size(), 0));
        m_txWaveformOffset += bytesToCopy;
        emit txAudioReady(chunk);
    } else {
        emit txAudioReady(QByteArray(chunkBytes, 0));
        ++m_txDrainGuardChunks;
        if (m_txDrainGuardChunks >= 2)
            stopTransmissionNow(QStringLiteral("%1 TX complete.").arg(currentModeLabel()));
    }

    updateTxControls();
}

void WeakSignalDialog::openMapDialog()
{
    if (!m_mapDialog)
        m_mapDialog = new WeakSignalMapDialog(this);

    refreshMap();
    m_mapDialog->show();
    m_mapDialog->raise();
    m_mapDialog->activateWindow();
}

void WeakSignalDialog::requestHaltTx()
{
    if (m_slotTimer && m_slotTimer->isActive()) {
        stopTransmissionNow(QStringLiteral("Queued %1 transmission cancelled.").arg(currentModeLabel()));
        return;
    }

    if (m_txKeyRequested || m_txStartPending || m_txActive || !m_txWaveform.isEmpty() || m_radioModel->transmitModel().isMox()) {
        stopTransmissionNow(QStringLiteral("%1 transmission halted.").arg(currentModeLabel()));
        return;
    }

    updateStatusLine(QStringLiteral("No %1 transmission is queued or active.").arg(currentModeLabel()));
}

void WeakSignalDialog::requestReplay()
{
    m_decoder->requestReplay();
    updateStatusLine(QStringLiteral("Requested replay of buffered native weak-signal audio."));
}

void WeakSignalDialog::syncControlsFromSlice()
{
    if (!m_activeSlice)
        return;

    m_syncingControls = true;
    m_frequencySpin->setValue(m_activeSlice->frequency() * 1000.0);
    syncFrequencyPresetSelection();
    m_syncingControls = false;

    m_sliceSummaryLabel->setText(
        QStringLiteral("Slice %1  |  %2  |  %3 MHz")
            .arg(m_activeSlice->sliceId())
            .arg(m_activeSlice->mode())
            .arg(QString::number(m_activeSlice->frequency(), 'f', 6)));
    m_decoder->setDialFrequencyKhz(m_frequencySpin->value());
    refreshListenerSummary();
    updateWindowTitle();
}

void WeakSignalDialog::updateHomeStation(const QString&, int, int,
                                         const QString&, const QString&,
                                         const QString&, const QString&,
                                         const QString&)
{
    refreshMap();
}

void WeakSignalDialog::wipeDecodes()
{
    m_allDecodes.clear();
    refreshTable();
    m_decoder->requestClear(0);
    updateStatusLine(QStringLiteral("Cleared the local native decode list."));
}

void WeakSignalDialog::applySliceTuning(bool recenter)
{
    if (!m_activeSlice)
        return;

    const double mhz = m_frequencySpin->value() / 1000.0;
    if (recenter)
        m_activeSlice->tuneAndRecenter(mhz);
    else
        m_activeSlice->setFrequency(mhz);

    if (m_activeSlice->mode() != QStringLiteral("DIGU"))
        m_activeSlice->setMode(QStringLiteral("DIGU"));
    m_activeSlice->setFilterWidth(100, 3100);

    m_sliceSummaryLabel->setText(
        QStringLiteral("Slice %1  |  DIGU  |  %2 MHz")
            .arg(m_activeSlice->sliceId())
            .arg(QString::number(m_activeSlice->frequency(), 'f', 6)));
    updateWindowTitle();
}

QString WeakSignalDialog::bandLabelForFrequency(double khz) const
{
    if (khz >= 1800.0 && khz < 2000.0) return QStringLiteral("160m");
    if (khz >= 3500.0 && khz < 4000.0) return QStringLiteral("80m");
    if (khz >= 5000.0 && khz < 5500.0) return QStringLiteral("60m");
    if (khz >= 7000.0 && khz < 7300.0) return QStringLiteral("40m");
    if (khz >= 10100.0 && khz < 10150.0) return QStringLiteral("30m");
    if (khz >= 14000.0 && khz < 14350.0) return QStringLiteral("20m");
    if (khz >= 18068.0 && khz < 18168.0) return QStringLiteral("17m");
    if (khz >= 21000.0 && khz < 21450.0) return QStringLiteral("15m");
    if (khz >= 24890.0 && khz < 24990.0) return QStringLiteral("12m");
    if (khz >= 28000.0 && khz < 29700.0) return QStringLiteral("10m");
    if (khz >= 50000.0 && khz < 54000.0) return QStringLiteral("6m");
    if (khz >= 70000.0 && khz < 70500.0) return QStringLiteral("4m");
    if (khz >= 144000.0 && khz < 148000.0) return QStringLiteral("2m");
    if (khz >= 220000.0 && khz < 225000.0) return QStringLiteral("1.25m");
    if (khz >= 430000.0 && khz < 440000.0) return QStringLiteral("70cm");
    if (khz >= 1240000.0 && khz < 1300000.0) return QStringLiteral("23cm");
    if (khz >= 2400000.0 && khz < 2410000.0) return QStringLiteral("13cm");
    return QStringLiteral("Manual");
}

QString WeakSignalDialog::currentContextTitle() const
{
    return QStringLiteral("%1 %2")
        .arg(bandLabelForFrequency(m_frequencySpin->value()), currentModeLabel());
}

WeakSignalDialog::Mode WeakSignalDialog::currentMode() const
{
    const int id = m_modeGroup->checkedId();
    switch (id) {
    case static_cast<int>(Mode::FT4):
        return Mode::FT4;
    case static_cast<int>(Mode::JT65):
        return Mode::JT65;
    case static_cast<int>(Mode::WSPR):
        return Mode::WSPR;
    case static_cast<int>(Mode::FT8):
    default:
        return Mode::FT8;
    }
}

QString WeakSignalDialog::currentModeLabel() const
{
    switch (currentMode()) {
    case Mode::FT4:
        return QStringLiteral("FT4");
    case Mode::JT65:
        return QStringLiteral("JT65");
    case Mode::WSPR:
        return QStringLiteral("WSPR");
    case Mode::FT8:
    default:
        return QStringLiteral("FT8");
    }
}

QString WeakSignalDialog::currentHomeGrid() const
{
    const QString configuredGrid = AppSettings::instance()
                                       .value(QStringLiteral("StationGridSquare"), QString())
                                       .toString()
                                       .trimmed()
                                       .toUpper();
    if (!configuredGrid.isEmpty()) {
        double latitude = 0.0;
        double longitude = 0.0;
        if (maidenheadToLatLon(configuredGrid, &latitude, &longitude))
            return configuredGrid;
    }

    if (!m_radioModel->gpsGrid().trimmed().isEmpty())
        return m_radioModel->gpsGrid().trimmed().toUpper();
    return {};
}

QVector<WeakSignalDialog::FrequencyPreset> WeakSignalDialog::frequencyPresetsForMode(Mode mode) const
{
    QVector<FrequencyPreset> presets;
    auto appendPresets = [&presets](const auto& source) {
        for (const auto& preset : source)
            presets.append(FrequencyPreset{QString::fromLatin1(preset.label), preset.khz});
    };

    switch (mode) {
    case Mode::FT4:
        appendPresets(kFt4Presets);
        break;
    case Mode::JT65:
        appendPresets(kJt65Presets);
        break;
    case Mode::WSPR:
        appendPresets(kWsprPresets);
        break;
    case Mode::FT8:
    default:
        appendPresets(kFt8Presets);
        break;
    }
    return presets;
}

QString WeakSignalDialog::gridForCallsign(const QString& callsign, const QString& decodeGrid) const
{
    if (!decodeGrid.trimmed().isEmpty())
        return decodeGrid.trimmed().toUpper();
    return m_gridByCallsign.value(callsign.toUpper()).trimmed().toUpper();
}

void WeakSignalDialog::refreshReportingControls()
{
    const auto& settings = AppSettings::instance();
    const QString modeName = currentModeLabel();
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

bool WeakSignalDialog::maidenheadToLatLon(const QString& grid, double* latitude, double* longitude) const
{
    QString text = grid.trimmed().toUpper();
    if (text.size() != 4 && text.size() != 6)
        return false;

    if (text[0] < QLatin1Char('A') || text[0] > QLatin1Char('R')
        || text[1] < QLatin1Char('A') || text[1] > QLatin1Char('R')
        || !text[2].isDigit() || !text[3].isDigit()) {
        return false;
    }

    double lon = (text[0].unicode() - 'A') * 20.0 - 180.0;
    double lat = (text[1].unicode() - 'A') * 10.0 - 90.0;
    lon += text[2].digitValue() * 2.0;
    lat += text[3].digitValue();

    double lonCell = 2.0;
    double latCell = 1.0;

    if (text.size() >= 6) {
        if (text[4] < QLatin1Char('A') || text[4] > QLatin1Char('X')
            || text[5] < QLatin1Char('A') || text[5] > QLatin1Char('X')) {
            return false;
        }
        lon += (text[4].unicode() - 'A') * (2.0 / 24.0);
        lat += (text[5].unicode() - 'A') * (1.0 / 24.0);
        lonCell = 2.0 / 24.0;
        latCell = 1.0 / 24.0;
    }

    *longitude = lon + lonCell / 2.0;
    *latitude = lat + latCell / 2.0;
    return true;
}

void WeakSignalDialog::loadSettings()
{
    auto& settings = AppSettings::instance();
    const int savedMode = settings.value(QStringLiteral("WeakSignalMode"), 0).toInt();
    const bool cqOnly = settings.value(QStringLiteral("WeakSignalCqOnly"), QStringLiteral("False")).toString() == QStringLiteral("True");
    const double savedKhz = settings.value(QStringLiteral("WeakSignalFrequencyKhz"), 14074.0).toDouble();
    const QString freeText = settings.value(QStringLiteral("WeakSignalFreeText"), QString()).toString();
    const QString txDf = settings.value(QStringLiteral("WeakSignalTxDfHz"), QStringLiteral("1500")).toString();
    const QString report = settings.value(QStringLiteral("WeakSignalTxReport"), QString()).toString();

    m_syncingControls = true;
    m_cqOnlyCheck->setChecked(cqOnly);
    setModeSelection(static_cast<Mode>(std::clamp(savedMode, 0, 3)), false);
    m_frequencySpin->setValue(savedKhz);
    syncFrequencyPresetSelection();
    m_txDfEdit->setText(txDf);
    m_reportEdit->setText(report);
    m_messageEdit->setText(freeText);
    m_syncingControls = false;

    refreshReportingControls();
    updateWindowTitle();
    refreshListenerSummary();
    refreshGeneratedMessages();
    updateTxControls();
}

void WeakSignalDialog::refreshListenerSummary()
{
    if (!m_decoder)
        return;

    {
        QSignalBlocker blocker(m_rxToggleButton);
        m_rxToggleButton->setChecked(m_decoder->isListening());
    }

    if (!m_decoder->isListening()) {
        m_listenerLabel->setText(QStringLiteral("Native weak-signal decoder stopped."));
        return;
    }

    const QString modeText = currentModeLabel();
    const QString freqText = QStringLiteral("%1 kHz").arg(formatFrequencyKhz(m_frequencySpin->value()));
    m_listenerLabel->setText(QStringLiteral("Native %1 decoder active  |  Dial: %2")
                                 .arg(modeText, freqText));
}

bool WeakSignalDialog::resolveLocation(const QString& callsign, const QString& grid,
                                       QString* country, double* latitude, double* longitude) const
{
    bool hasLocation = false;
    const QString effectiveGrid = gridForCallsign(callsign, grid);
    if (!effectiveGrid.isEmpty())
        hasLocation = maidenheadToLatLon(effectiveGrid, latitude, longitude);

    if (!callsign.trimmed().isEmpty()) {
        const QString prefix = m_ctyParser.resolvePrimaryPrefix(callsign);
        if (const auto* entity = m_ctyParser.entityByPrefix(prefix)) {
            if (country)
                *country = entity->name;
            if (!hasLocation) {
                *latitude = entity->latitude;
                // cty.dat longitudes are positive West; the map uses positive East.
                *longitude = -entity->longitude;
                hasLocation = true;
            }
        }
    }

    return hasLocation;
}

void WeakSignalDialog::refreshMap()
{
    if (!m_mapDialog)
        return;

    WeakSignalMapStation home;
    home.callsign = m_radioModel->callsign().trimmed().toUpper();
    home.grid = currentHomeGrid();
    home.country = QStringLiteral("Home");
    home.isHome = true;
    const QString configuredGrid = AppSettings::instance()
                                       .value(QStringLiteral("StationGridSquare"), QString())
                                       .toString()
                                       .trimmed()
                                       .toUpper();
    if (!configuredGrid.isEmpty() && home.grid == configuredGrid)
        home.hasLocation = maidenheadToLatLon(home.grid, &home.latitude, &home.longitude);

    if (!home.hasLocation
        && !m_radioModel->gpsLat().trimmed().isEmpty()
        && !m_radioModel->gpsLon().trimmed().isEmpty()) {
        bool latOk = false;
        bool lonOk = false;
        home.latitude = m_radioModel->gpsLat().toDouble(&latOk);
        home.longitude = m_radioModel->gpsLon().toDouble(&lonOk);
        home.hasLocation = latOk && lonOk;
    }
    if (!home.hasLocation && !home.grid.isEmpty())
        home.hasLocation = maidenheadToLatLon(home.grid, &home.latitude, &home.longitude);

    QVector<WeakSignalMapStation> stations;
    const auto rows = visibleDecodes();
    const QString selectedKey = m_selectedDecodeKey;
    QSet<QString> seenCallsigns;
    for (const auto& row : rows) {
        if (!row.hasLocation || row.decode.callsign.isEmpty())
            continue;
        const QString normalizedCall = row.decode.callsign.toUpper();
        if (seenCallsigns.contains(normalizedCall))
            continue;
        seenCallsigns.insert(normalizedCall);

        WeakSignalMapStation station;
        station.callsign = row.decode.callsign;
        station.country = row.country;
        station.grid = gridForCallsign(row.decode.callsign, row.decode.grid);
        station.latitude = row.latitude;
        station.longitude = row.longitude;
        station.hasLocation = row.hasLocation;
        station.selected = (decodeKey(row) == selectedKey);
        stations.append(station);
    }

    m_mapDialog->setContext(currentContextTitle());
    m_mapDialog->setHomeStation(home);
    m_mapDialog->setStations(stations);
}

void WeakSignalDialog::refreshTable()
{
    const auto rows = visibleDecodes();
    if (!m_selectedDecodeKey.isEmpty()) {
        bool found = false;
        for (const auto& row : rows) {
            if (decodeKey(row) == m_selectedDecodeKey) {
                found = true;
                break;
            }
        }
        if (!found)
            m_selectedDecodeKey.clear();
    }
    if (m_selectedDecodeKey.isEmpty() && !rows.isEmpty())
        m_selectedDecodeKey = decodeKey(rows.first());

    populateDecodeTable(m_bandActivityTable, rows, true);
    const auto focusedRows = focusedDecodes();
    populateDecodeTable(m_rxFrequencyTable, focusedRows, false);
    setSelectedDecodeDetailsByKey(m_selectedDecodeKey);
    if (m_rxFrequencyLabel) {
        DisplayDecode selectedEntry;
        if (selectedDecode(&selectedEntry) && selectedEntry.decode.deltaFrequencyHz > 0) {
            m_rxFrequencyLabel->setText(
                QStringLiteral("Around %1 Hz").arg(selectedEntry.decode.deltaFrequencyHz));
        } else if (!focusedRows.isEmpty()) {
            m_rxFrequencyLabel->setText(QStringLiteral("Around %1 Hz").arg(currentTxDfHz()));
        } else {
            m_rxFrequencyLabel->setText(QStringLiteral("No focus frequency selected"));
        }
    }
    refreshGeneratedMessages();
    refreshMap();
    updateTxControls();
}

QVector<WeakSignalDialog::DisplayDecode> WeakSignalDialog::focusedDecodes() const
{
    int focusDf = currentTxDfHz();
    DisplayDecode selectedEntry;
    if (selectedDecode(&selectedEntry) && selectedEntry.decode.deltaFrequencyHz > 0)
        focusDf = static_cast<int>(selectedEntry.decode.deltaFrequencyHz);

    const int windowHz = currentMode() == Mode::WSPR ? 12 : (currentMode() == Mode::JT65 ? 120 : 90);
    QVector<DisplayDecode> rows;
    for (const auto& row : visibleDecodes()) {
        if (row.decode.deltaFrequencyHz == 0 && currentMode() != Mode::WSPR)
            continue;
        if (std::abs(static_cast<int>(row.decode.deltaFrequencyHz) - focusDf) <= windowHz)
            rows.append(row);
    }
    return rows;
}

QString WeakSignalDialog::formattedDialDisplay() const
{
    QString text = QString::number(m_frequencySpin->value() / 1000.0, 'f', 6);
    const int decimal = text.indexOf(QLatin1Char('.'));
    if (decimal >= 0 && decimal + 4 < text.size())
        text.insert(decimal + 4, QLatin1Char(' '));
    return text;
}

void WeakSignalDialog::populateDecodeTable(QTableWidget* table,
                                           const QVector<DisplayDecode>& rows,
                                           bool showCountryColumn) const
{
    if (!table)
        return;

    QSignalBlocker blocker(table);
    table->clearContents();
    table->setRowCount(rows.size());

    int selectedRow = -1;
    for (int row = 0; row < rows.size(); ++row) {
        const auto& entry = rows.at(row);
        const QString key = decodeKey(entry);

        auto* timeItem = new QTableWidgetItem(entry.decode.utcTime.toString(QStringLiteral("HH:mm")));
        auto* snrItem = new QTableWidgetItem(QString::number(entry.decode.snr));
        auto* dtItem = new QTableWidgetItem(QString::number(entry.decode.deltaTimeSec, 'f', 1));
        auto* freqItem = new QTableWidgetItem(
            currentMode() == Mode::WSPR
                ? formatFrequencyKhz(static_cast<double>(entry.decode.frequencyHz) / 1000.0)
                : QString::number(entry.decode.deltaFrequencyHz));
        auto* msgItem = new QTableWidgetItem(entry.decode.message);

        for (QTableWidgetItem* item : {timeItem, snrItem, dtItem, freqItem, msgItem})
            item->setData(Qt::UserRole, key);

        timeItem->setTextAlignment(Qt::AlignCenter);
        snrItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        dtItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        freqItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        table->setItem(row, 0, timeItem);
        table->setItem(row, 1, snrItem);
        table->setItem(row, 2, dtItem);
        table->setItem(row, 3, freqItem);
        table->setItem(row, 4, msgItem);
        if (showCountryColumn) {
            auto* countryItem = new QTableWidgetItem(entry.country);
            countryItem->setData(Qt::UserRole, key);
            table->setItem(row, 5, countryItem);
        }

        if (key == m_selectedDecodeKey)
            selectedRow = row;
    }

    if (selectedRow >= 0)
        table->selectRow(selectedRow);
}

void WeakSignalDialog::repopulateFrequencyPresets()
{
    QSignalBlocker blocker(m_frequencyCombo);
    const auto presets = frequencyPresetsForMode(currentMode());
    m_frequencyCombo->clear();
    m_frequencyCombo->addItem(QStringLiteral("Manual"));
    for (const auto& preset : presets)
        m_frequencyCombo->addItem(preset.label);
    syncFrequencyPresetSelection();
}

void WeakSignalDialog::saveSettings() const
{
    auto& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("WeakSignalMode"), QString::number(static_cast<int>(currentMode())));
    settings.setValue(QStringLiteral("WeakSignalCqOnly"), m_cqOnlyCheck->isChecked() ? QStringLiteral("True") : QStringLiteral("False"));
    settings.setValue(QStringLiteral("WeakSignalFrequencyKhz"), QString::number(m_frequencySpin->value(), 'f', 1));
    settings.setValue(QStringLiteral("WeakSignalTxDfHz"), m_txDfEdit->text().trimmed());
    settings.setValue(QStringLiteral("WeakSignalTxReport"), m_reportEdit->text().trimmed());
    settings.setValue(QStringLiteral("WeakSignalFreeText"), m_messageEdit->text());
    settings.save();
}

void WeakSignalDialog::setModeSelection(Mode mode, bool fromUser)
{
    if ((m_slotTimer && m_slotTimer->isActive()) || m_txActive || m_txStartPending || !m_txWaveform.isEmpty())
        stopTransmissionNow(QStringLiteral("%1 transmission stopped.").arg(currentModeLabel()));

    const bool suppressDecoderSync = m_syncingControls;
    m_syncingControls = true;
    if (auto* button = m_modeGroup->button(static_cast<int>(mode)))
        button->setChecked(true);
    repopulateFrequencyPresets();
    m_syncingControls = false;

    refreshTable();
    refreshListenerSummary();
    refreshReportingControls();
    updateWindowTitle();
    updateTxControls();

    if (!suppressDecoderSync) {
        m_decoder->setMode(decoderModeForUiMode(mode));
        m_decoder->setDialFrequencyKhz(m_frequencySpin->value());
    }

    if (fromUser)
        applySliceTuning(false);
}

void WeakSignalDialog::setSelectedDecodeDetails(int row)
{
    const auto rows = visibleDecodes();
    if (row < 0 || row >= rows.size()) {
        m_rxDfEdit->setText(QStringLiteral("--"));
        m_dxEdit->clear();
        m_dxGridEdit->clear();
        return;
    }

    setSelectedDecodeDetailsByKey(decodeKey(rows.at(row)));
}

void WeakSignalDialog::setSelectedDecodeDetailsByKey(const QString& key)
{
    m_selectedDecodeKey = key;

    DisplayDecode entry;
    if (!selectedDecode(&entry)) {
        m_rxDfEdit->setText(QStringLiteral("--"));
        m_dxEdit->clear();
        m_dxGridEdit->clear();
        return;
    }

    m_rxDfEdit->setText(entry.decode.deltaFrequencyHz ? QString::number(entry.decode.deltaFrequencyHz)
                                                      : QStringLiteral("--"));
    if (entry.decode.deltaFrequencyHz > 0)
        m_txDfEdit->setText(QString::number(entry.decode.deltaFrequencyHz));
    m_dxEdit->setText(entry.decode.callsign);
    m_dxGridEdit->setText(gridForCallsign(entry.decode.callsign, entry.decode.grid));
    if (currentMode() == Mode::WSPR)
        m_reportEdit->setText(QString::number(entry.decode.powerDbm));
    else
        m_reportEdit->setText(QStringLiteral("%1%2")
                                  .arg(entry.decode.snr >= 0 ? QStringLiteral("+") : QStringLiteral(""))
                                  .arg(entry.decode.snr));
}

bool WeakSignalDialog::selectedDecode(DisplayDecode* entry) const
{
    if (m_selectedDecodeKey.isEmpty())
        return false;

    for (const auto& row : visibleDecodes()) {
        if (decodeKey(row) == m_selectedDecodeKey) {
            if (entry)
                *entry = row;
            return true;
        }
    }
    return false;
}

void WeakSignalDialog::syncFrequencyPresetSelection()
{
    const auto presets = frequencyPresetsForMode(currentMode());
    const double currentKhz = m_frequencySpin->value();
    int matchedIndex = 0;
    for (int i = 0; i < presets.size(); ++i) {
        if (std::fabs(presets.at(i).khz - currentKhz) < 0.15) {
            matchedIndex = i + 1;
            break;
        }
    }

    QSignalBlocker blocker(m_frequencyCombo);
    m_frequencyCombo->setCurrentIndex(matchedIndex);
}

void WeakSignalDialog::updateStatusLine(const QString& text)
{
    if (!text.isEmpty())
        m_statusLabel->setText(text);
}

bool WeakSignalDialog::hasSelectedDecode() const
{
    return selectedDecode(nullptr);
}

int WeakSignalDialog::currentTxDfHz() const
{
    bool ok = false;
    const int parsed = m_txDfEdit->text().trimmed().toInt(&ok);
    if (!ok)
        return 1500;
    return std::clamp(parsed, 0, 4000);
}

QString WeakSignalDialog::defaultReplyMessage() const
{
    return generatedMessageAt(0);
}

QString WeakSignalDialog::defaultStandardMessage() const
{
    const QString selected = selectedGeneratedMessage();
    if (!selected.isEmpty())
        return selected;
    return generatedMessageAt(0);
}

QString WeakSignalDialog::generatedMessageAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_txMessageEdits.size()))
        return {};

    const QString myCall = m_radioModel->callsign().trimmed().toUpper();
    const QString dxCall = m_dxEdit->text().trimmed().toUpper();
    const QString homeGrid = currentHomeGrid().left(4).trimmed().toUpper();
    const QString freeText = m_messageEdit->text().trimmed().toUpper();

    if (currentMode() == Mode::WSPR) {
        if (index == 0 && !myCall.isEmpty() && homeGrid.size() >= 4)
            return QStringLiteral("%1 %2 %3 dBm")
                .arg(myCall,
                     homeGrid,
                     m_reportEdit->text().trimmed().isEmpty() ? QStringLiteral("30")
                                                              : m_reportEdit->text().trimmed());
        return {};
    }

    const QString cqMessage = homeGrid.isEmpty()
        ? QStringLiteral("CQ %1").arg(myCall)
        : QStringLiteral("CQ %1 %2").arg(myCall, homeGrid);

    if (myCall.isEmpty()) {
        if (index == 5)
            return freeText;
        return {};
    }

    QString report = m_reportEdit->text().trimmed().toUpper();
    if (report.startsWith(QLatin1Char('R')))
        report.remove(0, 1);

    bool selectedWasCq = false;
    DisplayDecode selectedEntry;
    if (selectedDecode(&selectedEntry))
        selectedWasCq = selectedEntry.decode.message.trimmed().toUpper().startsWith(QLatin1String("CQ"));

    if (dxCall.isEmpty()) {
        if (index == 0)
            return cqMessage;
        if (index == 5)
            return freeText;
        return {};
    }

    const QString openingMessage = (!selectedWasCq && !report.isEmpty())
        ? QStringLiteral("%1 %2 %3").arg(dxCall, myCall, report)
        : (homeGrid.isEmpty()
               ? QStringLiteral("%1 %2").arg(dxCall, myCall)
               : QStringLiteral("%1 %2 %3").arg(dxCall, myCall, homeGrid));

    switch (index) {
    case 0:
        return openingMessage;
    case 1:
        return report.isEmpty() ? openingMessage : QStringLiteral("%1 %2 %3").arg(dxCall, myCall, report);
    case 2:
        return report.isEmpty() ? QStringLiteral("%1 %2 RR73").arg(dxCall, myCall)
                                : QStringLiteral("%1 %2 R%3").arg(dxCall, myCall, report);
    case 3:
        return QStringLiteral("%1 %2 RR73").arg(dxCall, myCall);
    case 4:
        return QStringLiteral("%1 %2 73").arg(dxCall, myCall);
    case 5:
        return !freeText.isEmpty() ? freeText : cqMessage;
    default:
        return {};
    }
}

QString WeakSignalDialog::selectedGeneratedMessage() const
{
    if (!m_txMessageGroup)
        return {};

    const int checkedId = m_txMessageGroup->checkedId();
    if (checkedId < 0)
        return {};
    return generatedMessageAt(checkedId);
}

void WeakSignalDialog::refreshGeneratedMessages()
{
    QStringList messages;
    messages.reserve(static_cast<int>(m_txMessageEdits.size()));
    for (int i = 0; i < static_cast<int>(m_txMessageEdits.size()); ++i)
        messages.append(generatedMessageAt(i));

    int firstEnabled = -1;
    for (int i = 0; i < messages.size(); ++i) {
        if (m_txMessageEdits[static_cast<size_t>(i)])
            m_txMessageEdits[static_cast<size_t>(i)]->setText(messages.at(i));
        if (m_txMessageButtons[static_cast<size_t>(i)]) {
            const bool enabled = !messages.at(i).isEmpty();
            m_txMessageButtons[static_cast<size_t>(i)]->setEnabled(enabled);
            if (enabled && firstEnabled < 0)
                firstEnabled = i;
        }
    }

    if (m_txMessageGroup) {
        const int checkedId = m_txMessageGroup->checkedId();
        if (checkedId < 0 || checkedId >= messages.size() || messages.at(checkedId).isEmpty()) {
            QSignalBlocker blocker(m_txMessageGroup);
            if (firstEnabled >= 0 && m_txMessageButtons[static_cast<size_t>(firstEnabled)])
                m_txMessageButtons[static_cast<size_t>(firstEnabled)]->setChecked(true);
        }
    }
}

bool WeakSignalDialog::buildTxWaveform(bool reply, QString* statusText)
{
    if ((m_slotTimer && m_slotTimer->isActive()) || m_txActive || m_txStartPending || !m_txWaveform.isEmpty()) {
        if (statusText)
            *statusText = QStringLiteral("A %1 transmission is already queued or active. Halt it before starting another.")
                              .arg(currentModeLabel());
        return false;
    }

    if (!m_activeSlice) {
        if (statusText)
            *statusText = QStringLiteral("Select an active slice before transmitting %1.").arg(currentModeLabel());
        return false;
    }

    WeakSignalTxCodec::Request request;
    switch (currentMode()) {
    case Mode::FT4:
        request.mode = WeakSignalTxCodec::Mode::FT4;
        break;
    case Mode::JT65:
        request.mode = WeakSignalTxCodec::Mode::JT65;
        break;
    case Mode::WSPR:
        request.mode = WeakSignalTxCodec::Mode::WSPR;
        break;
    case Mode::FT8:
    default:
        request.mode = WeakSignalTxCodec::Mode::FT8;
        break;
    }

    request.toneHz = currentTxDfHz();
    request.sampleRate = 24000;
    request.callsign = m_radioModel->callsign().trimmed().toUpper();
    request.grid = currentHomeGrid().left(4);
    request.powerDbm = m_reportEdit->text().trimmed().toInt();

    if (currentMode() == Mode::WSPR) {
        if (request.callsign.isEmpty() || request.grid.size() < 4) {
            if (statusText)
                *statusText = QStringLiteral("WSPR TX needs a station callsign and 4-character grid square.");
            return false;
        }
    } else {
        request.message = reply ? defaultReplyMessage() : defaultStandardMessage();
        if (request.message.isEmpty()) {
            if (statusText)
                *statusText = QStringLiteral("Fill in DX/Text fields or select a decode before transmitting %1.")
                                  .arg(currentModeLabel());
            return false;
        }
    }

    WeakSignalTxCodec::Waveform waveform;
    QString error;
    if (!WeakSignalTxCodec::buildWaveform(request, &waveform, &error)) {
        if (statusText)
            *statusText = error;
        return false;
    }

    m_txWaveform = waveform.pcm24kStereo;
    m_lastTxMessage = waveform.normalizedMessage;
    m_txWaveformOffset = 0;
    m_txDrainGuardChunks = 0;

    const qint64 nowMs = currentUtcMs();
    const qint64 slotMs = WeakSignalTxCodec::slotDurationMs(request.mode);
    m_scheduledStartMs = ((nowMs / slotMs) + 1) * slotMs;
    m_txBoundaryMs = m_scheduledStartMs;

    const qint64 preKeyLeadMs = 250;
    const qint64 armDelayMs = std::max<qint64>(0, m_scheduledStartMs - preKeyLeadMs - nowMs);
    m_slotTimer->start(static_cast<int>(armDelayMs));

    if (statusText) {
        const QString when = QDateTime::fromMSecsSinceEpoch(m_scheduledStartMs, QTimeZone::UTC)
                                 .toString(QStringLiteral("HH:mm:ss"));
        *statusText = QStringLiteral("Queued %1 TX for %2 UTC: %3")
                          .arg(currentModeLabel(), when, waveform.normalizedMessage);
    }
    updateTxControls();
    return true;
}

void WeakSignalDialog::armScheduledTransmission()
{
    if (!m_activeSlice || m_txWaveform.isEmpty()) {
        updateTxControls();
        return;
    }

    applySliceTuning(false);
    const int txDf = currentTxDfHz();
    m_activeSlice->setDiguOffset(txDf);
    m_activeSlice->setFilterWidth(std::max(50, txDf - 200), std::min(4000, txDf + 200));
    if (!m_activeSlice->isTxSlice())
        m_activeSlice->setTxSlice(true);
    if (m_activeSlice->mode() != QStringLiteral("DIGU"))
        m_activeSlice->setMode(QStringLiteral("DIGU"));

    m_txStartPending = true;
    m_txActive = false;
    m_txWaveformOffset = 0;
    m_txDrainGuardChunks = 0;
    m_txKeyRequested = true;
    m_radioModel->setTransmit(true);
    updateStatusLine(QStringLiteral("Keying %1 transmitter...").arg(currentModeLabel()));
    updateTxControls();
}

void WeakSignalDialog::stopTransmissionNow(const QString& statusText)
{
    if (m_slotTimer)
        m_slotTimer->stop();
    if (m_txTimer)
        m_txTimer->stop();

    m_txStartPending = false;
    m_txActive = false;
    m_txWaveform.clear();
    m_txWaveformOffset = 0;
    m_txDrainGuardChunks = 0;
    m_txBoundaryMs = 0;
    m_scheduledStartMs = 0;
    m_lastTxMessage.clear();

    // Unkey if we requested TX even when mox= has not been echoed yet.
    const bool unkey = m_txKeyRequested
                       || (m_radioModel && m_radioModel->transmitModel().isMox());
    m_txKeyRequested = false;
    if (unkey && m_radioModel)
        m_radioModel->setTransmit(false);

    updateStatusLine(statusText);
    updateTxControls();
}

void WeakSignalDialog::updateTxControls()
{
    const bool busy = (m_slotTimer && m_slotTimer->isActive()) || m_txStartPending || m_txActive || !m_txWaveform.isEmpty();
    const bool isWspr = currentMode() == Mode::WSPR;
    const bool haveSlice = !m_activeSlice.isNull();
    const bool haveLocalCall = !m_radioModel->callsign().trimmed().isEmpty();
    const bool haveGrid = !currentHomeGrid().left(4).isEmpty();
    const bool validTxDf = !m_txDfEdit->text().trimmed().isEmpty();
    const bool haveMessage = !defaultStandardMessage().isEmpty();

    m_replyButton->setEnabled(!busy && !isWspr && haveSlice && haveLocalCall && validTxDf && hasSelectedDecode());
    m_sendTextButton->setEnabled(!busy && haveSlice && validTxDf
                                 && ((isWspr && haveLocalCall && haveGrid) || (!isWspr && haveMessage)));
    m_haltButton->setEnabled(busy || m_radioModel->transmitModel().isMox());
    if (m_stopButton)
        m_stopButton->setEnabled(m_decoder && m_decoder->isListening());
    if (m_monitorButton)
        m_monitorButton->setEnabled(m_decoder && !m_decoder->isListening());
    if (m_decodeButton)
        m_decodeButton->setEnabled(m_decoder && m_decoder->isListening());
    if (m_tuneButton)
        m_tuneButton->setEnabled(haveSlice);

    m_txDfEdit->setEnabled(!busy);
    m_dxEdit->setEnabled(!busy && !isWspr);
    m_messageEdit->setEnabled(!busy && !isWspr);
    m_reportEdit->setEnabled(!busy);
    if (m_bandActivityTable)
        m_bandActivityTable->setEnabled(!busy || hasSelectedDecode());
    if (m_rxFrequencyTable)
        m_rxFrequencyTable->setEnabled(!busy || hasSelectedDecode());
    m_messageEdit->setPlaceholderText(isWspr
                                          ? QStringLiteral("WSPR sends your station beacon automatically")
                                          : QStringLiteral("Optional free-text message"));
    m_sendTextButton->setText(isWspr ? QStringLiteral("Send Beacon")
                                     : QStringLiteral("Send Selected"));
}

void WeakSignalDialog::updateWindowTitle()
{
    if (m_bigDialLabel)
        m_bigDialLabel->setText(formattedDialDisplay());
    setWindowTitle(QStringLiteral("%1 - %2").arg(currentModeLabel(), currentContextTitle()));
}

QVector<WeakSignalDialog::DisplayDecode> WeakSignalDialog::visibleDecodes() const
{
    QVector<DisplayDecode> rows;
    const QString wantedMode = currentModeLabel();
    const bool cqOnly = m_cqOnlyCheck->isChecked();

    for (const auto& decode : m_allDecodes) {
        if (decode.decode.mode.trimmed().toUpper() != wantedMode)
            continue;
        if (cqOnly) {
            const QString msg = decode.decode.message.trimmed().toUpper();
            if (wantedMode == QLatin1String("WSPR")) {
                if (decode.decode.callsign.isEmpty())
                    continue;
            } else if (!msg.startsWith(QLatin1String("CQ ")) && !msg.startsWith(QLatin1String("CQ"))) {
                continue;
            }
        }
        rows.append(decode);
    }
    return rows;
}

} // namespace AetherSDR
