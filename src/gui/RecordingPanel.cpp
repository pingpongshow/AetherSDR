#include "RecordingPanel.h"

#include "core/AppSettings.h"
#include "core/IqSpectrumConverter.h"
#include "core/LogManager.h"
#include "gui/SpectrumWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace AetherSDR {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kKindRole = Qt::UserRole + 2;   // "spectrum" | "iq"

bool readBool(const QString& key, bool def)
{
    return AppSettings::instance().value(key, def ? "True" : "False").toString()
               .compare(QStringLiteral("True"), Qt::CaseInsensitive) == 0;
}

int readInt(const QString& key, int def)
{
    bool ok = false;
    const int v = AppSettings::instance().value(key, QString::number(def)).toString().toInt(&ok);
    return ok ? v : def;
}

QString humanBytes(qint64 b)
{
    if (b >= (1LL << 30)) { return QStringLiteral("%1 GB").arg(b / double(1LL << 30), 0, 'f', 2); }
    if (b >= (1LL << 20)) { return QStringLiteral("%1 MB").arg(b / double(1LL << 20), 0, 'f', 1); }
    return QStringLiteral("%1 KB").arg(b / 1024);
}

QString humanDuration(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

bool isSegmentPart(const QString& fileName)
{
    static const QRegularExpression re(QStringLiteral("_part\\d{3,}\\.aesr$"));
    return re.match(fileName).hasMatch();
}

QRgb schemeColor(const WfGradientStop* stops, int count, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    if (!stops || count <= 0) {
        const int g = static_cast<int>(t * 255.0f);
        return qRgb(g, g, g);
    }
    for (int i = 1; i < count; ++i) {
        if (t <= stops[i].pos) {
            const float span = std::max(1e-6f, stops[i].pos - stops[i - 1].pos);
            const float f = (t - stops[i - 1].pos) / span;
            return qRgb(static_cast<int>(stops[i - 1].r + f * (stops[i].r - stops[i - 1].r)),
                        static_cast<int>(stops[i - 1].g + f * (stops[i].g - stops[i - 1].g)),
                        static_cast<int>(stops[i - 1].b + f * (stops[i].b - stops[i - 1].b)));
        }
    }
    return qRgb(stops[count - 1].r, stops[count - 1].g, stops[count - 1].b);
}

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

RecordingPanel::RecordingPanel(QWidget* parent)
    : QWidget(parent)
    , m_bank(new SpectrumRecorderBank(this))
    , m_iq(std::make_unique<IqRecorder>())
    , m_player(new SpectrumPlayer(this))
{
    buildUi();

    for (int i = 0; i < SpectrumRecorderBank::kSlots; ++i) {
        connect(m_bank->slot(i), &SpectrumRecorder::stopped, this,
                [this](const QString& reason, const QString&) {
                    if (reason != QLatin1String("user")) {
                        QMessageBox::warning(this, tr("Spectrum recording stopped"),
                                             tr("Recording stopped: %1").arg(reason));
                    }
                    updateSpectrumStatus();
                    refreshLibrary();
                });
    }
    connect(m_iq.get(), &IqRecorder::fileOpened, this, [this](const QString& p) {
        m_iqCurrentFile = p;
    });
    connect(m_iq.get(), &IqRecorder::stopped, this, [this](const QString& reason) {
        if (reason != QLatin1String("user")) {
            QMessageBox::warning(this, tr("IQ recording stopped"),
                                 tr("Recording stopped: %1").arg(reason));
        }
        m_iqRecBtn->setText(tr("Record IQ"));
        refreshLibrary();
    });
    connect(m_player, &SpectrumPlayer::positionChanged, this, [this] { updateTransport(); });
    connect(m_player, &SpectrumPlayer::stateChanged, this, [this] { updateTransport(); });

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(500);
    connect(m_statusTimer, &QTimer::timeout, this, [this] {
        updateSpectrumStatus();
        if (m_iq->isRecording()) {
            m_iqStatus->setText(tr("%1 — %2, dropped %3")
                .arg(QFileInfo(m_iqCurrentFile).fileName(), humanBytes(m_iq->bytesWritten()))
                .arg(m_iq->droppedPackets()));
        }
    });
    m_statusTimer->start();

    refreshLibrary();
    refreshDiskSpace();
}

RecordingPanel::~RecordingPanel()
{
    stopPlayback();
}

QString RecordingPanel::defaultRecordingDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("AetherSDR/Recordings"));
}

QString RecordingPanel::recordingDirectory()
{
    const QString d = AppSettings::instance()
        .value(QStringLiteral("RecordingDirectory"), defaultRecordingDirectory()).toString().trimmed();
    return d.isEmpty() ? defaultRecordingDirectory() : d;
}

