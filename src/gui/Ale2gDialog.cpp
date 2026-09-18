#include "Ale2gDialog.h"

#include "DigitalModeStyles.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

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

// HFLink amateur ALE channels (USB dial / suppressed-carrier frequencies),
// from https://hflink.com/channels/ ("ALE Selcall Channels Frequencies List").
// HFN = HFLink HF Net (base-station net; sounding at 60 min on pilot channels),
// HFL = HFLink "HFL" net.  Retrieved 2026-09.  VHF/UHF entries (70/144/432 MHz)
// are omitted.  60 m channels are subject to national channelisation rules.
struct AlePresetChannel {
    const char* name;
    double khz;
};

constexpr AlePresetChannel kHflinkHfn[] = {
    {"01B", 1843.0},  {"03A", 3527.0},  {"03D", 3596.0},  {"07D", 7102.0},
    {"10D", 10145.5}, {"14B", 14109.0}, {"18A", 18106.0}, {"21B", 21096.0},
    {"24B", 24926.0}, {"28B", 28146.0},
};

constexpr AlePresetChannel kHflinkHfl[] = {
    {"00B", 475.5},   {"01D", 1909.0},  {"03E", 3600.5},  {"03H", 3791.0},
    {"05C", 5354.5},  {"05E", 5357.0},  {"05G", 5371.5},  {"07B", 7049.5},
    {"07E", 7185.0},  {"07H", 7296.0},  {"10B", 10131.0}, {"14E", 14346.0},
    {"18D", 18117.5}, {"21E", 21432.5}, {"24C", 24932.0}, {"28D", 28312.5},
    {"50B", 50162.5},
};

QVector<Ale2gChannel> presetChannels(int preset)
{
    QVector<Ale2gChannel> out;
    auto add = [&out](const AlePresetChannel& c) {
        out.push_back(Ale2gChannel{QString::fromLatin1(c.name),
                                   static_cast<qint64>(c.khz * 1000.0 + 0.5), false});
    };
    if (preset == 0 || preset == 2) {
        for (const auto& c : kHflinkHfn) {
            add(c);
        }
    }
    if (preset == 1 || preset == 2) {
        for (const auto& c : kHflinkHfl) {
            add(c);
        }
    }
    if (preset == 2) {
        std::sort(out.begin(), out.end(), [](const Ale2gChannel& a, const Ale2gChannel& b) {
            return a.frequencyHz < b.frequencyHz;
        });
    }
    return out;
}

QString serializeChannels(const QVector<Ale2gChannel>& channels)
{
    QStringList parts;
    for (const Ale2gChannel& c : channels) {
        QString name = c.name;
        name.remove(QLatin1Char('|'));
        name.remove(QLatin1Char(';'));
        parts << QStringLiteral("%1|%2|%3")
                     .arg(name, QString::number(c.frequencyHz / 1000.0, 'f', 3),
                          c.sound ? QStringLiteral("1") : QStringLiteral("0"));
    }
    return parts.join(QLatin1Char(';'));
}

QVector<Ale2gChannel> parseChannels(const QString& text)
{
    QVector<Ale2gChannel> out;
    const QStringList entries = text.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& entry : entries) {
        const QStringList f = entry.split(QLatin1Char('|'));
        if (f.size() < 2) {
            continue;
        }
        bool ok = false;
        const double khz = f.at(1).toDouble(&ok);
        if (!ok || khz <= 0.0) {
            continue;
        }
        out.push_back(Ale2gChannel{f.at(0).trimmed(), static_cast<qint64>(khz * 1000.0 + 0.5),
                                   f.value(2) == QStringLiteral("1")});
    }
    return out;
}

QString aleStateColor(Ale2gController::State state)
{
    switch (state) {
    case Ale2gController::State::Idle:       return QStringLiteral("#646464");
    case Ale2gController::State::Scanning:   return QStringLiteral("#0d3b84");
    case Ale2gController::State::Sounding:   return QStringLiteral("#8a5a00");
    case Ale2gController::State::Calling:    return QStringLiteral("#b36b00");
    case Ale2gController::State::Responding: return QStringLiteral("#b36b00");
    case Ale2gController::State::Linked:     return QStringLiteral("#1e7a34");
    }
    return QStringLiteral("#646464");
}

QString formatFrequencyKhz(double khz)
{
    QString text = QString::number(khz, 'f', 2);
    while (text.contains(QLatin1Char('.'))
           && (text.endsWith(QLatin1Char('0')) || text.endsWith(QLatin1Char('.')))) {
        text.chop(1);
    }
    return text;
}

} // namespace

