#include "LogUploaders.h"

#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr int kHttpTimeoutMs  = 30000;
constexpr int kTqslTimeoutMs  = 10 * 60 * 1000;
constexpr int kMaxBackoffMs   = 60 * 60 * 1000;
constexpr int kLotwBatchMax   = 500;

const char* settingsName(UploadService s)
{
    switch (s) {
    case UploadService::Lotw:    return "Lotw";
    case UploadService::Qrz:     return "Qrz";
    case UploadService::ClubLog: return "ClubLog";
    case UploadService::Eqsl:    return "Eqsl";
    }
    return "";
}

// Read several credentials one after another (keychain jobs are async).
void readChain(QPointer<CredentialStore> creds, const QStringList& keys, QStringList acc,
               std::function<void(QStringList)> cb)
{
    if (acc.size() >= keys.size() || !creds) {
        while (acc.size() < keys.size()) {
            acc.append(QString());
        }
        cb(acc);
        return;
    }
    const QString key = keys.at(acc.size());
    creds->read(key, [creds, keys, acc, cb](const QString& v) mutable {
        acc.append(v);
        readChain(creds, keys, acc, cb);
    });
}

Adif::WriteOptions uploadWriteOptions()
{
    // Never leak local paths / slice numbers / our own status fields.
    Adif::WriteOptions o;
    o.includeAppFields = false;
    o.includeUploadStatus = false;
    o.includeExtra = true;
    return o;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// CredentialStore
// ═══════════════════════════════════════════════════════════════════════════

CredentialStore::CredentialStore(Backend backend, QObject* parent)
    : QObject(parent)
    , m_backend(backend)
{
    if (m_backend == Backend::Auto) {
#ifdef HAVE_KEYCHAIN
        m_backend = Backend::Keychain;
#else
        m_backend = Backend::AppSettingsPlain;
#endif
    }
#ifndef HAVE_KEYCHAIN
    if (m_backend == Backend::Keychain) {
        m_backend = Backend::AppSettingsPlain;
    }
#endif
}

QString CredentialStore::insecureWarning()
{
    return tr("This build has no OS keychain support (QtKeychain). Upload "
              "passwords and API keys will be stored in plain text in "
              "AetherSDR.settings. Use application-specific passwords where "
              "the service offers them.");
}

void CredentialStore::read(const QString& key, std::function<void(const QString&)> cb)
{
    switch (m_backend) {
    case Backend::Memory:
        cb(m_memory.value(key));
        return;
    case Backend::AppSettingsPlain:
        cb(AppSettings::instance().value(QStringLiteral("LogUploadSecret_") + key, QString()).toString());
        return;
    case Backend::Keychain:
    case Backend::Auto:
        break;
    }
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR"), this);
    job->setAutoDelete(true);
    job->setKey(key);
    connect(job, &QKeychain::Job::finished, this, [cb](QKeychain::Job* j) {
        auto* rj = static_cast<QKeychain::ReadPasswordJob*>(j);
        if (j->error() != QKeychain::NoError && j->error() != QKeychain::EntryNotFound) {
            qCWarning(lcLogbook) << "keychain read failed:" << j->errorString();
        }
        cb(j->error() == QKeychain::NoError ? rj->textData() : QString());
    });
    job->start();
#else
    cb(QString());
#endif
}

void CredentialStore::write(const QString& key, const QString& value,
                            std::function<void(bool, const QString&)> cb)
{
    switch (m_backend) {
    case Backend::Memory:
        m_memory.insert(key, value);
        if (cb) { cb(true, {}); }
        return;
    case Backend::AppSettingsPlain: {
        auto& s = AppSettings::instance();
        s.setValue(QStringLiteral("LogUploadSecret_") + key, value);
        s.save();
        if (cb) { cb(true, {}); }
        return;
    }
    case Backend::Keychain:
    case Backend::Auto:
        break;
    }
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::WritePasswordJob(QStringLiteral("AetherSDR"), this);
    job->setAutoDelete(true);
    job->setKey(key);
    job->setTextData(value);
    connect(job, &QKeychain::Job::finished, this, [cb](QKeychain::Job* j) {
        const bool ok = j->error() == QKeychain::NoError;
        if (!ok) {
            qCWarning(lcLogbook) << "keychain write failed:" << j->errorString();
        }
        if (cb) { cb(ok, ok ? QString() : j->errorString()); }
    });
    job->start();
#else
    if (cb) { cb(false, QStringLiteral("keychain unavailable")); }
#endif
}

void CredentialStore::remove(const QString& key)
{
    m_memory.remove(key);
    if (m_backend == Backend::AppSettingsPlain) {
        auto& s = AppSettings::instance();
        s.remove(QStringLiteral("LogUploadSecret_") + key);
        s.save();
        return;
    }
#ifdef HAVE_KEYCHAIN
    if (m_backend == Backend::Keychain) {
        auto* job = new QKeychain::DeletePasswordJob(QStringLiteral("AetherSDR"), this);
        job->setAutoDelete(true);
        job->setKey(key);
        job->start();
    }
#endif
}

// ═══════════════════════════════════════════════════════════════════════════
// UploadApi
// ═══════════════════════════════════════════════════════════════════════════

namespace UploadApi {

QUrl defaultEndpoint(UploadService s)
{
    switch (s) {
    case UploadService::Qrz:     return QUrl(QStringLiteral("https://logbook.qrz.com/api"));
    case UploadService::ClubLog: return QUrl(QStringLiteral("https://clublog.org/realtime.php"));
    case UploadService::Eqsl:    return QUrl(QStringLiteral("https://www.eQSL.cc/qslcard/importADIF.cfm"));
    case UploadService::Lotw:    return {};
    }
    return {};
}

QByteArray userAgent()
{
    QString v = QCoreApplication::applicationVersion();
    if (v.isEmpty()) {
        v = QStringLiteral("dev");
    }
    return QByteArray("AetherSDR/") + v.toUtf8();
}

QByteArray formEncode(const QList<QPair<QString, QString>>& fields)
{
    QByteArray out;
    for (const auto& f : fields) {
        if (!out.isEmpty()) {
            out += '&';
        }
        out += QUrl::toPercentEncoding(f.first);
        out += '=';
        out += QUrl::toPercentEncoding(f.second);
    }
    return out;
}

HttpRequest qrzInsert(const QUrl& ep, const QString& apiKey, const QByteArray& adifRecord,
                      bool replace)
{
    HttpRequest r;
    r.url = ep;
    r.userAgent = userAgent();
    QList<QPair<QString, QString>> f = {
        {"KEY", apiKey}, {"ACTION", "INSERT"}, {"ADIF", QString::fromUtf8(adifRecord)}};
    if (replace) {
        f.append(qMakePair(QStringLiteral("OPTION"), QStringLiteral("REPLACE")));
    }
    r.body = formEncode(f);
    return r;
}

HttpRequest qrzStatus(const QUrl& ep, const QString& apiKey)
{
    HttpRequest r;
    r.url = ep;
    r.userAgent = userAgent();
    r.body = formEncode({{"KEY", apiKey}, {"ACTION", "STATUS"}});
    return r;
}

QMap<QString, QString> parseQrzKv(const QByteArray& body)
{
    QMap<QString, QString> m;
    const QString s = QString::fromUtf8(body).trimmed();
    for (const QString& part : s.split('&', Qt::SkipEmptyParts)) {
        const int eq = part.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        const QString k = part.left(eq).trimmed().toUpper();
        QString v = part.mid(eq + 1);
        v.replace('+', ' ');
        m.insert(k, QUrl::fromPercentEncoding(v.toUtf8()));
    }
    return m;
}

Result parseQrzResponse(int httpStatus, const QByteArray& body)
{
    Result r;
    if (httpStatus != 200) {
        r.outcome = Result::Outcome::RetryLater;
        r.message = QStringLiteral("HTTP %1").arg(httpStatus);
        return r;
    }
    const QMap<QString, QString> kv = parseQrzKv(body);
    const QString result = kv.value("RESULT").toUpper();
    const QString reason = kv.value("REASON");
    r.message = reason;
    r.remoteId = kv.value("LOGID");
    if (result == "OK" || result == "REPLACE") {
        r.outcome = Result::Outcome::Ok;
    } else if (result == "AUTH" || reason.contains("invalid api key", Qt::CaseInsensitive) ||
               reason.contains("access denied", Qt::CaseInsensitive) ||
               reason.contains("subscription", Qt::CaseInsensitive)) {
        r.outcome = Result::Outcome::AuthFailed;
    } else if (reason.contains("duplicate", Qt::CaseInsensitive)) {
        r.outcome = Result::Outcome::Duplicate;
    } else if (result == "FAIL") {
        r.outcome = Result::Outcome::Rejected;
    } else {
        r.outcome = Result::Outcome::RetryLater;
        if (r.message.isEmpty()) {
            r.message = QStringLiteral("Unexpected QRZ response");
        }
    }
    return r;
}

HttpRequest clubLogRealtime(const QUrl& ep, const QString& email, const QString& appPassword,
                            const QString& callsign, const QString& apiKey,
                            const QByteArray& adifRecord)
{
    HttpRequest r;
    r.url = ep;
    r.userAgent = userAgent();
    r.body = formEncode({{"email", email},
                         {"password", appPassword},
                         {"callsign", callsign},
                         {"adif", QString::fromUtf8(adifRecord)},
                         {"api", apiKey}});
    return r;
}

Result parseClubLogResponse(int httpStatus, const QByteArray& body)
{
    // Club Log realtime.php: 200 "QSO OK" / "QSO Modified" / "QSO Duplicate",
    // 400 rejected, 403 auth failure (must stop sending), 500 retry later.
    Result r;
    r.message = QString::fromUtf8(body).trimmed().left(300);
    switch (httpStatus) {
    case 200:
        r.outcome = r.message.contains("dup", Qt::CaseInsensitive)
            ? Result::Outcome::Duplicate : Result::Outcome::Ok;
        break;
    case 400:
        r.outcome = Result::Outcome::Rejected;
        break;
    case 403:
        r.outcome = Result::Outcome::AuthFailed;
        break;
    default:
        r.outcome = Result::Outcome::RetryLater;
        if (r.message.isEmpty()) {
            r.message = QStringLiteral("HTTP %1").arg(httpStatus);
        }
        break;
    }
    return r;
}

HttpRequest eqslImport(const QUrl& ep, const QString& user, const QString& password,
                       const QByteArray& adifFile)
{
    HttpRequest r;
    r.url = ep;
    r.userAgent = userAgent();
    r.body = formEncode({{"EQSL_USER", user},
                         {"EQSL_PSWD", password},
                         {"ADIFData", QString::fromUtf8(adifFile)}});
    return r;
}

Result parseEqslResponse(int httpStatus, const QByteArray& body)
{
    Result r;
    if (httpStatus != 200) {
        r.outcome = Result::Outcome::RetryLater;
        r.message = QStringLiteral("HTTP %1").arg(httpStatus);
        return r;
    }
    const QString s = QString::fromUtf8(body);
    // Strip tags for a readable message
    QString text = s;
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    r.message = text.simplified().left(300);
    if (s.contains("No match on eQSL_User", Qt::CaseInsensitive) ||
        s.contains("No such Username", Qt::CaseInsensitive) ||
        (s.contains("password", Qt::CaseInsensitive) && s.contains("Error", Qt::CaseInsensitive))) {
        r.outcome = Result::Outcome::AuthFailed;
    } else if (s.contains("Duplicate", Qt::CaseInsensitive)) {
        r.outcome = Result::Outcome::Duplicate;
    } else if (QRegularExpression(QStringLiteral("Result:\\s*[1-9]\\d*\\s+out of"),
                                  QRegularExpression::CaseInsensitiveOption).match(s).hasMatch()) {
        r.outcome = Result::Outcome::Ok;
    } else if (s.contains("Error", Qt::CaseInsensitive) || s.contains("Bad record", Qt::CaseInsensitive)) {
        r.outcome = Result::Outcome::Rejected;
    } else {
        r.outcome = Result::Outcome::RetryLater;
    }
    return r;
}

QString detectTqsl()
{
    const QString custom = AppSettings::instance().value("LogUploadTqslPath", QString()).toString();
    if (!custom.isEmpty() && QFileInfo(custom).isExecutable()) {
        return custom;
    }
    QStringList candidates;
#if defined(Q_OS_MAC)
    candidates << "/Applications/TrustedQSL/tqsl.app/Contents/MacOS/tqsl"
               << "/Applications/tqsl.app/Contents/MacOS/tqsl";
#elif defined(Q_OS_WIN)
    candidates << "C:/Program Files (x86)/TrustedQSL/tqsl.exe"
               << "C:/Program Files/TrustedQSL/tqsl.exe";
#else
    candidates << "/usr/bin/tqsl" << "/usr/local/bin/tqsl";
#endif
    for (const QString& c : candidates) {
        if (QFileInfo(c).isExecutable()) {
            return c;
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("tqsl"));
}

QStringList tqslArguments(const QString& stationLocation, const QString& adifPath)
{
    // -x batch (exit when done), -q quiet, -d no date-range dialog,
    // -u upload after signing, -a compliant = sign only QSOs that are not
    // already uploaded / out of range.  -p (password) is deliberately never
    // used: the certificate password is not stored by AetherSDR.
    return {QStringLiteral("-x"), QStringLiteral("-q"), QStringLiteral("-d"),
            QStringLiteral("-u"), QStringLiteral("-a"), QStringLiteral("compliant"),
            QStringLiteral("-l"), stationLocation, adifPath};
}

Result parseTqslExit(int code, const QByteArray& output)
{
    // TQSL command-line exit codes (TrustedQSL 2.x documentation).
    Result r;
    r.message = QString::fromUtf8(output).trimmed().right(400);
    switch (code) {
    case 0:  r.outcome = Result::Outcome::Ok; break;
    case 8:  r.outcome = Result::Outcome::Duplicate;          // all dupes / out of range
             if (r.message.isEmpty()) { r.message = QStringLiteral("All QSOs already uploaded"); }
             break;
    case 9:  r.outcome = Result::Outcome::Ok;                 // some dupes
             if (r.message.isEmpty()) { r.message = QStringLiteral("Some QSOs were duplicates"); }
             break;
    case 11: r.outcome = Result::Outcome::RetryLater;         // LoTW connection error
             if (r.message.isEmpty()) { r.message = QStringLiteral("LoTW connection error"); }
             break;
    case 3:  r.outcome = Result::Outcome::RetryLater; break;  // unexpected LoTW response
    case 1:  r.outcome = Result::Outcome::AuthFailed;         // cancelled (e.g. password prompt)
             if (r.message.isEmpty()) { r.message = QStringLiteral("TQSL cancelled"); }
             break;
    default: r.outcome = Result::Outcome::Rejected;
             if (r.message.isEmpty()) { r.message = QStringLiteral("TQSL exit code %1").arg(code); }
             break;
    }
    return r;
}

} // namespace UploadApi

// ═══════════════════════════════════════════════════════════════════════════
// LogUploadQueue
// ═══════════════════════════════════════════════════════════════════════════

LogUploadQueue::LogUploadQueue(Logbook* log, CredentialStore* creds, QObject* parent)
    : QObject(parent)
    , m_log(log)
    , m_creds(creds)
{
    for (int i = 0; i < kUploadServiceCount; ++i) {
        m_endpoint[i] = UploadApi::defaultEndpoint(static_cast<UploadService>(i));
    }
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &LogUploadQueue::processNow);
    m_tqslTimeout.setSingleShot(true);
    connect(&m_tqslTimeout, &QTimer::timeout, this, [this]() {
        if (m_tqsl) {
            qCWarning(lcLogbook) << "TQSL timed out; killing";
            m_tqsl->kill();
        }
    });
    if (m_log) {
        connect(m_log, &Logbook::qsoAdded, this, &LogUploadQueue::onQsoAdded);
    }
}

LogUploadQueue::~LogUploadQueue()
{
    if (m_tqsl) {
        m_tqsl->disconnect(this);
        if (m_tqsl->state() != QProcess::NotRunning) {
            m_tqsl->kill();
            m_tqsl->waitForFinished(2000);
        }
    }
}

void LogUploadQueue::loadSettings()
{
    auto& s = AppSettings::instance();
    for (int i = 0; i < kUploadServiceCount; ++i) {
        const auto svc = static_cast<UploadService>(i);
        m_enabled[i] = s.value(QStringLiteral("LogUpload%1Enabled").arg(QLatin1String(settingsName(svc))),
                               "False").toString() == "True";
    }
    m_autoUpload = s.value("LogUploadAuto", "False").toString() == "True";
    m_clubLogEmail = s.value("LogUploadClubLogEmail", QString()).toString();
    m_clubLogCall = s.value("LogUploadClubLogCallsign", QString()).toString();
    m_eqslUser = s.value("LogUploadEqslUser", QString()).toString();
    m_tqslLocation = s.value("LogUploadTqslStationLocation", QString()).toString();
    m_tqslPath = UploadApi::detectTqsl();
}

void LogUploadQueue::setEnabled(UploadService s, bool on)
{
    m_enabled[static_cast<int>(s)] = on;
    if (on) {
        schedule();
    }
}

void LogUploadQueue::setAutoUpload(bool on)
{
    m_autoUpload = on;
}

void LogUploadQueue::setClubLogAccount(const QString& email, const QString& callsign)
{
    m_clubLogEmail = email.trimmed();
    m_clubLogCall = callsign.trimmed().toUpper();
}

void LogUploadQueue::setEqslUser(const QString& user)
{
    m_eqslUser = user.trimmed();
}

void LogUploadQueue::setTqslStationLocation(const QString& loc)
{
    m_tqslLocation = loc;
}

bool LogUploadQueue::hasJobFor(qint64 id, UploadService s) const
{
    return std::any_of(m_queue.cbegin(), m_queue.cend(),
                       [id, s](const Job& j) { return j.id == id && j.svc == s; }) ||
           std::any_of(m_lotwBatch.cbegin(), m_lotwBatch.cend(),
                       [id, s](const Job& j) { return j.id == id && j.svc == s; });
}

void LogUploadQueue::enqueue(qint64 id, UploadService s)
{
    enqueue(QVector<qint64>{id}, s);
}

void LogUploadQueue::enqueue(const QVector<qint64>& ids, UploadService s)
{
    for (qint64 id : ids) {
        if (id <= 0 || hasJobFor(id, s)) {
            continue;
        }
        if (m_log) {
            bool ok = false;
            const LogQso q = m_log->qso(id, &ok);
            if (!ok) {
                continue;
            }
            // Keep "Modified" so QRZ gets OPTION=REPLACE; otherwise Pending.
            if (q.uploadState(s) != UploadState::Modified) {
                m_log->setUploadState(id, s, UploadState::Pending);
            }
        }
        Job j;
        j.id = id;
        j.svc = s;
        m_queue.append(j);
    }
    emit queueChanged(pendingCount());
    schedule();
}

int LogUploadQueue::enqueueOutstanding()
{
    if (!m_log) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < kUploadServiceCount; ++i) {
        const auto svc = static_cast<UploadService>(i);
        if (!m_enabled[i]) {
            continue;
        }
        QSet<UploadState> st = {UploadState::Pending, UploadState::Failed};
        if (svc == UploadService::Qrz || svc == UploadService::ClubLog) {
            st.insert(UploadState::Modified);
        }
        QVector<qint64> ids;
        for (const LogQso& q : m_log->qsosWithUploadState(svc, st)) {
            ids << q.id;
        }
        n += static_cast<int>(ids.size());
        enqueue(ids, svc);
    }
    return n;
}

