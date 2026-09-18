#include "gui/ScannerPanel.h"

#include "core/AppSettings.h"
#include "gui/ComboStyle.h"
#include "models/BandDefs.h"
#include "models/MeterModel.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// RX audio RMS above this counts as "squelch open". With squelch closed the
// radio sends digital silence, so any real audio is well above it.
// NOTE: remote_audio_rx is the mix of all unmuted slices — squelch-based
// detection is only meaningful when the scanned slice is the only audible one.
constexpr float kSquelchOpenRms = 0.001f;

constexpr int kMaxLogRows = 500;
constexpr int kMaxMemoryRowsText = 40;   // label truncation

enum LogColumn { kColTime = 0, kColFreq, kColMode, kColLabel, kColPeak, kColCount };

const QStringList& scanModes()
{
    static const QStringList modes = {
        QStringLiteral("USB"), QStringLiteral("LSB"), QStringLiteral("CW"),
        QStringLiteral("AM"),  QStringLiteral("SAM"), QStringLiteral("FM"),
        QStringLiteral("NFM"), QStringLiteral("DIGU"), QStringLiteral("DIGL"),
    };
    return modes;
}

QString sUnits(float dbm)
{
    // S9 = -73 dBm, 6 dB per S-unit (HF convention).
    if (dbm >= -73.0f) {
        return QStringLiteral("S9+%1").arg(static_cast<int>(std::lround(dbm + 73.0f)));
    }
    const int s = std::max(0, 9 - static_cast<int>(std::ceil((-73.0f - dbm) / 6.0f)));
    return QStringLiteral("S%1").arg(s);
}

QString formatMhz(double mhz)
{
    return QString::number(mhz, 'f', 6);
}

bool isTrue(const QVariant& v)
{
    return v.toString() == QStringLiteral("True");
}

QString boolStr(bool b)
{
    return b ? QStringLiteral("True") : QStringLiteral("False");
}

} // namespace

// ─── SliceScanTarget ─────────────────────────────────────────────────────────

SliceScanTarget::SliceScanTarget(SliceModel* slice, RadioModel* radio)
    : m_slice(slice), m_radio(radio)
{
}

bool SliceScanTarget::isValid() const { return !m_slice.isNull(); }
int SliceScanTarget::sliceId() const { return m_slice ? m_slice->sliceId() : -1; }
bool SliceScanTarget::isLocked() const { return m_slice && m_slice->isLocked(); }
double SliceScanTarget::frequencyMhz() const { return m_slice ? m_slice->frequency() : 0.0; }
QString SliceScanTarget::mode() const { return m_slice ? m_slice->mode() : QString(); }
int SliceScanTarget::filterLow() const { return m_slice ? m_slice->filterLow() : 0; }
int SliceScanTarget::filterHigh() const { return m_slice ? m_slice->filterHigh() : 0; }

void SliceScanTarget::tune(double mhz)
{
    if (!m_slice) {
        return;
    }
    // Stay put on the panadapter while the channel is visible; recenter only
    // when the scan leaves the displayed span (e.g. band scan, memory on
    // another band).
    const PanadapterModel* pan = m_radio ? m_radio->panadapter(m_slice->panId()) : nullptr;
    if (pan && pan->bandwidthMhz() > 0.0
        && std::abs(mhz - pan->centerMhz()) < pan->bandwidthMhz() * 0.45) {
        m_slice->setFrequency(mhz);
    } else {
        m_slice->tuneAndRecenter(mhz);
    }
}

void SliceScanTarget::setMode(const QString& mode)
{
    if (m_slice) {
        m_slice->setMode(mode);
    }
}

void SliceScanTarget::setFilter(int low, int high)
{
    if (m_slice) {
        m_slice->setFilterWidth(low, high);
    }
}

// ─── ScannerPanel ────────────────────────────────────────────────────────────

ScannerPanel::ScannerPanel(RadioModel* radio, QWidget* parent)
    : QWidget(parent), m_radio(radio)
{
    setWindowTitle(QStringLiteral("Scanner"));
    m_scanner = new Scanner(this);

    buildUi();
    loadSettings();
    refreshSlices();
    refreshMemories();

    connect(m_scanner, &Scanner::stateChanged, this, &ScannerPanel::onStateChanged);
    connect(m_scanner, &Scanner::finished, this, [this](bool restored) {
        if (restored) {
            restorePanView();
        }
        m_scanPanId.clear();
    });
    connect(m_scanner, &Scanner::channelChanged, this, &ScannerPanel::onChannelChanged);
    connect(m_scanner, &Scanner::hitAdded, this, &ScannerPanel::onHitAdded);
    connect(m_scanner, &Scanner::hitPeakUpdated, this, &ScannerPanel::onHitPeakUpdated);
    connect(m_scanner, &Scanner::levelUpdated, this, [this](float dbm) {
        m_levelLabel->setText(QStringLiteral("%1 dBm  %2")
                                  .arg(QString::number(dbm, 'f', 1), sUnits(dbm)));
    });
    connect(m_scanner, &Scanner::errorOccurred, this, [this](const QString& msg) {
        m_stateLabel->setText(msg);
    });
    connect(m_scanner, &Scanner::skipListChanged, this, [this]() {
        saveSkipList();
        updateSkipLabel();
    });

    if (m_radio) {
        connect(&m_radio->meterModel(), &MeterModel::sLevelChanged,
                m_scanner, &Scanner::onSignalLevel);
        connect(&m_radio->transmitModel(), &TransmitModel::moxChanged,
                m_scanner, &Scanner::setMox);
        connect(m_radio, &RadioModel::radioTransmittingChanged,
                m_scanner, &Scanner::setTransmitting);
        m_scanner->setMox(m_radio->transmitModel().isMox());
        m_scanner->setTransmitting(m_radio->transmitModel().isTransmitting());

        connect(m_radio, &RadioModel::memoryChanged, this, &ScannerPanel::refreshMemories);
        connect(m_radio, &RadioModel::memoryRemoved, this, &ScannerPanel::refreshMemories);
        connect(m_radio, &RadioModel::memoriesCleared, this, &ScannerPanel::refreshMemories);
        connect(m_radio, &RadioModel::sliceAdded, this, &ScannerPanel::refreshSlices);
        connect(m_radio, &RadioModel::sliceRemoved, this, [this](int sliceId) {
            const ScanTarget* t = m_scanner->target();
            if (m_scanner->isRunning() && t && t->sliceId() == sliceId) {
                m_scanner->abort();
                m_stateLabel->setText(QStringLiteral("Scanned slice was closed"));
            }
            refreshSlices();
        });
        connect(m_radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            if (!connected) {
                m_scanner->abort();
            }
        });
    }

    pushConfig();
    onStateChanged(Scanner::State::Idle);
}