Ale2gDialog::Ale2gDialog(RadioModel* radioModel, QWidget* parent)
    : QDialog(parent)
    , m_radioModel(radioModel)
{
    setWindowTitle(QStringLiteral("2G ALE"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1240, 820);

    buildUi();
    loadFrequencyGroups();
    loadSettings();

    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(2250);
    connect(m_scanTimer, &QTimer::timeout, this, &Ale2gDialog::onScanTick);

    connect(&m_decoder, &Ale2gDecoder::frameDecoded,
            this, &Ale2gDialog::onFrameDecoded);
    connect(&m_decoder, &Ale2gDecoder::metricsUpdated,
            this, &Ale2gDialog::onMetricsUpdated);
    connect(&m_decoder, &Ale2gDecoder::statusChanged,
            this, &Ale2gDialog::onStatusChanged);
    connect(&m_decoder, &Ale2gDecoder::wordDecoded,
            this, &Ale2gDialog::onWordDecoded);

    // ALE link controller.  Transmit stays disabled until the user enables it.
    connect(&m_controller, &Ale2gController::stateChanged,
            this, &Ale2gDialog::onAleStateChanged);
    connect(&m_controller, &Ale2gController::requestTune,
            this, &Ale2gDialog::onAleTuneRequested);
    connect(&m_controller, &Ale2gController::requestTransmit,
            this, &Ale2gDialog::onAleTransmitRequested);
    connect(&m_controller, &Ale2gController::txAudio,
            this, &Ale2gDialog::aleTxAudioReady);
    connect(&m_controller, &Ale2gController::frameReceived,
            this, &Ale2gDialog::onAleFrame);
    connect(&m_controller, &Ale2gController::amdReceived,
            this, &Ale2gDialog::onAleAmd);
    connect(&m_controller, &Ale2gController::logMessage,
            this, &Ale2gDialog::onAleLog);
    connect(&m_controller, &Ale2gController::errorOccurred,
            this, &Ale2gDialog::onAleError);
    connect(&m_controller, &Ale2gController::heardStationsChanged,
            this, &Ale2gDialog::onAleHeardChanged);
    loadAleSettings();
    applyAleSettings();
    updateAleButtons();

    // Opening the dialog must not retune the radio: a saved "Scan" selection
    // used to start hopping frequencies immediately. The user starts scanning
    // by choosing a group/channel or pressing Start ALE.
    if (m_groupCombo->currentIndex() > 0 && m_frequencyCombo->currentIndex() == 0) {
        m_statusLabel->setText(QStringLiteral(
            "2G ALE decoder ready. Scan is paused — reselect the channel group or press Start ALE to scan."));
    }

    updateSliceSummary();
}

Ale2gDialog::~Ale2gDialog()
{
    // Stop the controller (and release PTT) while every member is still alive,
    // then detach it so nothing it emits during destruction reaches us.
    m_controller.stop();
    if (m_controller.isKeyed()) {
        emit aleTransmitRequested(false);
    }
    disconnect(&m_controller, nullptr, this, nullptr);
    saveSettings();
    saveAleSettings();
}

void Ale2gDialog::setActiveSlice(SliceModel* slice)
{
    if (slice == m_activeSlice)
        return;

    // Only carry a grouped channel selection over when switching slices while
    // the dialog is already in use. The first attach (dialog just opened) must
    // not retune: opening a window should never change the radio's frequency,
    // mode or filter — the radio is authoritative until the user acts.
    const bool firstAttach = (m_activeSlice == nullptr);
    // A plain channel selection is not carried over to a newly selected slice
    // (that silently retuned the other slice). Only an active scan — which the
    // user explicitly started — follows the active slice.
    const bool preserveGroupedSelection = false;
    Q_UNUSED(firstAttach);
    const bool preserveScan = m_scanTimer && m_scanTimer->isActive();

    m_activeSlice = slice;
    // Assigning {} only drops the handle; the old slice would stay connected.
    disconnect(m_sliceFrequencyConnection);
    disconnect(m_sliceModeConnection);

    if (slice) {
        m_sliceFrequencyConnection = connect(slice, &SliceModel::frequencyChanged,
                                            this, &Ale2gDialog::syncControlsFromSlice);
        m_sliceModeConnection = connect(slice, &SliceModel::modeChanged,
                                        this, &Ale2gDialog::syncControlsFromSlice);
    }

    if (preserveScan && isGroupSelectionActive()) {
        const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
        if (!group.frequencies.isEmpty()) {
            if (m_scanPosition < 0 || m_scanPosition >= group.frequencies.size())
                m_scanPosition = 0;
            const double khz = group.frequencies[static_cast<size_t>(m_scanPosition)];
            {
                const QSignalBlocker blocker(m_frequencySpin);
                m_frequencySpin->setValue(khz);
            }
            tuneToFrequencyKhz(khz, false);
        }
    } else if (preserveGroupedSelection && isGroupSelectionActive() && m_frequencyCombo->currentIndex() > 0) {
        const double khz = m_frequencyCombo->currentData().toDouble();
        if (khz > 0.0) {
            {
                const QSignalBlocker blocker(m_frequencySpin);
                m_frequencySpin->setValue(khz);
            }
            tuneToFrequencyKhz(khz, false);
        }
    } else {
        syncControlsFromSlice();
    }

    updateSliceSummary();
}

void Ale2gDialog::feedAudio(const QByteArray& pcm24kStereo)
{
    m_decoder.feedAudio(pcm24kStereo);
    m_controller.feedRxAudio(pcm24kStereo);
}

void Ale2gDialog::setRadioTransmitting(bool transmitting)
{
    m_controller.setRadioTransmitting(transmitting);
}

void Ale2gDialog::clearOutput()
{
    m_table->setRowCount(0);
    m_historyEdit->clear();
    m_decoderLocked = false;
    m_lastAleActivityMs = 0;
    m_decoder.reset();
    m_statusLabel->setText(QStringLiteral("2G ALE decoder ready."));
}

int Ale2gDialog::findFrequencyIndex(const QVector<double>& frequencies, double khz) const
{
    for (int i = 0; i < frequencies.size(); ++i) {
        if (std::fabs(frequencies.at(i) - khz) < 0.11)
            return i;
    }
    return -1;
}

bool Ale2gDialog::isGroupSelectionActive() const
{
    return m_groupCombo
           && m_groupCombo->currentIndex() > 0
           && (m_groupCombo->currentIndex() - 1) < m_frequencyGroups.size();
}

void Ale2gDialog::loadFrequencyGroups()
{
    m_frequencyGroups.clear();

    QFile file(QStringLiteral(":/data/ale2g_catalog.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_statusLabel->setText(QStringLiteral("Unable to load the 2G ALE network catalog."));
        return;
    }

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const int separator = line.indexOf(QLatin1Char('|'));
        if (separator <= 0)
            continue;

        FrequencyGroup group;
        group.name = line.left(separator).trimmed();
        const QStringList frequencyParts =
            line.mid(separator + 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& frequencyText : frequencyParts) {
            bool ok = false;
            const double khz = frequencyText.trimmed().toDouble(&ok);
            if (!ok || khz <= 0.0)
                continue;
            if (findFrequencyIndex(group.frequencies, khz) < 0)
                group.frequencies.push_back(khz);
        }

        if (!group.name.isEmpty() && !group.frequencies.isEmpty())
            m_frequencyGroups.push_back(group);
    }

    const QSignalBlocker blocker(m_groupCombo);
    m_groupCombo->clear();
    m_groupCombo->addItem(QStringLiteral("Manual"));
    for (const auto& group : m_frequencyGroups)
        m_groupCombo->addItem(group.name);

    m_groupCombo->setEnabled(!m_frequencyGroups.isEmpty());
}

void Ale2gDialog::loadSettings()
{
    auto& settings = AppSettings::instance();
    const double savedKhz =
        settings.value(QStringLiteral("Ale2gFrequencyKhz"), 11175.0).toDouble();
    const QString savedGroup =
        settings.value(QStringLiteral("Ale2gGroupName"), QStringLiteral("Manual")).toString();
    const QString savedSelection =
        settings.value(QStringLiteral("Ale2gFrequencySelection"), QStringLiteral("Manual")).toString();

    m_syncingControls = true;
    m_frequencySpin->setValue(savedKhz);

    int groupIndex = 0;
    for (int i = 0; i < m_frequencyGroups.size(); ++i) {
        if (m_frequencyGroups.at(i).name == savedGroup) {
            groupIndex = i + 1;
            break;
        }
    }
    m_groupCombo->setCurrentIndex(groupIndex);
    rebuildFrequencySelectionCombo();

    if (groupIndex > 0) {
        if (savedSelection.compare(QStringLiteral("Scan"), Qt::CaseInsensitive) == 0) {
            m_frequencyCombo->setCurrentIndex(0);
            if (!m_frequencyGroups.at(groupIndex - 1).frequencies.isEmpty())
                m_frequencySpin->setValue(m_frequencyGroups.at(groupIndex - 1).frequencies.first());
        } else {
            bool ok = false;
            const double presetKhz = savedSelection.toDouble(&ok);
            const int frequencyIndex =
                ok ? findFrequencyIndex(m_frequencyGroups.at(groupIndex - 1).frequencies, presetKhz) : -1;
            if (frequencyIndex >= 0) {
                m_frequencyCombo->setCurrentIndex(frequencyIndex + 1);
                m_frequencySpin->setValue(m_frequencyGroups.at(groupIndex - 1).frequencies.at(frequencyIndex));
            } else {
                m_groupCombo->setCurrentIndex(0);
                rebuildFrequencySelectionCombo();
                m_frequencyCombo->setCurrentIndex(0);
            }
        }
    } else {
        m_frequencyCombo->setCurrentIndex(0);
    }
    m_syncingControls = false;
}

void Ale2gDialog::saveSettings() const
{
    auto& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("Ale2gFrequencyKhz"),
                      QString::number(m_frequencySpin->value(), 'f', 2));
    settings.setValue(QStringLiteral("Ale2gGroupName"),
                      isGroupSelectionActive() ? m_groupCombo->currentText()
                                               : QStringLiteral("Manual"));

    QString selection = QStringLiteral("Manual");
    if (isGroupSelectionActive()) {
        if (m_frequencyCombo->currentIndex() <= 0) {
            selection = QStringLiteral("Scan");
        } else {
            selection = QString::number(m_frequencyCombo->currentData().toDouble(), 'f', 2);
        }
    }
    settings.setValue(QStringLiteral("Ale2gFrequencySelection"), selection);
    settings.save();
}

