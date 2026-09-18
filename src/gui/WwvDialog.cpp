#include "WwvDialog.h"

#include "DigitalModeStyles.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace AetherSDR {

namespace {

struct FrequencyPreset {
    const char* label;
    double khz;
};

constexpr std::array<FrequencyPreset, 5> kWwvPresets{{
    {"WWV / WWVH 2.5 MHz", 2500.0},
    {"WWV / WWVH 5 MHz", 5000.0},
    {"WWV / WWVH 10 MHz", 10000.0},
    {"WWV / WWVH 15 MHz", 15000.0},
    {"WWV 20 MHz", 20000.0},
}};

constexpr std::array<FrequencyPreset, 3> kChuPresets{{
    {"CHU 3330 kHz", 3330.0},
    {"CHU 7850 kHz", 7850.0},
    {"CHU 14670 kHz", 14670.0},
}};

constexpr std::array<FrequencyPreset, 4> kBpmPresets{{
    {"BPM 2.5 MHz", 2500.0},
    {"BPM 5 MHz", 5000.0},
    {"BPM 10 MHz", 10000.0},
    {"BPM 15 MHz", 15000.0},
}};

std::span<const FrequencyPreset> presetsForProfile(WwvTimecodeDecoder::Profile profile)
{
    switch (profile) {
    case WwvTimecodeDecoder::Profile::Chu:
        return std::span<const FrequencyPreset>(kChuPresets);
    case WwvTimecodeDecoder::Profile::Bpm:
        return std::span<const FrequencyPreset>(kBpmPresets);
    case WwvTimecodeDecoder::Profile::Wwv:
    default:
        return std::span<const FrequencyPreset>(kWwvPresets);
    }
}

QString frequencySettingsKey(WwvTimecodeDecoder::Profile profile)
{
    switch (profile) {
    case WwvTimecodeDecoder::Profile::Chu:
        // "_" not ".": AppSettings rejects keys that aren't valid XML element
        // names, so the dotted keys were silently never saved.
        return QStringLiteral("TimeStationFrequencyKhz_Chu");
    case WwvTimecodeDecoder::Profile::Bpm:
        return QStringLiteral("TimeStationFrequencyKhz_Bpm");
    case WwvTimecodeDecoder::Profile::Wwv:
    default:
        return QStringLiteral("TimeStationFrequencyKhz_Wwv");
    }
}

double defaultFrequencyKhz(WwvTimecodeDecoder::Profile profile)
{
    const auto presets = presetsForProfile(profile);
    return presets.empty() ? 10000.0 : presets.front().khz;
}

QString defaultFrameText(WwvTimecodeDecoder::Profile profile)
{
    switch (profile) {
    case WwvTimecodeDecoder::Profile::Chu:
        return QStringLiteral("Frame: Awaiting validated CHU Bell 103 burst.");
    case WwvTimecodeDecoder::Profile::Bpm:
        return QStringLiteral("Frame: Awaiting BPM minute alignment.");
    case WwvTimecodeDecoder::Profile::Wwv:
    default:
        return QStringLiteral("Frame: Awaiting validated WWV / WWVH minute frame.");
    }
}

QString sliceModeForProfile(WwvTimecodeDecoder::Profile profile)
{
    return profile == WwvTimecodeDecoder::Profile::Chu ? QStringLiteral("USB")
                                                       : QStringLiteral("AM");
}

std::pair<int, int> sliceFilterForProfile(WwvTimecodeDecoder::Profile profile)
{
    if (profile == WwvTimecodeDecoder::Profile::Chu)
        return {100, 2600};
    return {-3500, 3500};
}

QString badgeStyle(const QString& bg)
{
    return QStringLiteral(
               "QLabel { background: %1; color: white; font-weight: bold; "
               "padding: 10px 18px; border-radius: 4px; min-width: 44px; }")
        .arg(bg);
}

QString actionButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: #0d3b84; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; }");
}

} // namespace

