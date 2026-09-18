#include "HfFaxDialog.h"

#include "core/AppSettings.h"
#include "models/SliceModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr int kImageWidth = HfFaxDecoder::kImageWidth;
constexpr int kMinCanvasHeight = 520;
constexpr int kCanvasWidth = 980;
constexpr int kCapacityGrow = 512;
constexpr int kFaxFilterWidthHz = 1400;

struct HfFaxStation {
    const char* label;
    double frequencyKhz;
};

constexpr HfFaxStation kStations[] = {
    {"Germany - DDH1 (3855)", 3855.0},
    {"Germany - DDK3 (7880)", 7880.0},
    {"Germany - DDK6 (13882.5)", 13882.5},
    {"UK - GYA (2618.5)", 2618.5},
    {"UK - GYA (4610)", 4610.0},
    {"UK - GYA (6834)", 6834.0},
    {"UK - GYA (8040)", 8040.0},
    {"UK - GYA (11086.5)", 11086.5},
    {"UK - GYA (11886.5)", 11886.5},
    {"UK - GYA (12390)", 12390.0},
    {"UK - GYA (18261)", 18261.0},
    {"USA - KVM (9982.5)", 9982.5},
    {"USA - KVM (11090)", 11090.0},
    {"USA - KVM (16135)", 16135.0},
    {"USA - NMF (4235)", 4235.0},
    {"USA - NMF (6340.5)", 6340.5},
    {"USA - NMF (9110)", 9110.0},
    {"USA - NMF (12750)", 12750.0},
    {"USA - NMG (4317.9)", 4317.9},
    {"USA - NMG (8503.9)", 8503.9},
    {"USA - NMG (12789.9)", 12789.9},
    {"USA - NMG (17146.4)", 17146.4},
    {"USA - NMC (4346)", 4346.0},
    {"USA - NMC (8682)", 8682.0},
    {"USA - NMC (12786)", 12786.0},
    {"USA - NMC (17151.2)", 17151.2},
    {"USA - NMC (22527)", 22527.0},
    {"Canada - VFF (3251.1)", 3251.1},
    {"Canada - VCS (4271)", 4271.0},
    {"Canada - VCO (4416)", 4416.0},
    {"Canada - VCS (6496.4)", 6496.4},
    {"Canada - VCO (6915.1)", 6915.1},
    {"Canada - VFF (7708.1)", 7708.1},
    {"Canada - VFA (8457.8)", 8457.8},
    {"Canada - VCS (10536)", 10536.0},
    {"Canada - VCS (13510)", 13510.0},
    {"Australia - VMC (2628)", 2628.0},
    {"Australia - VMC (5100)", 5100.0},
    {"Australia - VMW (5755)", 5755.0},
    {"Australia - VMW (7535)", 7535.0},
    {"Australia - VMC (11030)", 11030.0},
    {"Australia - VMC (13920)", 13920.0},
    {"Australia - VMW (10555)", 10555.0},
    {"Australia - VMW (15615)", 15615.0},
    {"Australia - VMW (18060)", 18060.0},
    {"Australia - VMC (20469)", 20469.0},
    {"South Africa - ZSJ (4014)", 4014.0},
    {"South Africa - ZSJ (7508)", 7508.0},
    {"South Africa - ZSJ (13538)", 13538.0},
    {"South Africa - ZSJ (18238)", 18238.0},
    {"Antarctica - VLM (7074)", 7074.0},
    {"Brazil - PWZ (12665)", 12665.0},
    {"Brazil - PWZ (16978)", 16978.0},
    {"Greece - SWA (8106.9)", 8106.9},
    {"Chile - CBV (4228)", 4228.0},
    {"Chile - CBV (8677)", 8677.0},
    {"Chile - CBV (17146.4)", 17146.4},
    {"Japan - JMH (3622.5)", 3622.5},
    {"Japan - JMH (7795)", 7795.0},
    {"Japan - JMH (13988.5)", 13988.5},
    {"Senegal - 6VA (13667.5)", 13667.5},
    {"Senegal - 6VU (19750)", 19750.0},
};

QString sanitizeFileStem(QString text)
{
    text.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    text.replace(QRegularExpression(QStringLiteral("-+")), QStringLiteral("-"));
    text.remove(QRegularExpression(QStringLiteral("^-|-$")));
    return text.isEmpty() ? QStringLiteral("hf-fax") : text.toLower();
}

} // namespace