ScannerPanel::~ScannerPanel()
{
    // Closing the scanner is a user stop: hand the slice back as it was.
    if (m_scanner && m_scanner->isRunning()) {
        m_scanner->stop(true);
    }
}

void ScannerPanel::closeEvent(QCloseEvent* ev)
{
    if (m_scanner->isRunning()) {
        m_scanner->stop(true);
    }
    saveSettings();
    QWidget::closeEvent(ev);
}

// ─── UI ──────────────────────────────────────────────────────────────────────

void ScannerPanel::buildUi()
{
    auto* root = new QVBoxLayout(this);

    // Source row
    auto* srcRow = new QHBoxLayout;
    srcRow->addWidget(new QLabel(QStringLiteral("Slice:")));
    m_sliceCombo = new QComboBox;
    applyComboStyle(m_sliceCombo);
    srcRow->addWidget(m_sliceCombo);
    srcRow->addSpacing(12);
    srcRow->addWidget(new QLabel(QStringLiteral("Scan:")));
    m_modeCombo = new QComboBox;
    m_modeCombo->addItem(QStringLiteral("Memories"), QStringLiteral("Memory"));
    m_modeCombo->addItem(QStringLiteral("Band"), QStringLiteral("Band"));
    m_modeCombo->addItem(QStringLiteral("Frequency list"), QStringLiteral("List"));
    applyComboStyle(m_modeCombo);
    srcRow->addWidget(m_modeCombo);
    srcRow->addStretch(1);
    root->addLayout(srcRow);

    m_stack = new QStackedWidget;
    root->addWidget(m_stack);
    connect(m_modeCombo, &QComboBox::currentIndexChanged, m_stack, &QStackedWidget::setCurrentIndex);

    // Memory page
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        auto* row = new QHBoxLayout;
        m_memSelectCombo = new QComboBox;
        m_memSelectCombo->addItem(QStringLiteral("All memories"), QStringLiteral("All"));
        m_memSelectCombo->addItem(QStringLiteral("Group"), QStringLiteral("Group"));
        m_memSelectCombo->addItem(QStringLiteral("Checked memories"), QStringLiteral("Selected"));
        applyComboStyle(m_memSelectCombo);
        m_memGroupCombo = new QComboBox;
        applyComboStyle(m_memGroupCombo);
        row->addWidget(m_memSelectCombo);
        row->addWidget(m_memGroupCombo, 1);
        v->addLayout(row);
        m_memList = new QListWidget;
        m_memList->setMinimumHeight(120);
        v->addWidget(m_memList);
        connect(m_memSelectCombo, &QComboBox::currentIndexChanged, this, [this]() {
            m_memGroupCombo->setEnabled(m_memSelectCombo->currentData().toString() == "Group");
        });
        m_stack->addWidget(page);
    }

    // Band page
    {
        auto* page = new QWidget;
        auto* form = new QFormLayout(page);
        form->setContentsMargins(0, 0, 0, 0);
        m_bandCombo = new QComboBox;
        m_bandCombo->addItem(QStringLiteral("Current band"), QStringLiteral("Current"));
        m_bandCombo->addItem(QStringLiteral("Custom range"), QStringLiteral("Custom"));
        for (const BandDef& b : kBands) {
            m_bandCombo->addItem(QString::fromLatin1(b.name), QString::fromLatin1(b.name));
        }
        applyComboStyle(m_bandCombo);
        form->addRow(QStringLiteral("Band:"), m_bandCombo);

        m_bandStart = new QDoubleSpinBox;
        m_bandStop = new QDoubleSpinBox;
        for (QDoubleSpinBox* sb : {m_bandStart, m_bandStop}) {
            sb->setDecimals(6);
            sb->setRange(0.001, 10000.0);
            sb->setSuffix(QStringLiteral(" MHz"));
        }
        form->addRow(QStringLiteral("Start:"), m_bandStart);
        form->addRow(QStringLiteral("Stop:"), m_bandStop);
        m_bandStep = new QSpinBox;
        m_bandStep->setRange(10, 1000000);
        m_bandStep->setSingleStep(500);
        m_bandStep->setSuffix(QStringLiteral(" Hz"));
        form->addRow(QStringLiteral("Step:"), m_bandStep);
        m_bandMode = new QComboBox;
        m_bandMode->addItem(QStringLiteral("Keep slice mode"), QString());
        for (const QString& m : scanModes()) {
            m_bandMode->addItem(m, m);
        }
        applyComboStyle(m_bandMode);
        form->addRow(QStringLiteral("Mode:"), m_bandMode);
        m_bandCount = new QLabel;
        form->addRow(QString(), m_bandCount);

        auto updateCount = [this]() {
            const QVector<ScanChannel> ch = Scanner::bandChannels(
                m_bandStart->value(), m_bandStop->value(), m_bandStep->value());
            m_bandCount->setText(QStringLiteral("%1 channels%2").arg(ch.size())
                .arg(ch.size() >= Scanner::kMaxBandChannels ? QStringLiteral(" (capped)") : QString()));
        };
        connect(m_bandStart, &QDoubleSpinBox::valueChanged, this, updateCount);
        connect(m_bandStop, &QDoubleSpinBox::valueChanged, this, updateCount);
        connect(m_bandStep, &QSpinBox::valueChanged, this, updateCount);
        connect(m_bandCombo, &QComboBox::currentIndexChanged, this, [this]() {
            const QString key = m_bandCombo->currentData().toString();
            const bool custom = key == QStringLiteral("Custom");
            m_bandStart->setEnabled(custom);
            m_bandStop->setEnabled(custom);
            if (key == QStringLiteral("Current")) {
                useCurrentBand();
                return;
            }
            for (const BandDef& b : kBands) {
                if (key == QLatin1String(b.name)) {
                    m_bandStart->setValue(b.lowMhz);
                    m_bandStop->setValue(b.highMhz);
                }
            }
        });
        m_stack->addWidget(page);
    }

    // Frequency-list page
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(new QLabel(QStringLiteral(
            "One per line: frequency [MHz, or k/kHz] [mode] [label]")));
        m_listEdit = new QPlainTextEdit;
        m_listEdit->setPlaceholderText(QStringLiteral("14.074 DIGU FT8\n7074k DIGU\n146.52 FM Simplex"));
        v->addWidget(m_listEdit);
        m_stack->addWidget(page);
    }

    // Behaviour
    auto* behav = new QGroupBox(QStringLiteral("Stop / resume"));
    auto* form = new QFormLayout(behav);
    m_dwellSpin = new QSpinBox;
    m_dwellSpin->setRange(100, 60000);
    m_dwellSpin->setSingleStep(50);
    m_dwellSpin->setSuffix(QStringLiteral(" ms"));
    form->addRow(QStringLiteral("Dwell per channel:"), m_dwellSpin);
    m_settleSpin = new QSpinBox;
    m_settleSpin->setRange(0, 5000);
    m_settleSpin->setSingleStep(50);
    m_settleSpin->setSuffix(QStringLiteral(" ms"));
    m_settleSpin->setToolTip(QStringLiteral("Ignore meter readings this long after each retune"));
    form->addRow(QStringLiteral("Settle time:"), m_settleSpin);

    m_detectCombo = new QComboBox;
    m_detectCombo->addItem(QStringLiteral("S-meter threshold"), QStringLiteral("SMeter"));
    m_detectCombo->addItem(QStringLiteral("Squelch open"), QStringLiteral("Squelch"));
    m_detectCombo->addItem(QStringLiteral("Either"), QStringLiteral("Either"));
    applyComboStyle(m_detectCombo);
    form->addRow(QStringLiteral("Stop on:"), m_detectCombo);
    m_thresholdSpin = new QSpinBox;
    m_thresholdSpin->setRange(-150, 0);
    m_thresholdSpin->setSuffix(QStringLiteral(" dBm"));
    form->addRow(QStringLiteral("Threshold:"), m_thresholdSpin);

    auto* resumeRow = new QHBoxLayout;
    m_resumeCombo = new QComboBox;
    m_resumeCombo->addItem(QStringLiteral("Resume after time"), QStringLiteral("Time"));
    m_resumeCombo->addItem(QStringLiteral("Resume when carrier drops"), QStringLiteral("Carrier"));
    m_resumeCombo->addItem(QStringLiteral("Hold"), QStringLiteral("Hold"));
    applyComboStyle(m_resumeCombo);
    m_resumeSpin = new QDoubleSpinBox;
    m_resumeSpin->setRange(0.0, 600.0);
    m_resumeSpin->setDecimals(1);
    m_resumeSpin->setSuffix(QStringLiteral(" s"));
    resumeRow->addWidget(m_resumeCombo, 1);
    resumeRow->addWidget(m_resumeSpin);
    form->addRow(QStringLiteral("Resume:"), resumeRow);

    auto* priRow = new QHBoxLayout;
    m_priorityCheck = new QCheckBox(QStringLiteral("Priority"));
    m_priorityFreq = new QDoubleSpinBox;
    m_priorityFreq->setDecimals(6);
    m_priorityFreq->setRange(0.0, 10000.0);
    m_priorityFreq->setSuffix(QStringLiteral(" MHz"));
    m_priorityMode = new QComboBox;
    m_priorityMode->addItem(QStringLiteral("Keep"), QString());
    for (const QString& m : scanModes()) {
        m_priorityMode->addItem(m, m);
    }
    applyComboStyle(m_priorityMode);
    m_priorityInterval = new QSpinBox;
    m_priorityInterval->setRange(1, 600);
    m_priorityInterval->setPrefix(QStringLiteral("every "));
    m_priorityInterval->setSuffix(QStringLiteral(" s"));
    priRow->addWidget(m_priorityCheck);
    priRow->addWidget(m_priorityFreq, 1);
    priRow->addWidget(m_priorityMode);
    priRow->addWidget(m_priorityInterval);
    form->addRow(priRow);
    root->addWidget(behav);

    // Controls
    auto* ctlRow = new QHBoxLayout;
    m_startBtn = new QPushButton(QStringLiteral("Start"));
    m_pauseBtn = new QPushButton(QStringLiteral("Pause"));
    m_skipBtn = new QPushButton(QStringLiteral("Skip this"));
    m_skipBtn->setToolTip(QStringLiteral("Lock out the current frequency and continue"));
    m_clearSkipBtn = new QPushButton(QStringLiteral("Clear skips"));
    m_skipLabel = new QLabel;
    ctlRow->addWidget(m_startBtn);
    ctlRow->addWidget(m_pauseBtn);
    ctlRow->addWidget(m_skipBtn);
    ctlRow->addStretch(1);
    ctlRow->addWidget(m_skipLabel);
    ctlRow->addWidget(m_clearSkipBtn);
    root->addLayout(ctlRow);

    // Status
    m_stateLabel = new QLabel;
    m_channelLabel = new QLabel(QStringLiteral("—"));
    m_channelLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_levelLabel = new QLabel;
    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(m_stateLabel);
    statusRow->addStretch(1);
    statusRow->addWidget(m_levelLabel);
    root->addLayout(statusRow);
    root->addWidget(m_channelLabel);

    // Activity log
    auto* logHeader = new QHBoxLayout;
    logHeader->addWidget(new QLabel(QStringLiteral("Activity (double-click to tune):")));
    logHeader->addStretch(1);
    m_clearLogBtn = new QPushButton(QStringLiteral("Clear"));
    logHeader->addWidget(m_clearLogBtn);
    root->addLayout(logHeader);

    m_log = new QTableWidget(0, kColCount);
    m_log->setHorizontalHeaderLabels({QStringLiteral("Time (UTC)"), QStringLiteral("Frequency"),
                                      QStringLiteral("Mode"), QStringLiteral("Label"),
                                      QStringLiteral("Peak")});
    m_log->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_log->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_log->verticalHeader()->setVisible(false);
    m_log->horizontalHeader()->setSectionResizeMode(kColLabel, QHeaderView::Stretch);
    m_log->setMinimumHeight(140);
    root->addWidget(m_log, 1);

    connect(m_startBtn, &QPushButton::clicked, this, &ScannerPanel::toggleStartStop);
    connect(m_pauseBtn, &QPushButton::clicked, this, &ScannerPanel::togglePause);
    connect(m_skipBtn, &QPushButton::clicked, this, &ScannerPanel::skipCurrent);
    connect(m_clearSkipBtn, &QPushButton::clicked, m_scanner, &Scanner::clearSkipList);
    connect(m_clearLogBtn, &QPushButton::clicked, this, [this]() { m_log->setRowCount(0); });
    connect(m_log, &QTableWidget::cellDoubleClicked, this, &ScannerPanel::onLogActivated);

    // Stop/resume parameters apply live, even mid-scan.
    connect(m_dwellSpin, &QSpinBox::valueChanged, this, &ScannerPanel::pushConfig);
    connect(m_settleSpin, &QSpinBox::valueChanged, this, &ScannerPanel::pushConfig);
    connect(m_thresholdSpin, &QSpinBox::valueChanged, this, &ScannerPanel::pushConfig);
    connect(m_resumeSpin, &QDoubleSpinBox::valueChanged, this, &ScannerPanel::pushConfig);
    connect(m_priorityFreq, &QDoubleSpinBox::valueChanged, this, &ScannerPanel::pushConfig);
    connect(m_priorityInterval, &QSpinBox::valueChanged, this, &ScannerPanel::pushConfig);
    connect(m_priorityCheck, &QCheckBox::toggled, this, &ScannerPanel::pushConfig);
    connect(m_detectCombo, &QComboBox::currentIndexChanged, this, &ScannerPanel::pushConfig);
    connect(m_priorityMode, &QComboBox::currentIndexChanged, this, &ScannerPanel::pushConfig);
    connect(m_resumeCombo, &QComboBox::currentIndexChanged, this, [this]() {
        m_resumeSpin->setEnabled(m_resumeCombo->currentData().toString() != "Hold");
        pushConfig();
    });

    resize(520, 720);
}