void LogUploadQueue::resume(UploadService s)
{
    m_suspended[static_cast<int>(s)] = false;
    enqueueOutstanding();
    schedule();
}

void LogUploadQueue::onQsoAdded(const LogQso& q)
{
    if (!m_autoUpload) {
        return;
    }
    for (int i = 0; i < kUploadServiceCount; ++i) {
        if (m_enabled[i] && q.upload[i] != UploadState::DoNotUpload) {
            enqueue(q.id, static_cast<UploadService>(i));
        }
    }
}

int LogUploadQueue::backoffMs(int attempts) const
{
    const qint64 ms = static_cast<qint64>(m_retryBaseMs) << std::min(attempts, 16);
    return static_cast<int>(std::min<qint64>(ms, kMaxBackoffMs));
}

void LogUploadQueue::processNow()
{
    schedule();
}

void LogUploadQueue::schedule()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 nextWake = -1;
    for (int i = 0; i < kUploadServiceCount; ++i) {
        const auto svc = static_cast<UploadService>(i);
        if (!m_enabled[i] || m_suspended[i] || m_busy[i]) {
            continue;
        }
        if (svc == UploadService::Lotw) {
            startLotwBatch();
            continue;
        }
        for (int k = 0; k < m_queue.size(); ++k) {
            if (m_queue[k].svc != svc) {
                continue;
            }
            if (m_queue[k].notBeforeMs <= now) {
                const Job j = m_queue.takeAt(k);
                m_busy[i] = true;
                startHttp(j);
                break;
            }
        }
    }
    for (const Job& j : m_queue) {
        const int i = static_cast<int>(j.svc);
        if (m_enabled[i] && !m_suspended[i] && j.notBeforeMs > now) {
            nextWake = (nextWake < 0) ? j.notBeforeMs : std::min(nextWake, j.notBeforeMs);
        }
    }
    if (nextWake > 0) {
        m_timer.start(static_cast<int>(std::max<qint64>(1, nextWake - now)));
    }
}

