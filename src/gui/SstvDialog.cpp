#include "SstvDialog.h"

#include "DigitalModeStyles.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/SsdvCodec.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHideEvent>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr int kTxChunkFrames = 480;
constexpr int kSstvOffsetHz = 1900;
constexpr int kSstvFilterLowHz = 1100;
constexpr int kSstvFilterHighHz = 2500;
constexpr int kSsdvFilterLowHz = 900;
constexpr int kSsdvFilterHighHz = 2600;
constexpr auto kSsdvModeLabel = "SSDV (Bell 202 AFSK 1200)";

QString boxedStyle(const QString& bg)
{
    return QString(
        "QLabel { background: %1; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; min-width: 44px; }").arg(bg);
}

QString ensureSaveSuffix(QString path, const QString& selectedFilter)
{
    if (QFileInfo(path).suffix().isEmpty()) {
        if (selectedFilter.contains(QStringLiteral("*.ssdv")))
            path += QStringLiteral(".ssdv");
        else if (selectedFilter.contains(QStringLiteral("*.jpg")))
            path += QStringLiteral(".jpg");
        else
            path += QStringLiteral(".png");
    }
    return path;
}

QString modeDisplayName(const SstvModeDefinition* mode)
{
    return mode ? QString::fromLatin1(mode->name) : QStringLiteral("Unknown");
}

} // namespace

class SstvImageView : public QWidget {
public:
    explicit SstvImageView(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(860, 480);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void clearImage()
    {
        m_image = QImage();
        update();
    }

    bool hasImage() const
    {
        return !m_image.isNull();
    }

    QImage image() const
    {
        return m_image;
    }

    void roll(int delta)
    {
        if (m_image.isNull() || delta == 0)
            return;

        const int height = m_image.height();
        const int normalized = ((delta % height) + height) % height;
        if (normalized == 0)
            return;

        QImage rolled(m_image.size(), m_image.format());
        for (int y = 0; y < height; ++y) {
            const int srcY = (y - normalized + height) % height;
            std::memcpy(rolled.scanLine(y), m_image.constScanLine(srcY),
                        static_cast<size_t>(m_image.bytesPerLine()));
        }
        m_image = std::move(rolled);
        update();
    }

    void setImage(const QImage& image)
    {
        m_image = image;
        update();
    }

    void setOverlayText(const QString& text)
    {
        if (m_overlayText == text)
            return;
        m_overlayText = text;
        update();
    }

    void shift(int delta)
    {
        if (m_image.isNull() || delta == 0)
            return;

        const int width = m_image.width();
        const int normalized = ((delta % width) + width) % width;
        if (normalized == 0)
            return;

        QImage shifted(m_image.size(), m_image.format());
        for (int y = 0; y < m_image.height(); ++y) {
            const auto* src = reinterpret_cast<const QRgb*>(m_image.constScanLine(y));
            auto* dst = reinterpret_cast<QRgb*>(shifted.scanLine(y));
            for (int x = 0; x < width; ++x)
                dst[x] = src[(x - normalized + width) % width];
        }
        m_image = std::move(shifted);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#202020"));

        if (m_image.isNull()) {
            painter.setPen(QColor("#8ea5bc"));
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("Load an image or wait for an SSTV transmission."));
        } else {
            const QSize scaled = m_image.size().scaled(rect().size() - QSize(24, 24),
                                                       Qt::KeepAspectRatio);
            const QRect target(QPoint((width() - scaled.width()) / 2,
                                      (height() - scaled.height()) / 2),
                               scaled);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(target, m_image);
        }

        if (!m_overlayText.trimmed().isEmpty()) {
            const QRect badgeRect = QRect(16, 14,
                                          std::min(width() - 32, 280), 28);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(13, 59, 132, 220));
            painter.drawRoundedRect(badgeRect, 6, 6);
            painter.setPen(Qt::white);
            painter.drawText(badgeRect.adjusted(10, 0, -10, 0),
                             Qt::AlignVCenter | Qt::AlignLeft,
                             m_overlayText);
        }
    }

private:
    QImage m_image;
    QString m_overlayText;
};