void Ale2gDialog::rebuildFrequencySelectionCombo()
{
    const QSignalBlocker blocker(m_frequencyCombo);
    m_frequencyCombo->clear();

    if (!isGroupSelectionActive()) {
        m_frequencyCombo->addItem(QStringLiteral("Manual"));
        return;
    }

    const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
    m_frequencyCombo->addItem(QStringLiteral("Scan"));
    for (double khz : group.frequencies)
        m_frequencyCombo->addItem(formatFrequencyKhz(khz), khz);
}

void Ale2gDialog::startScan()
{
    if (!m_scanTimer || !isGroupSelectionActive())
        return;

    if (m_controller.isRunning()) {
        // Passive catalog scan retunes the slice; it cannot share it with ALE.
        m_controller.stop();
        updateAleButtons();
    }

    const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
    if (group.frequencies.isEmpty())
        return;

    const int matchIndex = findFrequencyIndex(group.frequencies, m_frequencySpin->value());
    m_scanPosition = matchIndex >= 0 ? matchIndex : 0;
    m_decoderLocked = false;
    m_lastAleActivityMs = 0;
    m_scanTimer->start();
}

void Ale2gDialog::stopScan()
{
    if (m_scanTimer)
        m_scanTimer->stop();
    m_scanPosition = -1;
}

void Ale2gDialog::syncSelectionToCurrentFrequency()
{
    if (!m_frequencyCombo)
        return;

    if (!isGroupSelectionActive()) {
        const QSignalBlocker blocker(m_frequencyCombo);
        m_frequencyCombo->setCurrentIndex(0);
        return;
    }

    if (m_scanTimer && m_scanTimer->isActive()) {
        const QSignalBlocker blocker(m_frequencyCombo);
        m_frequencyCombo->setCurrentIndex(0);
        return;
    }

    const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
    const int matchIndex = findFrequencyIndex(group.frequencies, m_frequencySpin->value());
    const QSignalBlocker blocker(m_frequencyCombo);
    m_frequencyCombo->setCurrentIndex(matchIndex >= 0 ? matchIndex + 1 : 0);
}

void Ale2gDialog::tuneToFrequencyKhz(double khz, bool recenter)
{
    if (khz <= 0.0)
        return;

    m_decoderLocked = false;
    m_lastAleActivityMs = 0;
    m_decoder.reset();
    m_statusLabel->setText(QStringLiteral("2G ALE decoder ready."));

    if (m_activeSlice) {
        const double mhz = khz / 1000.0;
        if (recenter)
            m_activeSlice->tuneAndRecenter(mhz);
        else
            m_activeSlice->setFrequency(mhz);

        if (m_activeSlice->mode() != QStringLiteral("DIGU"))
            m_activeSlice->setMode(QStringLiteral("DIGU"));
        m_activeSlice->setFilterWidth(500, 2800);
    }

    updateSliceSummary();
}

void Ale2gDialog::onGroupSelected(int)
{
    if (m_syncingControls)
        return;

    stopScan();
    rebuildFrequencySelectionCombo();

    if (!isGroupSelectionActive()) {
        updateSliceSummary();
        return;
    }

    const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
    if (group.frequencies.isEmpty()) {
        updateSliceSummary();
        return;
    }

    {
        const QSignalBlocker blocker(m_frequencySpin);
        m_frequencySpin->setValue(group.frequencies.first());
    }
    {
        const QSignalBlocker blocker(m_frequencyCombo);
        m_frequencyCombo->setCurrentIndex(0);
    }

    tuneToFrequencyKhz(group.frequencies.first(), true);
    startScan();
}

void Ale2gDialog::onFrequencySelectionChanged(int index)
{
    if (m_syncingControls)
        return;

    stopScan();

    if (!isGroupSelectionActive()) {
        updateSliceSummary();
        return;
    }

    const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
    if (group.frequencies.isEmpty()) {
        updateSliceSummary();
        return;
    }

    if (index <= 0) {
        {
            const QSignalBlocker blocker(m_frequencySpin);
            m_frequencySpin->setValue(group.frequencies.first());
        }
        tuneToFrequencyKhz(group.frequencies.first(), true);
        startScan();
        return;
    }

    const double khz = m_frequencyCombo->currentData().toDouble();
    if (khz > 0.0) {
        {
            const QSignalBlocker blocker(m_frequencySpin);
            m_frequencySpin->setValue(khz);
        }
        tuneToFrequencyKhz(khz, true);
    }
}

void Ale2gDialog::onScanTick()
{
    if (!isGroupSelectionActive() || !m_scanTimer || !m_scanTimer->isActive())
        return;

    const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
    if (group.frequencies.isEmpty())
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_decoderLocked || (m_lastAleActivityMs > 0 && (nowMs - m_lastAleActivityMs) < 6000)) {
        updateSliceSummary();
        return;
    }

    m_scanPosition = (m_scanPosition + 1) % group.frequencies.size();
    const double khz = group.frequencies.at(m_scanPosition);
    {
        const QSignalBlocker blocker(m_frequencySpin);
        m_frequencySpin->setValue(khz);
    }
    tuneToFrequencyKhz(khz, false);
}

void Ale2gDialog::onFrameDecoded(const QString& utcText, const QString& summary)
{
    m_lastAleActivityMs = QDateTime::currentMSecsSinceEpoch();

    auto cursor = m_historyEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("[%1] %2\n").arg(utcText, summary));
    m_historyEdit->setTextCursor(cursor);
    m_historyEdit->ensureCursorVisible();
}