WwvDialog::WwvDialog(RadioModel* radioModel, QWidget* parent)
    : QDialog(parent)
    , m_radioModel(radioModel)
{
    setWindowTitle(QStringLiteral("Time Stations"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1120, 760);

    buildUi();
    connect(&m_decoder, &WwvTimecodeDecoder::currentTimeUpdated,
            this, &WwvDialog::onCurrentTimeUpdated);
    connect(&m_decoder, &WwvTimecodeDecoder::frameDecoded,
            this, &WwvDialog::onFrameDecoded);
    connect(&m_decoder, &WwvTimecodeDecoder::metricsUpdated,
            this, &WwvDialog::onMetricsUpdated);
    connect(&m_decoder, &WwvTimecodeDecoder::statusChanged,
            this, &WwvDialog::onStatusChanged);

    loadSettings();
    clearHistory();
}

WwvDialog::~WwvDialog()
{
    saveSettings();
}

void WwvDialog::setActiveSlice(SliceModel* slice)
{
    if (slice == m_activeSlice)
        return;

    m_activeSlice = slice;
    // Assigning {} only drops the handle; the old slice would stay connected.
    disconnect(m_sliceFrequencyConnection);
    disconnect(m_sliceModeConnection);

    if (slice) {
        m_sliceFrequencyConnection = connect(slice, &SliceModel::frequencyChanged,
                                            this, &WwvDialog::syncControlsFromSlice);
        m_sliceModeConnection = connect(slice, &SliceModel::modeChanged,
                                        this, &WwvDialog::syncControlsFromSlice);
    }

    syncControlsFromSlice();
    updateSliceSummary();
}

void WwvDialog::feedAudio(const QByteArray& pcm24kStereo)
{
    m_decoder.feedAudio(pcm24kStereo);
}

void WwvDialog::applyFrequencyPreset(int index)
{
    if (m_syncingControls || index <= 0)
        return;

    const auto presets = presetsForProfile(currentProfile());
    if (index - 1 >= static_cast<int>(presets.size()))
        return;

    const auto& preset = presets[static_cast<size_t>(index - 1)];
    m_frequencySpin->setValue(preset.khz);
    applySliceTuning(true);
}

void WwvDialog::clearHistory()
{
    m_historyEdit->clear();
    m_bitsLabel->setText(QStringLiteral("Bits: --"));
    m_frameLabel->setText(defaultFrameText(currentProfile()));
    m_flagsLabel->setText(QStringLiteral("Flags: --"));
    m_stationLabel->setText(
        QStringLiteral("Detected: %1").arg(WwvTimecodeDecoder::profileName(currentProfile())));
    m_lockLabel->setText(QStringLiteral("Lock: Searching"));
    m_metricsLabel->setText(QStringLiteral("Tick -- dB  |  Code -- dB"));
    m_decoder.reset();
}

void WwvDialog::onCurrentTimeUpdated(const QString& utcText)
{
    m_currentUtcLabel->setText(utcText);
}

void WwvDialog::onFrameDecoded(const QString& utcText,
                               const QString& frameText,
                               const QString& flagsText,
                               const QString& bitsText)
{
    m_frameLabel->setText(QStringLiteral("Frame: %1").arg(frameText));
    m_flagsLabel->setText(QStringLiteral("Flags: %1").arg(flagsText));
    m_bitsLabel->setText(QStringLiteral("Bits: %1").arg(bitsText));

    auto cursor = m_historyEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("[%1] %2\n%3\n%4\nBits %5\n\n")
                          .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")),
                               utcText,
                               frameText,
                               flagsText,
                               bitsText));
    m_historyEdit->setTextCursor(cursor);
    m_historyEdit->ensureCursorVisible();
}

void WwvDialog::onFrequencyEdited(double)
{
    if (m_syncingControls)
        return;

    syncFrequencyPresetSelection();
    applySliceTuning(false);
}

void WwvDialog::onStationProfileChanged(int)
{
    if (m_syncingControls)
        return;

    auto& settings = AppSettings::instance();
    settings.setValue(frequencySettingsKey(m_decoder.profile()),
                      QString::number(m_frequencySpin->value(), 'f', 1));

    const auto profile = currentProfile();
    m_decoder.setProfile(profile);

    m_syncingControls = true;
    repopulateFrequencyPresets();
    m_frequencySpin->setValue(
        settings.value(frequencySettingsKey(profile), defaultFrequencyKhz(profile)).toDouble());
    syncFrequencyPresetSelection();
    m_syncingControls = false;

    clearHistory();
    applySliceTuning(true);
    saveSettings();
}

void WwvDialog::onMetricsUpdated(const QString& station,
                                 float tickLevelDb,
                                 float codeLevelDb,
                                 bool locked,
                                 int secondOfMinute,
                                 bool minuteMarker,
                                 bool hourMarker,
                                 const QString& pulseClass)
{
    m_stationLabel->setText(QStringLiteral("Detected: %1").arg(station));
    m_lockLabel->setText(QStringLiteral("Lock: %1").arg(locked ? QStringLiteral("Locked") : QStringLiteral("Searching")));
    m_metricsLabel->setText(
        QStringLiteral("Tick %1 dB  |  Code %2 dB  |  Sec %3  |  Pulse %4%5%6")
            .arg(QString::number(tickLevelDb, 'f', 1),
                 QString::number(codeLevelDb, 'f', 1),
                 secondOfMinute >= 0 ? QString::number(secondOfMinute) : QStringLiteral("--"),
                 pulseClass,
                 minuteMarker ? QStringLiteral("  |  Minute") : QString(),
                 hourMarker ? QStringLiteral("  |  Hour") : QString()));
}