SstvDialog::SstvDialog(RadioModel* radioModel, AudioEngine* audio, QWidget* parent)
    : QDialog(parent)
    , m_radioModel(radioModel)
    , m_audio(audio)
{
    setWindowTitle(QStringLiteral("SSTV"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1280, 820);

    buildUi();

    connect(&m_decoder, &SstvDecoder::detectedModeChanged,
            this, &SstvDialog::onDetectedModeChanged);
    connect(&m_decoder, &SstvDecoder::imageUpdated,
            this, &SstvDialog::onImageUpdated);
    connect(&m_decoder, &SstvDecoder::lineProgressChanged,
            this, &SstvDialog::onLineProgressChanged);
    connect(&m_decoder, &SstvDecoder::statusChanged,
            this, &SstvDialog::onAnalogStatusChanged);
    connect(&m_decoder, &SstvDecoder::syncStateChanged,
            this, &SstvDialog::onSyncStateChanged);
    connect(&m_ssdvDecoder, &SsdvAfskDecoder::imageUpdated,
            this, &SstvDialog::onSsdvImageUpdated);
    connect(&m_ssdvDecoder, &SsdvAfskDecoder::metricsUpdated,
            this, &SstvDialog::onSsdvMetricsUpdated);
    connect(&m_ssdvDecoder, &SsdvAfskDecoder::packetStatsChanged,
            this, &SstvDialog::onSsdvPacketStatsChanged);
    connect(&m_ssdvDecoder, &SsdvAfskDecoder::statusChanged,
            this, &SstvDialog::onSsdvStatusChanged);

    connect(this, &SstvDialog::txAudioReady,
            m_audio, &AudioEngine::feedDaxTxAudio,
            Qt::QueuedConnection);
    connect(m_radioModel, &RadioModel::radioTransmittingChanged,
            this, &SstvDialog::onRadioTransmittingChanged);

    m_txTimer = new QTimer(this);
    m_txTimer->setInterval(20);
    m_txTimer->setTimerType(Qt::PreciseTimer);
    connect(m_txTimer, &QTimer::timeout, this, &SstvDialog::pumpTxAudio);

    loadSettings();
    applyRxModeSelection(m_rxModeCombo->currentIndex());
    updateSliceSummary();
    updateSourceLabel();
    updateTxControls();
}

SstvDialog::~SstvDialog()
{
    // Do not touch m_radioModel here: at application shutdown this child is
    // destroyed after MainWindow's RadioModel member. PTT is released in
    // hideEvent() when the (WA_DeleteOnClose) window is closed.
    saveSettings();
}

void SstvDialog::hideEvent(QHideEvent* event)
{
    if (!event->spontaneous() && (m_txKeyRequested || m_txStartPending || m_txTimer->isActive())) {
        stopTransmissionNow();
    }
    QDialog::hideEvent(event);
}

void SstvDialog::buildUi()
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

    grid->addWidget(new QLabel(QStringLiteral("Mode:"), controls), 0, 1);
    m_rxModeCombo = new QComboBox(controls);
    m_rxModeCombo->addItem(QStringLiteral("Auto"));
    for (const auto& mode : allSstvModes())
        m_rxModeCombo->addItem(QString::fromLatin1(mode.name));
    m_rxModeCombo->addItem(QString::fromLatin1(kSsdvModeLabel));
    grid->addWidget(m_rxModeCombo, 0, 2);

    m_detectedModeLabel = new QLabel(QStringLiteral("Mode: Unknown"), controls);
    m_detectedModeLabel->setMinimumWidth(240);
    grid->addWidget(m_detectedModeLabel, 0, 3, 1, 2);

    m_syncLabel = new QLabel(QStringLiteral("Sync: Searching"), controls);
    grid->addWidget(m_syncLabel, 0, 5);

    m_progressLabel = new QLabel(QStringLiteral("Lines: 0 / 0"), controls);
    grid->addWidget(m_progressLabel, 0, 6);

    grid->addWidget(new QLabel(QStringLiteral("Adjust:"), controls), 0, 7);

    auto* shiftLeftButton = new QPushButton(QStringLiteral("←"), controls);
    auto* shiftRightButton = new QPushButton(QStringLiteral("→"), controls);
    auto* rollUpButton = new QPushButton(QStringLiteral("↑"), controls);
    auto* rollDownButton = new QPushButton(QStringLiteral("↓"), controls);
    shiftLeftButton->setMinimumWidth(46);
    shiftRightButton->setMinimumWidth(46);
    rollUpButton->setMinimumWidth(46);
    rollDownButton->setMinimumWidth(46);
    grid->addWidget(shiftLeftButton, 0, 8);
    grid->addWidget(shiftRightButton, 0, 9);
    grid->addWidget(rollUpButton, 0, 10);
    grid->addWidget(rollDownButton, 0, 11);

    auto* clearButton = new QPushButton(QStringLiteral("Erase"), controls);
    clearButton->setStyleSheet(
        "QPushButton { background: #0d3b84; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; }");
    grid->addWidget(clearButton, 0, 12);

    auto* saveButton = new QPushButton(QStringLiteral("Save"), controls);
    saveButton->setStyleSheet(
        "QPushButton { background: #0d3b84; color: white; font-weight: bold; "
        "padding: 10px 18px; border-radius: 4px; }");
    grid->addWidget(saveButton, 1, 12);

    m_sourceLabel = new QLabel(QStringLiteral("Showing: None"), controls);
    grid->addWidget(m_sourceLabel, 1, 1, 1, 3);

    m_sliceSummaryLabel = new QLabel(QStringLiteral("No active slice selected."), controls);
    grid->addWidget(m_sliceSummaryLabel, 1, 4, 1, 8);

    root->addWidget(controls);

    m_canvas = new SstvImageView(this);
    m_canvas->setStyleSheet("QWidget { border: 1px solid #303030; border-radius: 6px; }");
    root->addWidget(m_canvas, 1);

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

    txGrid->addWidget(new QLabel(QStringLiteral("Mode:"), txPanel), 0, 2);
    m_txModeCombo = new QComboBox(txPanel);
    for (const auto& mode : allSstvModes())
        m_txModeCombo->addItem(QString::fromLatin1(mode.name));
    m_txModeCombo->addItem(QString::fromLatin1(kSsdvModeLabel));
    txGrid->addWidget(m_txModeCombo, 0, 3);

    txGrid->addWidget(new QLabel(QStringLiteral("My ID:"), txPanel), 0, 4);
    m_myIdEdit = new QLineEdit(txPanel);
    m_myIdEdit->setMaxLength(16);
    txGrid->addWidget(m_myIdEdit, 0, 5);

    txGrid->addWidget(new QLabel(QStringLiteral("His ID:"), txPanel), 0, 6);
    m_hisIdEdit = new QLineEdit(txPanel);
    m_hisIdEdit->setMaxLength(16);
    txGrid->addWidget(m_hisIdEdit, 0, 7);

    m_logButton = new QPushButton(QStringLiteral("Log"), txPanel);
    m_logButton->setStyleSheet(
        "QPushButton { background: #646464; color: white; font-weight: bold; "
        "padding: 10px 16px; border-radius: 4px; }");
    txGrid->addWidget(m_logButton, 0, 8);

    m_loadButton = new QPushButton(QStringLiteral("Load"), txPanel);
    m_loadButton->setStyleSheet(
        "QPushButton { background: #646464; color: white; font-weight: bold; "
        "padding: 10px 16px; border-radius: 4px; }");
    txGrid->addWidget(m_loadButton, 0, 9);

    m_pendingLabel = new QLabel(QStringLiteral("TX image: none"), txPanel);
    txGrid->addWidget(m_pendingLabel, 1, 1, 1, 9);

    m_statusLabel = new QLabel(QStringLiteral("SSTV decoder ready."), this);
    m_statusLabel->setWordWrap(true);

    root->addWidget(txPanel);
    root->addWidget(m_statusLabel);

    connect(m_rxModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SstvDialog::applyRxModeSelection);
    connect(shiftLeftButton, &QPushButton::clicked, this, &SstvDialog::shiftImageLeft);
    connect(shiftRightButton, &QPushButton::clicked, this, &SstvDialog::shiftImageRight);
    connect(rollUpButton, &QPushButton::clicked, this, &SstvDialog::rollImageUp);
    connect(rollDownButton, &QPushButton::clicked, this, &SstvDialog::rollImageDown);
    connect(clearButton, &QPushButton::clicked, this, &SstvDialog::clearDisplayedImage);
    connect(saveButton, &QPushButton::clicked, this, &SstvDialog::saveCurrentImage);
    connect(m_txModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SstvDialog::onTxModeChanged);
    connect(m_txButton, &QPushButton::toggled,
            this, &SstvDialog::onTxToggled);
    connect(m_logButton, &QPushButton::clicked,
            this, &SstvDialog::logContact);
    connect(m_loadButton, &QPushButton::clicked,
            this, &SstvDialog::loadImageOrSsdv);
}