void Ale2gDialog::onFrequencyEdited(double khz)
{
    if (m_syncingControls)
        return;

    stopScan();

    if (m_groupCombo->currentIndex() != 0) {
        {
            const QSignalBlocker blocker(m_groupCombo);
            m_groupCombo->setCurrentIndex(0);
        }
        rebuildFrequencySelectionCombo();
    }

    Q_UNUSED(khz);
    syncSelectionToCurrentFrequency();
    tuneToFrequencyKhz(m_frequencySpin->value(), false);
}

void Ale2gDialog::onMetricsUpdated(bool locked,
                                   int phaseSamples,
                                   int boundary,
                                   float score,
                                   int totalWords)
{
    m_decoderLocked = locked;
    m_lockLabel->setText(
        QStringLiteral("Link: %1").arg(locked ? QStringLiteral("Locked")
                                              : QStringLiteral("Searching")));
    m_metricsLabel->setText(
        QStringLiteral("Phase %1  |  Boundary %2  |  Score %3  |  Words %4")
            .arg(phaseSamples >= 0 ? QString::number(phaseSamples) : QStringLiteral("--"),
                 boundary >= 0 ? QString::number(boundary) : QStringLiteral("--"),
                 QString::number(score, 'f', 2),
                 QString::number(totalWords)));
    updateSliceSummary();
}

void Ale2gDialog::onStatusChanged(const QString& text)
{
    m_statusLabel->setText(text);
}

void Ale2gDialog::onWordDecoded(const QString& utcText,
                                const QString& preamble,
                                const QString& text,
                                int errors,
                                float confidence,
                                const QString& summary)
{
    m_lastAleActivityMs = QDateTime::currentMSecsSinceEpoch();

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(utcText));
    m_table->setItem(row, 1, new QTableWidgetItem(preamble));
    m_table->setItem(row, 2, new QTableWidgetItem(text));
    m_table->setItem(row, 3, new QTableWidgetItem(QString::number(errors)));
    m_table->setItem(row, 4, new QTableWidgetItem(QString::number(confidence, 'f', 2)));
    m_table->setItem(row, 5, new QTableWidgetItem(summary));
    m_table->scrollToBottom();
}

void Ale2gDialog::syncControlsFromSlice()
{
    if (!m_activeSlice)
        return;

    const double sliceKhz = m_activeSlice->frequency() * 1000.0;
    m_syncingControls = true;
    m_frequencySpin->setValue(sliceKhz);

    if (!isGroupSelectionActive()) {
        syncSelectionToCurrentFrequency();
    } else if (m_scanTimer && m_scanTimer->isActive()) {
        const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
        const int matchIndex = findFrequencyIndex(group.frequencies, sliceKhz);
        if (matchIndex >= 0)
            m_scanPosition = matchIndex;
        syncSelectionToCurrentFrequency();
    } else {
        const auto& group = m_frequencyGroups[static_cast<size_t>(m_groupCombo->currentIndex() - 1)];
        const int matchIndex = findFrequencyIndex(group.frequencies, sliceKhz);
        if (matchIndex >= 0) {
            m_frequencyCombo->setCurrentIndex(matchIndex + 1);
        } else {
            m_groupCombo->setCurrentIndex(0);
            rebuildFrequencySelectionCombo();
            m_frequencyCombo->setCurrentIndex(0);
        }
    }

    m_syncingControls = false;
    updateSliceSummary();
}

void Ale2gDialog::buildUi()
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
    grid->setColumnStretch(4, 1);
    grid->setColumnStretch(6, 1);

    auto* rxBadge = new QLabel(QStringLiteral("RX"), controls);
    rxBadge->setStyleSheet(badgeStyle(QStringLiteral("#646464")));
    grid->addWidget(rxBadge, 0, 0, 2, 1);

    grid->addWidget(new QLabel(QStringLiteral("Network:"), controls), 0, 1);
    m_groupCombo = new QComboBox(controls);
    m_groupCombo->setMinimumContentsLength(22);
    m_groupCombo->setMaxVisibleItems(24);
    grid->addWidget(m_groupCombo, 0, 2);

    grid->addWidget(new QLabel(QStringLiteral("Freq:"), controls), 0, 3);
    m_frequencyCombo = new QComboBox(controls);
    m_frequencyCombo->setMinimumContentsLength(16);
    m_frequencyCombo->setMaxVisibleItems(24);
    grid->addWidget(m_frequencyCombo, 0, 4);

    m_frequencySpin = new QDoubleSpinBox(controls);
    m_frequencySpin->setDecimals(2);
    m_frequencySpin->setRange(100.0, 30000.0);
    m_frequencySpin->setSingleStep(0.1);
    m_frequencySpin->setSuffix(QStringLiteral(" kHz"));
    grid->addWidget(m_frequencySpin, 0, 5);

    m_lockLabel = new QLabel(QStringLiteral("Link: Searching"), controls);
    grid->addWidget(m_lockLabel, 0, 6);

    auto* clearButton = new QPushButton(QStringLiteral("Erase"), controls);
    clearButton->setStyleSheet(actionButtonStyle());
    grid->addWidget(clearButton, 0, 7);

    m_metricsLabel = new QLabel(QStringLiteral("Phase --  |  Boundary --  |  Score 0.00  |  Words 0"),
                                controls);
    grid->addWidget(m_metricsLabel, 1, 1, 1, 4);

    m_sliceSummaryLabel = new QLabel(QStringLiteral("No active slice selected."), controls);
    m_sliceSummaryLabel->setWordWrap(true);
    grid->addWidget(m_sliceSummaryLabel, 1, 5, 1, 3);

    root->addWidget(controls);
    root->addWidget(buildAlePanel());

    m_tabs = new QTabWidget(this);
    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("UTC"), QStringLiteral("Preamble"), QStringLiteral("Text"),
         QStringLiteral("Err"), QStringLiteral("Conf"), QStringLiteral("Summary")});
    m_table->setEditTriggers(QTableWidget::NoEditTriggers);
    m_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_table->setSelectionMode(QTableWidget::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->setStyleSheet(
        "QTableWidget { background: #1f1f1f; color: #f6f9fc; border: 1px solid #303030; "
        "alternate-background-color: #252525; }"
        "QHeaderView::section { background: #121212; color: #dce7f3; padding: 6px; border: none; }"
        "QTableWidget::item:selected { background: #274b75; color: white; }");
    m_tabs->addTab(m_table, QStringLiteral("Decoded Words"));

    m_historyEdit = new QPlainTextEdit(this);
    m_historyEdit->setReadOnly(true);
    m_historyEdit->setMaximumBlockCount(400);
    m_historyEdit->setMinimumHeight(140);
    m_historyEdit->setStyleSheet(
        "QPlainTextEdit { background: #202020; color: #f5f7fa; border: 1px solid #303030; "
        "padding: 10px; font-family: Menlo, monospace; font-size: 14px; }");
    m_tabs->addTab(m_historyEdit, QStringLiteral("Frames"));

    m_aleLogEdit = new QPlainTextEdit(this);
    m_aleLogEdit->setReadOnly(true);
    m_aleLogEdit->setMaximumBlockCount(2000);
    m_aleLogEdit->setStyleSheet(m_historyEdit->styleSheet());
    m_tabs->addTab(m_aleLogEdit, QStringLiteral("ALE Log / AMD"));

    m_heardTable = new QTableWidget(this);
    m_heardTable->setColumnCount(8);
    m_heardTable->setHorizontalHeaderLabels(
        {QStringLiteral("UTC"), QStringLiteral("Address"), QStringLiteral("Channel"),
         QStringLiteral("kHz"), QStringLiteral("SNR dB"), QStringLiteral("BER"),
         QStringLiteral("Count"), QStringLiteral("Last")});
    m_heardTable->setEditTriggers(QTableWidget::NoEditTriggers);
    m_heardTable->setSelectionBehavior(QTableWidget::SelectRows);
    m_heardTable->setSelectionMode(QTableWidget::SingleSelection);
    m_heardTable->setAlternatingRowColors(true);
    m_heardTable->setShowGrid(false);
    m_heardTable->verticalHeader()->setVisible(false);
    m_heardTable->horizontalHeader()->setStretchLastSection(true);
    m_heardTable->setStyleSheet(m_table->styleSheet());
    m_tabs->addTab(m_heardTable, QStringLiteral("Heard / LQA"));
    connect(m_heardTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto* item = m_heardTable->item(row, 1)) {
            m_callEdit->setText(item->text());
        }
    });

    root->addWidget(m_tabs, 1);

    m_statusLabel = new QLabel(QStringLiteral("2G ALE decoder ready."), this);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    connect(m_groupCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &Ale2gDialog::onGroupSelected);
    connect(m_frequencyCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &Ale2gDialog::onFrequencySelectionChanged);
    connect(m_frequencySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &Ale2gDialog::onFrequencyEdited);
    connect(clearButton, &QPushButton::clicked,
            this, &Ale2gDialog::clearOutput);
}