// ─── Settings (client-side preferences only) ─────────────────────────────────

void ScannerPanel::loadSettings()
{
    const AppSettings& s = AppSettings::instance();
    auto selectData = [](QComboBox* c, const QString& data) {
        const int i = c->findData(data);
        if (i >= 0) {
            c->setCurrentIndex(i);
        }
    };

    selectData(m_modeCombo, s.value("ScannerSource", "Memory").toString());
    m_stack->setCurrentIndex(m_modeCombo->currentIndex());
    selectData(m_memSelectCombo, s.value("ScannerMemorySelect", "All").toString());
    m_memGroupCombo->setEnabled(m_memSelectCombo->currentData().toString() == "Group");

    m_bandStep->setValue(s.value("ScannerBandStepHz", 5000).toInt());
    m_bandStart->setValue(s.value("ScannerBandStartMhz", 14.000).toDouble());
    m_bandStop->setValue(s.value("ScannerBandStopMhz", 14.350).toDouble());
    selectData(m_bandMode, s.value("ScannerBandMode", QString()).toString());
    {
        // Band combo last: selecting a named band overwrites start/stop.
        const QSignalBlocker b(m_bandCombo);
        selectData(m_bandCombo, s.value("ScannerBand", "Current").toString());
        const bool custom = m_bandCombo->currentData().toString() == "Custom";
        m_bandStart->setEnabled(custom);
        m_bandStop->setEnabled(custom);
    }

    QString list = s.value("ScannerFrequencyList", QString()).toString();
    list.replace(QLatin1Char(';'), QLatin1Char('\n'));
    m_listEdit->setPlainText(list);

    m_dwellSpin->setValue(s.value("ScannerDwellMs", 400).toInt());
    m_settleSpin->setValue(s.value("ScannerSettleMs", 200).toInt());
    selectData(m_detectCombo, s.value("ScannerDetect", "SMeter").toString());
    m_thresholdSpin->setValue(s.value("ScannerThresholdDbm", -100).toInt());
    selectData(m_resumeCombo, s.value("ScannerResume", "Time").toString());
    m_resumeSpin->setValue(s.value("ScannerResumeSec", 5.0).toDouble());
    m_resumeSpin->setEnabled(m_resumeCombo->currentData().toString() != "Hold");
    m_priorityCheck->setChecked(isTrue(s.value("ScannerPriorityEnabled", "False")));
    m_priorityFreq->setValue(s.value("ScannerPriorityFreqMhz", 0.0).toDouble());
    selectData(m_priorityMode, s.value("ScannerPriorityMode", QString()).toString());
    m_priorityInterval->setValue(s.value("ScannerPriorityIntervalSec", 5).toInt());

    QSet<qint64> skip;
    const QStringList parts = s.value("ScannerSkipList", QString()).toString()
                                  .split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        bool ok = false;
        const qint64 hz = p.trimmed().toLongLong(&ok);
        if (ok && hz > 0) {
            skip.insert(hz);
        }
    }
    {
        const QSignalBlocker b(m_scanner);
        m_scanner->setSkipList(skip);
    }
    updateSkipLabel();
}

