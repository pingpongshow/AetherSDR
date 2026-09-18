#include "LogbookDialog.h"

#include "core/AppSettings.h"
#include "models/SliceModel.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kDisplayLimit = 20000;     // rows shown in the table (newest first)
constexpr int kReloadDebounceMs = 150;

const QStringList& bandList()
{
    static const QStringList b = {"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m",
                                  "12m", "10m", "6m", "4m", "2m", "70cm"};
    return b;
}

QString uploadLetters(const LogQso& q)
{
    // L Q C E — upper case = uploaded, lower = pending, '!' = failed, '·' none
    static const char kLetters[] = {'L', 'Q', 'C', 'E'};
    QString out;
    for (int i = 0; i < kUploadServiceCount; ++i) {
        switch (q.upload[i]) {
        case UploadState::Uploaded:    out += QChar(kLetters[i]); break;
        case UploadState::Pending:
        case UploadState::Modified:    out += QChar(kLetters[i]).toLower(); break;
        case UploadState::Failed:      out += QChar('!'); break;
        case UploadState::DoNotUpload:
        case UploadState::NotSent:     out += QChar(0x00B7); break;
        }
    }
    return out;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// LogTableModel
// ═══════════════════════════════════════════════════════════════════════════

class LogTableModel : public QAbstractTableModel {
public:
    enum Col { Time, Call, Band, Freq, Mode, RstS, RstR, Name, Country, Grid, Exch,
               Slice, Contest, Uploads, Rec, Comment, ColCount };

    explicit LogTableModel(QObject* parent) : QAbstractTableModel(parent) {}

    void setRows(QVector<LogQso> rows)
    {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }
    const LogQso& row(int r) const { return m_rows.at(r); }

    int rowCount(const QModelIndex& p = {}) const override { return p.isValid() ? 0 : static_cast<int>(m_rows.size()); }
    int columnCount(const QModelIndex& p = {}) const override { return p.isValid() ? 0 : ColCount; }

    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (o != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        static const char* const kHeaders[ColCount] = {
            "UTC", "Call", "Band", "MHz", "Mode", "Sent", "Rcvd", "Name", "Country", "Grid",
            "Exchange", "Slice", "Contest", "LQCE", "Rec", "Comment"};
        return QString::fromLatin1(kHeaders[section]);
    }

    QVariant data(const QModelIndex& idx, int role) const override
    {
        if (!idx.isValid() || idx.row() >= m_rows.size()) {
            return {};
        }
        const LogQso& q = m_rows.at(idx.row());
        if (role == Qt::UserRole) {   // sort key
            switch (idx.column()) {
            case Time: return q.timeOn.toSecsSinceEpoch();
            case Freq: return q.freqMhz;
            case Slice: return q.sliceId;
            default: return data(idx, Qt::DisplayRole);
            }
        }
        if (role == Qt::ToolTipRole) {
            if (idx.column() == Rec && !q.recordingPath.isEmpty()) {
                return q.recordingPath;
            }
            if (idx.column() == Uploads) {
                return QStringLiteral("L=LoTW Q=QRZ C=Club Log E=eQSL\n"
                                      "Upper case: uploaded · lower: queued · !: failed");
            }
            return {};
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (idx.column()) {
        case Time:    return q.timeOn.toUTC().toString("yyyy-MM-dd HH:mm");
        case Call:    return q.call;
        case Band:    return q.band;
        case Freq:    return q.freqMhz > 0 ? QString::number(q.freqMhz, 'f', 4) : QString();
        case Mode:    return q.submode.isEmpty() ? q.mode : q.submode;
        case RstS:    return q.rstSent;
        case RstR:    return q.rstRcvd;
        case Name:    return q.name;
        case Country: return q.country;
        case Grid:    return q.gridsquare;
        case Exch:    return q.srxString;
        case Slice:   return q.sliceId >= 0
                          ? QString(QChar('A' + q.sliceId)) + (q.so2rRole.isEmpty() ? QString()
                                                                                    : ' ' + q.so2rRole)
                          : QString();
        case Contest: return q.contestId;
        case Uploads: return uploadLetters(q);
        case Rec:     return q.recordingPath.isEmpty() ? QString() : QStringLiteral("♪");
        case Comment: return q.comment;
        default:      return {};
        }
    }

private:
    QVector<LogQso> m_rows;
};

// ═══════════════════════════════════════════════════════════════════════════
// QSO edit dialog
// ═══════════════════════════════════════════════════════════════════════════

namespace {

class QsoEditDialog : public QDialog {
public:
    QsoEditDialog(const LogQso& q, QWidget* parent) : QDialog(parent), m_q(q)
    {
        setWindowTitle(tr("Edit QSO — %1").arg(q.call));
        auto* form = new QFormLayout;
        auto line = [&](const QString& label, const QString& v) {
            auto* e = new QLineEdit(v, this);
            form->addRow(label, e);
            return e;
        };
        m_call = line(tr("Call"), q.call);
        m_timeOn = new QDateTimeEdit(q.timeOn.toUTC(), this);
        m_timeOn->setTimeZone(QTimeZone::UTC);
        m_timeOn->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
        form->addRow(tr("Time on"), m_timeOn);
        m_freq = new QDoubleSpinBox(this);
        m_freq->setDecimals(6);
        m_freq->setRange(0, 300000);
        m_freq->setValue(q.freqMhz);
        form->addRow(tr("Freq (MHz)"), m_freq);
        m_mode = line(tr("Mode"), q.mode);
        m_submode = line(tr("Submode"), q.submode);
        m_rstS = line(tr("RST sent"), q.rstSent);
        m_rstR = line(tr("RST rcvd"), q.rstRcvd);
        m_name = line(tr("Name"), q.name);
        m_qth = line(tr("QTH"), q.qth);
        m_grid = line(tr("Grid"), q.gridsquare);
        m_state = line(tr("State"), q.state);
        m_comment = line(tr("Comment"), q.comment);
        m_contest = line(tr("Contest ID"), q.contestId);
        m_srx = line(tr("Exchange rcvd"), q.srxString);
        m_stx = line(tr("Exchange sent"), q.stxString);
        m_power = new QDoubleSpinBox(this);
        m_power->setRange(0, 5000);
        m_power->setValue(q.txPowerW);
        form->addRow(tr("TX power (W)"), m_power);
        m_rec = line(tr("Recording"), q.recordingPath);

        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto* v = new QVBoxLayout(this);
        v->addLayout(form);
        v->addWidget(bb);
    }

    LogQso result() const
    {
        LogQso q = m_q;
        q.call = m_call->text().trimmed().toUpper();
        q.timeOn = m_timeOn->dateTime().toUTC();
        if (std::abs(m_freq->value() - q.freqMhz) > 1e-9) {
            q.freqMhz = m_freq->value();
            q.band = LogUtil::freqToBand(q.freqMhz);
        }
        q.mode = m_mode->text().trimmed().toUpper();
        q.submode = m_submode->text().trimmed().toUpper();
        q.rstSent = m_rstS->text().trimmed();
        q.rstRcvd = m_rstR->text().trimmed();
        q.name = m_name->text().trimmed();
        q.qth = m_qth->text().trimmed();
        q.gridsquare = m_grid->text().trimmed().toUpper();
        q.state = m_state->text().trimmed().toUpper();
        q.comment = m_comment->text();
        q.contestId = m_contest->text().trimmed();
        q.srxString = m_srx->text().trimmed();
        q.stxString = m_stx->text().trimmed();
        q.txPowerW = m_power->value();
        q.recordingPath = m_rec->text().trimmed();
        return q;
    }

private:
    LogQso m_q;
    QLineEdit* m_call{nullptr};
    QDateTimeEdit* m_timeOn{nullptr};
    QDoubleSpinBox* m_freq{nullptr};
    QLineEdit* m_mode{nullptr};
    QLineEdit* m_submode{nullptr};
    QLineEdit* m_rstS{nullptr};
    QLineEdit* m_rstR{nullptr};
    QLineEdit* m_name{nullptr};
    QLineEdit* m_qth{nullptr};
    QLineEdit* m_grid{nullptr};
    QLineEdit* m_state{nullptr};
    QLineEdit* m_comment{nullptr};
    QLineEdit* m_contest{nullptr};
    QLineEdit* m_srx{nullptr};
    QLineEdit* m_stx{nullptr};
    QDoubleSpinBox* m_power{nullptr};
    QLineEdit* m_rec{nullptr};
};

// Log / upload settings.  Secrets typed here go to CredentialStore; an
// empty secret field means "unchanged".
class LogSettingsDialog : public QDialog {
public:
    LogSettingsDialog(CredentialStore* creds, QWidget* parent) : QDialog(parent), m_creds(creds)
    {
        setWindowTitle(tr("Logbook Settings"));
        auto& s = AppSettings::instance();
        auto* v = new QVBoxLayout(this);

        auto* station = new QGroupBox(tr("Station"), this);
        auto* sf = new QFormLayout(station);
        m_stationCall = new QLineEdit(s.value("LogbookStationCallsign", QString()).toString(), this);
        m_stationCall->setPlaceholderText(tr("defaults to radio callsign"));
        m_operator = new QLineEdit(s.value("LogbookOperator", QString()).toString(), this);
        m_power = new QLineEdit(s.value("LogbookTxPowerDefault", QString()).toString(), this);
        m_digi = new QComboBox(this);
        m_digi->addItems({"FT8", "FT4", "JS8", "RTTY", "PSK31", "PSK63", "OLIVIA"});
        m_digi->setCurrentText(s.value("LogbookDigiModeDefault", "FT8").toString());
        sf->addRow(tr("Station callsign"), m_stationCall);
        sf->addRow(tr("Operator"), m_operator);
        sf->addRow(tr("Default TX power (W)"), m_power);
        sf->addRow(tr("DIGU/DIGL logs as"), m_digi);
        v->addWidget(station);

        auto* up = new QGroupBox(tr("Online logs"), this);
        auto* uf = new QFormLayout(up);
        auto check = [&](const char* key) {
            auto* c = new QCheckBox(this);
            c->setChecked(s.value(key, "False").toString() == "True");
            return c;
        };
        auto secret = [&](const QString& placeholder) {
            auto* e = new QLineEdit(this);
            e->setEchoMode(QLineEdit::Password);
            e->setPlaceholderText(placeholder);
            return e;
        };
        m_auto = check("LogUploadAuto");
        uf->addRow(tr("Upload new QSOs automatically"), m_auto);

        m_lotw = check("LogUploadLotwEnabled");
        const QString detected = UploadApi::detectTqsl();
        m_tqslPath = new QLineEdit(s.value("LogUploadTqslPath", detected).toString(), this);
        m_tqslPath->setPlaceholderText(detected.isEmpty() ? tr("TQSL not found") : detected);
        m_tqslLoc = new QLineEdit(s.value("LogUploadTqslStationLocation", QString()).toString(), this);
        uf->addRow(tr("LoTW (TQSL)"), m_lotw);
        uf->addRow(tr("  TQSL path"), m_tqslPath);
        uf->addRow(tr("  Station location"), m_tqslLoc);
        auto* tqslNote = new QLabel(tr("The TQSL certificate password is never stored by "
                                       "AetherSDR; TQSL prompts for it if needed."), this);
        tqslNote->setWordWrap(true);
        uf->addRow(QString(), tqslNote);

        m_qrz = check("LogUploadQrzEnabled");
        m_qrzKey = secret(tr("unchanged"));
        uf->addRow(tr("QRZ.com Logbook"), m_qrz);
        uf->addRow(tr("  API key"), m_qrzKey);

        m_cl = check("LogUploadClubLogEnabled");
        m_clEmail = new QLineEdit(s.value("LogUploadClubLogEmail", QString()).toString(), this);
        m_clCall = new QLineEdit(s.value("LogUploadClubLogCallsign", QString()).toString(), this);
        m_clPw = secret(tr("application password (unchanged)"));
        m_clApi = secret(tr("unchanged"));
        uf->addRow(tr("Club Log"), m_cl);
        uf->addRow(tr("  Email"), m_clEmail);
        uf->addRow(tr("  Callsign"), m_clCall);
        uf->addRow(tr("  App password"), m_clPw);
        uf->addRow(tr("  API key"), m_clApi);

        m_eq = check("LogUploadEqslEnabled");
        m_eqUser = new QLineEdit(s.value("LogUploadEqslUser", QString()).toString(), this);
        m_eqPw = secret(tr("unchanged"));
        uf->addRow(tr("eQSL"), m_eq);
        uf->addRow(tr("  User"), m_eqUser);
        uf->addRow(tr("  Password"), m_eqPw);
        v->addWidget(up);

        if (m_creds && !m_creds->isSecure()) {
            auto* warn = new QLabel(CredentialStore::insecureWarning(), this);
            warn->setWordWrap(true);
            warn->setStyleSheet("color: #e0a000;");
            v->addWidget(warn);
        }
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        connect(bb, &QDialogButtonBox::accepted, this, [this]() { save(); accept(); });
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        v->addWidget(bb);
    }

private:
    void save()
    {
        auto& s = AppSettings::instance();
        auto b = [](QCheckBox* c) { return c->isChecked() ? QStringLiteral("True") : QStringLiteral("False"); };
        s.setValue("LogbookStationCallsign", m_stationCall->text().trimmed().toUpper());
        s.setValue("LogbookOperator", m_operator->text().trimmed().toUpper());
        s.setValue("LogbookTxPowerDefault", m_power->text().trimmed());
        s.setValue("LogbookDigiModeDefault", m_digi->currentText());
        s.setValue("LogUploadAuto", b(m_auto));
        s.setValue("LogUploadLotwEnabled", b(m_lotw));
        s.setValue("LogUploadTqslPath", m_tqslPath->text().trimmed());
        s.setValue("LogUploadTqslStationLocation", m_tqslLoc->text());
        s.setValue("LogUploadQrzEnabled", b(m_qrz));
        s.setValue("LogUploadClubLogEnabled", b(m_cl));
        s.setValue("LogUploadClubLogEmail", m_clEmail->text().trimmed());
        s.setValue("LogUploadClubLogCallsign", m_clCall->text().trimmed().toUpper());
        s.setValue("LogUploadEqslEnabled", b(m_eq));
        s.setValue("LogUploadEqslUser", m_eqUser->text().trimmed());
        s.save();
        if (!m_creds) {
            return;
        }
        auto put = [this](QLineEdit* e, const char* key) {
            if (!e->text().isEmpty()) {
                m_creds->write(QString::fromLatin1(key), e->text());
            }
        };
        put(m_qrzKey, CredentialStore::kQrzApiKey);
        put(m_clPw, CredentialStore::kClubLogPassword);
        put(m_clApi, CredentialStore::kClubLogApiKey);
        put(m_eqPw, CredentialStore::kEqslPassword);
    }

    QPointer<CredentialStore> m_creds;
    QLineEdit* m_stationCall{nullptr};
    QLineEdit* m_operator{nullptr};
    QLineEdit* m_power{nullptr};
    QComboBox* m_digi{nullptr};
    QCheckBox* m_auto{nullptr};
    QCheckBox* m_lotw{nullptr};
    QLineEdit* m_tqslPath{nullptr};
    QLineEdit* m_tqslLoc{nullptr};
    QCheckBox* m_qrz{nullptr};
    QLineEdit* m_qrzKey{nullptr};
    QCheckBox* m_cl{nullptr};
    QLineEdit* m_clEmail{nullptr};
    QLineEdit* m_clCall{nullptr};
    QLineEdit* m_clPw{nullptr};
    QLineEdit* m_clApi{nullptr};
    QCheckBox* m_eq{nullptr};
    QLineEdit* m_eqUser{nullptr};
    QLineEdit* m_eqPw{nullptr};
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// LogbookDialog
// ═══════════════════════════════════════════════════════════════════════════

LogbookDialog::LogbookDialog(Logbook* log, ContestEngine* contest, LogUploadQueue* uploads,
                             CredentialStore* creds, QWidget* parent)
    : QDialog(parent)
    , m_log(log)
    , m_contest(contest)
    , m_uploads(uploads)
    , m_creds(creds)
{
    setWindowTitle(tr("Logbook"));
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setModal(false);
    buildUi();

    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(kReloadDebounceMs);
    connect(&m_reloadTimer, &QTimer::timeout, this, &LogbookDialog::reloadTable);

    if (m_log) {
        connect(m_log, &Logbook::qsoAdded, this, [this](const LogQso&) { scheduleReload(); updatePreview(); });
        connect(m_log, &Logbook::qsoUpdated, this, [this](const LogQso&) { scheduleReload(); });
        connect(m_log, &Logbook::qsoDeleted, this, [this](qint64) { scheduleReload(); });
        connect(m_log, &Logbook::logReset, this, &LogbookDialog::scheduleReload);
        connect(m_log, &Logbook::importProgress, this, [this](qint64 done, qint64 total, qint64 added) {
            m_importProgress->setVisible(true);
            m_importProgress->setRange(0, 1000);
            m_importProgress->setValue(total > 0 ? static_cast<int>(done * 1000 / total) : 0);
            m_status->setText(tr("Importing… %1 QSOs added").arg(added));
        });
        connect(m_log, &Logbook::importFinished, this, [this](const ImportResult& r) {
            m_importProgress->setVisible(false);
            QString msg = tr("Import finished: %1 added, %2 duplicates skipped, %3 errors")
                              .arg(r.added).arg(r.skippedDupes).arg(r.errors);
            if (!r.errorString.isEmpty()) {
                msg += QStringLiteral(" (") + r.errorString + ')';
            }
            m_status->setText(msg);
        });
    }
    if (m_contest) {
        connect(m_contest, &ContestEngine::scoreChanged, this, &LogbookDialog::updateScore);
        connect(m_contest, &ContestEngine::profileChanged, this, &LogbookDialog::updateContestUi);
        connect(m_contest, &ContestEngine::macroRefused, this, [this](const QString& why) {
            m_status->setText(tr("Macro not sent: %1").arg(why));
        });
        connect(m_contest, &ContestEngine::macroSent, this, [this](int fkey, const QString& text) {
            m_status->setText(tr("F%1: %2").arg(fkey).arg(text));
        });
    }
    if (m_uploads) {
        connect(m_uploads, &LogUploadQueue::uploadFinished, this,
                [this](qint64, UploadService svc, bool ok, const QString& msg) {
            if (!ok) {
                m_status->setText(tr("%1 upload failed: %2").arg(LogUtil::uploadServiceName(svc), msg));
            }
        });
        connect(m_uploads, &LogUploadQueue::serviceSuspended, this,
                [this](UploadService svc, const QString& reason) {
            QMessageBox::warning(this, tr("Upload stopped"),
                                 tr("%1 uploads are paused: %2\n\nFix the settings and "
                                    "save them to resume.").arg(LogUtil::uploadServiceName(svc), reason));
        });
    }

    const QByteArray geo = QByteArray::fromBase64(
        AppSettings::instance().value("LogbookDialogGeometry", QString()).toString().toLatin1());
    if (!geo.isEmpty()) {
        restoreGeometry(geo);
    } else {
        resize(1100, 700);
    }
    updateContestUi();
    reloadTable();
}

LogbookDialog::~LogbookDialog() = default;

void LogbookDialog::closeEvent(QCloseEvent* e)
{
    auto& s = AppSettings::instance();
    s.setValue("LogbookDialogGeometry", QString::fromLatin1(saveGeometry().toBase64()));
    s.save();
    QDialog::closeEvent(e);
}

void LogbookDialog::buildUi()
{
    auto* v = new QVBoxLayout(this);
    v->addWidget(buildQuickEntry());
    v->addWidget(buildContestPanel());
    v->addWidget(buildTablePanel(), 1);

    auto* statusRow = new QHBoxLayout;
    m_status = new QLabel(this);
    m_importProgress = new QProgressBar(this);
    m_importProgress->setVisible(false);
    m_importProgress->setMaximumWidth(220);
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_importProgress);
    v->addLayout(statusRow);

    // F1–F8 macro keys (active while the log window has focus)
    for (int f = 1; f <= ContestEngine::kMacroCount; ++f) {
        auto* sc = new QShortcut(QKeySequence(Qt::Key_F1 + (f - 1)), this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, [this, f]() { sendMacro(f); });
    }
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &LogbookDialog::clearEntry);
}

QWidget* LogbookDialog::buildQuickEntry()
{
    auto* box = new QGroupBox(tr("Quick entry"), this);
    auto* v = new QVBoxLayout(box);
    auto* row = new QHBoxLayout;

    auto field = [&](const QString& label, int width, const QString& placeholder = {}) {
        auto* col = new QVBoxLayout;
        auto* l = new QLabel(label, box);
        auto* e = new QLineEdit(box);
        e->setPlaceholderText(placeholder);
        if (width > 0) {
            e->setMaximumWidth(width);
        }
        col->addWidget(l);
        col->addWidget(e);
        row->addLayout(col);
        connect(e, &QLineEdit::returnPressed, this, &LogbookDialog::logCurrentEntry);
        return e;
    };
    m_call = field(tr("Call"), 140);
    m_call->setMinimumWidth(110);
    connect(m_call, &QLineEdit::textEdited, this, [this](const QString& t) {
        const int pos = m_call->cursorPosition();
        m_call->setText(t.toUpper());
        m_call->setCursorPosition(pos);
        if (!t.isEmpty() && !m_timeOn.isValid()) {
            m_timeOn = QDateTime::currentDateTimeUtc();   // N1MM-style: time of first keystroke
        }
        updatePreview();
    });
    m_rstS = field(tr("RST S"), 50);
    m_rstR = field(tr("RST R"), 50);
    m_exchange = field(tr("Exchange"), 170);
    m_exchangeLabel = qobject_cast<QLabel*>(row->itemAt(row->count() - 1)->layout()->itemAt(0)->widget());
    connect(m_exchange, &QLineEdit::textEdited, this, &LogbookDialog::updatePreview);
    m_name = field(tr("Name"), 120);
    m_grid = field(tr("Grid"), 70);
    connect(m_grid, &QLineEdit::textEdited, this, &LogbookDialog::updatePreview);
    m_comment = field(tr("Comment"), 0);

    auto* freqCol = new QVBoxLayout;
    auto* freqRow = new QHBoxLayout;
    m_freq = new QLineEdit(box);
    m_freq->setMaximumWidth(100);
    m_freq->setPlaceholderText(tr("MHz"));
    m_mode = new QComboBox(box);
    m_mode->setEditable(true);
    m_mode->addItems({"USB", "LSB", "CW", "AM", "FM", "DIGU", "DIGL", "RTTY", "FDV"});
    m_followSlice = new QCheckBox(tr("Slice"), box);
    m_followSlice->setChecked(true);
    m_followSlice->setToolTip(tr("Take frequency and mode from the active slice"));
    connect(m_followSlice, &QCheckBox::toggled, this, [this](bool on) {
        m_freq->setReadOnly(on);
        m_mode->setEnabled(!on);
        updateSliceInfo();
    });
    m_freq->setReadOnly(true);
    m_mode->setEnabled(false);
    freqRow->addWidget(m_freq);
    freqRow->addWidget(m_mode);
    freqRow->addWidget(m_followSlice);
    freqCol->addWidget(new QLabel(tr("Frequency / mode"), box));
    freqCol->addLayout(freqRow);
    row->addLayout(freqCol);

    auto* logBtn = new QPushButton(tr("Log"), box);
    logBtn->setDefault(false);
    logBtn->setAutoDefault(false);
    connect(logBtn, &QPushButton::clicked, this, &LogbookDialog::logCurrentEntry);
    auto* clrBtn = new QPushButton(tr("Clear"), box);
    clrBtn->setAutoDefault(false);
    connect(clrBtn, &QPushButton::clicked, this, &LogbookDialog::clearEntry);
    auto* btnCol = new QVBoxLayout;
    btnCol->addWidget(logBtn);
    btnCol->addWidget(clrBtn);
    row->addLayout(btnCol);
    v->addLayout(row);

    auto* info = new QHBoxLayout;
    m_preview = new QLabel(box);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_dupe = new QLabel(box);
    m_sentLabel = new QLabel(box);
    info->addWidget(m_preview, 1);
    info->addWidget(m_dupe);
    info->addWidget(m_sentLabel);
    v->addLayout(info);
    return box;
}

QWidget* LogbookDialog::buildContestPanel()
{
    m_contestBox = new QGroupBox(tr("Contest"), this);
    auto* v = new QVBoxLayout(m_contestBox);
    auto* row = new QHBoxLayout;

    m_profile = new QComboBox(m_contestBox);
    for (const ContestProfile& p : ContestEngine::builtinProfiles()) {
        m_profile->addItem(p.name, p.key);
    }
    m_modeVariant = new QComboBox(m_contestBox);
    m_modeVariant->addItems({"CW", "SSB", "RTTY"});
    m_myExchange = new QLineEdit(m_contestBox);
    m_myExchange->setPlaceholderText(tr("My exchange, e.g. ZONE=5 STATE=CT NAME=BOB CLASS=2A SECT=CT PWR=100"));
    m_customTemplate = new QLineEdit(m_contestBox);
    m_customTemplate->setPlaceholderText(tr("Custom: RST SN *STATE (* = mult)"));
    m_customTemplate->setText(AppSettings::instance().value("ContestCustomTemplate", "RST SN").toString());
    auto* newContest = new QPushButton(tr("Start now"), m_contestBox);
    newContest->setAutoDefault(false);
    newContest->setToolTip(tr("Only QSOs from now on count for dupes/score"));
    row->addWidget(new QLabel(tr("Profile"), m_contestBox));
    row->addWidget(m_profile);
    row->addWidget(m_modeVariant);
    row->addWidget(m_myExchange, 1);
    row->addWidget(m_customTemplate);
    row->addWidget(newContest);
    v->addLayout(row);

    auto apply = [this]() {
        if (!m_contest) {
            return;
        }
        const QString key = m_profile->currentData().toString();
        ContestProfile p = ContestEngine::profileFor(ContestType::None);
        for (const ContestProfile& c : ContestEngine::builtinProfiles()) {
            if (c.key == key) {
                p = c;
            }
        }
        if (p.type == ContestType::Custom) {
            p = ContestEngine::customProfile(m_customTemplate->text(),
                                             AppSettings::instance().value("ContestCustomId", QString()).toString());
        }
        QMap<QString, QString> mine;
        for (const QString& kv : m_myExchange->text().split(' ', Qt::SkipEmptyParts)) {
            const int eq = kv.indexOf('=');
            if (eq > 0) {
                mine.insert(kv.left(eq).trimmed().toUpper(), kv.mid(eq + 1).trimmed().toUpper());
            }
        }
        m_contest->setMyExchange(mine);
        m_contest->setModeVariant(m_modeVariant->currentText());
        m_contest->setProfile(p);
        saveContestSettings();
    };
    connect(m_profile, &QComboBox::currentIndexChanged, this, apply);
    connect(m_modeVariant, &QComboBox::currentIndexChanged, this, apply);
    connect(m_myExchange, &QLineEdit::editingFinished, this, apply);
    connect(m_customTemplate, &QLineEdit::editingFinished, this, apply);
    connect(newContest, &QPushButton::clicked, this, [this]() {
        if (m_contest) {
            m_contest->setStartTime(QDateTime::currentDateTimeUtc());
            saveContestSettings();
        }
    });

    auto* row2 = new QHBoxLayout;
    m_serialLabel = new QLabel(m_contestBox);
    m_scoreLabel = new QLabel(m_contestBox);
    row2->addWidget(m_serialLabel);
    row2->addWidget(m_scoreLabel, 1);
    v->addLayout(row2);

    // Macros F1–F8
    auto* row3 = new QHBoxLayout;
    m_armTx = new QCheckBox(tr("Enable macro TX"), m_contestBox);
    m_armTx->setToolTip(tr("F1–F8 transmit CW (CWX) or voice (DVK) only while this is checked"));
    m_armTx->setChecked(false);   // never persisted: always starts disarmed
    connect(m_armTx, &QCheckBox::toggled, this, [this](bool on) {
        if (m_contest) {
            m_contest->setMacroTxArmed(on);
        }
    });
    row3->addWidget(m_armTx);
    for (int f = 1; f <= ContestEngine::kMacroCount; ++f) {
        auto* b = new QPushButton(QStringLiteral("F%1").arg(f), m_contestBox);
        b->setAutoDefault(false);
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QPushButton::clicked, this, [this, f]() { sendMacro(f); });
        connect(b, &QPushButton::customContextMenuRequested, this, [this, f]() {
            if (!m_contest) {
                return;
            }
            bool ok = false;
            const QString t = QInputDialog::getText(
                this, tr("Edit F%1").arg(f),
                tr("CW macro — {CALL} {MYCALL} {RST} {SN} {EXCH} {NAME} {MYNAME} "
                   "{STATE} {ZONE} {CLASS} {SECT} {PWR}, ! = his call"),
                QLineEdit::Normal, m_contest->cwMacro(f), &ok);
            if (ok) {
                m_contest->setCwMacro(f, t);
                updateContestUi();
            }
        });
        m_macroButtons.append(b);
        row3->addWidget(b);
    }
    v->addLayout(row3);

    // SO2R
    m_so2rRow = new QWidget(m_contestBox);
    auto* so2rLay = new QHBoxLayout(m_so2rRow);
    so2rLay->setContentsMargins(0, 0, 0, 0);
    m_so2rLabel = new QLabel(m_so2rRow);
    auto* swapTx = new QPushButton(tr("Swap TX"), m_so2rRow);
    auto* swapFocus = new QPushButton(tr("Swap focus"), m_so2rRow);
    auto* role = new QPushButton(tr("Run ⇄ S&&P"), m_so2rRow);
    for (QPushButton* b : {swapTx, swapFocus, role}) {
        b->setAutoDefault(false);
    }
    connect(swapTx, &QPushButton::clicked, this, [this]() { if (m_so2r) { m_so2r->swapTx(); } });
    connect(swapFocus, &QPushButton::clicked, this, [this]() { if (m_so2r) { m_so2r->swapFocus(); } });
    connect(role, &QPushButton::clicked, this, [this]() { if (m_so2r) { m_so2r->toggleRole(); } });
    so2rLay->addWidget(m_so2rLabel, 1);
    so2rLay->addWidget(swapTx);
    so2rLay->addWidget(swapFocus);
    so2rLay->addWidget(role);
    m_so2rRow->setVisible(false);
    v->addWidget(m_so2rRow);

    // Restore persisted contest UI state (engine state is loaded by MainWindow).
    if (m_contest) {
        const QSignalBlocker b1(m_profile);
        const QSignalBlocker b2(m_modeVariant);
        const int idx = m_profile->findData(m_contest->profile().key);
        m_profile->setCurrentIndex(idx < 0 ? 0 : idx);
        m_modeVariant->setCurrentText(m_contest->modeVariant());
        QStringList mine;
        const QMap<QString, QString> ex = m_contest->myExchange();
        for (auto it = ex.cbegin(); it != ex.cend(); ++it) {
            mine << it.key() + '=' + it.value();
        }
        m_myExchange->setText(mine.join(' '));
    }
    return m_contestBox;
}

QWidget* LogbookDialog::buildTablePanel()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);

    auto* bar = new QHBoxLayout;
    m_filterText = new QLineEdit(w);
    m_filterText->setPlaceholderText(tr("Search call / name / comment / QTH / country"));
    m_filterText->setClearButtonEnabled(true);
    m_filterBand = new QComboBox(w);
    m_filterBand->addItem(tr("All bands"), QString());
    for (const QString& b : bandList()) {
        m_filterBand->addItem(b, b);
    }
    m_filterMode = new QComboBox(w);
    m_filterMode->addItem(tr("All modes"), QString());
    m_filterMode->addItem("CW", "CW");
    m_filterMode->addItem(tr("Phone"), "PHONE");
    m_filterMode->addItem(tr("Data"), "DATA");
    m_filterContest = new QCheckBox(tr("This contest"), w);
    connect(m_filterText, &QLineEdit::textChanged, this, &LogbookDialog::scheduleReload);
    connect(m_filterBand, &QComboBox::currentIndexChanged, this, &LogbookDialog::scheduleReload);
    connect(m_filterMode, &QComboBox::currentIndexChanged, this, &LogbookDialog::scheduleReload);
    connect(m_filterContest, &QCheckBox::toggled, this, &LogbookDialog::scheduleReload);

    auto* importBtn = new QPushButton(tr("Import ADIF…"), w);
    auto* exportBtn = new QPushButton(tr("Export ADIF…"), w);
    auto* uploadBtn = new QPushButton(tr("Upload"), w);
    auto* settingsBtn = new QPushButton(tr("Settings…"), w);
    for (QPushButton* b : {importBtn, exportBtn, uploadBtn, settingsBtn}) {
        b->setAutoDefault(false);
    }
    auto* upMenu = new QMenu(uploadBtn);
    for (int i = 0; i < kUploadServiceCount; ++i) {
        const auto svc = static_cast<UploadService>(i);
        upMenu->addAction(tr("Selected QSOs → %1").arg(LogUtil::uploadServiceName(svc)),
                          this, [this, svc]() { uploadSelected(svc); });
    }
    upMenu->addSeparator();
    upMenu->addAction(tr("Retry all pending / failed"), this, [this]() {
        if (m_uploads) {
            const int n = m_uploads->enqueueOutstanding();
            m_status->setText(tr("%1 uploads queued").arg(n));
        }
    });
    uploadBtn->setMenu(upMenu);
    connect(importBtn, &QPushButton::clicked, this, &LogbookDialog::importAdif);
    connect(exportBtn, &QPushButton::clicked, this, &LogbookDialog::exportAdif);
    connect(settingsBtn, &QPushButton::clicked, this, &LogbookDialog::openLogSettings);

    bar->addWidget(m_filterText, 1);
    bar->addWidget(m_filterBand);
    bar->addWidget(m_filterMode);
    bar->addWidget(m_filterContest);
    bar->addWidget(importBtn);
    bar->addWidget(exportBtn);
    bar->addWidget(uploadBtn);
    bar->addWidget(settingsBtn);
    v->addLayout(bar);

    m_model = new LogTableModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(Qt::UserRole);
    m_table = new QTableView(w);
    m_table->setModel(m_proxy);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(LogTableModel::Time, Qt::DescendingOrder);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(20);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this, &LogbookDialog::showTableMenu);
    connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex& idx) {
        if (m_proxy->mapToSource(idx).column() == LogTableModel::Rec) {
            playRecording();
        } else {
            editSelected();
        }
    });
    auto* del = new QShortcut(QKeySequence::Delete, m_table);
    del->setContext(Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, &LogbookDialog::deleteSelected);
    v->addWidget(m_table, 1);
    return w;
}