void Ale2gDialog::updateSliceSummary()
{
    QString tuningText = QStringLiteral("Manual tune");
    if (isGroupSelectionActive()) {
        const QString networkName = m_groupCombo->currentText();
        if (m_scanTimer && m_scanTimer->isActive()) {
            tuningText = QStringLiteral("%1  |  Scan @ %2 kHz")
                             .arg(networkName, formatFrequencyKhz(m_frequencySpin->value()));
        } else if (m_frequencyCombo->currentIndex() > 0) {
            tuningText = QStringLiteral("%1  |  %2 kHz")
                             .arg(networkName, formatFrequencyKhz(m_frequencySpin->value()));
        } else {
            tuningText = QStringLiteral("%1  |  Manual").arg(networkName);
        }
    }

    const QString activityText =
        m_decoderLocked ? QStringLiteral("Decoder locked") : QStringLiteral("Decoder searching");

    if (!m_activeSlice) {
        m_sliceSummaryLabel->setText(
            QStringLiteral("%1  |  %2  |  No active slice selected.")
                .arg(tuningText, activityText));
        return;
    }

    m_sliceSummaryLabel->setText(
        QStringLiteral("Slice %1  |  %2  |  %3 MHz  |  %4  |  %5")
            .arg(m_activeSlice->sliceId())
            .arg(m_activeSlice->mode())
            .arg(QString::number(m_activeSlice->frequency(), 'f', 6))
            .arg(tuningText, activityText));
}

// ─── ALE controller UI ─────────────────────────────────────────────────────