void ScannerPanel::saveSettings() const
{
    AppSettings& s = AppSettings::instance();
    s.setValue("ScannerSource", m_modeCombo->currentData().toString());
    s.setValue("ScannerMemorySelect", m_memSelectCombo->currentData().toString());
    s.setValue("ScannerMemoryGroup", m_memGroupCombo->currentText());

    QStringList checked;
    for (int i = 0; i < m_memList->count(); ++i) {
        const QListWidgetItem* it = m_memList->item(i);
        if (it->checkState() == Qt::Checked) {
            checked << QString::number(it->data(Qt::UserRole).toInt());
        }
    }
    s.setValue("ScannerMemorySelection", checked.join(QLatin1Char(',')));

    s.setValue("ScannerBand", m_bandCombo->currentData().toString());
    s.setValue("ScannerBandStartMhz", QString::number(m_bandStart->value(), 'f', 6));
    s.setValue("ScannerBandStopMhz", QString::number(m_bandStop->value(), 'f', 6));
    s.setValue("ScannerBandStepHz", m_bandStep->value());
    s.setValue("ScannerBandMode", m_bandMode->currentData().toString());

    QString list = m_listEdit->toPlainText();
    list.replace(QLatin1Char('\n'), QLatin1Char(';'));
    s.setValue("ScannerFrequencyList", list);

    s.setValue("ScannerDwellMs", m_dwellSpin->value());
    s.setValue("ScannerSettleMs", m_settleSpin->value());
    s.setValue("ScannerDetect", m_detectCombo->currentData().toString());
    s.setValue("ScannerThresholdDbm", m_thresholdSpin->value());
    s.setValue("ScannerResume", m_resumeCombo->currentData().toString());
    s.setValue("ScannerResumeSec", QString::number(m_resumeSpin->value(), 'f', 1));
    s.setValue("ScannerPriorityEnabled", boolStr(m_priorityCheck->isChecked()));
    s.setValue("ScannerPriorityFreqMhz", QString::number(m_priorityFreq->value(), 'f', 6));
    s.setValue("ScannerPriorityMode", m_priorityMode->currentData().toString());
    s.setValue("ScannerPriorityIntervalSec", m_priorityInterval->value());
    s.save();
}