qint64 RecordingPanel::minFreeBytes() const
{
    return static_cast<qint64>(std::max(64, readInt(QStringLiteral("RecordingMinFreeMB"), 512))) << 20;
}

void RecordingPanel::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    // Spectrum recording
    auto* specBox = new QGroupBox(tr("Spectrum / waterfall recording"), this);
    auto* specLay = new QVBoxLayout(specBox);
    auto* specRow = new QHBoxLayout;
    m_panCombo = new QComboBox(specBox);
    m_specRecBtn = new QPushButton(tr("Record"), specBox);
    specRow->addWidget(new QLabel(tr("Pan:"), specBox));
    specRow->addWidget(m_panCombo, 1);
    specRow->addWidget(m_specRecBtn);
    specLay->addLayout(specRow);
    m_specStatus = new QLabel(tr("Idle"), specBox);
    m_specStatus->setWordWrap(true);
    specLay->addWidget(m_specStatus);
    root->addWidget(specBox);
    connect(m_specRecBtn, &QPushButton::clicked, this, &RecordingPanel::onSpectrumRecordClicked);
    connect(m_panCombo, &QComboBox::currentIndexChanged, this, [this] { updateSpectrumStatus(); });

    // IQ recording
    auto* iqBox = new QGroupBox(tr("DAX IQ recording"), this);
    auto* iqLay = new QVBoxLayout(iqBox);
    auto* iqRow = new QHBoxLayout;
    m_iqChannelCombo = new QComboBox(iqBox);
    for (int ch = 1; ch <= 4; ++ch) { m_iqChannelCombo->addItem(tr("IQ %1").arg(ch), ch); }
    m_iqRateCombo = new QComboBox(iqBox);
    for (int r : {24000, 48000, 96000, 192000}) { m_iqRateCombo->addItem(tr("%1 kHz").arg(r / 1000), r); }
    m_iqFormatCombo = new QComboBox(iqBox);
    m_iqFormatCombo->addItem(tr("float32"), QStringLiteral("Float32"));
    m_iqFormatCombo->addItem(tr("int16"), QStringLiteral("Int16"));
    m_iqRecBtn = new QPushButton(tr("Record IQ"), iqBox);
    iqRow->addWidget(m_iqChannelCombo);
    iqRow->addWidget(m_iqRateCombo);
    iqRow->addWidget(m_iqFormatCombo);
    iqRow->addWidget(m_iqRecBtn);
    iqLay->addLayout(iqRow);
    m_iqStatus = new QLabel(tr("Idle"), iqBox);
    m_iqStatus->setWordWrap(true);
    iqLay->addWidget(m_iqStatus);
    root->addWidget(iqBox);

    auto& s = AppSettings::instance();
    m_iqChannelCombo->setCurrentIndex(std::clamp(readInt(QStringLiteral("IqRecordingChannel"), 1), 1, 4) - 1);
    m_iqRateCombo->setCurrentIndex(std::max(0, m_iqRateCombo->findData(readInt(QStringLiteral("IqRecordingRate"), 48000))));
    m_iqFormatCombo->setCurrentIndex(std::max(0, m_iqFormatCombo->findData(
        s.value(QStringLiteral("IqRecordingFormat"), QStringLiteral("Float32")).toString())));
    connect(m_iqRecBtn, &QPushButton::clicked, this, &RecordingPanel::onIqRecordClicked);

    // Library
    auto* libBox = new QGroupBox(tr("Recordings"), this);
    auto* libLay = new QVBoxLayout(libBox);
    auto* dirRow = new QHBoxLayout;
    m_dirLabel = new QLabel(libBox);
    m_dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* dirBtn = new QPushButton(tr("Folder…"), libBox);
    auto* openBtn = new QPushButton(tr("Open"), libBox);
    dirRow->addWidget(m_dirLabel, 1);
    dirRow->addWidget(dirBtn);
    dirRow->addWidget(openBtn);
    libLay->addLayout(dirRow);
    m_diskLabel = new QLabel(libBox);
    libLay->addWidget(m_diskLabel);
    m_library = new QTreeWidget(libBox);
    m_library->setHeaderLabels({tr("Name"), tr("Type"), tr("Size"), tr("Duration"), tr("Date")});
    m_library->setRootIsDecorated(false);
    m_library->setSelectionMode(QAbstractItemView::SingleSelection);
    m_library->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    libLay->addWidget(m_library, 1);
    auto* libBtns = new QHBoxLayout;
    auto* playBtn = new QPushButton(tr("Play"), libBox);
    auto* renameBtn = new QPushButton(tr("Rename"), libBox);
    auto* deleteBtn = new QPushButton(tr("Delete"), libBox);
    auto* exportBtn = new QPushButton(tr("Export PNG"), libBox);
    auto* refreshBtn = new QPushButton(tr("Refresh"), libBox);
    for (QPushButton* b : {playBtn, renameBtn, deleteBtn, exportBtn, refreshBtn}) { libBtns->addWidget(b); }
    libLay->addLayout(libBtns);
    root->addWidget(libBox, 1);
    connect(dirBtn, &QPushButton::clicked, this, &RecordingPanel::onChooseFolder);
    connect(openBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(recordingDirectory()));
    });
    connect(playBtn, &QPushButton::clicked, this, &RecordingPanel::onPlayClicked);
    connect(m_library, &QTreeWidget::itemDoubleClicked, this, [this] { onPlayClicked(); });
    connect(renameBtn, &QPushButton::clicked, this, &RecordingPanel::onRenameClicked);
    connect(deleteBtn, &QPushButton::clicked, this, &RecordingPanel::onDeleteClicked);
    connect(exportBtn, &QPushButton::clicked, this, &RecordingPanel::onExportClicked);
    connect(refreshBtn, &QPushButton::clicked, this, [this] { refreshLibrary(); refreshDiskSpace(); });

    // Transport
    auto* tBox = new QGroupBox(tr("Playback"), this);
    auto* tLay = new QVBoxLayout(tBox);
    auto* tRow = new QHBoxLayout;
    m_playBtn = new QPushButton(tr("Play"), tBox);
    m_stopBtn = new QPushButton(tr("Stop"), tBox);
    m_speedCombo = new QComboBox(tBox);
    for (int x : {1, 2, 4, 8}) { m_speedCombo->addItem(QStringLiteral("%1×").arg(x), x); }
    m_loopCheck = new QCheckBox(tr("Loop"), tBox);
    m_timeLabel = new QLabel(QStringLiteral("0:00:00 / 0:00:00"), tBox);
    tRow->addWidget(m_playBtn);
    tRow->addWidget(m_stopBtn);
    tRow->addWidget(m_speedCombo);
    tRow->addWidget(m_loopCheck);
    tRow->addStretch(1);
    tRow->addWidget(m_timeLabel);
    tLay->addLayout(tRow);
    m_seekSlider = new QSlider(Qt::Horizontal, tBox);
    m_seekSlider->setRange(0, 0);
    tLay->addWidget(m_seekSlider);
    root->addWidget(tBox);

    m_speedCombo->setCurrentIndex(std::max(0, m_speedCombo->findData(readInt(QStringLiteral("SpectrumPlaybackSpeed"), 1))));
    m_loopCheck->setChecked(readBool(QStringLiteral("SpectrumPlaybackLoop"), false));
    m_player->setSpeed(m_speedCombo->currentData().toInt());
    m_player->setLoop(m_loopCheck->isChecked());

    connect(m_playBtn, &QPushButton::clicked, this, [this] {
        if (!m_player->isOpen()) { onPlayClicked(); return; }
        if (m_player->state() == SpectrumPlayer::State::Playing) { m_player->pause(); }
        else { m_player->play(); }
    });
    connect(m_stopBtn, &QPushButton::clicked, this, [this] { stopPlayback(); });
    connect(m_speedCombo, &QComboBox::currentIndexChanged, this, [this] {
        const int x = m_speedCombo->currentData().toInt();
        m_player->setSpeed(x);
        AppSettings::instance().setValue(QStringLiteral("SpectrumPlaybackSpeed"), QString::number(x));
    });
    connect(m_loopCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_player->setLoop(on);
        AppSettings::instance().setValue(QStringLiteral("SpectrumPlaybackLoop"), on ? "True" : "False");
    });
    connect(m_seekSlider, &QSlider::sliderPressed, this, [this] { m_sliderDragging = true; });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this] {
        m_sliderDragging = false;
        m_player->seek(static_cast<qint64>(m_seekSlider->value()) * 100);
    });
    updateTransport();
}