void WwvDialog::onStatusChanged(const QString& text)
{
    m_statusLabel->setText(text);
}

void WwvDialog::syncControlsFromSlice()
{
    if (!m_activeSlice)
        return;

    m_syncingControls = true;
    m_frequencySpin->setValue(m_activeSlice->frequency() * 1000.0);
    syncFrequencyPresetSelection();
    m_syncingControls = false;
    updateSliceSummary();
}

void WwvDialog::applySliceTuning(bool recenter)
{
    if (m_syncingControls || !m_activeSlice)
        return;

    const double mhz = m_frequencySpin->value() / 1000.0;
    const auto profile = currentProfile();
    if (recenter)
        m_activeSlice->tuneAndRecenter(mhz);
    else
        m_activeSlice->setFrequency(mhz);

    const QString targetMode = sliceModeForProfile(profile);
    if (m_activeSlice->mode() != targetMode)
        m_activeSlice->setMode(targetMode);
    const auto [filterLow, filterHigh] = sliceFilterForProfile(profile);
    m_activeSlice->setFilterWidth(filterLow, filterHigh);
    updateSliceSummary();
}

void WwvDialog::buildUi()
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
    rxBadge->setStyleSheet(badgeStyle(QStringLiteral("#646464")));
    grid->addWidget(rxBadge, 0, 0, 2, 1);

    grid->addWidget(new QLabel(QStringLiteral("Station:"), controls), 0, 1);
    m_stationCombo = new QComboBox(controls);
    m_stationCombo->addItem(QStringLiteral("WWV / WWVH"),
                            static_cast<int>(WwvTimecodeDecoder::Profile::Wwv));
    m_stationCombo->addItem(QStringLiteral("CHU"),
                            static_cast<int>(WwvTimecodeDecoder::Profile::Chu));
    m_stationCombo->addItem(QStringLiteral("BPM"),
                            static_cast<int>(WwvTimecodeDecoder::Profile::Bpm));
    grid->addWidget(m_stationCombo, 0, 2);

    grid->addWidget(new QLabel(QStringLiteral("Freq:"), controls), 0, 3);
    m_frequencyCombo = new QComboBox(controls);
    grid->addWidget(m_frequencyCombo, 0, 4);

    m_frequencySpin = new QDoubleSpinBox(controls);
    m_frequencySpin->setDecimals(1);
    m_frequencySpin->setRange(100.0, 30000.0);
    m_frequencySpin->setSingleStep(0.5);
    m_frequencySpin->setSuffix(QStringLiteral(" kHz"));
    grid->addWidget(m_frequencySpin, 0, 5);

    m_stationLabel = new QLabel(QStringLiteral("Detected: Unknown"), controls);
    grid->addWidget(m_stationLabel, 0, 6);

    m_lockLabel = new QLabel(QStringLiteral("Lock: Searching"), controls);
    grid->addWidget(m_lockLabel, 0, 7);

    auto* clearButton = new QPushButton(QStringLiteral("Erase"), controls);
    clearButton->setStyleSheet(actionButtonStyle());
    grid->addWidget(clearButton, 0, 8);

    m_metricsLabel = new QLabel(QStringLiteral("Tick -- dB  |  Code -- dB"), controls);
    grid->addWidget(m_metricsLabel, 1, 1, 1, 5);

    m_sliceSummaryLabel = new QLabel(QStringLiteral("No active slice selected."), controls);
    grid->addWidget(m_sliceSummaryLabel, 1, 6, 1, 3);

    root->addWidget(controls);

    auto* statusPanel = new QWidget(this);
    statusPanel->setStyleSheet(QStringLiteral(
        "QWidget { background: #e6eef5; border-radius: 6px; color: #17324a; }"
        "QLabel { color: #17324a; background: transparent; }"));
    auto* statusLayout = new QVBoxLayout(statusPanel);
    statusLayout->setContentsMargins(12, 12, 12, 12);
    statusLayout->setSpacing(8);

    m_currentUtcLabel = new QLabel(QStringLiteral("UTC: Awaiting time-station lock."), statusPanel);
    m_currentUtcLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: #17324a; background: transparent; font-size: 20px; font-weight: bold; }"));
    statusLayout->addWidget(m_currentUtcLabel);

    m_frameLabel = new QLabel(QStringLiteral("Frame: Awaiting validated timecode frame."), statusPanel);
    m_frameLabel->setWordWrap(true);
    statusLayout->addWidget(m_frameLabel);

    m_flagsLabel = new QLabel(QStringLiteral("Flags: --"), statusPanel);
    m_flagsLabel->setWordWrap(true);
    statusLayout->addWidget(m_flagsLabel);

    m_bitsLabel = new QLabel(QStringLiteral("Bits: --"), statusPanel);
    m_bitsLabel->setWordWrap(true);
    m_bitsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLayout->addWidget(m_bitsLabel);

    m_statusLabel = new QLabel(QStringLiteral("Time-station decoder ready."), statusPanel);
    m_statusLabel->setWordWrap(true);
    statusLayout->addWidget(m_statusLabel);

    root->addWidget(statusPanel);

    m_historyEdit = new QPlainTextEdit(this);
    m_historyEdit->setReadOnly(true);
    m_historyEdit->setStyleSheet(
        "QPlainTextEdit { background: #1f1f1f; color: #f6f9fc; border: 1px solid #303030; "
        "padding: 10px; font-family: Menlo, monospace; font-size: 14px; }");
    root->addWidget(m_historyEdit, 1);

    connect(m_stationCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &WwvDialog::onStationProfileChanged);
    connect(m_frequencyCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &WwvDialog::applyFrequencyPreset);
    connect(m_frequencySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &WwvDialog::onFrequencyEdited);
    connect(clearButton, &QPushButton::clicked,
            this, &WwvDialog::clearHistory);
}