void ScannerPanel::saveSkipList() const
{
    QList<qint64> hz = m_scanner->skipList().values();
    std::sort(hz.begin(), hz.end());
    QStringList parts;
    for (qint64 h : hz) {
        parts << QString::number(h);
    }
    AppSettings& s = AppSettings::instance();
    s.setValue("ScannerSkipList", parts.join(QLatin1Char(',')));
    s.save();
}

void ScannerPanel::updateSkipLabel()
{
    const int n = static_cast<int>(m_scanner->skipList().size());
    m_skipLabel->setText(QStringLiteral("%1 locked out").arg(n));
    m_clearSkipBtn->setEnabled(n > 0);
}

// ─── Config / channel building ───────────────────────────────────────────────

Scanner::Config ScannerPanel::configFromUi() const
{
    Scanner::Config cfg;
    cfg.dwellMs      = m_dwellSpin->value();
    cfg.settleMs     = m_settleSpin->value();
    cfg.thresholdDbm = static_cast<float>(m_thresholdSpin->value());

    const QString det = m_detectCombo->currentData().toString();
    cfg.detect = det == "Squelch" ? Scanner::DetectSource::Squelch
               : det == "Either"  ? Scanner::DetectSource::Either
                                  : Scanner::DetectSource::SMeter;
    const QString res = m_resumeCombo->currentData().toString();
    cfg.resume = res == "Carrier" ? Scanner::ResumeMode::Carrier
               : res == "Hold"    ? Scanner::ResumeMode::Hold
                                  : Scanner::ResumeMode::Time;
    const int resumeMs = static_cast<int>(std::lround(m_resumeSpin->value() * 1000.0));
    cfg.resumeMs      = resumeMs;
    cfg.carrierDropMs = resumeMs;

    cfg.priorityEnabled = m_priorityCheck->isChecked() && m_priorityFreq->value() > 0.0;
    cfg.priorityChannel.freqMhz = m_priorityFreq->value();
    cfg.priorityChannel.mode    = m_priorityMode->currentData().toString();
    cfg.priorityChannel.label   = QStringLiteral("Priority");
    cfg.priorityIntervalMs = m_priorityInterval->value() * 1000;
    cfg.priorityDwellMs    = m_dwellSpin->value();
    return cfg;
}