// ─── Coordinator API ─────────────────────────────────────────────────────────

void RecordingPanel::setPanadapters(const QVector<PanInfo>& pans)
{
    const QString prev = selectedPanId();
    m_pans = pans;

    // Keep slots of pans that still exist; stop recorders of vanished pans.
    QHash<QString, int> kept;
    for (auto it = m_panSlot.cbegin(); it != m_panSlot.cend(); ++it) {
        const bool alive = std::any_of(pans.cbegin(), pans.cend(),
                                       [&](const PanInfo& p) { return p.panId == it.key(); });
        if (alive) { kept.insert(it.key(), it.value()); }
        else if (auto* r = m_bank->slot(it.value())) { r->stop(); }
    }
    m_panSlot = kept;
    for (const PanInfo& p : pans) {
        if (m_panSlot.contains(p.panId)) { continue; }
        for (int i = 0; i < SpectrumRecorderBank::kSlots; ++i) {
            const bool used = std::find(m_panSlot.cbegin(), m_panSlot.cend(), i) != m_panSlot.cend();
            if (!used) { m_panSlot.insert(p.panId, i); break; }
        }
    }
    for (const PanInfo& p : pans) {
        if (auto* r = m_bank->slot(slotForPan(p.panId))) {
            r->setStreamIds(p.panStreamId, p.wfStreamId);
        }
    }

    const QSignalBlocker block(m_panCombo);
    m_panCombo->clear();
    for (const PanInfo& p : pans) { m_panCombo->addItem(p.label.isEmpty() ? p.panId : p.label, p.panId); }
    const int idx = m_panCombo->findData(prev);
    m_panCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    updateSpectrumStatus();
}