// ── Slices / SO2R ─────────────────────────────────────────────────────────

void LogbookDialog::setSo2r(So2rController* so2r)
{
    if (m_so2r) {
        m_so2r->disconnect(this);
    }
    m_so2r = so2r;
    if (m_so2r) {
        connect(m_so2r, &So2rController::focusChanged, this, [this](int) {
            if (m_so2r) {
                setActiveSlice(m_so2r->focusedSlice());
            }
            updateSo2rUi();
        });
        connect(m_so2r, &So2rController::roleChanged, this, [this](int, So2rController::Role) { updateSo2rUi(); });
        connect(m_so2r, &So2rController::txRequested, this, [this](int) {
            QTimer::singleShot(300, this, &LogbookDialog::updateSo2rUi);   // after radio status
        });
    }
    updateSo2rUi();
}

void LogbookDialog::setActiveSlice(SliceModel* slice)
{
    disconnect(m_sliceFreqConn);
    disconnect(m_sliceModeConn);
    m_slice = slice;
    if (m_slice) {
        m_sliceFreqConn = connect(m_slice, &SliceModel::frequencyChanged, this, &LogbookDialog::updateSliceInfo);
        m_sliceModeConn = connect(m_slice, &SliceModel::modeChanged, this, &LogbookDialog::updateSliceInfo);
    }
    updateSliceInfo();
}