void SstvDialog::setActiveSlice(SliceModel* slice)
{
    if (slice == m_activeSlice)
        return;

    m_activeSlice = slice;
    updateSliceConnections(slice);
    syncControlsFromSlice();
    // No applySliceTuning() here: opening the dialog (or the active slice
    // changing) must not change the radio's mode/offset/filter. Tuning is
    // applied on explicit RX/TX mode selection and when transmitting.
    updateSliceSummary();
}

void SstvDialog::feedAudio(const QByteArray& pcm24kStereo)
{
    if (m_rxModeCombo->currentIndex() <= 0) {
        m_decoder.feedAudio(pcm24kStereo);
        m_ssdvDecoder.feedAudio(pcm24kStereo);
        return;
    }

    if (isSsdvRxSelected()) {
        m_ssdvDecoder.feedAudio(pcm24kStereo);
        return;
    }

    m_decoder.feedAudio(pcm24kStereo);
}

void SstvDialog::applyRxModeSelection(int index)
{
    if (m_syncingControls)
        return;

    clearAllRxState();

    if (index <= 0) {
        m_decoder.setAutoMode(true);
        m_detectedModeLabel->setText(QStringLiteral("Mode: Unknown"));
        updateStatus(QStringLiteral("SSTV/SSDV auto mode enabled."));
    } else {
        if (isSsdvRxSelected()) {
            m_detectedModeLabel->setText(QStringLiteral("Mode: SSDV"));
            m_progressLabel->setText(QStringLiteral("Packets: 0"));
            m_syncLabel->setText(QStringLiteral("Link: Searching"));
            updateStatus(QStringLiteral("SSDV manual mode armed."));
            applySliceTuning();
            return;
        }

        const auto* mode = currentRxSelection();
        if (!mode)
            return;
        m_decoder.setAutoMode(false);
        m_decoder.setManualMode(QString::fromLatin1(mode->name));
        updateStatus(QStringLiteral("SSTV manual mode armed for %1.").arg(QString::fromLatin1(mode->name)));
    }

    applySliceTuning();
}