void RecordingPanel::updatePanState(const QString& panId, const SpecRec::PanState& state)
{
    for (PanInfo& p : m_pans) {
        if (p.panId == panId) { p.state = state; }
    }
    if (auto* r = m_bank->slot(slotForPan(panId)); r && r->isRecording()) {
        r->submitPanState(state);
    }
}

void RecordingPanel::updateSliceMarkers(const QString& panId, const QVector<SpecRec::SliceMarker>& markers)
{
    if (auto* r = m_bank->slot(slotForPan(panId)); r && r->isRecording()) {
        r->submitSliceMarkers(markers);
    }
}

void RecordingPanel::setIqChannelCenter(int channel, qint64 centerHz)
{
    if (channel < 1 || channel > 4) { return; }
    m_iqCenter[channel] = centerHz;
    if (m_iq->isRecording() && m_iq->channel() == channel) {
        m_iq->setCenterFrequency(centerHz);   // rotates to a new, correctly-stamped file
    }
}

void RecordingPanel::setSpectrumResolver(std::function<SpectrumWidget*(const QString&)> fn)
{
    m_resolver = std::move(fn);
}

int RecordingPanel::slotForPan(const QString& panId) const
{
    return m_panSlot.value(panId, -1);
}

QString RecordingPanel::selectedPanId() const
{
    return m_panCombo ? m_panCombo->currentData().toString() : QString();
}

// ─── Spectrum recording ──────────────────────────────────────────────────────

void RecordingPanel::onSpectrumRecordClicked()
{
    const QString panId = selectedPanId();
    SpectrumRecorder* r = m_bank->slot(slotForPan(panId));
    if (!r) {
        QMessageBox::information(this, tr("Spectrum recording"), tr("No panadapter available."));
        return;
    }
    if (r->isRecording()) {
        r->stop();
        updateSpectrumStatus();
        refreshLibrary();
        return;
    }
    const auto it = std::find_if(m_pans.cbegin(), m_pans.cend(),
                                 [&](const PanInfo& p) { return p.panId == panId; });
    if (it == m_pans.cend()) { return; }

    SpectrumRecorder::Options o;
    o.directory       = recordingDirectory();
    o.losslessFft     = readBool(QStringLiteral("SpectrumRecordingLosslessFft"), false);
    o.maxSegmentBytes = static_cast<qint64>(std::max(16, readInt(QStringLiteral("SpectrumRecordingSegmentMB"), 512))) << 20;
    o.minFreeBytes    = minFreeBytes();
    o.metadata.insert(QStringLiteral("panId"), panId);
    o.metadata.insert(QStringLiteral("panLabel"), it->label);
    r->setStreamIds(it->panStreamId, it->wfStreamId);
    QString err;
    if (!r->start(o, it->state, &err)) {
        QMessageBox::warning(this, tr("Spectrum recording"), err);
        return;
    }
    updateSpectrumStatus();
}