QWidget* Ale2gDialog::buildAlePanel()
{
    auto* panel = new QWidget(this);
    panel->setStyleSheet(DigitalModeStyles::txPanelStyle());
    auto* grid = new QGridLayout(panel);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    auto* aleBadge = new QLabel(QStringLiteral("ALE"), panel);
    aleBadge->setStyleSheet(badgeStyle(QStringLiteral("#8c2f39")));
    grid->addWidget(aleBadge, 0, 0, 4, 1);

    // Row 0: identity, transmit master switch, state
    grid->addWidget(new QLabel(QStringLiteral("Self address:"), panel), 0, 1);
    m_selfEdit = new QLineEdit(panel);
    m_selfEdit->setPlaceholderText(QStringLiteral("callsign (A-Z 0-9, max 15)"));
    m_selfEdit->setMaxLength(15);
    grid->addWidget(m_selfEdit, 0, 2);

    m_txEnableCheck = new QCheckBox(QStringLiteral("Transmit OFF (receive only)"), panel);
    m_txEnableCheck->setChecked(false);
    m_txEnableCheck->setToolTip(QStringLiteral(
        "Master transmit switch. Off every time this window opens. While off, ALE only "
        "receives: no calls, responses, acknowledgments or sounds are transmitted."));
    grid->addWidget(m_txEnableCheck, 0, 3, 1, 2);

    grid->addWidget(new QLabel(QStringLiteral("TX level %:"), panel), 0, 5);
    m_txLevelSpin = new QSpinBox(panel);
    m_txLevelSpin->setRange(1, 100);
    m_txLevelSpin->setValue(50);
    m_txLevelSpin->setToolTip(QStringLiteral("Peak modem audio level sent to the radio."));
    grid->addWidget(m_txLevelSpin, 0, 6);

    m_aleStateLabel = new QLabel(QStringLiteral("IDLE"), panel);
    m_aleStateLabel->setAlignment(Qt::AlignCenter);
    m_aleStateLabel->setStyleSheet(badgeStyle(aleStateColor(Ale2gController::State::Idle)));
    grid->addWidget(m_aleStateLabel, 0, 7, 1, 2);

    // Row 1: channels and scanning
    grid->addWidget(new QLabel(QStringLiteral("Channels:"), panel), 1, 1);
    m_channelPresetCombo = new QComboBox(panel);
    m_channelPresetCombo->addItem(QStringLiteral("HFLink HFN"));
    m_channelPresetCombo->addItem(QStringLiteral("HFLink HFL"));
    m_channelPresetCombo->addItem(QStringLiteral("HFLink HFN + HFL"));
    m_channelPresetCombo->addItem(QStringLiteral("Custom"));
    grid->addWidget(m_channelPresetCombo, 1, 2);

    m_editChannelsButton = new QPushButton(QStringLiteral("Edit..."), panel);
    grid->addWidget(m_editChannelsButton, 1, 3);
    m_channelSummaryLabel = new QLabel(panel);
    grid->addWidget(m_channelSummaryLabel, 1, 4);

    grid->addWidget(new QLabel(QStringLiteral("Dwell ms:"), panel), 1, 5);
    m_dwellSpin = new QSpinBox(panel);
    m_dwellSpin->setRange(200, 5000);
    m_dwellSpin->setSingleStep(100);
    m_dwellSpin->setValue(500);
    m_dwellSpin->setToolTip(QStringLiteral("Receive dwell per channel. HFLink: 2 ch/s (500 ms) "
                                           "when scanning more than 10 channels."));
    grid->addWidget(m_dwellSpin, 1, 6);

    grid->addWidget(new QLabel(QStringLiteral("Call s:"), panel), 1, 7);
    m_scanCallSpin = new QSpinBox(panel);
    m_scanCallSpin->setRange(0, 30);
    m_scanCallSpin->setValue(15);
    m_scanCallSpin->setSpecialValueText(QStringLiteral("Auto"));
    m_scanCallSpin->setToolTip(QStringLiteral("Scanning-call length. HFLink standard: 15 s. "
                                              "Auto = channels x dwell + 784 ms."));
    grid->addWidget(m_scanCallSpin, 1, 8);

    // Row 2: calling
    grid->addWidget(new QLabel(QStringLiteral("Call:"), panel), 2, 1);
    m_callEdit = new QLineEdit(panel);
    m_callEdit->setPlaceholderText(QStringLiteral("station address"));
    m_callEdit->setMaxLength(15);
    grid->addWidget(m_callEdit, 2, 2);
    m_callButton = new QPushButton(QStringLiteral("Call"), panel);
    grid->addWidget(m_callButton, 2, 3);
    m_allCallButton = new QPushButton(QStringLiteral("AllCall"), panel);
    grid->addWidget(m_allCallButton, 2, 4);
    m_terminateButton = new QPushButton(QStringLiteral("Terminate Link"), panel);
    grid->addWidget(m_terminateButton, 2, 5, 1, 2);
    m_startStopButton = new QPushButton(QStringLiteral("Start ALE"), panel);
    m_startStopButton->setStyleSheet(actionButtonStyle());
    grid->addWidget(m_startStopButton, 2, 7, 1, 2);

    // Row 3: AMD and sounding
    grid->addWidget(new QLabel(QStringLiteral("AMD:"), panel), 3, 1);
    m_amdEdit = new QLineEdit(panel);
    m_amdEdit->setMaxLength(90);
    m_amdEdit->setPlaceholderText(QStringLiteral("message (sent with Call/AllCall, or in link)"));
    grid->addWidget(m_amdEdit, 3, 2, 1, 2);
    m_sendAmdButton = new QPushButton(QStringLiteral("Send AMD"), panel);
    grid->addWidget(m_sendAmdButton, 3, 4);
    m_soundButton = new QPushButton(QStringLiteral("Sound Now"), panel);
    grid->addWidget(m_soundButton, 3, 5);
    m_soundTypeCombo = new QComboBox(panel);
    m_soundTypeCombo->addItem(QStringLiteral("TWAS"));
    m_soundTypeCombo->addItem(QStringLiteral("TIS"));
    m_soundTypeCombo->setToolTip(QStringLiteral("HFLink recommends TWAS sounds."));
    grid->addWidget(m_soundTypeCombo, 3, 6);
    grid->addWidget(new QLabel(QStringLiteral("Sound every:"), panel), 3, 7);
    m_soundIntervalSpin = new QSpinBox(panel);
    m_soundIntervalSpin->setRange(0, 1440);
    m_soundIntervalSpin->setValue(0);
    m_soundIntervalSpin->setSuffix(QStringLiteral(" min"));
    m_soundIntervalSpin->setSpecialValueText(QStringLiteral("Off"));
    m_soundIntervalSpin->setToolTip(QStringLiteral(
        "Scheduled sounding on channels flagged 'Sound' (HFLink pilot channels). "
        "Minimum 30 min; HFLink etiquette: about every 60 min, never more than twice per hour."));
    grid->addWidget(m_soundIntervalSpin, 3, 8);

    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(4, 1);

    connect(m_txEnableCheck, &QCheckBox::toggled, this, &Ale2gDialog::onTxEnableToggled);
    connect(m_startStopButton, &QPushButton::clicked, this, &Ale2gDialog::onStartStopClicked);
    connect(m_callButton, &QPushButton::clicked, this, &Ale2gDialog::onCallClicked);
    connect(m_callEdit, &QLineEdit::returnPressed, this, &Ale2gDialog::onCallClicked);
    connect(m_allCallButton, &QPushButton::clicked, this, &Ale2gDialog::onAllCallClicked);
    connect(m_terminateButton, &QPushButton::clicked, this, &Ale2gDialog::onTerminateClicked);
    connect(m_sendAmdButton, &QPushButton::clicked, this, &Ale2gDialog::onSendAmdClicked);
    connect(m_soundButton, &QPushButton::clicked, this, &Ale2gDialog::onSoundClicked);
    connect(m_editChannelsButton, &QPushButton::clicked, this, &Ale2gDialog::onEditChannelsClicked);
    connect(m_channelPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &Ale2gDialog::onChannelPresetSelected);
    connect(m_selfEdit, &QLineEdit::editingFinished, this, &Ale2gDialog::applyAleSettings);
    connect(m_dwellSpin, qOverload<int>(&QSpinBox::valueChanged), this, &Ale2gDialog::applyAleSettings);
    connect(m_scanCallSpin, qOverload<int>(&QSpinBox::valueChanged), this, &Ale2gDialog::applyAleSettings);
    connect(m_txLevelSpin, qOverload<int>(&QSpinBox::valueChanged), this, &Ale2gDialog::applyAleSettings);
    connect(m_soundIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, &Ale2gDialog::applyAleSettings);
    connect(m_soundTypeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &Ale2gDialog::applyAleSettings);
    return panel;
}

void Ale2gDialog::loadAleSettings()
{
    m_loadingAleSettings = true;
    auto& settings = AppSettings::instance();

    QString self = settings.value(QStringLiteral("Ale2gSelfAddress"), QString()).toString();
    if (self.isEmpty() && m_radioModel) {
        self = m_radioModel->callsign();
    }
    if (self.isEmpty()) {
        self = settings.value(QStringLiteral("DxClusterCallsign"), QString()).toString();
    }
    m_selfEdit->setText(Ale2g::normalizeAddress(self));

    const int preset = std::clamp(
        settings.value(QStringLiteral("Ale2gChannelPreset"), 0).toInt(), 0, 3);
    const QString saved = settings.value(QStringLiteral("Ale2gChannelList"), QString()).toString();
    m_aleChannels = saved.isEmpty() ? presetChannels(preset == 3 ? 0 : preset) : parseChannels(saved);
    if (m_aleChannels.isEmpty()) {
        m_aleChannels = presetChannels(0);
    }
    {
        const QSignalBlocker blocker(m_channelPresetCombo);
        m_channelPresetCombo->setCurrentIndex(preset);
    }

    m_dwellSpin->setValue(settings.value(QStringLiteral("Ale2gDwellMs"), 500).toInt());
    m_scanCallSpin->setValue(settings.value(QStringLiteral("Ale2gScanCallSec"), 15).toInt());
    m_txLevelSpin->setValue(settings.value(QStringLiteral("Ale2gTxLevelPercent"), 50).toInt());
    const int interval = settings.value(QStringLiteral("Ale2gSoundIntervalMin"), 0).toInt();
    m_soundIntervalSpin->setValue(interval > 0 ? std::max(30, interval) : 0);
    m_soundTypeCombo->setCurrentIndex(
        settings.value(QStringLiteral("Ale2gSoundTis"), QStringLiteral("False")).toString()
                == QStringLiteral("True")
            ? 1
            : 0);
    // Deliberately not persisted: the transmit master switch (always off).
    m_loadingAleSettings = false;
}