void LogbookDialog::updateSliceInfo()
{
    if (m_followSlice->isChecked() && m_slice) {
        m_freq->setText(QString::number(m_slice->frequency(), 'f', 6));
        m_mode->setCurrentText(m_slice->mode());
        if (m_rstS->text().isEmpty() || m_rstS->text() == "59" || m_rstS->text() == "599") {
            const LogQso d = m_log ? m_log->draftFromFrequency(m_slice->frequency(), m_slice->mode())
                                   : LogQso{};
            m_rstS->setPlaceholderText(d.rstSent);
            m_rstR->setPlaceholderText(d.rstRcvd);
        }
    }
    updatePreview();
}

void LogbookDialog::updateSo2rUi()
{
    const bool on = m_so2r && m_so2r->slice(0) && m_so2r->slice(1);
    m_so2rRow->setVisible(on);
    if (!on) {
        return;
    }
    QStringList parts;
    for (int r = 0; r < 2; ++r) {
        SliceModel* s = m_so2r->slice(r);
        QString t = QStringLiteral("%1: slice %2 %3 %4")
                        .arg(r == 0 ? "A" : "B")
                        .arg(QChar('A' + s->sliceId()))
                        .arg(QString::number(s->frequency(), 'f', 3))
                        .arg(So2rController::roleTag(m_so2r->role(r)));
        if (m_so2r->txRadio() == r) {
            t += QStringLiteral(" [TX]");
        }
        if (m_so2r->focusRadio() == r) {
            t = QStringLiteral("▶ ") + t;
        }
        parts << t;
    }
    m_so2rLabel->setText(parts.join(QStringLiteral("    ")));
}