void RecordingPanel::updateSpectrumStatus()
{
    SpectrumRecorder* r = m_bank->slot(slotForPan(selectedPanId()));
    const bool rec = r && r->isRecording();
    m_specRecBtn->setText(rec ? tr("Stop") : tr("Record"));
    m_specRecBtn->setEnabled(r != nullptr);
    int active = 0;
    for (int i = 0; i < SpectrumRecorderBank::kSlots; ++i) {
        active += m_bank->slot(i)->isRecording() ? 1 : 0;
    }
    if (rec) {
        m_specStatus->setText(tr("%1 — %2, %3, dropped %4 (%5 pan(s) recording)")
            .arg(QFileInfo(r->firstFilePath()).fileName(), humanBytes(r->bytesWritten()),
                 humanDuration(r->elapsedMs()))
            .arg(r->droppedFrames()).arg(active));
    } else {
        m_specStatus->setText(active > 0 ? tr("%1 pan(s) recording").arg(active) : tr("Idle"));
    }
}

// ─── IQ recording ────────────────────────────────────────────────────────────

void RecordingPanel::onIqRecordClicked()
{
    if (m_iq->isRecording()) {
        m_iq->stop();
        m_iqRecBtn->setText(tr("Record IQ"));
        m_iqStatus->setText(tr("Saved %1").arg(QFileInfo(m_iqCurrentFile).fileName()));
        refreshLibrary();
        return;
    }
    const int ch   = m_iqChannelCombo->currentData().toInt();
    const int rate = m_iqRateCombo->currentData().toInt();
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("IqRecordingChannel"), QString::number(ch));
    s.setValue(QStringLiteral("IqRecordingRate"), QString::number(rate));
    s.setValue(QStringLiteral("IqRecordingFormat"), m_iqFormatCombo->currentData().toString());

    // Worst case 192 kHz float32 stereo ≈ 5.5 GB/h — warn below one hour.
    const qint64 perHour = static_cast<qint64>(rate) * 8 * 3600;
    const QStorageInfo storage(recordingDirectory());
    if (storage.isValid() && storage.bytesAvailable() < minFreeBytes() + perHour / 4) {
        if (QMessageBox::question(this, tr("Low disk space"),
                tr("Only %1 free — about %2 min of IQ at this rate. Record anyway?")
                    .arg(humanBytes(storage.bytesAvailable()))
                    .arg(std::max<qint64>(0, (storage.bytesAvailable() - minFreeBytes()) * 60 / perHour)))
            != QMessageBox::Yes) {
            return;
        }
    }

    emit iqStreamSetupRequested(ch, rate);

    IqRecorder::Options o;
    o.directory    = recordingDirectory();
    o.channel      = ch;
    o.centerHz     = m_iqCenter[ch];
    o.format       = m_iqFormatCombo->currentData().toString() == QLatin1String("Int16")
                         ? IqSampleFormat::Int16 : IqSampleFormat::Float32;
    o.splitBytes   = static_cast<qint64>(std::max(0, readInt(QStringLiteral("IqRecordingSplitMB"), 0))) << 20;
    o.minFreeBytes = minFreeBytes();
    QString err;
    if (!m_iq->start(o, &err)) {
        QMessageBox::warning(this, tr("IQ recording"), err);
        return;
    }
    m_iqRecBtn->setText(tr("Stop IQ"));
    m_iqStatus->setText(tr("Waiting for DAX IQ %1 data…").arg(ch));
}

// ─── Library ─────────────────────────────────────────────────────────────────

void RecordingPanel::refreshDiskSpace()
{
    const QString dir = recordingDirectory();
    m_dirLabel->setText(dir);
    const QStorageInfo storage(QDir(dir).exists() ? dir : QDir::homePath());
    m_diskLabel->setText(storage.isValid()
        ? tr("Free: %1").arg(humanBytes(storage.bytesAvailable())) : QString());
}