void SstvDialog::clearAllRxState()
{
    m_decoder.setAutoMode(true);
    m_decoder.clear();
    m_ssdvDecoder.reset();
}

void SstvDialog::clearDisplayedImage()
{
    switch (m_displaySource) {
    case DisplaySource::Rx:
        m_rxImage = QImage();
        clearAllRxState();
        if (!m_txImage.isNull())
            setDisplayedImage(m_txImage, DisplaySource::Tx);
        else
            setDisplayedImage(QImage(), DisplaySource::None);
        break;
    case DisplaySource::Tx:
        m_txImage = QImage();
        if (!m_rxImage.isNull())
            setDisplayedImage(m_rxImage, DisplaySource::Rx);
        else
            setDisplayedImage(QImage(), DisplaySource::None);
        break;
    case DisplaySource::None:
        clearAllRxState();
        setDisplayedImage(QImage(), DisplaySource::None);
        break;
    }
    updateStatus(QStringLiteral("SSTV image cleared."));
    updateTxControls();
}

void SstvDialog::loadImageOrSsdv()
{
    QString startDir = m_lastLoadDirectory;
    if (startDir.isEmpty()) {
        startDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (startDir.isEmpty())
            startDir = QDir::homePath();
    }

    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Load SSTV Image"),
        startDir,
        QStringLiteral("Images and SSDV (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.ssdv);;Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;SSDV Packets (*.ssdv)"));
    if (path.isEmpty())
        return;

    QImage image;
    QString loadedCallsign;
    int loadedImageId = 0;

    if (path.endsWith(QStringLiteral(".ssdv"), Qt::CaseInsensitive)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, QStringLiteral("SSTV"),
                                 QStringLiteral("Unable to read the SSDV packet file."));
            return;
        }
        QString error;
        if (!SsdvCodec::decodePackets(file.readAll(), &image, &loadedCallsign, &loadedImageId, &error)) {
            QMessageBox::warning(this, QStringLiteral("SSTV"), error);
            return;
        }
        if (!loadedCallsign.isEmpty())
            m_myIdEdit->setText(loadedCallsign);
        m_ssdvImageId = loadedImageId;
        const int ssdvIndex = m_txModeCombo->findText(QString::fromLatin1(kSsdvModeLabel));
        if (ssdvIndex >= 0)
            m_txModeCombo->setCurrentIndex(ssdvIndex);
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        image = reader.read();
        if (image.isNull()) {
            QMessageBox::warning(this, QStringLiteral("SSTV"),
                                 QStringLiteral("Unable to decode the selected image file."));
            return;
        }
    }

    m_lastLoadDirectory = QFileInfo(path).absolutePath();
    m_txImage = image;
    setDisplayedImage(m_txImage, DisplaySource::Tx);
    updateStatus(QStringLiteral("Loaded TX image %1").arg(QFileInfo(path).fileName()));
    updateTxControls();
}

void SstvDialog::logContact()
{
    QFile file(logFilePath());
    const bool newFile = !file.exists();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        updateStatus(QStringLiteral("Unable to open SSTV log file for writing."));
        return;
    }

    QTextStream ts(&file);
    if (newFile)
        ts << "timestamp_local,tx_mode,my_id,his_id,active_slice,image_summary,display_source\n";

    QString sliceLetter = QStringLiteral("-");
    if (m_activeSlice)
        sliceLetter = QString(QChar('A' + std::clamp(m_activeSlice->sliceId(), 0, 25)));

    const QString displaySource =
        (m_displaySource == DisplaySource::Rx) ? QStringLiteral("RX")
        : (m_displaySource == DisplaySource::Tx) ? QStringLiteral("TX")
                                                 : QStringLiteral("None");

    ts << csvEscape(QDateTime::currentDateTime().toString(Qt::ISODate)) << ','
       << csvEscape(currentTxModeName()) << ','
       << csvEscape(m_myIdEdit->text().trimmed().toUpper()) << ','
       << csvEscape(m_hisIdEdit->text().trimmed().toUpper()) << ','
       << csvEscape(sliceLetter) << ','
       << csvEscape(currentImageSummary()) << ','
       << csvEscape(displaySource)
       << '\n';

    updateStatus(QStringLiteral("Logged SSTV activity to %1").arg(file.fileName()));
}