class HfFaxToneScaleWidget : public QWidget {
public:
    explicit HfFaxToneScaleWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(56);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setTone(float toneHz)
    {
        m_toneHz = toneHz;
        update();
    }

    void setSyncState(bool locked, float quality)
    {
        m_locked = locked;
        m_quality = std::clamp(quality, 0.0f, 1.0f);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF outer = rect().adjusted(1, 1, -1, -1);
        painter.fillRect(rect(), QColor("#0b1016"));

        painter.setPen(QColor("#1f2d3c"));
        painter.setBrush(QColor("#06090e"));
        painter.drawRoundedRect(outer, 8, 8);

        const QRectF track = outer.adjusted(12, 10, -12, -12);
        painter.setPen(QPen(QColor("#2d8cff"), 2.0));
        for (int i = 0; i <= 10; ++i) {
            const qreal x = track.left() + ((track.width() * i) / 10.0);
            painter.drawLine(QPointF(x, track.top() + 6), QPointF(x, track.bottom() - 8));
        }

        const QColor syncColor = m_locked ? QColor("#34c759") : QColor("#f6c343");
        painter.setPen(QPen(syncColor, 3.0));
        painter.drawLine(QPointF(track.left(), track.bottom()),
                         QPointF(track.left() + (track.width() * m_quality), track.bottom()));

        const float clampedTone = std::clamp(m_toneHz, 1200.0f, 2600.0f);
        const qreal markerX = track.left()
            + ((clampedTone - 1200.0f) / (2600.0f - 1200.0f)) * track.width();
        painter.setBrush(syncColor);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(markerX, track.center().y()), 6.0, 6.0);
    }

private:
    float m_toneHz{1900.0f};
    bool m_locked{false};
    float m_quality{0.0f};
};

class HfFaxImageView : public QWidget {
public:
    explicit HfFaxImageView(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        updateCanvasSize();
    }

    void clearImage()
    {
        m_rawImage = QImage();
        m_displayImage = QImage();
        m_lineCount = 0;
        updateCanvasSize();
        update();
    }

    bool appendRawLine(const QByteArray& grayLine)
    {
        if (grayLine.size() != kImageWidth)
            return false;

        ensureCapacity(m_lineCount + 1);
        std::memcpy(m_rawImage.scanLine(m_lineCount), grayLine.constData(), static_cast<size_t>(kImageWidth));
        renderLines(m_lineCount, 1);
        ++m_lineCount;
        updateCanvasSize();
        update();
        return true;
    }

    void setBlackBias(float bias)
    {
        if (qFuzzyCompare(m_blackBias, bias))
            return;
        m_blackBias = bias;
        rerenderAll();
    }

    void setContrast(float contrast)
    {
        contrast = std::max(0.15f, contrast);
        if (qFuzzyCompare(m_contrast, contrast))
            return;
        m_contrast = contrast;
        rerenderAll();
    }

    void setInverted(bool inverted)
    {
        if (m_inverted == inverted)
            return;
        m_inverted = inverted;
        rerenderAll();
    }

    int lineCount() const
    {
        return m_lineCount;
    }

    bool hasImage() const
    {
        return m_lineCount > 0;
    }

    bool saveImage(const QString& path) const
    {
        if (!hasImage() || m_displayImage.isNull())
            return false;
        return m_displayImage.copy(0, 0, kImageWidth, m_lineCount).save(path);
    }

    QSize sizeHint() const override
    {
        return {kCanvasWidth, std::max(kMinCanvasHeight, m_lineCount)};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#0b1016"));

        if (m_lineCount <= 0 || m_displayImage.isNull()) {
            painter.setPen(QColor("#7b8ea1"));
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("Waiting for HF-Fax image data"));
            return;
        }

        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(QRect(0, 0, width(), m_lineCount),
                          m_displayImage,
                          QRect(0, 0, kImageWidth, m_lineCount));
    }