void RecordingPanel::refreshLibrary()
{
    const QString sel = selectedPath();
    m_library->clear();
    const QDir dir(recordingDirectory());
    const QFileInfoList files = dir.entryInfoList(
        {QStringLiteral("*.aesr"), QStringLiteral("*.wav")}, QDir::Files, QDir::Time);
    for (const QFileInfo& fi : files) {
        if (isSegmentPart(fi.fileName())) { continue; }
        auto* item = new QTreeWidgetItem(m_library);
        item->setText(0, fi.completeBaseName());
        item->setData(0, kPathRole, fi.absoluteFilePath());
        item->setText(4, fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        if (fi.suffix() == QLatin1String("aesr")) {
            item->setData(0, kKindRole, QStringLiteral("spectrum"));
            item->setText(1, tr("Spectrum"));
            SpecRec::Reader rd;
            if (rd.open(fi.absoluteFilePath())) {
                item->setText(2, humanBytes(rd.totalBytes()));
                item->setText(3, humanDuration(rd.durationMs()));
                if (rd.hadTruncation()) { item->setToolTip(0, tr("Recovered after an unclean stop")); }
            } else {
                item->setText(2, humanBytes(fi.size()));
                item->setText(3, tr("unreadable"));
            }
        } else {
            IqWavInfo info;
            if (!IqWav::parse(fi.absoluteFilePath(), info) || info.channels != 2) {
                delete item;   // not an IQ WAV (e.g. a QSO audio recording)
                continue;
            }
            item->setData(0, kKindRole, QStringLiteral("iq"));
            item->setText(1, tr("IQ %1 kHz").arg(info.sampleRate / 1000));
            item->setText(2, humanBytes(fi.size()));
            item->setText(3, humanDuration(info.sampleRate > 0 ? info.frames * 1000 / info.sampleRate : 0));
            item->setToolTip(0, tr("Centre %1 MHz").arg(info.centerHz / 1e6, 0, 'f', 6));
        }
        if (item->data(0, kPathRole).toString() == sel) { m_library->setCurrentItem(item); }
    }
    for (int c = 1; c < 5; ++c) { m_library->resizeColumnToContents(c); }
}

QString RecordingPanel::selectedPath() const
{
    const QTreeWidgetItem* it = m_library ? m_library->currentItem() : nullptr;
    return it ? it->data(0, kPathRole).toString() : QString();
}

void RecordingPanel::onChooseFolder()
{
    const QString d = QFileDialog::getExistingDirectory(this, tr("Recording folder"), recordingDirectory());
    if (d.isEmpty()) { return; }
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("RecordingDirectory"), d);
    s.save();
    refreshLibrary();
    refreshDiskSpace();
}

void RecordingPanel::onRenameClicked()
{
    const QString path = selectedPath();
    if (path.isEmpty()) { return; }
    const QFileInfo fi(path);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename recording"), tr("New name:"),
                                               QLineEdit::Normal, fi.completeBaseName(), &ok).trimmed();
    if (!ok || name.isEmpty() || name == fi.completeBaseName()) { return; }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        QMessageBox::warning(this, tr("Rename"), tr("Name must not contain path separators."));
        return;
    }
    if (m_player->path() == path) { stopPlayback(); }
    const QString newFirst = fi.dir().filePath(name + QLatin1Char('.') + fi.suffix());
    QStringList from{path};
    if (fi.suffix() == QLatin1String("aesr")) { from = SpecRec::segmentPaths(path); }
    for (int i = 0; i < from.size(); ++i) {
        const QString to = (fi.suffix() == QLatin1String("aesr")) ? SpecRec::segmentPath(newFirst, i) : newFirst;
        if (QFileInfo::exists(to) || !QFile::rename(from[i], to)) {
            QMessageBox::warning(this, tr("Rename"), tr("Could not rename %1").arg(QFileInfo(from[i]).fileName()));
            break;
        }
    }
    refreshLibrary();
}

void RecordingPanel::onDeleteClicked()
{
    const QString path = selectedPath();
    if (path.isEmpty()) { return; }
    if (QMessageBox::question(this, tr("Delete recording"),
            tr("Move \"%1\" to the trash?").arg(QFileInfo(path).fileName())) != QMessageBox::Yes) {
        return;
    }
    if (m_player->path() == path) { stopPlayback(); }
    QStringList files{path};
    if (path.endsWith(QLatin1String(".aesr"))) { files = SpecRec::segmentPaths(path); }
    for (const QString& f : files) {
        if (!QFile::moveToTrash(f)) {
            QMessageBox::warning(this, tr("Delete"), tr("Could not move %1 to the trash.").arg(QFileInfo(f).fileName()));
            break;
        }
    }
    refreshLibrary();
    refreshDiskSpace();
}

// ─── Playback ────────────────────────────────────────────────────────────────

void RecordingPanel::onPlayClicked()
{
    const QTreeWidgetItem* it = m_library->currentItem();
    if (!it) { return; }
    const QString path = it->data(0, kPathRole).toString();
    if (it->data(0, kKindRole).toString() == QLatin1String("iq")) {
        playIqInWaterfall(path);
    } else {
        startPlayback(path);
    }
}

bool RecordingPanel::startPlayback(const QString& aesrPath)
{
    SpectrumWidget* w = m_resolver ? m_resolver(selectedPanId()) : nullptr;
    if (!w) {
        QMessageBox::information(this, tr("Playback"),
                                 tr("Select a panadapter to display the recording in."));
        return false;
    }
    m_player->detach();
    QString err;
    if (!m_player->open(aesrPath, &err)) {
        QMessageBox::warning(this, tr("Playback"), err);
        return false;
    }
    m_player->attach(w);
    m_player->play();
    updateTransport();
    return true;
}

