#pragma once

// ---------------------------------------------------------------------------
// LogUploaders — online log uploads (LoTW via TQSL, QRZ.com Logbook API,
// Club Log realtime API, eQSL) with a persistent retry queue.
//
// * Everything is asynchronous (QNetworkAccessManager / QProcess); nothing
//   blocks the GUI thread.
// * Per-QSO status is written back through Logbook::setUploadState().
// * Secrets (QRZ API key, Club Log application password, eQSL password)
//   go through CredentialStore: OS keychain via QtKeychain when built with
//   HAVE_KEYCHAIN, otherwise AppSettings in plain text (UI must warn —
//   CredentialStore::isSecure() == false).
// * The TQSL certificate password is NEVER stored or passed (-p is never
//   used).  If the certificate is password-protected TQSL itself prompts.
// * Request builders / response parsers are pure static functions so they
//   can be unit-tested against a local fake server.
// ---------------------------------------------------------------------------

#include "models/Logbook.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUrl>

#include <array>
#include <functional>
#include <memory>

class QTemporaryFile;
class QNetworkReply;

namespace AetherSDR {

// ── Credential storage ─────────────────────────────────────────────────────
class CredentialStore : public QObject {
    Q_OBJECT

public:
    enum class Backend { Auto, Keychain, AppSettingsPlain, Memory };

    // Keys used by the uploaders.
    static constexpr const char* kQrzApiKey        = "logbook_qrz_api_key";
    static constexpr const char* kClubLogPassword  = "logbook_clublog_app_password";
    static constexpr const char* kClubLogApiKey    = "logbook_clublog_api_key";
    static constexpr const char* kEqslPassword     = "logbook_eqsl_password";

    explicit CredentialStore(Backend backend = Backend::Auto, QObject* parent = nullptr);

    Backend backend() const { return m_backend; }
    bool isSecure() const { return m_backend == Backend::Keychain || m_backend == Backend::Memory; }
    static QString insecureWarning();

    // Async; callbacks are invoked on the GUI thread (possibly synchronously).
    void read(const QString& key, std::function<void(const QString& value)> cb);
    void write(const QString& key, const QString& value,
               std::function<void(bool ok, const QString& error)> cb = {});
    void remove(const QString& key);

private:
    Backend m_backend;
    QHash<QString, QString> m_memory;   // Memory backend (tests)
};

// ── Pure request / response helpers ────────────────────────────────────────
namespace UploadApi {

struct HttpRequest {
    QUrl       url;
    QByteArray body;          // application/x-www-form-urlencoded
    QByteArray contentType{"application/x-www-form-urlencoded"};
    QByteArray userAgent;
};

struct Result {
    enum class Outcome { Ok, Duplicate, Rejected, AuthFailed, RetryLater };
    Outcome outcome{Outcome::RetryLater};
    QString message;
    QString remoteId;         // QRZ LOGID
    bool success() const { return outcome == Outcome::Ok || outcome == Outcome::Duplicate; }
};

// Default endpoints (overridable for tests).
QUrl defaultEndpoint(UploadService s);
QByteArray userAgent();       // "AetherSDR/<version>" (QRZ requirement)

// Form encoding (RFC 3986 percent-encoding of every reserved char).
QByteArray formEncode(const QList<QPair<QString, QString>>& fields);

// QRZ Logbook API — POST https://logbook.qrz.com/api
HttpRequest qrzInsert(const QUrl& ep, const QString& apiKey, const QByteArray& adifRecord,
                      bool replace);
HttpRequest qrzStatus(const QUrl& ep, const QString& apiKey);
QMap<QString, QString> parseQrzKv(const QByteArray& body);
Result parseQrzResponse(int httpStatus, const QByteArray& body);

// Club Log realtime — POST https://clublog.org/realtime.php
// (email, Club Log *application* password, callsign, api, adif)
HttpRequest clubLogRealtime(const QUrl& ep, const QString& email, const QString& appPassword,
                            const QString& callsign, const QString& apiKey,
                            const QByteArray& adifRecord);
Result parseClubLogResponse(int httpStatus, const QByteArray& body);

// eQSL — POST https://www.eQSL.cc/qslcard/importADIF.cfm
HttpRequest eqslImport(const QUrl& ep, const QString& user, const QString& password,
                       const QByteArray& adifFile);
Result parseEqslResponse(int httpStatus, const QByteArray& body);

// TQSL (LoTW)
QString detectTqsl();         // AppSettings "LogUploadTqslPath", platform paths, PATH
QStringList tqslArguments(const QString& stationLocation, const QString& adifPath);
Result parseTqslExit(int exitCode, const QByteArray& output);

} // namespace UploadApi

// ── Upload queue ───────────────────────────────────────────────────────────
class LogUploadQueue : public QObject {
    Q_OBJECT

public:
    LogUploadQueue(Logbook* log, CredentialStore* creds, QObject* parent = nullptr);
    ~LogUploadQueue() override;