private:
    static float clamp01(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    void ensureCapacity(int requiredLines)
    {
        if (!m_rawImage.isNull() && requiredLines <= m_rawImage.height())
            return;

        const int newHeight = std::max(requiredLines, (m_rawImage.isNull() ? kCapacityGrow
                                                                            : m_rawImage.height() + kCapacityGrow));
        QImage newRaw(kImageWidth, newHeight, QImage::Format_Grayscale8);
        QImage newDisplay(kImageWidth, newHeight, QImage::Format_Grayscale8);
        newRaw.fill(127);
        newDisplay.fill(127);

        if (!m_rawImage.isNull()) {
            for (int y = 0; y < m_lineCount; ++y) {
                std::memcpy(newRaw.scanLine(y), m_rawImage.constScanLine(y), static_cast<size_t>(kImageWidth));
                std::memcpy(newDisplay.scanLine(y), m_displayImage.constScanLine(y), static_cast<size_t>(kImageWidth));
            }
        }

        m_rawImage = std::move(newRaw);
        m_displayImage = std::move(newDisplay);
    }

    void renderLines(int startLine, int lineCount)
    {
        if (m_rawImage.isNull() || m_displayImage.isNull() || lineCount <= 0)
            return;

        const int begin = std::max(0, startLine);
        const int end = std::min(begin + lineCount, m_rawImage.height());
        for (int y = begin; y < end; ++y) {
            const auto* src = m_rawImage.constScanLine(y);
            auto* dst = m_displayImage.scanLine(y);
            for (int x = 0; x < kImageWidth; ++x) {
                float normalized = static_cast<float>(src[x]) / 255.0f;
                normalized = ((normalized - 0.5f - m_blackBias) * m_contrast) + 0.5f;
                normalized = clamp01(normalized);
                if (m_inverted)
                    normalized = 1.0f - normalized;
                dst[x] = static_cast<uchar>(std::lround(normalized * 255.0f));
            }
        }
    }

    void rerenderAll()
    {
        if (m_lineCount > 0)
            renderLines(0, m_lineCount);
        update();
    }

    void updateCanvasSize()
    {
        resize(kCanvasWidth, std::max(kMinCanvasHeight, m_lineCount));
        updateGeometry();
    }

    QImage m_rawImage;
    QImage m_displayImage;
    int m_lineCount{0};
    float m_blackBias{0.0f};
    float m_contrast{1.0f};
    bool m_inverted{true};
};

HfFaxDialog::HfFaxDialog(QWidget* parent)
    : QDialog(parent)
{
    buildUi();
    connectDecoder();
    loadSettings();
    renderSettingsFromControls();
    m_decoder.start();
}

HfFaxDialog::~HfFaxDialog()
{
    saveSettings();
    m_decoder.stop();
}