void RecordingPanel::stopPlayback()
{
    if (m_player) {
        m_player->detach();   // widget returns to the live feed
        m_player->close();
    }
    if (!m_tempAesr.isEmpty()) {
        QFile::remove(m_tempAesr);   // our own temporary conversion output
        m_tempAesr.clear();
    }
    if (m_seekSlider) { updateTransport(); }
}

void RecordingPanel::playIqInWaterfall(const QString& wavPath)
{
    const QString out = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QFileInfo(wavPath).completeBaseName() + QStringLiteral(".iqview.aesr"));

    auto progress = std::make_shared<std::atomic<int>>(0);
    auto cancel   = std::make_shared<std::atomic<bool>>(false);
    auto ok       = std::make_shared<std::atomic<bool>>(false);
    auto err      = std::make_shared<QString>();

    QThread* th = QThread::create([=] {
        IqSpectrumConverter::Options o;
        *ok = IqSpectrumConverter::convert(wavPath, out, o, [=](double p) {
            progress->store(static_cast<int>(p * 1000));
            return !cancel->load();
        }, err.get());
    });
    auto* dlg = new QProgressDialog(tr("Computing spectrum from IQ…"), tr("Cancel"), 0, 1000, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setMinimumDuration(300);
    auto* poll = new QTimer(dlg);
    connect(poll, &QTimer::timeout, dlg, [dlg, progress] { dlg->setValue(progress->load()); });
    connect(dlg, &QProgressDialog::canceled, dlg, [cancel] { cancel->store(true); });
    connect(th, &QThread::finished, this, [=, this] {
        th->deleteLater();
        dlg->close();
        if (!*ok) {
            if (!cancel->load()) { QMessageBox::warning(this, tr("IQ playback"), *err); }
            return;
        }
        stopPlayback();
        if (startPlayback(out)) { m_tempAesr = out; }
        else { QFile::remove(out); }
    });
    poll->start(100);
    th->start();
}

void RecordingPanel::updateTransport()
{
    const bool open = m_player->isOpen();
    const qint64 dur = open ? m_player->durationMs() : 0;
    const qint64 pos = open ? m_player->positionMs() : 0;
    m_playBtn->setText(m_player->state() == SpectrumPlayer::State::Playing ? tr("Pause") : tr("Play"));
    m_stopBtn->setEnabled(open);
    m_seekSlider->setEnabled(open);
    m_seekSlider->setRange(0, static_cast<int>(dur / 100));
    if (!m_sliderDragging) {
        const QSignalBlocker b(m_seekSlider);
        m_seekSlider->setValue(static_cast<int>(pos / 100));
    }
    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(humanDuration(pos), humanDuration(dur)));
}

// ─── PNG export ──────────────────────────────────────────────────────────────

void RecordingPanel::onExportClicked()
{
    const QTreeWidgetItem* it = m_library->currentItem();
    if (!it || it->data(0, kKindRole).toString() != QLatin1String("spectrum")) {
        QMessageBox::information(this, tr("Export PNG"),
                                 tr("Select a spectrum recording (play IQ files in the waterfall first)."));
        return;
    }
    const QString path = it->data(0, kPathRole).toString();
    SpecRec::Reader rd;
    if (!rd.open(path)) { return; }
    const int durS = static_cast<int>(rd.durationMs() / 1000);
    bool ok = false;
    const int from = QInputDialog::getInt(this, tr("Export PNG"), tr("From (seconds):"), 0, 0, durS, 1, &ok);
    if (!ok) { return; }
    const int to = QInputDialog::getInt(this, tr("Export PNG"), tr("To (seconds):"), durS, from, durS, 1, &ok);
    if (!ok) { return; }
    const QString png = QFileDialog::getSaveFileName(this, tr("Export PNG"),
        QFileInfo(path).dir().filePath(QFileInfo(path).completeBaseName() + QStringLiteral(".png")),
        tr("PNG images (*.png)"));
    if (png.isEmpty()) { return; }
    QString err;
    SpectrumWidget* w = m_resolver ? m_resolver(selectedPanId()) : nullptr;
    if (!exportWaterfallPng(path, png, qint64(from) * 1000, qint64(to) * 1000,
                            w ? w->wfColorScheme() : 0, &err)) {
        QMessageBox::warning(this, tr("Export PNG"), err);
    }
}