void WwvDialog::loadSettings()
{
    auto& settings = AppSettings::instance();
    int profileValue = settings.value(QStringLiteral("TimeStationProfile"),
                                      static_cast<int>(WwvTimecodeDecoder::Profile::Wwv)).toInt();
    profileValue = std::clamp(profileValue,
                              static_cast<int>(WwvTimecodeDecoder::Profile::Wwv),
                              static_cast<int>(WwvTimecodeDecoder::Profile::Bpm));
    const auto profile = static_cast<WwvTimecodeDecoder::Profile>(profileValue);
    const double savedKhz =
        settings.value(frequencySettingsKey(profile), defaultFrequencyKhz(profile)).toDouble();

    m_syncingControls = true;
    {
        QSignalBlocker blocker(m_stationCombo);
        const int comboIndex = m_stationCombo->findData(static_cast<int>(profile));
        m_stationCombo->setCurrentIndex(comboIndex >= 0 ? comboIndex : 0);
    }
    m_decoder.setProfile(profile);
    repopulateFrequencyPresets();
    m_frequencySpin->setValue(savedKhz);
    syncFrequencyPresetSelection();
    m_syncingControls = false;
}

void WwvDialog::saveSettings() const
{
    auto& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("TimeStationProfile"), static_cast<int>(currentProfile()));
    settings.setValue(frequencySettingsKey(currentProfile()),
                      QString::number(m_frequencySpin->value(), 'f', 1));
    settings.save();
}

WwvTimecodeDecoder::Profile WwvDialog::currentProfile() const
{
    if (!m_stationCombo)
        return m_decoder.profile();

    bool ok = false;
    const int value = m_stationCombo->currentData().toInt(&ok);
    if (!ok)
        return m_decoder.profile();

    if (value < static_cast<int>(WwvTimecodeDecoder::Profile::Wwv)
        || value > static_cast<int>(WwvTimecodeDecoder::Profile::Bpm)) {
        return m_decoder.profile();
    }
    return static_cast<WwvTimecodeDecoder::Profile>(value);
}

void WwvDialog::repopulateFrequencyPresets()
{
    QSignalBlocker blocker(m_frequencyCombo);
    m_frequencyCombo->clear();
    m_frequencyCombo->addItem(QStringLiteral("Manual"));
    for (const auto& preset : presetsForProfile(currentProfile()))
        m_frequencyCombo->addItem(QString::fromLatin1(preset.label));
}

void WwvDialog::syncFrequencyPresetSelection()
{
    int matchedIndex = 0;
    const auto presets = presetsForProfile(currentProfile());
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        if (std::fabs(presets[static_cast<size_t>(i)].khz - m_frequencySpin->value()) < 0.15) {
            matchedIndex = i + 1;
            break;
        }
    }

    QSignalBlocker blocker(m_frequencyCombo);
    m_frequencyCombo->setCurrentIndex(matchedIndex);
}

void WwvDialog::updateSliceSummary()
{
    if (!m_activeSlice) {
        m_sliceSummaryLabel->setText(QStringLiteral("No active slice selected."));
        return;
    }

    m_sliceSummaryLabel->setText(
        QStringLiteral("Slice %1  |  %2  |  %3 MHz")
            .arg(m_activeSlice->sliceId())
            .arg(m_activeSlice->mode())
            .arg(QString::number(m_activeSlice->frequency(), 'f', 6)));
}

} // namespace AetherSDR