void SstvDialog::onAnalogStatusChanged(const QString& text)
{
    if (isSsdvRxSelected())
        return;
    updateStatus(text);
}

void SstvDialog::onDetectedModeChanged(const QString& modeName)
{
    if (isSsdvRxSelected())
        return;
    m_detectedModeLabel->setText(QStringLiteral("Mode: %1").arg(modeName));
}

void SstvDialog::onImageUpdated(const QImage& image)
{
    if (isSsdvRxSelected())
        return;

    m_rxImage = image;
    if (image.isNull()) {
        if (m_displaySource == DisplaySource::Rx) {
            if (!m_txImage.isNull())
                setDisplayedImage(m_txImage, DisplaySource::Tx);
            else
                setDisplayedImage(QImage(), DisplaySource::None);
        }
        return;
    }

    if (m_displaySource != DisplaySource::Tx || m_txImage.isNull())
        setDisplayedImage(m_rxImage, DisplaySource::Rx);
    updateTxControls();
}

void SstvDialog::onLineProgressChanged(int line, int totalLines)
{
    if (isSsdvRxSelected())
        return;
    m_progressLabel->setText(QStringLiteral("Lines: %1 / %2").arg(line).arg(totalLines));
}

void SstvDialog::onRadioTransmittingChanged(bool on)
{
    if (on) {
        if (m_txStartPending || m_txButton->isChecked()) {
            m_txStartPending = false;
            if (!m_txTimer->isActive())
                m_txTimer->start();
            updateStatus(m_activeTxTransport == TxTransport::SsdvAfsk
                             ? QStringLiteral("SSDV transmitter active.")
                             : QStringLiteral("SSTV transmitter active."));
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

void SstvDialog::onSsdvImageUpdated(const QImage& image)
{
    if (!(m_rxModeCombo->currentIndex() <= 0 || isSsdvRxSelected()))
        return;

    m_rxImage = image;
    if (image.isNull()) {
        if (m_displaySource == DisplaySource::Rx) {
            if (!m_txImage.isNull())
                setDisplayedImage(m_txImage, DisplaySource::Tx);
            else
                setDisplayedImage(QImage(), DisplaySource::None);
        }
        return;
    }

    m_detectedModeLabel->setText(QStringLiteral("Mode: SSDV"));
    if (m_displaySource != DisplaySource::Tx || m_txImage.isNull())
        setDisplayedImage(m_rxImage, DisplaySource::Rx);
    updateTxControls();
}

void SstvDialog::onSsdvMetricsUpdated(float, float, float confidence, bool locked)
{
    if (!(m_rxModeCombo->currentIndex() <= 0 || isSsdvRxSelected()))
        return;

    if (locked) {
        m_syncLabel->setText(
            QStringLiteral("Link: Locked (%1%)")
                .arg(static_cast<int>(std::lround(std::clamp(confidence, 0.0f, 1.0f) * 100.0f))));
    } else {
        m_syncLabel->setText(QStringLiteral("Link: Searching"));
    }
}

void SstvDialog::onSsdvPacketStatsChanged(int packetCount, int imageId, int lastPacketId, const QString& callsign)
{
    if (!(m_rxModeCombo->currentIndex() <= 0 || isSsdvRxSelected()))
        return;

    if (packetCount <= 0) {
        m_progressLabel->setText(QStringLiteral("Packets: 0"));
        return;
    }

    QString label = QStringLiteral("Packets: %1 | Last: %2 | Image: %3")
                        .arg(packetCount)
                        .arg(lastPacketId)
                        .arg(imageId);
    if (!callsign.trimmed().isEmpty())
        label += QStringLiteral(" | %1").arg(callsign.trimmed().toUpper());
    m_progressLabel->setText(label);
}

void SstvDialog::onSsdvStatusChanged(const QString& text)
{
    if (!(m_rxModeCombo->currentIndex() <= 0 || isSsdvRxSelected()))
        return;
    updateStatus(text);
}

void SstvDialog::onSyncStateChanged(bool locked, float quality)
{
    if (isSsdvRxSelected())
        return;

    if (locked) {
        m_syncLabel->setText(
            QStringLiteral("Sync: Locked (%1%)")
                .arg(static_cast<int>(std::lround(std::clamp(quality, 0.0f, 1.0f) * 100.0f))));
    } else {
        m_syncLabel->setText(QStringLiteral("Sync: Searching"));
    }
}

void SstvDialog::onTxModeChanged(int)
{
    applySliceTuning();
    updateTxControls();
}

void SstvDialog::onTxToggled(bool on)
{
    if (on) {
        if (!startTransmission()) {
            QSignalBlocker blocker(m_txButton);
            m_txButton->setChecked(false);
        }
        return;
    }

    stopTransmissionNow();
}

void SstvDialog::pumpTxAudio()
{
    if (!m_radioModel->transmitModel().isMox() && !m_txStartPending)
        return;

    QByteArray audio;
    switch (m_activeTxTransport) {
    case TxTransport::AnalogSstv:
        audio = m_modulator.renderAudio(kTxChunkFrames);
        break;
    case TxTransport::SsdvAfsk:
        audio = m_ssdvModulator.renderAudio(kTxChunkFrames);
        break;
    case TxTransport::None:
        return;
    }

    emit txAudioReady(audio);
    updateTxControls();

    const bool hasPendingData =
        (m_activeTxTransport == TxTransport::SsdvAfsk)
            ? m_ssdvModulator.hasPendingData()
            : m_modulator.hasPendingData();
    if (!hasPendingData) {
        ++m_drainGuardChunks;
        if (m_drainGuardChunks >= 3)
            stopTransmissionNow();
    } else {
        m_drainGuardChunks = 0;
    }
}

void SstvDialog::rollImageDown()
{
    if (!m_canvas->hasImage())
        return;
    m_canvas->roll(1);
    storeCanvasBackToSource();
}

void SstvDialog::rollImageUp()
{
    if (!m_canvas->hasImage())
        return;
    m_canvas->roll(-1);
    storeCanvasBackToSource();
}

void SstvDialog::saveCurrentImage()
{
    if (!m_canvas->hasImage()) {
        QMessageBox::information(this, QStringLiteral("SSTV"),
                                 QStringLiteral("There is no SSTV image to save."));
        return;
    }

    QString startDir = m_lastSaveDirectory;
    if (startDir.isEmpty()) {
        startDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (startDir.isEmpty())
            startDir = QDir::homePath();
    }

    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save SSTV Image"),
        QDir(startDir).filePath(QStringLiteral("sstv-%1")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))),
        QStringLiteral("PNG Image (*.png);;JPEG Image (*.jpg);;SSDV Packets (*.ssdv)"),
        &selectedFilter);
    if (path.isEmpty())
        return;

    path = ensureSaveSuffix(path, selectedFilter);

    const QImage image = m_canvas->image();
    bool ok = false;
    QString error;

    if (path.endsWith(QStringLiteral(".ssdv"), Qt::CaseInsensitive)) {
        QByteArray packets;
        ok = SsdvCodec::encodeImage(image,
                                    m_myIdEdit->text().trimmed().toUpper(),
                                    m_ssdvImageId,
                                    &packets,
                                    &error);
        if (ok) {
            QFile file(path);
            ok = file.open(QIODevice::WriteOnly) && (file.write(packets) == packets.size());
            if (!ok)
                error = QStringLiteral("Unable to write the SSDV packet stream.");
            else
                ++m_ssdvImageId;
        }
    } else {
        ok = image.save(path);
        if (!ok)
            error = QStringLiteral("Unable to save the selected image format.");
    }

    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("SSTV"), error);
        return;
    }

    m_lastSaveDirectory = QFileInfo(path).absolutePath();
    updateStatus(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()));
}