void HfFaxDialog::buildUi()
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("HF-Fax"));
    setMinimumSize(1180, 760);
    setStyleSheet(QStringLiteral(
        "QDialog { background: #111821; color: #dde6ef; }"
        "QGroupBox { border: 1px solid #27374a; border-radius: 8px; margin-top: 12px; padding-top: 14px; font-weight: 600; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #9ec1e8; }"
        "QLabel { color: #d7e0e8; }"
        "QComboBox, QDoubleSpinBox, QSpinBox { background: #0b1016; color: #e8eef4; border: 1px solid #32475d; border-radius: 6px; padding: 4px 8px; }"
        "QPushButton { background: #1e5aa8; color: white; border: 1px solid #3570bd; border-radius: 6px; padding: 6px 14px; }"
        "QPushButton:hover { background: #2766b7; }"
        "QPushButton:pressed { background: #174a89; }"
        "QCheckBox { color: #d7e0e8; spacing: 6px; }"
        "QSlider::groove:horizontal { height: 6px; background: #223246; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #69a7ff; width: 18px; margin: -6px 0; border-radius: 9px; }"
        "QScrollArea { border: 1px solid #27374a; background: #0b1016; }"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* controlsGroup = new QGroupBox(QStringLiteral("Controls"), this);
    auto* controls = new QGridLayout(controlsGroup);
    controls->setHorizontalSpacing(12);
    controls->setVerticalSpacing(8);

    auto* stationLabel = new QLabel(QStringLiteral("Station:"));
    controls->addWidget(stationLabel, 0, 0);

    m_stationCombo = new QComboBox(controlsGroup);
    m_stationCombo->addItem(QStringLiteral("Manual"));
    for (const auto& station : kStations)
        m_stationCombo->addItem(QString::fromUtf8(station.label), station.frequencyKhz);
    m_stationCombo->setMinimumWidth(320);
    controls->addWidget(m_stationCombo, 0, 1, 1, 2);

    auto* frequencyLabel = new QLabel(QStringLiteral("Tune (kHz):"));
    controls->addWidget(frequencyLabel, 0, 3);

    m_frequencySpin = new QDoubleSpinBox(controlsGroup);
    m_frequencySpin->setRange(1000.0, 30000.0);
    m_frequencySpin->setDecimals(1);
    m_frequencySpin->setSingleStep(0.1);
    m_frequencySpin->setKeyboardTracking(false);
    m_frequencySpin->setAlignment(Qt::AlignRight);
    controls->addWidget(m_frequencySpin, 0, 4);

    m_autoStartCheck = new QCheckBox(QStringLiteral("Auto start"), controlsGroup);
    controls->addWidget(m_autoStartCheck, 0, 5);

    auto* blackLabel = new QLabel(QStringLiteral("Black level:"));
    controls->addWidget(blackLabel, 0, 6);

    m_blackLevelSlider = new QSlider(Qt::Horizontal, controlsGroup);
    m_blackLevelSlider->setRange(0, 100);
    controls->addWidget(m_blackLevelSlider, 0, 7);

    auto* eraseButton = new QPushButton(QStringLiteral("Erase"), controlsGroup);
    controls->addWidget(eraseButton, 0, 8);

    auto* offsetLabel = new QLabel(QStringLiteral("RX offset (Hz):"));
    controls->addWidget(offsetLabel, 1, 0);

    m_rxOffsetSpin = new QSpinBox(controlsGroup);
    m_rxOffsetSpin->setRange(300, 4000);
    m_rxOffsetSpin->setSingleStep(10);
    m_rxOffsetSpin->setAlignment(Qt::AlignRight);
    controls->addWidget(m_rxOffsetSpin, 1, 1);

    m_lsbCheck = new QCheckBox(QStringLiteral("LSB"), controlsGroup);
    controls->addWidget(m_lsbCheck, 1, 2);

    m_pauseCheck = new QCheckBox(QStringLiteral("Pause"), controlsGroup);
    controls->addWidget(m_pauseCheck, 1, 5);

    auto* contrastLabel = new QLabel(QStringLiteral("Contrast:"));
    controls->addWidget(contrastLabel, 1, 6);

    m_contrastSlider = new QSlider(Qt::Horizontal, controlsGroup);
    m_contrastSlider->setRange(0, 100);
    controls->addWidget(m_contrastSlider, 1, 7);

    auto* saveButton = new QPushButton(QStringLiteral("Save"), controlsGroup);
    controls->addWidget(saveButton, 1, 8);

    auto* clockLabel = new QLabel(QStringLiteral("Clock adjust (ppm):"));
    controls->addWidget(clockLabel, 2, 0);

    m_clockAdjustSpin = new QSpinBox(controlsGroup);
    m_clockAdjustSpin->setRange(-500, 500);
    m_clockAdjustSpin->setSingleStep(1);
    controls->addWidget(m_clockAdjustSpin, 2, 1);

    m_invertedCheck = new QCheckBox(QStringLiteral("Inverted"), controlsGroup);
    controls->addWidget(m_invertedCheck, 2, 2);

    m_syncLabel = new QLabel(QStringLiteral("Sync: Searching"));
    controls->addWidget(m_syncLabel, 2, 3, 1, 2);

    m_toneLabel = new QLabel(QStringLiteral("Tone: 1900 Hz"));
    controls->addWidget(m_toneLabel, 2, 5);

    m_linesLabel = new QLabel(QStringLiteral("Lines: 0"));
    controls->addWidget(m_linesLabel, 2, 6);

    m_autoSaveCheck = new QCheckBox(QStringLiteral("Auto save"), controlsGroup);
    controls->addWidget(m_autoSaveCheck, 2, 8);

    m_toneScale = new HfFaxToneScaleWidget(controlsGroup);
    controls->addWidget(m_toneScale, 3, 0, 1, 9);

    m_sliceSummaryLabel = new QLabel(QStringLiteral("No active slice selected."));
    m_sliceSummaryLabel->setStyleSheet(QStringLiteral("color: #9fb6cb;"));
    controls->addWidget(m_sliceSummaryLabel, 4, 0, 1, 9);

    root->addWidget(controlsGroup);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_imageView = new HfFaxImageView(m_scrollArea);
    m_scrollArea->setWidget(m_imageView);
    root->addWidget(m_scrollArea, 1);

    m_statusLabel = new QLabel(QStringLiteral("HF-Fax ready. Waiting for sync."));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #8fb0d0;"));
    root->addWidget(m_statusLabel);

    connect(eraseButton, &QPushButton::clicked, this, &HfFaxDialog::clearImage);
    connect(saveButton, &QPushButton::clicked, this, &HfFaxDialog::saveImage);
    connect(m_stationCombo, &QComboBox::currentIndexChanged, this, &HfFaxDialog::onStationSelected);
    connect(m_frequencySpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &HfFaxDialog::onFrequencyEdited);
    connect(m_rxOffsetSpin, &QSpinBox::valueChanged, this, [this](int) {
        if (!m_syncingControls)
            applySliceTuning();
    });
    connect(m_lsbCheck, &QCheckBox::toggled, this, [this](bool) {
        if (!m_syncingControls)
            applySliceTuning();
    });
    connect(m_autoStartCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_decoder.setAutoStart(on);
    });
    connect(m_pauseCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_decoder.setPaused(on);
        updateStatusText(on ? QStringLiteral("HF-Fax paused.") : QStringLiteral("HF-Fax resumed."));
    });
    connect(m_invertedCheck, &QCheckBox::toggled, this, [this](bool) {
        renderSettingsFromControls();
    });
    connect(m_blackLevelSlider, &QSlider::valueChanged, this, [this](int) {
        renderSettingsFromControls();
    });
    connect(m_contrastSlider, &QSlider::valueChanged, this, [this](int) {
        renderSettingsFromControls();
    });
    connect(m_clockAdjustSpin, &QSpinBox::valueChanged, this, [this](int ppm) {
        m_decoder.setClockAdjustPpm(ppm);
    });
}