void ScannerPanel::pushConfig()
{
    m_scanner->setConfig(configFromUi());
}

QVector<ScanChannel> ScannerPanel::allMemoryChannels() const
{
    QVector<ScanChannel> out;
    if (!m_radio) {
        return out;
    }
    const QMap<int, MemoryEntry>& mems = m_radio->memories();
    for (auto it = mems.constBegin(); it != mems.constEnd(); ++it) {
        const MemoryEntry& m = it.value();
        ScanChannel ch;
        ch.freqMhz     = m.freq;
        ch.mode        = m.mode.toUpper();
        ch.filterLow   = m.rxFilterLow;
        ch.filterHigh  = m.rxFilterHigh;
        ch.label       = m.name;
        ch.group       = m.group;
        ch.memoryIndex = it.key();
        out.append(ch);
    }
    return out;
}

QVector<ScanChannel> ScannerPanel::buildChannels(QString* error) const
{
    const QString src = m_modeCombo->currentData().toString();
    if (src == "Memory") {
        const QString sel = m_memSelectCombo->currentData().toString();
        const Scanner::MemorySelect select = sel == "Group" ? Scanner::MemorySelect::Group
                                           : sel == "Selected" ? Scanner::MemorySelect::Selected
                                                               : Scanner::MemorySelect::All;
        QSet<int> indices;
        for (int i = 0; i < m_memList->count(); ++i) {
            const QListWidgetItem* it = m_memList->item(i);
            if (it->checkState() == Qt::Checked) {
                indices.insert(it->data(Qt::UserRole).toInt());
            }
        }
        const QVector<ScanChannel> ch = Scanner::selectMemories(
            allMemoryChannels(), select, m_memGroupCombo->currentText(), indices);
        if (ch.isEmpty() && error) {
            *error = QStringLiteral("No memories match the selection");
        }
        return ch;
    }
    if (src == "Band") {
        double lo = m_bandStart->value();
        double hi = m_bandStop->value();
        if (m_bandCombo->currentData().toString() == "Current") {
            const SliceModel* s = chosenSlice();
            const BandDef* b = s ? Scanner::bandForFrequency(s->frequency()) : nullptr;
            if (!b) {
                if (error) {
                    *error = QStringLiteral("Slice is not inside an amateur band");
                }
                return {};
            }
            lo = b->lowMhz;
            hi = b->highMhz;
        }
        const QVector<ScanChannel> ch = Scanner::bandChannels(
            lo, hi, m_bandStep->value(), m_bandMode->currentData().toString());
        if (ch.isEmpty() && error) {
            *error = QStringLiteral("Invalid band range");
        }
        return ch;
    }
    QStringList bad;
    const QVector<ScanChannel> ch = Scanner::parseFrequencyList(m_listEdit->toPlainText(), &bad);
    if (error) {
        if (!bad.isEmpty()) {
            *error = QStringLiteral("Ignored: %1").arg(bad.join(QStringLiteral(", ")));
        } else if (ch.isEmpty()) {
            *error = QStringLiteral("Frequency list is empty");
        }
    }
    return ch;
}

SliceModel* ScannerPanel::chosenSlice() const
{
    if (!m_radio) {
        return nullptr;
    }
    const int id = m_sliceCombo->currentData().toInt();
    return m_radio->slice(id >= 0 ? id : m_activeSliceId);
}

// ─── Refresh ─────────────────────────────────────────────────────────────────

void ScannerPanel::setActiveSliceId(int sliceId)
{
    m_activeSliceId = sliceId;
    refreshSlices();
}