void LogUploadQueue::withSecrets(UploadService s, std::function<void(QStringList)> cb)
{
    QStringList keys;
    switch (s) {
    case UploadService::Qrz:     keys << CredentialStore::kQrzApiKey; break;
    case UploadService::ClubLog: keys << CredentialStore::kClubLogPassword << CredentialStore::kClubLogApiKey; break;
    case UploadService::Eqsl:    keys << CredentialStore::kEqslPassword; break;
    case UploadService::Lotw:    break;
    }
    if (!m_creds || keys.isEmpty()) {
        cb(QStringList(keys.size(), QString()));
        return;
    }
    readChain(m_creds, keys, {}, std::move(cb));
}

void LogUploadQueue::startHttp(const Job& job)
{
    if (!m_log) {
        return;
    }
    bool ok = false;
    const LogQso q = m_log->qso(job.id, &ok);
    const int si = static_cast<int>(job.svc);
    if (!ok) {
        m_busy[si] = false;
        schedule();
        return;
    }
    QPointer<LogUploadQueue> self(this);
    withSecrets(job.svc, [self, job, q](QStringList secrets) {
        if (!self) {
            return;
        }
        const QByteArray rec = Adif::record(q, uploadWriteOptions());
        UploadApi::HttpRequest req;
        const QUrl ep = self->m_endpoint[static_cast<int>(job.svc)];
        switch (job.svc) {
        case UploadService::Qrz:
            if (secrets.value(0).isEmpty()) {
                UploadApi::Result r{UploadApi::Result::Outcome::AuthFailed,
                                    tr("QRZ API key not set"), {}};
                self->finishJob(job, r);
                return;
            }
            req = UploadApi::qrzInsert(ep, secrets.value(0), rec,
                                       q.uploadState(UploadService::Qrz) == UploadState::Modified);
            break;
        case UploadService::ClubLog:
            if (self->m_clubLogEmail.isEmpty() || secrets.value(0).isEmpty() ||
                secrets.value(1).isEmpty()) {
                UploadApi::Result r{UploadApi::Result::Outcome::AuthFailed,
                                    tr("Club Log email / application password / API key not set"), {}};
                self->finishJob(job, r);
                return;
            }
            req = UploadApi::clubLogRealtime(
                ep, self->m_clubLogEmail, secrets.value(0),
                self->m_clubLogCall.isEmpty() ? q.stationCallsign : self->m_clubLogCall,
                secrets.value(1), rec);
            break;
        case UploadService::Eqsl:
            if (self->m_eqslUser.isEmpty() || secrets.value(0).isEmpty()) {
                UploadApi::Result r{UploadApi::Result::Outcome::AuthFailed,
                                    tr("eQSL user / password not set"), {}};
                self->finishJob(job, r);
                return;
            }
            req = UploadApi::eqslImport(ep, self->m_eqslUser, secrets.value(0),
                                        Adif::header(QCoreApplication::applicationVersion()) + rec);
            break;
        case UploadService::Lotw:
            return;
        }
        QNetworkRequest nr(req.url);
        nr.setHeader(QNetworkRequest::ContentTypeHeader, req.contentType);
        nr.setHeader(QNetworkRequest::UserAgentHeader, req.userAgent);
        nr.setTransferTimeout(kHttpTimeoutMs);
        QNetworkReply* reply = self->m_nam.post(nr, req.body);
        connect(reply, &QNetworkReply::finished, self, [self, reply, job]() {
            reply->deleteLater();
            if (!self) {
                return;
            }
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            UploadApi::Result r;
            if (status == 0) {
                r.outcome = UploadApi::Result::Outcome::RetryLater;
                r.message = reply->errorString();
            } else if (job.svc == UploadService::Qrz) {
                r = UploadApi::parseQrzResponse(status, body);
            } else if (job.svc == UploadService::ClubLog) {
                r = UploadApi::parseClubLogResponse(status, body);
            } else {
                r = UploadApi::parseEqslResponse(status, body);
            }
            self->finishJob(job, r);
        });
    });
}