void HfFaxDialog::connectDecoder()
{
    connect(&m_decoder, &HfFaxDecoder::rawLineDecoded,
            this, &HfFaxDialog::appendRawLine);
    connect(&m_decoder, &HfFaxDecoder::syncStateChanged,
            this, &HfFaxDialog::updateSyncState);
    connect(&m_decoder, &HfFaxDecoder::toneChanged,
            this, &HfFaxDialog::updateTone);
    connect(&m_decoder, &HfFaxDecoder::statusChanged,
            this, &HfFaxDialog::updateStatusText);
    connect(&m_decoder, &HfFaxDecoder::transmissionStarted,
            this, &HfFaxDialog::handleTransmissionStarted);
    connect(&m_decoder, &HfFaxDecoder::transmissionEnded,
            this, &HfFaxDialog::handleTransmissionEnded);
}

void HfFaxDialog::loadSettings()
{
    auto& settings = AppSettings::instance();

    restoreGeometry(settings.value(QStringLiteral("HfFaxGeometry")).toByteArray());

    const int savedIndex = settings.value(QStringLiteral("HfFaxStationIndex"), 0).toInt();
    const int safeIndex = std::clamp(savedIndex, 0, m_stationCombo->count() - 1);

    m_syncingControls = true;
    m_stationCombo->setCurrentIndex(safeIndex);
    m_frequencySpin->setValue(settings.value(QStringLiteral("HfFaxFrequencyKhz"), 11090.0).toDouble());
    m_rxOffsetSpin->setValue(settings.value(QStringLiteral("HfFaxRxOffset"), 1900).toInt());
    m_lsbCheck->setChecked(settings.value(QStringLiteral("HfFaxUseLsb"), false).toBool());
    m_autoStartCheck->setChecked(settings.value(QStringLiteral("HfFaxAutoStart"), true).toBool());
    m_pauseCheck->setChecked(false);
    m_blackLevelSlider->setValue(settings.value(QStringLiteral("HfFaxBlackLevel"), 50).toInt());
    m_contrastSlider->setValue(settings.value(QStringLiteral("HfFaxContrast"), 50).toInt());
    m_invertedCheck->setChecked(settings.value(QStringLiteral("HfFaxInverted"), true).toBool());
    m_clockAdjustSpin->setValue(settings.value(QStringLiteral("HfFaxClockAdjustPpm"), 0).toInt());
    m_autoSaveCheck->setChecked(settings.value(QStringLiteral("HfFaxAutoSave"), false).toBool());
    m_lastSaveDirectory = settings.value(QStringLiteral("HfFaxSaveDirectory")).toString();
    m_syncingControls = false;

    m_decoder.setAutoStart(m_autoStartCheck->isChecked());
    m_decoder.setPaused(m_pauseCheck->isChecked());
    m_decoder.setClockAdjustPpm(m_clockAdjustSpin->value());

    if (safeIndex > 0)
        m_frequencySpin->setValue(m_stationCombo->currentData().toDouble());
}