// ── Quick entry ───────────────────────────────────────────────────────────

void LogbookDialog::focusQuickEntry()
{
    show();
    raise();
    activateWindow();
    m_call->setFocus();
    m_call->selectAll();
}

void LogbookDialog::prefillCall(const QString& call, double freqMhz, const QString& mode)
{
    m_call->setText(call.trimmed().toUpper());
    m_timeOn = QDateTime::currentDateTimeUtc();
    if (freqMhz > 0) {
        m_followSlice->setChecked(false);
        m_freq->setText(QString::number(freqMhz, 'f', 6));
        if (!mode.isEmpty()) {
            m_mode->setCurrentText(mode);
        }
    }
    updatePreview();
    focusQuickEntry();
}

void LogbookDialog::updatePreview()
{
    if (!m_log) {
        return;
    }
    const QString call = m_call->text().trimmed().toUpper();
    const double f = m_freq->text().toDouble();
    const QString band = LogUtil::freqToBand(f);
    QString mode;
    QString sub;
    LogUtil::flexModeToAdif(m_mode->currentText(), mode, sub);
    const QString group = LogUtil::modeGroup(mode, sub);

    if (m_contest && m_contest->isActive()) {
        const int sn = m_contest->nextSerial();
        m_serialLabel->setText(tr("Next serial: %1").arg(sn));
        m_sentLabel->setText(tr("Sent: %1").arg(m_contest->sentExchange(sn)));
    } else {
        m_sentLabel->clear();
    }

    if (call.size() < 2) {
        m_preview->clear();
        m_dupe->clear();
        return;
    }
    const WorkedStatus w = m_log->workedStatus(call, band, group);
    QStringList bits;
    if (w.dxccKnown) {
        bits << QStringLiteral("%1 (%2) · %3 · CQ %4 · ITU %5")
                    .arg(w.country, w.dxccPrefix, w.continent).arg(w.cqZone).arg(w.ituZone);
    }
    double myLat = 0;
    double myLon = 0;
    const QString myGrid = AppSettings::instance().value("StationGridSquare", QString()).toString();
    double hisLat = w.lat;
    double hisLon = w.lon;
    bool haveHis = w.dxccKnown;
    if (LogUtil::gridToLatLon(m_grid->text(), hisLat, hisLon)) {
        haveHis = true;
    }
    if (haveHis && LogUtil::gridToLatLon(myGrid, myLat, myLon)) {
        const double km = LogUtil::distanceKm(myLat, myLon, hisLat, hisLon);
        const double sp = LogUtil::bearingDeg(myLat, myLon, hisLat, hisLon);
        bits << QStringLiteral("%1° (LP %2°) · %3 km")
                    .arg(qRound(sp)).arg(qRound(std::fmod(sp + 180.0, 360.0))).arg(qRound(km));
    }
    if (w.callWorked) {
        bits << tr("worked %1×").arg(w.callQsoCount);
    } else if (w.dxccKnown) {
        switch (w.dxccStatus()) {
        case DxccStatus::NewDxcc: bits << tr("NEW DXCC"); break;
        case DxccStatus::NewBand: bits << tr("new band"); break;
        case DxccStatus::NewMode: bits << tr("new mode"); break;
        default: break;
        }
    }
    m_preview->setText(bits.join(QStringLiteral("  ·  ")));

    bool dupe = false;
    QString multText;
    if (m_contest && m_contest->isActive()) {
        dupe = m_contest->isDupe(call, band, group);
        if (!dupe && m_contest->isNewMult(call, band, m_contest->parseExchange(m_exchange->text()))) {
            multText = tr("MULT");
        }
    } else {
        dupe = w.callBandModeWorked;
    }
    if (dupe) {
        m_dupe->setText(m_contest && m_contest->isActive() ? tr("DUPE") : tr("worked on %1 %2").arg(band, group));
        m_dupe->setStyleSheet(m_contest && m_contest->isActive() ? "color: #ff4040; font-weight: bold;" : QString());
    } else {
        m_dupe->setText(multText);
        m_dupe->setStyleSheet(multText.isEmpty() ? QString() : "color: #ff8c00; font-weight: bold;");
    }
}