void ScannerPanel::refreshSlices()
{
    const int keep = m_sliceCombo->currentData().isValid() ? m_sliceCombo->currentData().toInt() : -1;
    const QSignalBlocker b(m_sliceCombo);
    m_sliceCombo->clear();
    const QString activeLetter = m_activeSliceId >= 0
        ? QString(QChar('A' + m_activeSliceId)) : QStringLiteral("—");
    m_sliceCombo->addItem(QStringLiteral("Active slice (%1)").arg(activeLetter), -1);
    if (m_radio) {
        for (const SliceModel* s : m_radio->slices()) {
            m_sliceCombo->addItem(QStringLiteral("Slice %1").arg(QChar('A' + s->sliceId())),
                                  s->sliceId());
        }
    }
    const int idx = m_sliceCombo->findData(keep);
    m_sliceCombo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void ScannerPanel::refreshMemories()
{
    // Preserve checks across refreshes (fall back to saved selection first time).
    QSet<int> checked;
    if (m_memList->count() == 0) {
        const QStringList saved = AppSettings::instance()
            .value("ScannerMemorySelection", QString()).toString()
            .split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& p : saved) {
            checked.insert(p.toInt());
        }
    } else {
        for (int i = 0; i < m_memList->count(); ++i) {
            const QListWidgetItem* it = m_memList->item(i);
            if (it->checkState() == Qt::Checked) {
                checked.insert(it->data(Qt::UserRole).toInt());
            }
        }
    }

    const QString keepGroup = m_memGroupCombo->count() > 0
        ? m_memGroupCombo->currentText()
        : AppSettings::instance().value("ScannerMemoryGroup", QString()).toString();

    m_memList->clear();
    QStringList groups;
    for (const ScanChannel& ch : allMemoryChannels()) {
        QString name = ch.label;
        if (name.size() > kMaxMemoryRowsText) {
            name = name.left(kMaxMemoryRowsText) + QStringLiteral("…");
        }
        const QString text = QStringLiteral("%1  %2  %3  %4%5")
            .arg(ch.memoryIndex, 3)
            .arg(formatMhz(ch.freqMhz))
            .arg(ch.mode, -4)
            .arg(name)
            .arg(ch.group.isEmpty() ? QString() : QStringLiteral("  [%1]").arg(ch.group));
        auto* item = new QListWidgetItem(text, m_memList);
        item->setData(Qt::UserRole, ch.memoryIndex);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked.contains(ch.memoryIndex) ? Qt::Checked : Qt::Unchecked);
        if (m_scanner->isSkipped(ch.freqMhz)) {
            item->setToolTip(QStringLiteral("Locked out"));
        }
        if (!ch.group.isEmpty() && !groups.contains(ch.group)) {
            groups << ch.group;
        }
    }
    groups.sort(Qt::CaseInsensitive);

    const QSignalBlocker b(m_memGroupCombo);
    m_memGroupCombo->clear();
    m_memGroupCombo->addItems(groups);
    const int gi = m_memGroupCombo->findText(keepGroup);
    if (gi >= 0) {
        m_memGroupCombo->setCurrentIndex(gi);
    }
}

void ScannerPanel::useCurrentBand()
{
    const SliceModel* s = chosenSlice();
    const BandDef* b = s ? Scanner::bandForFrequency(s->frequency()) : nullptr;
    if (b) {
        m_bandStart->setValue(b->lowMhz);
        m_bandStop->setValue(b->highMhz);
    }
}

// ─── Control ─────────────────────────────────────────────────────────────────

void ScannerPanel::toggleStartStop()
{
    if (m_scanner->isRunning()) {
        onStop();
    } else {
        onStart();
    }
}

void ScannerPanel::togglePause()
{
    const Scanner::State st = m_scanner->state();
    if (st == Scanner::State::Paused || st == Scanner::State::Stopped) {
        m_scanner->resume();
    } else if (st == Scanner::State::Scanning || st == Scanner::State::PausedTx) {
        m_scanner->pause();
    }
}

void ScannerPanel::skipCurrent()
{
    m_scanner->skipCurrent();
}

void ScannerPanel::setRxAudioLevel(float rms)
{
    if (m_scanner->isRunning() && m_scanner->config().detect != Scanner::DetectSource::SMeter) {
        m_scanner->setSquelchOpen(rms > kSquelchOpenRms);
    }
}

void ScannerPanel::onStart()
{
    if (m_scanner->isRunning()) {
        return;
    }
    SliceModel* slice = chosenSlice();
    if (!slice) {
        m_stateLabel->setText(QStringLiteral("No slice available"));
        return;
    }
    QString error;
    const QVector<ScanChannel> channels = buildChannels(&error);
    if (channels.isEmpty()) {
        m_stateLabel->setText(error);
        return;
    }
    m_scanner->setTarget(std::make_unique<SliceScanTarget>(slice, m_radio));
    m_scanner->setChannels(channels);
    pushConfig();
    m_scanPanId.clear();
    if (const PanadapterModel* pan = m_radio ? m_radio->panadapter(slice->panId()) : nullptr) {
        m_scanPanId = pan->panId();
        m_scanPanCenterMhz = pan->centerMhz();
        m_scanPanBandwidthMhz = pan->bandwidthMhz();
    }
    if (m_radio) {
        m_scanner->setMox(m_radio->transmitModel().isMox());
        m_scanner->setTransmitting(m_radio->transmitModel().isTransmitting());
    }
    saveSettings();
    if (m_scanner->start() && !error.isEmpty()) {
        m_stateLabel->setText(error);   // e.g. ignored list entries
    }
}