void Ale2gDialog::saveAleSettings() const
{
    auto& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("Ale2gSelfAddress"), Ale2g::normalizeAddress(m_selfEdit->text()));
    settings.setValue(QStringLiteral("Ale2gChannelPreset"), m_channelPresetCombo->currentIndex());
    settings.setValue(QStringLiteral("Ale2gChannelList"), serializeChannels(m_aleChannels));
    settings.setValue(QStringLiteral("Ale2gDwellMs"), m_dwellSpin->value());
    settings.setValue(QStringLiteral("Ale2gScanCallSec"), m_scanCallSpin->value());
    settings.setValue(QStringLiteral("Ale2gTxLevelPercent"), m_txLevelSpin->value());
    settings.setValue(QStringLiteral("Ale2gSoundIntervalMin"), m_soundIntervalSpin->value());
    settings.setValue(QStringLiteral("Ale2gSoundTis"),
                      m_soundTypeCombo->currentIndex() == 1 ? QStringLiteral("True")
                                                            : QStringLiteral("False"));
    settings.save();
}

void Ale2gDialog::applyAleSettings()
{
    if (m_loadingAleSettings) {
        return;
    }
    if (m_soundIntervalSpin->value() > 0 && m_soundIntervalSpin->value() < 30) {
        const QSignalBlocker blocker(m_soundIntervalSpin);
        m_soundIntervalSpin->setValue(30);
    }
    const QString self = Ale2g::normalizeAddress(m_selfEdit->text());
    if (self != m_selfEdit->text()) {
        m_selfEdit->setText(self);
    }
    m_controller.setSelfAddresses(self.isEmpty() ? QStringList{} : QStringList{self});
    m_controller.setChannels(m_aleChannels);
    m_controller.setDwellMs(m_dwellSpin->value());
    m_controller.setScanCallMs(m_scanCallSpin->value() * 1000);
    m_controller.setTxAmplitude(static_cast<float>(m_txLevelSpin->value()) / 100.0f);
    m_controller.setSoundingIntervalMinutes(m_soundIntervalSpin->value());
    m_controller.setSoundWithTis(m_soundTypeCombo->currentIndex() == 1);

    int soundCount = 0;
    for (const Ale2gChannel& c : m_aleChannels) {
        soundCount += c.sound ? 1 : 0;
    }
    m_channelSummaryLabel->setText(QStringLiteral("%1 ch, %2 sound").arg(m_aleChannels.size()).arg(soundCount));
    saveAleSettings();
    updateAleButtons();
}

void Ale2gDialog::onChannelPresetSelected(int index)
{
    if (m_loadingAleSettings || index < 0 || index > 2) {
        return;   // "Custom" keeps the current list
    }
    m_aleChannels = presetChannels(index);
    applyAleSettings();
}

void Ale2gDialog::onEditChannelsClicked()
{
    QDialog editor(this);
    editor.setWindowTitle(QStringLiteral("ALE Channels"));
    editor.resize(460, 520);
    auto* layout = new QVBoxLayout(&editor);

    auto* table = new QTableWidget(&editor);
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("USB dial kHz"),
                                      QStringLiteral("Sound")});
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    auto addRow = [table](const Ale2gChannel& c) {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(c.name));
        table->setItem(row, 1, new QTableWidgetItem(QString::number(c.frequencyHz / 1000.0, 'f', 1)));
        auto* sound = new QTableWidgetItem();
        sound->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        sound->setCheckState(c.sound ? Qt::Checked : Qt::Unchecked);
        table->setItem(row, 2, sound);
    };
    for (const Ale2gChannel& c : m_aleChannels) {
        addRow(c);
    }
    layout->addWidget(table, 1);

    auto* buttons = new QHBoxLayout();
    auto* addButton = new QPushButton(QStringLiteral("Add"), &editor);
    auto* removeButton = new QPushButton(QStringLiteral("Remove"), &editor);
    auto* hfnButton = new QPushButton(QStringLiteral("Load HFN"), &editor);
    auto* hflButton = new QPushButton(QStringLiteral("Load HFL"), &editor);
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton);
    buttons->addStretch(1);
    buttons->addWidget(hfnButton);
    buttons->addWidget(hflButton);
    layout->addLayout(buttons);
    auto* note = new QLabel(QStringLiteral("Presets: hflink.com/channels. Flag 'Sound' only on pilot "
                                           "channels you intend to sound on."), &editor);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
    layout->addWidget(box);

    connect(addButton, &QPushButton::clicked, &editor, [addRow]() {
        addRow(Ale2gChannel{QStringLiteral("NEW"), 7102000, false});
    });
    connect(removeButton, &QPushButton::clicked, &editor, [table]() {
        const int row = table->currentRow();
        if (row >= 0) {
            table->removeRow(row);
        }
    });
    connect(hfnButton, &QPushButton::clicked, &editor, [table, addRow]() {
        table->setRowCount(0);
        for (const Ale2gChannel& c : presetChannels(0)) {
            addRow(c);
        }
    });
    connect(hflButton, &QPushButton::clicked, &editor, [table, addRow]() {
        table->setRowCount(0);
        for (const Ale2gChannel& c : presetChannels(1)) {
            addRow(c);
        }
    });
    connect(box, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &editor, &QDialog::reject);

    if (editor.exec() != QDialog::Accepted) {
        return;
    }
    QVector<Ale2gChannel> channels;
    for (int row = 0; row < table->rowCount(); ++row) {
        bool ok = false;
        const double khz = table->item(row, 1) ? table->item(row, 1)->text().toDouble(&ok) : 0.0;
        if (!ok || khz < 100.0 || khz > 450000.0) {
            continue;
        }
        channels.push_back(Ale2gChannel{table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString(),
                                        static_cast<qint64>(khz * 1000.0 + 0.5),
                                        table->item(row, 2) && table->item(row, 2)->checkState() == Qt::Checked});
    }
    m_aleChannels = channels;
    {
        const QSignalBlocker blocker(m_channelPresetCombo);
        m_channelPresetCombo->setCurrentIndex(3);
    }
    applyAleSettings();
}