bool RecordingPanel::exportWaterfallPng(const QString& aesrPath, const QString& pngPath,
                                        qint64 fromMs, qint64 toMs, int colorScheme, QString* err)
{
    constexpr int kMaxWidth  = 4096;
    constexpr int kMaxHeight = 8000;
    SpecRec::Reader rd;
    if (!rd.open(aesrPath, err)) { return false; }
    const qint64 fromUs = rd.startUs() + fromMs * 1000;
    const qint64 toUs   = rd.startUs() + toMs * 1000;

    // Pass 1: does the range have native waterfall rows? What's the span/width?
    bool haveRows = false;
    int width = 0;
    SpecRec::PanState pan;
    bool havePan = false;
    rd.forEach(fromUs, toUs, [&](const SpecRec::Record& r) {
        if (r.type == SpecRec::RecordType::PanState && !havePan) { pan = r.pan; havePan = true; }
        if (r.type == SpecRec::RecordType::WaterfallRow) { haveRows = true; width = std::max(width, int(r.bins.size())); }
        if (r.type == SpecRec::RecordType::FftFrame && !haveRows) { width = std::max(width, int(r.bins.size())); }
        return true;
    });
    if (!havePan || width == 0 || pan.bandwidthMhz <= 0.0) {
        if (err) { *err = QObject::tr("No spectrum data in that range"); }
        return false;
    }
    width = std::min(width, kMaxWidth);
    const SpecRec::RecordType want = haveRows ? SpecRec::RecordType::WaterfallRow
                                              : SpecRec::RecordType::FftFrame;
    const double panLow = pan.centerMhz - pan.bandwidthMhz / 2.0;

    // Pass 2: resample each row onto the pan's frequency axis.
    std::vector<std::vector<float>> rows;
    SpecRec::PanState cur = pan;
    rd.forEach(fromUs, toUs, [&](const SpecRec::Record& r) {
        if (r.type == SpecRec::RecordType::PanState) { cur = r.pan; }
        if (r.type != want || r.bins.isEmpty()) { return true; }
        const double lo = haveRows ? r.lowMhz : cur.centerMhz - cur.bandwidthMhz / 2.0;
        const double hi = haveRows ? r.highMhz : cur.centerMhz + cur.bandwidthMhz / 2.0;
        const int n = int(r.bins.size());
        std::vector<float> row(size_t(width), std::numeric_limits<float>::quiet_NaN());
        for (int x = 0; x < width; ++x) {
            const double f = panLow + (x + 0.5) / width * pan.bandwidthMhz;
            const int b = int((f - lo) / (hi - lo) * n);
            if (b >= 0 && b < n) { row[size_t(x)] = r.bins[b]; }
        }
        rows.push_back(std::move(row));
        return rows.size() < 200000;
    });
    if (rows.empty()) {
        if (err) { *err = QObject::tr("No spectrum data in that range"); }
        return false;
    }

    // Robust contrast: 5th percentile → black, 99.8th → full scale.
    std::vector<float> sample;
    const size_t step = std::max<size_t>(1, rows.size() * size_t(width) / 200000);
    size_t k = 0;
    for (const auto& row : rows) {
        for (float v : row) {
            if ((k++ % step) == 0 && !std::isnan(v)) { sample.push_back(v); }
        }
    }
    if (sample.empty()) {
        if (err) { *err = QObject::tr("No spectrum data in that range"); }
        return false;
    }
    std::sort(sample.begin(), sample.end());
    const float lo = sample[sample.size() * 5 / 100];
    const float hi = std::max(lo + 1.0f, sample[std::min(sample.size() - 1, sample.size() * 998 / 1000)]);

    // Newest at the top, like the live waterfall. Decimate by max-hold.
    const int height = std::min<int>(int(rows.size()), kMaxHeight);
    const double perPixel = double(rows.size()) / height;
    int nStops = 0;
    const WfGradientStop* stops = wfSchemeStops(
        static_cast<WfColorScheme>(std::clamp(colorScheme, 0, int(WfColorScheme::Count) - 1)), nStops);
    QImage img(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        const int r0 = int(rows.size()) - 1 - int((y + 1) * perPixel - 1);
        const int r1 = int(rows.size()) - 1 - int(y * perPixel);
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < width; ++x) {
            float v = -std::numeric_limits<float>::infinity();
            for (int r = std::max(0, r0); r <= r1; ++r) {
                const float s = rows[size_t(r)][size_t(x)];
                if (!std::isnan(s)) { v = std::max(v, s); }
            }
            line[x] = std::isinf(v) ? qRgb(0, 0, 0) : schemeColor(stops, nStops, (v - lo) / (hi - lo));
        }
    }
    if (!img.save(pngPath, "PNG")) {
        if (err) { *err = QObject::tr("Could not write %1").arg(pngPath); }
        return false;
    }
    return true;
}

} // namespace AetherSDR