void LogUploadQueue::finishJob(const Job& jobIn, const UploadApi::Result& r, bool fromBatch)
{
    using O = UploadApi::Result::Outcome;
    Job job = jobIn;
    const int si = static_cast<int>(job.svc);
    if (!fromBatch) {
        m_busy[si] = false;
    }
    switch (r.outcome) {
    case O::Ok:
    case O::Duplicate:
        if (m_log) {
            m_log->setUploadState(job.id, job.svc, UploadState::Uploaded,
                                  QDateTime::currentDateTimeUtc().date());
        }
        emit uploadFinished(job.id, job.svc, true, r.message);
        break;
    case O::Rejected:
        if (m_log) {
            m_log->setUploadState(job.id, job.svc, UploadState::Failed);
        }
        qCWarning(lcLogbook) << LogUtil::uploadServiceName(job.svc) << "rejected QSO"
                             << job.id << r.message;
        emit uploadFinished(job.id, job.svc, false, r.message);
        break;
    case O::AuthFailed: {
        // Stop talking to the service (Club Log blocks IPs that keep
        // retrying after 403).  QSOs stay Pending for resume().
        m_suspended[si] = true;
        for (int k = static_cast<int>(m_queue.size()) - 1; k >= 0; --k) {
            if (m_queue[k].svc == job.svc) {
                m_queue.removeAt(k);
            }
        }
        qCWarning(lcLogbook) << LogUtil::uploadServiceName(job.svc) << "suspended:" << r.message;
        emit uploadFinished(job.id, job.svc, false, r.message);
        emit serviceSuspended(job.svc, r.message);
        break;
    }
    case O::RetryLater:
        ++job.attempts;
        if (job.attempts >= m_maxAttempts) {
            if (m_log) {
                m_log->setUploadState(job.id, job.svc, UploadState::Failed);
            }
            emit uploadFinished(job.id, job.svc, false, r.message);
        } else {
            job.notBeforeMs = QDateTime::currentMSecsSinceEpoch() + backoffMs(job.attempts - 1);
            m_queue.append(job);
        }
        break;
    }
    emit queueChanged(pendingCount());
    if (!fromBatch) {
        schedule();
    }
}