void LogbookDialog::logCurrentEntry()
{
    if (!m_log) {
        return;
    }
    const QString call = m_call->text().trimmed().toUpper();
    if (call.isEmpty()) {
        m_call->setFocus();
        return;
    }
    LogQso q;
    if (m_followSlice->isChecked() && m_slice) {
        q = m_log->draftFromSlice(m_slice);
    } else {
        q = m_log->draftFromFrequency(m_freq->text().toDouble(), m_mode->currentText());
    }
    q.call = call;
    if (m_timeOn.isValid()) {
        q.timeOn = m_timeOn;
    }
    if (!m_rstS->text().trimmed().isEmpty()) {
        q.rstSent = m_rstS->text().trimmed();
    }
    if (!m_rstR->text().trimmed().isEmpty()) {
        q.rstRcvd = m_rstR->text().trimmed();
    }
    q.name = m_name->text().trimmed();
    q.gridsquare = m_grid->text().trimmed().toUpper();
    q.comment = m_comment->text().trimmed();
    if (m_so2r && m_so2r->slice(0) && m_so2r->slice(1)) {
        m_so2r->stampQso(q);
    }

    if (m_contest && m_contest->isActive()) {
        if (m_contest->isDupe(q.call, q.band, LogUtil::modeGroup(q.mode, q.submode))) {
            const auto btn = QMessageBox::question(this, tr("Dupe"),
                tr("%1 is a dupe on %2. Log it anyway (0 points)?").arg(q.call, q.band));
            if (btn != QMessageBox::Yes) {
                return;
            }
        }
        const int sn = m_contest->nextSerial();
        const QMap<QString, QString> ex = m_contest->parseExchange(m_exchange->text());
        m_contest->applyExchange(q, ex, sn);
        if (ex.contains("RST")) {
            q.rstRcvd = ex.value("RST");
        }
        if (q.name.isEmpty() && ex.contains("NAME")) {
            q.name = ex.value("NAME");
        }
    } else if (!m_exchange->text().trimmed().isEmpty()) {
        q.srxString = m_exchange->text().trimmed();
    }
    const qint64 id = m_log->addQso(q);
    if (id <= 0) {
        QMessageBox::warning(this, tr("Log"), tr("Could not log QSO: %1").arg(m_log->lastError()));
        return;
    }
    m_status->setText(tr("Logged %1 on %2 %3").arg(q.call, q.band, q.submode.isEmpty() ? q.mode : q.submode));
    emit qsoLogged(id);
    clearEntry();
}