    // Config (AppSettings: LogUpload<Svc>Enabled, LogUploadAuto, …)
    void loadSettings();
    bool isEnabled(UploadService s) const { return m_enabled[static_cast<int>(s)]; }
    void setEnabled(UploadService s, bool on);
    void setAutoUpload(bool on);          // enqueue new QSOs automatically
    bool autoUpload() const { return m_autoUpload; }
    void setEndpoint(UploadService s, const QUrl& url) { m_endpoint[static_cast<int>(s)] = url; }
    void setTqslPath(const QString& p) { m_tqslPath = p; }
    void setRetryBaseMs(int ms) { m_retryBaseMs = ms; }
    void setMaxAttempts(int n) { m_maxAttempts = n; }

    // Account fields that are not secret.
    void setClubLogAccount(const QString& email, const QString& callsign);
    void setEqslUser(const QString& user);
    void setTqslStationLocation(const QString& loc);

    // Queue a QSO for a service (marks it Pending in the log).
    void enqueue(qint64 qsoId, UploadService s);
    void enqueue(const QVector<qint64>& ids, UploadService s);
    // Re-queue every QSO whose state is Pending / Failed (and Modified for
    // QRZ / Club Log) for enabled services — call at startup.
    int enqueueOutstanding();
    int pendingCount() const { return static_cast<int>(m_queue.size()); }
    bool isSuspended(UploadService s) const { return m_suspended[static_cast<int>(s)]; }
    void resume(UploadService s);          // after credentials were fixed

    // QRZ ACTION=STATUS (emits qrzStatusReceived).
    void requestQrzStatus();

public slots:
    void processNow();

signals:
    void uploadFinished(qint64 qsoId, AetherSDR::UploadService service, bool ok,
                        const QString& message);
    void serviceSuspended(AetherSDR::UploadService service, const QString& reason);
    void qrzStatusReceived(bool ok, const QMap<QString, QString>& data);
    void queueChanged(int pending);

private:
    struct Job {
        qint64        id{0};
        UploadService svc{UploadService::Qrz};
        int           attempts{0};
        qint64        notBeforeMs{0};
    };

    void onQsoAdded(const LogQso& q);
    void schedule();
    void startHttp(const Job& job);
    void startLotwBatch();
    void finishJob(const Job& job, const UploadApi::Result& r, bool fromBatch = false);
    void withSecrets(UploadService s, std::function<void(QStringList)> cb);
    int  backoffMs(int attempts) const;
    bool hasJobFor(qint64 id, UploadService s) const;

    QPointer<Logbook>        m_log;
    QPointer<CredentialStore> m_creds;
    QNetworkAccessManager    m_nam;
    QList<Job>               m_queue;
    std::array<bool, kUploadServiceCount> m_enabled{false, false, false, false};
    std::array<bool, kUploadServiceCount> m_busy{false, false, false, false};
    std::array<bool, kUploadServiceCount> m_suspended{false, false, false, false};
    std::array<QUrl, kUploadServiceCount> m_endpoint;
    bool      m_autoUpload{false};
    int       m_retryBaseMs{30000};
    int       m_maxAttempts{8};
    QTimer    m_timer;
    QString   m_tqslPath;
    QString   m_tqslLocation;
    QString   m_clubLogEmail;
    QString   m_clubLogCall;
    QString   m_eqslUser;

    // LoTW batch in flight
    std::unique_ptr<QProcess>       m_tqsl;
    std::unique_ptr<QTemporaryFile> m_tqslFile;
    QList<Job>                      m_lotwBatch;
    QTimer                          m_tqslTimeout;
};

} // namespace AetherSDR