void Ale2gDialog::onTxEnableToggled(bool checked)
{
    if (checked) {
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("Enable ALE transmit"),
            QStringLiteral("ALE will key the radio automatically to answer calls, send "
                           "acknowledgments, calls and scheduled sounds.\n\n"
                           "Make sure an antenna/dummy load is connected, the ALE slice is "
                           "the TX slice, and you are licensed for these frequencies.\n\n"
                           "Enable transmit?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            const QSignalBlocker blocker(m_txEnableCheck);
            m_txEnableCheck->setChecked(false);
            return;
        }
    }
    m_controller.setTransmitEnabled(checked);
    updateAleButtons();
}

void Ale2gDialog::onStartStopClicked()
{
    if (m_controller.isRunning()) {
        m_controller.stop();
    } else {
        applyAleSettings();
        stopScan();   // passive catalog scan and ALE scan are mutually exclusive
        m_controller.start();
    }
    updateAleButtons();
}

void Ale2gDialog::onCallClicked()
{
    applyAleSettings();
    const QString address = Ale2g::normalizeAddress(m_callEdit->text());
    if (address.isEmpty()) {
        return;
    }
    stopScan();
    m_controller.call(address, m_amdEdit->text());
    updateAleButtons();
}

void Ale2gDialog::onAllCallClicked()
{
    applyAleSettings();
    stopScan();
    m_controller.allCall(m_amdEdit->text());
    updateAleButtons();
}

void Ale2gDialog::onSoundClicked()
{
    applyAleSettings();
    stopScan();
    m_controller.soundNow();
    updateAleButtons();
}

void Ale2gDialog::onSendAmdClicked()
{
    if (m_controller.sendAmd(m_amdEdit->text())) {
        appendAleLog(QStringLiteral("AMD to %1: %2")
                         .arg(m_controller.remoteAddress(), Ale2g::normalizeAmdText(m_amdEdit->text())));
        m_amdEdit->clear();
    }
}

void Ale2gDialog::onTerminateClicked()
{
    m_controller.terminateLink();
    updateAleButtons();
}

void Ale2gDialog::onAleStateChanged(Ale2gController::State state, const QString& remote)
{
    const QString text = remote.isEmpty()
                             ? Ale2gController::stateName(state)
                             : QStringLiteral("%1  %2").arg(Ale2gController::stateName(state), remote);
    m_aleStateLabel->setText(text);
    m_aleStateLabel->setStyleSheet(badgeStyle(aleStateColor(state)));
    updateAleButtons();
}

void Ale2gDialog::onAleTuneRequested(qint64 frequencyHz)
{
    stopScan();
    const double khz = static_cast<double>(frequencyHz) / 1000.0;
    {
        const QSignalBlocker blocker(m_frequencySpin);
        m_frequencySpin->setValue(khz);
    }
    tuneToFrequencyKhz(khz, false);
}

void Ale2gDialog::onAleTransmitRequested(bool on)
{
    if (on) {
        if (!m_activeSlice) {
            m_controller.abortTransmit(QStringLiteral("No active slice for ALE transmit."));
            return;
        }
        if (!m_activeSlice->isTxSlice()) {
            m_activeSlice->setTxSlice(true);
        }
    }
    emit aleTransmitRequested(on);
}

void Ale2gDialog::onAleFrame(const QString& summary)
{
    const QString utc = QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss"));
    appendAleLog(QStringLiteral("[%1] RX %2").arg(utc, summary));
}

void Ale2gDialog::onAleAmd(const QString& from, const QString& text)
{
    const QString utc = QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss"));
    appendAleLog(QStringLiteral("[%1] AMD from %2: %3").arg(utc, from, text));
    m_statusLabel->setText(QStringLiteral("AMD from %1: %2").arg(from, text));
}

void Ale2gDialog::onAleLog(const QString& text)
{
    const QString utc = QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss"));
    appendAleLog(QStringLiteral("[%1] %2").arg(utc, text));
}

void Ale2gDialog::onAleError(const QString& text)
{
    m_statusLabel->setText(QStringLiteral("ALE: %1").arg(text));
}

void Ale2gDialog::onAleHeardChanged()
{
    const auto& heard = m_controller.heardStations();
    m_heardTable->setRowCount(heard.size());
    for (int row = 0; row < heard.size(); ++row) {
        const Ale2gHeardStation& h = heard.at(row);
        const QString channel = (h.channelIndex >= 0 && h.channelIndex < m_aleChannels.size())
                                    ? m_aleChannels.at(h.channelIndex).name
                                    : QStringLiteral("-");
        const QStringList cells{
            h.lastHeardUtc.toString(QStringLiteral("HH:mm:ss")),
            h.address,
            channel,
            h.frequencyHz > 0 ? formatFrequencyKhz(h.frequencyHz / 1000.0) : QStringLiteral("-"),
            QString::number(h.snrDb, 'f', 1),
            QString::number(h.berCode),
            QString::number(h.count),
            h.lastKind,
        };
        for (int col = 0; col < cells.size(); ++col) {
            m_heardTable->setItem(row, col, new QTableWidgetItem(cells.at(col)));
        }
    }
}

void Ale2gDialog::appendAleLog(const QString& text)
{
    m_aleLogEdit->appendPlainText(text);
}

void Ale2gDialog::updateAleButtons()
{
    if (!m_startStopButton) {
        return;
    }
    const bool tx = m_controller.transmitEnabled();
    {
        const QSignalBlocker blocker(m_txEnableCheck);
        m_txEnableCheck->setChecked(tx);
    }
    m_txEnableCheck->setText(tx ? QStringLiteral("TRANSMIT ENABLED (ALE keys the radio)")
                                : QStringLiteral("Transmit OFF (receive only)"));
    m_txEnableCheck->setStyleSheet(tx ? QStringLiteral("QCheckBox { font-weight: bold; color: #b0101f; }")
                                      : QStringLiteral("QCheckBox { font-weight: bold; }"));
    const auto state = m_controller.state();
    const bool linked = state == Ale2gController::State::Linked;
    const bool busy = state == Ale2gController::State::Calling
                      || state == Ale2gController::State::Responding
                      || state == Ale2gController::State::Sounding || linked;
    m_startStopButton->setText(m_controller.isRunning() ? QStringLiteral("Stop ALE")
                                                        : QStringLiteral("Start ALE"));
    m_callButton->setEnabled(tx && !busy);
    m_allCallButton->setEnabled(tx && !busy);
    m_soundButton->setEnabled(tx && !busy);
    m_sendAmdButton->setEnabled(tx && linked);
    m_terminateButton->setEnabled(busy && state != Ale2gController::State::Sounding);
}

} // namespace AetherSDR