void LogbookDialog::clearEntry()
{
    for (QLineEdit* e : {m_call, m_rstS, m_rstR, m_name, m_grid, m_comment, m_exchange}) {
        e->clear();
    }
    m_timeOn = QDateTime();
    m_preview->clear();
    m_dupe->clear();
    m_call->setFocus();
    updatePreview();
}

void LogbookDialog::sendMacro(int fkey)
{
    if (!m_contest) {
        return;
    }
    QString mode;
    QString sub;
    LogUtil::flexModeToAdif(m_mode->currentText(), mode, sub);
    const bool phone = LogUtil::modeGroup(mode, sub) == "PHONE";
    const int serial = m_contest->isActive() ? m_contest->nextSerial() : 0;
    if (phone) {
        m_contest->sendVoiceMacro(fkey);
    } else {
        m_contest->sendCwMacro(fkey, m_call->text(), m_name->text(), serial);
    }
}

// ── Contest UI ────────────────────────────────────────────────────────────

void LogbookDialog::updateContestUi()
{
    const bool active = m_contest && m_contest->isActive();
    QStringList labels;
    if (active) {
        for (const ExchangeField& f : m_contest->profile().rcvd) {
            labels << f.label;
        }
    }
    if (m_exchangeLabel) {
        m_exchangeLabel->setText(active ? tr("Exch (%1)").arg(labels.join(' ')) : tr("Exchange"));
    }
    m_exchange->setPlaceholderText(active ? labels.join(' ') : tr("free text"));
    m_serialLabel->setVisible(active);
    m_scoreLabel->setVisible(active);
    m_modeVariant->setEnabled(active && m_contest->profile().modeInId);
    m_customTemplate->setVisible(m_contest && m_contest->profile().type == ContestType::Custom);
    for (int i = 0; i < m_macroButtons.size(); ++i) {
        const QString t = m_contest ? m_contest->cwMacro(i + 1) : QString();
        m_macroButtons[i]->setToolTip(tr("F%1: %2\nRight-click to edit").arg(i + 1).arg(t));
    }
    if (m_contest) {
        updateScore(m_contest->score());
    }
    updatePreview();
}