void SstvDialog::shiftImageLeft()
{
    if (!m_canvas->hasImage())
        return;
    m_canvas->shift(-1);
    storeCanvasBackToSource();
}

void SstvDialog::shiftImageRight()
{
    if (!m_canvas->hasImage())
        return;
    m_canvas->shift(1);
    storeCanvasBackToSource();
}

void SstvDialog::syncControlsFromSlice()
{
    updateSliceSummary();
}

void SstvDialog::updateStatus(const QString& text)
{
    m_statusLabel->setText(text);
}

void SstvDialog::applySliceTuning()
{
    if (!m_activeSlice)
        return;

    if (m_activeSlice->mode() != QStringLiteral("DIGU"))
        m_activeSlice->setMode(QStringLiteral("DIGU"));
    m_activeSlice->setDiguOffset(kSstvOffsetHz);
    const bool preferSsdv = isSsdvRxSelected() || isSsdvTxSelected();
    m_activeSlice->setFilterWidth(preferSsdv ? kSsdvFilterLowHz : kSstvFilterLowHz,
                                  preferSsdv ? kSsdvFilterHighHz : kSstvFilterHighHz);
    updateSliceSummary();
}

QString SstvDialog::csvEscape(const QString& text) const
{
    QString escaped = text;
    escaped.replace('"', QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString SstvDialog::currentImageSummary() const
{
    const QImage image = m_canvas->image();
    if (image.isNull())
        return QStringLiteral("none");
    return QStringLiteral("%1x%2").arg(image.width()).arg(image.height());
}

const SstvModeDefinition* SstvDialog::currentRxSelection() const
{
    if (!m_rxModeCombo || m_rxModeCombo->currentIndex() <= 0)
        return nullptr;
    if (isSsdvModeName(m_rxModeCombo->currentText()))
        return nullptr;
    return findSstvModeByName(m_rxModeCombo->currentText());
}

const SstvModeDefinition* SstvDialog::currentTxSelection() const
{
    if (!m_txModeCombo || m_txModeCombo->currentIndex() < 0)
        return nullptr;
    if (isSsdvModeName(m_txModeCombo->currentText()))
        return nullptr;
    return findSstvModeByName(m_txModeCombo->currentText());
}

QImage SstvDialog::imageForTransmit() const
{
    if (!m_txImage.isNull())
        return m_txImage;
    if (!m_rxImage.isNull())
        return m_rxImage;
    return m_canvas->image();
}

bool SstvDialog::isSsdvModeName(const QString& text) const
{
    return text.trimmed() == QString::fromLatin1(kSsdvModeLabel);
}

bool SstvDialog::isSsdvRxSelected() const
{
    return m_rxModeCombo && isSsdvModeName(m_rxModeCombo->currentText());
}

bool SstvDialog::isSsdvTxSelected() const
{
    return m_txModeCombo && isSsdvModeName(m_txModeCombo->currentText());
}

QString SstvDialog::currentTxModeName() const
{
    if (!m_txModeCombo || m_txModeCombo->currentIndex() < 0)
        return QStringLiteral("Unknown");
    if (isSsdvTxSelected())
        return QString::fromLatin1(kSsdvModeLabel);
    return modeDisplayName(currentTxSelection());
}

QString SstvDialog::logFilePath() const
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(base);
    dir.mkpath(QStringLiteral("AetherSDR"));
    return dir.filePath(QStringLiteral("AetherSDR/sstv-log.csv"));
}

void SstvDialog::loadSettings()
{
    auto& s = AppSettings::instance();
    restoreGeometry(s.value("SstvDialogGeometry").toByteArray());

    m_syncingControls = true;
    m_rxModeCombo->setCurrentIndex(std::clamp(
        s.value("SstvRxModeIndex", 0).toInt(), 0, m_rxModeCombo->count() - 1));
    m_txModeCombo->setCurrentIndex(std::clamp(
        s.value("SstvTxModeIndex", 0).toInt(), 0, m_txModeCombo->count() - 1));
    m_myIdEdit->setText(s.value("SstvMyId").toString());
    m_hisIdEdit->setText(s.value("SstvHisId").toString());
    m_lastLoadDirectory = s.value("SstvLastLoadDirectory").toString();
    m_lastSaveDirectory = s.value("SstvLastSaveDirectory").toString();
    m_ssdvImageId = std::clamp(s.value("SstvSsdvImageId", 0).toInt(), 0, 255);
    m_syncingControls = false;
}

void SstvDialog::saveSettings() const
{
    auto& s = AppSettings::instance();
    s.setValue("SstvDialogGeometry", saveGeometry());
    s.setValue("SstvRxModeIndex", m_rxModeCombo->currentIndex());
    s.setValue("SstvTxModeIndex", m_txModeCombo->currentIndex());
    s.setValue("SstvMyId", m_myIdEdit->text());
    s.setValue("SstvHisId", m_hisIdEdit->text());
    s.setValue("SstvLastLoadDirectory", m_lastLoadDirectory);
    s.setValue("SstvLastSaveDirectory", m_lastSaveDirectory);
    s.setValue("SstvSsdvImageId", m_ssdvImageId);
    s.save();
}

void SstvDialog::setDisplayedImage(const QImage& image, DisplaySource source)
{
    m_displaySource = source;
    m_canvas->setImage(image);
    updateSourceLabel();
}

void SstvDialog::stopTransmissionNow()
{
    m_txTimer->stop();
    m_txStartPending = false;
    m_drainGuardChunks = 0;
    m_modulator.clear();
    m_ssdvModulator.clear();
    m_activeTxTransport = TxTransport::None;

    // Unkey if we requested TX even when mox= has not been echoed yet.
    const bool unkey = m_txKeyRequested || m_radioModel->transmitModel().isMox();
    m_txKeyRequested = false;
    if (unkey)
        m_radioModel->setTransmit(false);

    {
        QSignalBlocker blocker(m_txButton);
        m_txButton->setChecked(false);
    }

    updateTxControls();
}

bool SstvDialog::startTransmission()
{
    const QImage image = imageForTransmit();
    if (image.isNull()) {
        updateStatus(QStringLiteral("Load an image or receive one before transmitting."));
        return false;
    }

    if (!m_activeSlice) {
        updateStatus(QStringLiteral("Select an active slice before transmitting."));
        return false;
    }

    m_modulator.clear();
    m_ssdvModulator.clear();
    m_activeTxTransport = TxTransport::None;

    if (isSsdvTxSelected()) {
        QString error;
        if (!m_ssdvModulator.startTransmission(image,
                                               m_myIdEdit->text().trimmed().toUpper(),
                                               m_ssdvImageId,
                                               &error)) {
            updateStatus(error.isEmpty()
                             ? QStringLiteral("Failed to build the SSDV waveform for transmit.")
                             : error);
            return false;
        }
        m_activeTxTransport = TxTransport::SsdvAfsk;
        m_ssdvImageId = (m_ssdvImageId + 1) & 0xFF;
    } else {
        const auto* mode = currentTxSelection();
        if (!mode) {
            updateStatus(QStringLiteral("Select an SSTV transmit mode first."));
            return false;
        }

        if (!m_modulator.startTransmission(image, *mode)) {
            updateStatus(QStringLiteral("Failed to build the SSTV waveform for transmit."));
            return false;
        }
        m_activeTxTransport = TxTransport::AnalogSstv;
    }

    applySliceTuning();
    if (!m_activeSlice->isTxSlice())
        m_activeSlice->setTxSlice(true);

    m_txStartPending = true;
    m_drainGuardChunks = 0;
    m_txKeyRequested = true;
    m_radioModel->setTransmit(true);
    if (m_activeTxTransport == TxTransport::SsdvAfsk) {
        updateStatus(QStringLiteral("Keying SSDV transmitter for %1 packets...")
                         .arg(m_ssdvModulator.packetCount()));
    } else {
        const auto* mode = currentTxSelection();
        updateStatus(QStringLiteral("Keying SSTV transmitter for %1...")
                         .arg(mode ? QString::fromLatin1(mode->name) : QStringLiteral("Unknown")));
    }
    updateTxControls();
    return true;
}

void SstvDialog::storeCanvasBackToSource()
{
    switch (m_displaySource) {
    case DisplaySource::Rx:
        m_rxImage = m_canvas->image();
        break;
    case DisplaySource::Tx:
        m_txImage = m_canvas->image();
        break;
    case DisplaySource::None:
        break;
    }
    updateTxControls();
}

void SstvDialog::updateSliceConnections(SliceModel* slice)
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
                                      this, [this](int) { updateSliceSummary(); });
}