void ScannerPanel::restorePanView()
{
    if (!m_radio || m_scanPanId.isEmpty() || m_scanPanBandwidthMhz <= 0.0) {
        return;
    }
    PanadapterModel* pan = m_radio->panadapter(m_scanPanId);
    if (!pan) {
        return;
    }
    const bool centerMoved = std::abs(pan->centerMhz() - m_scanPanCenterMhz) > 1e-6;
    const bool bwChanged = std::abs(pan->bandwidthMhz() - m_scanPanBandwidthMhz) > 1e-6;
    if (!centerMoved && !bwChanged) {
        return;
    }
    // Same command SmartSDR uses to pan; bandwidth only if the recenter changed it.
    QString cmd = QString("display pan set %1 center=%2")
                      .arg(m_scanPanId).arg(m_scanPanCenterMhz, 0, 'f', 6);
    if (bwChanged) {
        cmd += QString(" bandwidth=%1").arg(m_scanPanBandwidthMhz, 0, 'f', 6);
    }
    // fw 4.1.5 answers "display pan set ... center=" with R|0|<center> but
    // sends no pan status, so apply the confirmed values to the model here
    // (the spectrum widget follows PanadapterModel::infoChanged).
    QPointer<PanadapterModel> panGuard(pan);
    const double center = m_scanPanCenterMhz;
    const double bw = m_scanPanBandwidthMhz;
    m_radio->sendCmdPublic(cmd, [panGuard, center, bw, bwChanged](int code, const QString& body) {
        if (code != 0 || !panGuard) {
            return;
        }
        bool ok = false;
        const double confirmed = body.trimmed().toDouble(&ok);
        QMap<QString, QString> kvs;
        kvs.insert(QStringLiteral("center"), QString::number(ok ? confirmed : center, 'f', 6));
        if (bwChanged) {
            kvs.insert(QStringLiteral("bandwidth"), QString::number(bw, 'f', 6));
        }
        panGuard->applyPanStatus(kvs);
    });
}

void ScannerPanel::onStop()
{
    m_scanner->stop(true);
}

void ScannerPanel::onStateChanged(Scanner::State state)
{
    const bool running = state != Scanner::State::Idle;
    m_startBtn->setText(running ? QStringLiteral("Stop") : QStringLiteral("Start"));
    m_pauseBtn->setEnabled(running);
    m_pauseBtn->setText(state == Scanner::State::Paused || state == Scanner::State::Stopped
                            ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    m_skipBtn->setEnabled(state == Scanner::State::Scanning || state == Scanner::State::Stopped);
    // The scan list is fixed for the duration of a scan; timings stay live.
    m_sliceCombo->setEnabled(!running);
    m_modeCombo->setEnabled(!running);
    m_stack->setEnabled(!running);
    m_stateLabel->setText(Scanner::stateName(state));
    if (!running) {
        m_levelLabel->clear();
    }
}

void ScannerPanel::onChannelChanged(int index, const ScanChannel& ch, bool priority)
{
    QString text = priority ? QStringLiteral("PRIORITY  ") : QString();
    text += QStringLiteral("%1 MHz").arg(formatMhz(ch.freqMhz));
    if (!ch.mode.isEmpty()) {
        text += QStringLiteral("  %1").arg(ch.mode);
    }
    if (!ch.label.isEmpty()) {
        text += QStringLiteral("  %1").arg(ch.label);
    }
    if (index >= 0) {
        text += QStringLiteral("   (%1/%2)").arg(index + 1).arg(m_scanner->channels().size());
    }
    m_channelLabel->setText(text);
}

void ScannerPanel::onHitAdded(const ScanHit& hit)
{
    m_log->insertRow(0);
    auto* timeItem = new QTableWidgetItem(hit.timeUtc.toString(QStringLiteral("HH:mm:ss")));
    timeItem->setData(Qt::UserRole, hit.id);
    auto* freqItem = new QTableWidgetItem(formatMhz(hit.freqMhz));
    freqItem->setData(Qt::UserRole, hit.freqMhz);
    m_log->setItem(0, kColTime, timeItem);
    m_log->setItem(0, kColFreq, freqItem);
    m_log->setItem(0, kColMode, new QTableWidgetItem(hit.mode));
    m_log->setItem(0, kColLabel, new QTableWidgetItem(
        hit.priority ? QStringLiteral("[P] %1").arg(hit.label) : hit.label));
    m_log->setItem(0, kColPeak, new QTableWidgetItem(
        hit.peakDbm > -149.0f ? QStringLiteral("%1 dBm").arg(hit.peakDbm, 0, 'f', 1)
                              : QStringLiteral("—")));
    while (m_log->rowCount() > kMaxLogRows) {
        m_log->removeRow(m_log->rowCount() - 1);
    }
}

void ScannerPanel::onHitPeakUpdated(int hitId, float peakDbm)
{
    for (int r = 0; r < m_log->rowCount(); ++r) {
        const QTableWidgetItem* it = m_log->item(r, kColTime);
        if (it && it->data(Qt::UserRole).toInt() == hitId) {
            if (QTableWidgetItem* p = m_log->item(r, kColPeak)) {
                p->setText(QStringLiteral("%1 dBm").arg(peakDbm, 0, 'f', 1));
            }
            return;
        }
    }
}

void ScannerPanel::onLogActivated(int row, int /*column*/)
{
    const QTableWidgetItem* freqItem = m_log->item(row, kColFreq);
    const QTableWidgetItem* modeItem = m_log->item(row, kColMode);
    if (!freqItem) {
        return;
    }
    ScanChannel ch;
    ch.freqMhz = freqItem->data(Qt::UserRole).toDouble();
    ch.mode = modeItem ? modeItem->text() : QString();

    // Idle: aim at the currently chosen slice. Scanning: tuneTo() stops the
    // scan on its own slice without restoring — the user picked this frequency.
    if (!m_scanner->isRunning()) {
        SliceModel* slice = chosenSlice();
        if (!slice) {
            return;
        }
        m_scanner->setTarget(std::make_unique<SliceScanTarget>(slice, m_radio));
    }
    m_scanner->tuneTo(ch);
}

} // namespace AetherSDR