void LogbookDialog::updateScore(const ContestScore& s)
{
    QStringList cats;
    for (auto it = s.multsByCategory.cbegin(); it != s.multsByCategory.cend(); ++it) {
        cats << QStringLiteral("%1 %2").arg(it.key()).arg(it.value());
    }
    m_scoreLabel->setText(tr("QSOs %1 (dupes %2) · Points %3 · Mults %4%5 · Score %6")
                              .arg(s.qsos).arg(s.dupes).arg(s.points).arg(s.mults)
                              .arg(cats.isEmpty() ? QString() : QStringLiteral(" [") + cats.join(", ") + ']')
                              .arg(s.score));
    updatePreview();
}

void LogbookDialog::saveContestSettings()
{
    if (!m_contest) {
        return;
    }
    auto& s = AppSettings::instance();
    s.setValue("ContestCustomTemplate", m_customTemplate->text());
    m_contest->saveSettings();   // also saves AppSettings
}

// ── Table ─────────────────────────────────────────────────────────────────

void LogbookDialog::scheduleReload()
{
    m_reloadTimer.start();
}

void LogbookDialog::reloadTable()
{
    if (!m_log) {
        return;
    }
    LogFilter f;
    f.text = m_filterText->text();
    f.band = m_filterBand->currentData().toString();
    f.modeGroup = m_filterMode->currentData().toString();
    if (m_filterContest->isChecked() && m_contest && m_contest->isActive()) {
        f.contestId = m_contest->contestId();
        f.from = m_contest->startTime();
    }
    f.limit = kDisplayLimit;
    m_model->setRows(m_log->search(f));
    m_table->resizeColumnsToContents();
    const qint64 total = m_log->count();
    m_status->setText(tr("%1 of %2 QSOs shown").arg(m_model->rowCount()).arg(total));
}