void LogUploadQueue::startLotwBatch()
{
    constexpr int si = static_cast<int>(UploadService::Lotw);
    if (m_busy[si] || !m_log) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<Job> batch;
    for (int k = static_cast<int>(m_queue.size()) - 1; k >= 0 && batch.size() < kLotwBatchMax; --k) {
        if (m_queue[k].svc == UploadService::Lotw && m_queue[k].notBeforeMs <= now) {
            batch.prepend(m_queue.takeAt(k));
        }
    }
    if (batch.isEmpty()) {
        return;
    }
    auto failAll = [this, &batch](UploadApi::Result::Outcome o, const QString& msg) {
        for (const Job& j : batch) {
            finishJob(j, UploadApi::Result{o, msg, {}}, true);
        }
    };
    if (m_tqslPath.isEmpty() || !QFileInfo(m_tqslPath).isExecutable()) {
        failAll(UploadApi::Result::Outcome::AuthFailed, tr("TQSL not found — install TrustedQSL"));
        return;
    }
    if (m_tqslLocation.isEmpty()) {
        failAll(UploadApi::Result::Outcome::AuthFailed, tr("TQSL station location not set"));
        return;
    }
    m_tqslFile = std::make_unique<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/aether_lotw_XXXXXX.adi"));
    if (!m_tqslFile->open()) {
        m_tqslFile.reset();
        failAll(UploadApi::Result::Outcome::RetryLater, tr("Cannot create temporary ADIF file"));
        return;
    }
    m_tqslFile->write(Adif::header(QCoreApplication::applicationVersion()));
    for (const Job& j : batch) {
        bool ok = false;
        const LogQso q = m_log->qso(j.id, &ok);
        if (ok) {
            m_tqslFile->write(Adif::record(q, uploadWriteOptions()));
        }
    }
    m_tqslFile->flush();
    m_tqslFile->close();

    m_busy[si] = true;
    m_lotwBatch = batch;
    m_tqsl = std::make_unique<QProcess>();
    m_tqsl->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_tqsl.get(), &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        m_tqslTimeout.stop();
        const QByteArray out = m_tqsl ? m_tqsl->readAll() : QByteArray();
        UploadApi::Result r = (st == QProcess::NormalExit)
            ? UploadApi::parseTqslExit(code, out)
            : UploadApi::Result{UploadApi::Result::Outcome::RetryLater,
                                tr("TQSL crashed or timed out"), {}};
        const QList<Job> batchDone = m_lotwBatch;
        m_lotwBatch.clear();
        if (m_tqsl) {
            m_tqsl.release()->deleteLater();
        }
        m_tqslFile.reset();
        for (const Job& j : batchDone) {
            finishJob(j, r, true);
        }
        m_busy[static_cast<int>(UploadService::Lotw)] = false;
        schedule();
    });
    connect(m_tqsl.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && m_tqsl) {
            qCWarning(lcLogbook) << "TQSL failed to start:" << m_tqsl->errorString();
            emit m_tqsl->finished(-1, QProcess::CrashExit);
        }
    });
    qCInfo(lcLogbook) << "Starting TQSL for" << batch.size() << "QSOs";
    m_tqsl->start(m_tqslPath, UploadApi::tqslArguments(m_tqslLocation, m_tqslFile->fileName()));
    m_tqslTimeout.start(kTqslTimeoutMs);
}

void LogUploadQueue::requestQrzStatus()
{
    QPointer<LogUploadQueue> self(this);
    withSecrets(UploadService::Qrz, [self](QStringList secrets) {
        if (!self) {
            return;
        }
        const UploadApi::HttpRequest req =
            UploadApi::qrzStatus(self->m_endpoint[static_cast<int>(UploadService::Qrz)],
                                 secrets.value(0));
        QNetworkRequest nr(req.url);
        nr.setHeader(QNetworkRequest::ContentTypeHeader, req.contentType);
        nr.setHeader(QNetworkRequest::UserAgentHeader, req.userAgent);
        nr.setTransferTimeout(kHttpTimeoutMs);
        QNetworkReply* reply = self->m_nam.post(nr, req.body);
        connect(reply, &QNetworkReply::finished, self, [self, reply]() {
            reply->deleteLater();
            if (!self) {
                return;
            }
            const QMap<QString, QString> kv = UploadApi::parseQrzKv(reply->readAll());
            emit self->qrzStatusReceived(kv.value("RESULT").toUpper() == "OK", kv);
        });
    });
}

} // namespace AetherSDR