void SstvDialog::updateSliceSummary()
{
    if (!m_activeSlice) {
        m_sliceSummaryLabel->setText(
            QStringLiteral("No active slice selected. SSTV RX/TX follows the active slice."));
        return;
    }

    m_sliceSummaryLabel->setText(
        QStringLiteral("Active slice %1 | %2 MHz | %3 | offset %4 Hz")
            .arg(QChar('A' + std::clamp(m_activeSlice->sliceId(), 0, 25)))
            .arg(m_activeSlice->frequency(), 0, 'f', 3)
            .arg(m_activeSlice->mode())
            .arg(m_activeSlice->diguOffset()));
}

void SstvDialog::updateSourceLabel()
{
    QString sourceText = QStringLiteral("Showing: None");
    switch (m_displaySource) {
    case DisplaySource::Rx:
        sourceText = QStringLiteral("Showing: RX image");
        break;
    case DisplaySource::Tx:
        sourceText = QStringLiteral("Showing: TX preview");
        break;
    case DisplaySource::None:
        break;
    }
    m_sourceLabel->setText(sourceText);
    m_canvas->setOverlayText(sourceText);
}

void SstvDialog::updateTxControls()
{
    const QImage txImage = imageForTransmit();
    const bool txActive = m_radioModel && m_radioModel->transmitModel().isMox();
    const QString modeText = currentTxModeName();
    if (txImage.isNull()) {
        m_pendingLabel->setText(QStringLiteral("TX image: none"));
    } else {
        const bool usingSsdv =
            (m_activeTxTransport == TxTransport::SsdvAfsk)
            || (!txActive && isSsdvTxSelected());
        const double seconds = usingSsdv
            ? static_cast<double>(m_ssdvModulator.pendingFrames()) / 24000.0
            : static_cast<double>(m_modulator.pendingFrames()) / 24000.0;

        if (txActive) {
            if (usingSsdv) {
                m_pendingLabel->setText(
                    QStringLiteral("Transmitting %1 | %2 packets | %3x%4 | %5 s remaining")
                        .arg(modeText)
                        .arg(std::max(1, m_ssdvModulator.packetCount()))
                        .arg(txImage.width())
                        .arg(txImage.height())
                        .arg(seconds, 0, 'f', 1));
            } else {
                m_pendingLabel->setText(
                    QStringLiteral("Transmitting %1 | %2x%3 | %4 s remaining")
                        .arg(modeText)
                        .arg(txImage.width())
                        .arg(txImage.height())
                        .arg(seconds, 0, 'f', 1));
            }
        } else {
            if (usingSsdv) {
                m_pendingLabel->setText(
                    QStringLiteral("TX image ready | %1 | %2x%3 | Bell-202 AFSK packet mode")
                        .arg(modeText)
                        .arg(txImage.width())
                        .arg(txImage.height()));
            } else {
                m_pendingLabel->setText(
                    QStringLiteral("TX image ready | %1 | %2x%3")
                        .arg(modeText)
                        .arg(txImage.width())
                        .arg(txImage.height()));
            }
        }
    }
}

} // namespace AetherSDR