QVector<qint64> LogbookDialog::selectedIds() const
{
    QVector<qint64> ids;
    for (const QModelIndex& idx : m_table->selectionModel()->selectedRows()) {
        ids << m_model->row(m_proxy->mapToSource(idx).row()).id;
    }
    return ids;
}

void LogbookDialog::editSelected()
{
    const QVector<qint64> ids = selectedIds();
    if (ids.size() != 1 || !m_log) {
        return;
    }
    bool ok = false;
    const LogQso q = m_log->qso(ids.first(), &ok);
    if (!ok) {
        return;
    }
    QsoEditDialog dlg(q, this);
    if (dlg.exec() == QDialog::Accepted) {
        if (!m_log->updateQso(dlg.result())) {
            QMessageBox::warning(this, tr("Edit"), m_log->lastError());
        }
    }
}

void LogbookDialog::deleteSelected()
{
    const QVector<qint64> ids = selectedIds();
    if (ids.isEmpty() || !m_log) {
        return;
    }
    if (QMessageBox::question(this, tr("Delete"),
                              tr("Delete %n QSO(s)? This cannot be undone.", nullptr,
                                 static_cast<int>(ids.size()))) != QMessageBox::Yes) {
        return;
    }
    m_log->deleteQsos(ids);
}

void LogbookDialog::playRecording()
{
    const QVector<qint64> ids = selectedIds();
    if (ids.size() != 1 || !m_log) {
        return;
    }
    const QString path = m_log->qso(ids.first()).recordingPath;
    if (path.isEmpty()) {
        return;
    }
    if (!QFileInfo::exists(path)) {
        QMessageBox::information(this, tr("Recording"), tr("File not found:\n%1").arg(path));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void LogbookDialog::attachRecording()
{
    const QVector<qint64> ids = selectedIds();
    if (ids.size() != 1 || !m_log) {
        return;
    }
    const QString dir = AppSettings::instance().value("QsoRecordingDir", QString()).toString();
    const QString path = QFileDialog::getOpenFileName(this, tr("Attach recording"), dir,
                                                      tr("Audio (*.wav)"));
    if (!path.isEmpty()) {
        m_log->setRecordingPath(ids.first(), path);
    }
}

void LogbookDialog::showTableMenu(const QPoint& pos)
{
    const QVector<qint64> ids = selectedIds();
    QMenu menu(this);
    QAction* edit = menu.addAction(tr("Edit…"), this, &LogbookDialog::editSelected);
    edit->setEnabled(ids.size() == 1);
    QAction* play = menu.addAction(tr("Play recording"), this, &LogbookDialog::playRecording);
    play->setEnabled(ids.size() == 1 && m_log && !m_log->qso(ids.first()).recordingPath.isEmpty());
    QAction* attach = menu.addAction(tr("Attach recording…"), this, &LogbookDialog::attachRecording);
    attach->setEnabled(ids.size() == 1);
    menu.addSeparator();
    QMenu* up = menu.addMenu(tr("Upload to"));
    for (int i = 0; i < kUploadServiceCount; ++i) {
        const auto svc = static_cast<UploadService>(i);
        up->addAction(LogUtil::uploadServiceName(svc), this, [this, svc]() { uploadSelected(svc); });
    }
    up->setEnabled(!ids.isEmpty() && m_uploads);
    menu.addAction(tr("Export selected…"), this, &LogbookDialog::exportAdif)->setEnabled(!ids.isEmpty());
    if (ids.size() == 1 && m_log) {
        const LogQso q = m_log->qso(ids.first());
        menu.addSeparator();
        menu.addAction(tr("Turn rotator to %1 (SP)").arg(q.call), this,
                       [this, q]() { emit rotatorTurnRequested(q.call, q.gridsquare, false); });
        menu.addAction(tr("Turn rotator to %1 (LP)").arg(q.call), this,
                       [this, q]() { emit rotatorTurnRequested(q.call, q.gridsquare, true); });
    }
    menu.addSeparator();
    menu.addAction(tr("Delete…"), this, &LogbookDialog::deleteSelected)->setEnabled(!ids.isEmpty());
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void LogbookDialog::importAdif()
{
    if (!m_log || m_log->isImporting()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Import ADIF"), QString(),
                                                      tr("ADIF (*.adi *.adif);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (m_log->importAdifAsync(path, true)) {
        m_importProgress->setVisible(true);
        m_importProgress->setRange(0, 0);
        m_status->setText(tr("Importing %1…").arg(QFileInfo(path).fileName()));
    }
}

void LogbookDialog::exportAdif()
{
    if (!m_log) {
        return;
    }
    const QVector<qint64> sel = selectedIds();
    QString scope = tr("all QSOs");
    if (sel.size() > 1) {
        scope = tr("%1 selected QSOs").arg(sel.size());
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export ADIF (%1)").arg(scope),
                                                      QStringLiteral("aethersdr.adi"),
                                                      tr("ADIF (*.adi)"));
    if (path.isEmpty()) {
        return;
    }
    const bool ok = (sel.size() > 1) ? m_log->exportAdif(path, sel)
                                     : m_log->exportAdif(path, LogFilter{});
    m_status->setText(ok ? tr("Exported to %1").arg(path) : tr("Export failed"));
}

void LogbookDialog::uploadSelected(UploadService svc)
{
    if (!m_uploads) {
        return;
    }
    const QVector<qint64> ids = selectedIds();
    if (ids.isEmpty()) {
        return;
    }
    if (!m_uploads->isEnabled(svc)) {
        QMessageBox::information(this, tr("Upload"),
                                 tr("%1 uploads are disabled. Enable them in Settings.")
                                     .arg(LogUtil::uploadServiceName(svc)));
        return;
    }
    m_uploads->enqueue(ids, svc);
    m_status->setText(tr("%1 QSO(s) queued for %2").arg(ids.size()).arg(LogUtil::uploadServiceName(svc)));
}

void LogbookDialog::openLogSettings()
{
    LogSettingsDialog dlg(m_creds, this);
    if (dlg.exec() == QDialog::Accepted && m_uploads) {
        m_uploads->loadSettings();
        for (int i = 0; i < kUploadServiceCount; ++i) {
            m_uploads->resume(static_cast<UploadService>(i));
        }
    }
    updatePreview();
}

} // namespace AetherSDR