void HfFaxDialog::saveSettings() const
{
    auto& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("HfFaxGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("HfFaxStationIndex"), m_stationCombo->currentIndex());
    settings.setValue(QStringLiteral("HfFaxFrequencyKhz"), m_frequencySpin->value());
    settings.setValue(QStringLiteral("HfFaxRxOffset"), m_rxOffsetSpin->value());
    settings.setValue(QStringLiteral("HfFaxUseLsb"), m_lsbCheck->isChecked());
    settings.setValue(QStringLiteral("HfFaxAutoStart"), m_autoStartCheck->isChecked());
    settings.setValue(QStringLiteral("HfFaxBlackLevel"), m_blackLevelSlider->value());
    settings.setValue(QStringLiteral("HfFaxContrast"), m_contrastSlider->value());
    settings.setValue(QStringLiteral("HfFaxInverted"), m_invertedCheck->isChecked());
    settings.setValue(QStringLiteral("HfFaxClockAdjustPpm"), m_clockAdjustSpin->value());
    settings.setValue(QStringLiteral("HfFaxAutoSave"), m_autoSaveCheck->isChecked());
    settings.setValue(QStringLiteral("HfFaxSaveDirectory"), m_lastSaveDirectory);
    settings.save();
}

void HfFaxDialog::renderSettingsFromControls()
{
    const float blackBias = (static_cast<float>(m_blackLevelSlider->value()) - 50.0f) / 140.0f;
    const float contrast = std::max(0.15f, static_cast<float>(m_contrastSlider->value()) / 50.0f);
    m_imageView->setBlackBias(blackBias);
    m_imageView->setContrast(contrast);
    m_imageView->setInverted(m_invertedCheck->isChecked());
}

void HfFaxDialog::setActiveSlice(SliceModel* slice)
{
    if (slice == m_activeSlice)
        return;

    m_activeSlice = slice;
    updateSliceConnections(slice);
    syncControlsFromSlice();
    refreshSliceSummary();
}

void HfFaxDialog::updateSliceConnections(SliceModel* slice)
{
    if (m_sliceFrequencyConnection)
        disconnect(m_sliceFrequencyConnection);
    if (m_sliceModeConnection)
        disconnect(m_sliceModeConnection);
    if (m_sliceDiglConnection)
        disconnect(m_sliceDiglConnection);
    if (m_sliceDiguConnection)
        disconnect(m_sliceDiguConnection);

    if (!slice)
        return;

    m_sliceFrequencyConnection = connect(slice, &SliceModel::frequencyChanged, this, [this](double) {
        syncControlsFromSlice();
    });
    m_sliceModeConnection = connect(slice, &SliceModel::modeChanged, this, [this](const QString&) {
        syncControlsFromSlice();
    });
    m_sliceDiglConnection = connect(slice, &SliceModel::diglOffsetChanged, this, [this](int) {
        syncControlsFromSlice();
    });
    m_sliceDiguConnection = connect(slice, &SliceModel::diguOffsetChanged, this, [this](int) {
        syncControlsFromSlice();
    });
}

void HfFaxDialog::syncControlsFromSlice()
{
    if (!m_activeSlice)
        return;

    m_syncingControls = true;
    m_frequencySpin->setValue(m_activeSlice->frequency() * 1000.0);

    const QString mode = m_activeSlice->mode().toUpper();
    const bool isLsb = (mode == QStringLiteral("DIGL") || mode == QStringLiteral("LSB"));
    m_lsbCheck->setChecked(isLsb);

    const int offset = isLsb ? m_activeSlice->diglOffset() : m_activeSlice->diguOffset();
    if (offset > 0)
        m_rxOffsetSpin->setValue(offset);

    const int matchingIndex = findStationIndex(m_frequencySpin->value());
    m_stationCombo->setCurrentIndex(matchingIndex >= 0 ? matchingIndex : 0);
    m_syncingControls = false;
}

void HfFaxDialog::refreshSliceSummary()
{
    if (!m_activeSlice) {
        m_sliceSummaryLabel->setText(QStringLiteral(
            "No active slice selected. Presets will tune whichever slice is currently active."));
        return;
    }

    m_sliceSummaryLabel->setText(
        QStringLiteral("Active slice %1  |  %2 kHz  |  %3")
            .arg(QChar('A' + std::clamp(m_activeSlice->sliceId(), 0, 25)))
            .arg(m_activeSlice->frequency() * 1000.0, 0, 'f', 1)
            .arg(m_activeSlice->mode()));
}

int HfFaxDialog::findStationIndex(double khz) const
{
    constexpr double kToleranceKhz = 0.06;
    for (int i = 1; i < m_stationCombo->count(); ++i) {
        const double stationKhz = m_stationCombo->itemData(i).toDouble();
        if (std::abs(stationKhz - khz) <= kToleranceKhz)
            return i;
    }
    return -1;
}

void HfFaxDialog::feedAudio(const QByteArray& pcm24kStereo)
{
    m_decoder.feedAudio(pcm24kStereo);
}

void HfFaxDialog::appendRawLine(const QByteArray& grayLine)
{
    // Without a stop tone the image grew forever (~26 MB/hour). A full
    // IOC-576 chart is ~1,800 lines; after several charts' worth, start a
    // new image (auto-saving first when enabled) instead of growing.
    constexpr int kMaxImageLines = 8000;
    if (m_imageView->lineCount() >= kMaxImageLines) {
        if (m_autoSaveCheck->isChecked()) {
            maybeAutoSave();
        }
        m_imageView->clearImage();
        m_linesLabel->setText(QStringLiteral("Lines: 0"));
        updateStatusText(QStringLiteral("Image reached %1 lines without a stop tone; started a new image.")
                             .arg(kMaxImageLines));
    }

    auto* bar = m_scrollArea->verticalScrollBar();
    const bool followBottom = !bar || (bar->value() >= (bar->maximum() - 24));

    if (m_imageView->appendRawLine(grayLine))
        m_linesLabel->setText(QStringLiteral("Lines: %1").arg(m_imageView->lineCount()));

    if (followBottom && bar)
        bar->setValue(bar->maximum());
}

void HfFaxDialog::onStationSelected(int index)
{
    if (m_syncingControls)
        return;

    if (index > 0)
        m_frequencySpin->setValue(m_stationCombo->itemData(index).toDouble());

    if (index > 0)
        applySliceTuning();
}

void HfFaxDialog::onFrequencyEdited(double khz)
{
    if (m_syncingControls)
        return;

    const int stationIndex = findStationIndex(khz);
    m_syncingControls = true;
    m_stationCombo->setCurrentIndex(stationIndex >= 0 ? stationIndex : 0);
    m_syncingControls = false;

    applySliceTuning();
}

void HfFaxDialog::applySliceTuning()
{
    if (m_syncingControls)
        return;

    refreshSliceSummary();

    if (!m_activeSlice) {
        updateStatusText(QStringLiteral("Select an active slice before applying HF-Fax tuning."));
        return;
    }

    const double mhz = m_frequencySpin->value() / 1000.0;
    const int offset = m_rxOffsetSpin->value();
    const bool lsb = m_lsbCheck->isChecked();

    int low = 0;
    int high = 0;
    if (lsb) {
        high = -offset + (kFaxFilterWidthHz / 2);
        low = -offset - (kFaxFilterWidthHz / 2);
        if (high > -95) {
            low -= (high + 95);
            high = -95;
        }
        m_activeSlice->setMode(QStringLiteral("DIGL"));
        m_activeSlice->setDiglOffset(offset);
    } else {
        low = offset - (kFaxFilterWidthHz / 2);
        high = offset + (kFaxFilterWidthHz / 2);
        if (low < 95) {
            high += (95 - low);
            low = 95;
        }
        m_activeSlice->setMode(QStringLiteral("DIGU"));
        m_activeSlice->setDiguOffset(offset);
    }

    m_activeSlice->tuneAndRecenter(mhz);
    m_activeSlice->setFilterWidth(low, high);

    refreshSliceSummary();
    updateStatusText(QStringLiteral("Tuned active slice for %1.")
                         .arg(currentStationLabel()));
}

void HfFaxDialog::clearImage()
{
    m_imageView->clearImage();
    m_linesLabel->setText(QStringLiteral("Lines: 0"));
    m_receivingTransmission = false;
    m_decoder.clear();
}

void HfFaxDialog::updateStatusText(const QString& text)
{
    m_statusLabel->setText(text);
}

void HfFaxDialog::updateSyncState(bool locked, float quality)
{
    m_toneScale->setSyncState(locked, quality);
    if (locked) {
        m_syncLabel->setText(
            QStringLiteral("Sync: Locked (%1%)")
                .arg(static_cast<int>(std::lround(std::clamp(quality, 0.0f, 1.0f) * 100.0f))));
    } else {
        m_syncLabel->setText(QStringLiteral("Sync: Searching"));
    }
}

void HfFaxDialog::updateTone(float toneHz)
{
    m_toneScale->setTone(toneHz);
    m_toneLabel->setText(QStringLiteral("Tone: %1 Hz").arg(static_cast<int>(std::lround(toneHz))));
}

void HfFaxDialog::handleTransmissionStarted()
{
    if (!m_receivingTransmission && m_imageView->hasImage()) {
        if (m_autoSaveCheck->isChecked())
            maybeAutoSave();
        m_imageView->clearImage();
        m_linesLabel->setText(QStringLiteral("Lines: 0"));
    }
    m_receivingTransmission = true;
}

void HfFaxDialog::handleTransmissionEnded()
{
    m_receivingTransmission = false;
    if (m_autoSaveCheck->isChecked())
        maybeAutoSave();
}

QString HfFaxDialog::currentStationLabel() const
{
    if (m_stationCombo->currentIndex() > 0)
        return m_stationCombo->currentText();

    return QStringLiteral("Manual %1 kHz").arg(m_frequencySpin->value(), 0, 'f', 1);
}

QString HfFaxDialog::defaultSavePath(bool automatic) const
{
    QString directory = m_lastSaveDirectory;
    if (directory.isEmpty()) {
        directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (directory.isEmpty())
            directory = QDir::homePath();
        directory = QDir(directory).filePath(QStringLiteral("HF-Fax"));
    }

    QDir().mkpath(directory);

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString baseName = sanitizeFileStem(currentStationLabel());
    if (automatic)
        baseName = QStringLiteral("%1-%2").arg(baseName, timestamp);
    else
        baseName = QStringLiteral("%1-%2").arg(baseName, timestamp);

    return QDir(directory).filePath(baseName + QStringLiteral(".png"));
}

void HfFaxDialog::saveImage()
{
    if (!m_imageView->hasImage()) {
        QMessageBox::information(this, QStringLiteral("HF-Fax"),
                                 QStringLiteral("There is no weather fax image to save yet."));
        return;
    }

    const QString suggestedPath = defaultSavePath(false);
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save HF-Fax Image"),
        suggestedPath,
        QStringLiteral("PNG Images (*.png)"));
    if (path.isEmpty())
        return;

    if (!m_imageView->saveImage(path)) {
        QMessageBox::warning(this, QStringLiteral("HF-Fax"),
                             QStringLiteral("Failed to save the weather fax image."));
        return;
    }

    m_lastSaveDirectory = QFileInfo(path).absolutePath();
    updateStatusText(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()));
}

void HfFaxDialog::maybeAutoSave()
{
    if (!m_imageView->hasImage())
        return;

    const QString path = defaultSavePath(true);
    if (m_imageView->saveImage(path)) {
        m_lastSaveDirectory = QFileInfo(path).absolutePath();
        updateStatusText(QStringLiteral("Auto-saved %1").arg(QFileInfo(path).fileName()));
    }
}

} // namespace AetherSDR
