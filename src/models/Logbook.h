#pragma once

// ---------------------------------------------------------------------------
// Logbook — built-in QSO log for AetherSDR.
//
// Storage: SQLite through Qt Sql (QSQLITE driver), one file at
//   <AppSettings dir>/logbook.sqlite   (override: AppSettings "LogbookPath")
// WAL journal, busy_timeout 5 s.  Every write is a single statement or an
// explicit transaction, so a crash never leaves a half-written QSO.
//
// Threading: a Logbook lives on the GUI thread.  Only ADIF import runs on a
// worker thread (its own QSqlDatabase connection to the same file); results
// come back through queued signals.  All query methods are GUI-thread only.
//
// The in-memory worked cache (call → band/mode, DXCC → band/mode) makes
// isDupe()/workedStatus() O(1)-ish so they can be called per spot redraw.
// ---------------------------------------------------------------------------

#include "core/CtyDatParser.h"
#include "core/DxccWorkedStatus.h"   // DxccStatus enum (reused for spot colouring)

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QLoggingCategory>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

class QThread;

namespace AetherSDR {

Q_DECLARE_LOGGING_CATEGORY(lcLogbook)

class SliceModel;

// Upload targets.  Order is the index into LogQso::upload / uploadDate.
enum class UploadService : int { Lotw = 0, Qrz = 1, ClubLog = 2, Eqsl = 3 };
inline constexpr int kUploadServiceCount = 4;

enum class UploadState : int {
    NotSent     = 0,   // never queued
    Pending     = 1,   // queued / requested (ADIF 'R' for LoTW/eQSL)
    Uploaded    = 2,   // accepted by service (ADIF 'Y')
    Modified    = 3,   // edited after upload (ADIF 'M' for QRZ/Club Log)
    Failed      = 4,   // last attempt failed (retry queue will pick it up)
    DoNotUpload = 5,   // ADIF 'N'
};

// One logged contact.  (Named LogQso because AdifParser.h already declares
// AetherSDR::QsoRecord for the DXCC colouring feature.)
struct LogQso {
    qint64    id{0};                 // SQLite rowid; 0 = not yet stored

    QString   call;                  // upper case
    QString   band;                  // ADIF band enum, lower case ("20m")
    QString   mode;                  // ADIF MODE ("SSB", "CW", "FT8", "MFSK" …)
    QString   submode;               // ADIF SUBMODE ("USB", "FT4" …)
    double    freqMhz{0.0};          // TX frequency
    double    freqRxMhz{0.0};        // RX frequency (split), 0 = same
    QDateTime timeOn;                // UTC
    QDateTime timeOff;               // UTC (may be invalid)

    QString   rstSent;
    QString   rstRcvd;
    QString   name;
    QString   qth;
    QString   gridsquare;
    QString   state;                 // ADIF STATE (US state / VE province)

    // DXCC — filled from cty.dat when empty
    QString   country;               // entity name
    QString   dxccPrefix;            // cty.dat primary prefix ("G", "VK" …)
    int       dxcc{0};               // ADIF numeric entity (only if imported)
    QString   continent;
    int       cqZone{0};
    int       ituZone{0};

    QString   comment;

    // My station
    QString   stationCallsign;       // ADIF STATION_CALLSIGN
    QString   operatorCall;          // ADIF OPERATOR
    QString   myGrid;                // ADIF MY_GRIDSQUARE
    double    txPowerW{0.0};         // ADIF TX_PWR

    // Contest
    QString   contestId;             // ADIF CONTEST_ID
    int       srx{0};                // serial received
    int       stx{0};                // serial sent
    QString   srxString;             // received exchange (free text)
    QString   stxString;             // sent exchange (free text)

    // SO2R / slice provenance
    int       sliceId{-1};           // Flex slice index the QSO was logged from
    QString   so2rRole;              // "RUN" / "S&P" / empty

    // QSO audio (QsoRecorder WAV)
    QString   recordingPath;

    std::array<UploadState, kUploadServiceCount> upload{
        UploadState::NotSent, UploadState::NotSent,
        UploadState::NotSent, UploadState::NotSent};
    std::array<QDate, kUploadServiceCount> uploadDate{};

    // Unmapped ADIF fields (upper-case name → value), preserved round-trip.
    // Contest exchange fields without a dedicated column (ARRL_SECT, CLASS,
    // RX_PWR, …) live here too.
    QMap<QString, QString> extra;

    UploadState uploadState(UploadService s) const { return upload[static_cast<int>(s)]; }
    void setUploadState(UploadService s, UploadState st) { upload[static_cast<int>(s)] = st; }
};

// ── Pure helpers (no Logbook instance required) ────────────────────────────
namespace LogUtil {

QString freqToBand(double mhz);                      // "" if out of band
QString modeGroup(const QString& mode, const QString& submode = {}); // CW/PHONE/DATA
// Flex slice mode (USB, LSB, CW, DIGU, FDV …) → ADIF mode/submode.
void flexModeToAdif(const QString& flexMode, QString& mode, QString& submode);
QString normalizeCall(const QString& call);           // trim + upper
QString baseCall(const QString& call);                // W1AW/P → W1AW, VP2E/K1ABC → K1ABC
bool gridToLatLon(const QString& grid, double& lat, double& lon);   // centre of square
bool isValidGrid(const QString& grid);
double distanceKm(double lat1, double lon1, double lat2, double lon2);
double bearingDeg(double lat1, double lon1, double lat2, double lon2);
QString uploadServiceName(UploadService s);           // "LoTW", "QRZ", …

} // namespace LogUtil

// ── ADIF codec (ADIF 3.1.4 .adi) ──────────────────────────────────────────
namespace Adif {

inline constexpr const char* kAdifVersion = "3.1.4";

struct WriteOptions {
    bool includeAppFields{true};      // APP_AETHERSDR_* (slice, recording path …)
    bool includeUploadStatus{true};   // LOTW_QSL_SENT, QRZCOM_QSO_UPLOAD_STATUS …
    bool includeExtra{true};          // preserved unmapped fields
};

// Header block ending in <EOH>\n.
QByteArray header(const QString& programVersion);
// Single record ending in <EOR>\n.  UTF-8; lengths are UTF-8 byte counts.
QByteArray record(const LogQso& q, const WriteOptions& opt = {});
// One field "<NAME:len>value " (empty value → empty array).
QByteArray field(const QString& name, const QString& value);

// Streaming parser over raw bytes.  Handles header / no header, mixed case
// tags, type indicators (<F:3:N>), Latin-1 or UTF-8 values, and length
// values that count characters instead of bytes.  Callback returns false
// to abort.  Returns number of records delivered.
using RecordFn = std::function<bool(const QMap<QString, QString>& fields)>;
qint64 parse(const QByteArray& data, const RecordFn& fn,
             const std::function<void(qint64 bytePos)>& progress = {});

// Map ADIF field map → LogQso (does not touch id; unknown fields → extra).
LogQso toQso(const QMap<QString, QString>& fields);

} // namespace Adif

// Search filter for Logbook::search().
struct LogFilter {
    QString   text;          // substring of call / name / comment / qth
    QString   call;          // exact call (normalised) — overrides text
    QString   band;
    QString   modeGroup;     // CW / PHONE / DATA
    QString   contestId;
    QDateTime from;          // inclusive
    QDateTime to;            // inclusive
    int       limit{0};      // 0 = unlimited; newest first
};

struct DupeRules {
    bool      perBand{true};
    bool      perMode{true};   // by mode group (CW / PHONE / DATA)
    QString   contestId;       // non-empty: only QSOs of this contest count
    QDateTime since;           // valid: only QSOs on/after this time count
};

struct WorkedStatus {
    QString dxccPrefix;
    QString country;
    int     cqZone{0};
    int     ituZone{0};
    QString continent;
    double  lat{0.0};
    double  lon{0.0};
    bool dxccKnown{false};
    bool dxccWorked{false};
    bool dxccBandWorked{false};
    bool dxccBandModeWorked{false};
    bool callWorked{false};
    bool callBandWorked{false};
    bool callBandModeWorked{false};
    int  callQsoCount{0};

    // Same semantics as DxccColorProvider (NewDxcc/NewBand/NewMode/Worked).
    DxccStatus dxccStatus() const;
};

struct ImportResult {
    qint64  added{0};
    qint64  skippedDupes{0};
    qint64  errors{0};
    QString errorString;
};

class Logbook : public QObject {
    Q_OBJECT

public:
    explicit Logbook(QObject* parent = nullptr);
    ~Logbook() override;

    // Open (create if needed) the SQLite logbook.  Empty path → default
    // location (AppSettings "LogbookPath" or <config>/AetherSDR/logbook.sqlite).
    bool open(const QString& path = {});
    void close();
    bool isOpen() const;
    QString path() const { return m_path; }
    QString lastError() const { return m_lastError; }
    static QString defaultPath();

    // cty.dat for DXCC / zone resolution.  If none loaded, tries ":/cty.dat".
    bool loadCtyDat(const QString& pathOrResource = QStringLiteral(":/cty.dat"));
    const CtyDatParser& cty() const { return m_cty; }

    // ── CRUD ─────────────────────────────────────────────────────────────
    qint64 addQso(LogQso q);                // returns id, 0 on failure
    bool   updateQso(const LogQso& q);      // by q.id; flags uploads Modified
    bool   deleteQso(qint64 id);
    bool   deleteQsos(const QVector<qint64>& ids);
    LogQso qso(qint64 id, bool* ok = nullptr) const;
    QVector<LogQso> search(const LogFilter& f = {}) const;
    qint64 count() const;

    // Upload bookkeeping (does not flag Modified).
    bool setUploadState(qint64 id, UploadService s, UploadState st,
                        const QDate& date = {});
    QVector<LogQso> qsosWithUploadState(UploadService s,
                                        const QSet<UploadState>& states,
                                        int limit = 0) const;

    // Attach a QsoRecorder WAV to a QSO.
    bool setRecordingPath(qint64 id, const QString& path);
    // Link a finished recording to QSOs whose time_on falls inside
    // [start - 2 min, end + 2 min] and that have no recording yet.
    // Returns the number of QSOs linked.
    int linkRecording(const QString& path, const QDateTime& startUtc,
                      const QDateTime& endUtc);

    // ── Queries ──────────────────────────────────────────────────────────
    bool isDupe(const QString& call, const QString& band, const QString& mode,
                const DupeRules& rules = {}) const;
    WorkedStatus workedStatus(const QString& call, const QString& band = {},
                              const QString& mode = {}) const;
    // DXCC-only query (for spot colouring when only an entity is known)
    DxccStatus dxccStatus(const QString& dxccPrefix, const QString& band,
                          const QString& modeGroup) const;
    int maxSerialSent(const QString& contestId) const;

    // Fill DXCC/country/zones/continent from cty.dat where empty.
    void fillDxcc(LogQso& q) const;
    // Fill station fields (STATION_CALLSIGN, MY_GRIDSQUARE, OPERATOR, TX_PWR)
    // from AppSettings where empty.
    void fillStation(LogQso& q) const;

    // Draft a QSO from the current slice (freq/mode/band/time/station).
    LogQso draftFromSlice(const SliceModel* slice) const;
    LogQso draftFromFrequency(double freqMhz, const QString& flexMode,
                              int sliceId = -1) const;

    // Station identity (falls back to AppSettings "LogbookStationCallsign",
    // then RadioModel callsign set by MainWindow).
    void setStationCallsign(const QString& call) { m_stationCallsign = call; }
    QString stationCallsign() const;

    // ── ADIF ─────────────────────────────────────────────────────────────
    // Synchronous import (tests / small files).  Runs on the calling thread.
    ImportResult importAdif(const QString& filePath, bool skipDupes = true);
    ImportResult importAdifData(const QByteArray& data, bool skipDupes = true);
    // Async import on a worker thread; emits importProgress/importFinished.
    bool importAdifAsync(const QString& filePath, bool skipDupes = true);
    bool isImporting() const { return m_importThread != nullptr; }
    void cancelImport();

    // Export (atomic QSaveFile).  Empty ids → all QSOs matching filter.
    bool exportAdif(const QString& filePath, const QVector<qint64>& ids = {},
                    const Adif::WriteOptions& opt = {}) const;
    bool exportAdif(const QString& filePath, const LogFilter& filter,
                    const Adif::WriteOptions& opt = {}) const;

    // Ingest one ADIF record (WSJT-X LoggedADIF, FreeDV, ALE, …).  Missing
    // station/DXCC fields are filled.  Returns new id, 0 if rejected or
    // (skipDupes) an identical QSO within ±60 s already exists.
    qint64 ingestAdifRecord(const QString& adif, bool skipDupes = true);

    // WSJT-X UDP message type 12 (LoggedADIF) → ADIF text; empty if the
    // datagram is not a LoggedADIF message.  See WSJT-X NetworkMessage.hpp.
    static QString wsjtxLoggedAdif(const QByteArray& datagram);

signals:
    void qsoAdded(const AetherSDR::LogQso& q);
    void qsoUpdated(const AetherSDR::LogQso& q);
    void qsoDeleted(qint64 id);
    void logReset();                          // bulk change (import/delete many)
    void importProgress(qint64 bytesDone, qint64 bytesTotal, qint64 added);
    void importFinished(const AetherSDR::ImportResult& result);

private:
    struct CacheEntry {
        QString exactCall;
        QString band;
        quint8  modeGroup{0};   // bit: 1=CW 2=PHONE 4=DATA
        QString contestId;
        qint64  timeOn{0};      // epoch secs
    };

    bool createSchema();
    void rebuildCache();
    void cacheAdd(const LogQso& q);
    static quint8 modeBit(const QString& modeGroup);
    QString connectionName() const { return m_connName; }
    ImportResult importInto(const QString& connName, const QByteArray& data,
                            bool skipDupes, const std::atomic<bool>* cancel,
                            const std::function<void(qint64, qint64)>& progress) const;

    QString          m_path;
    QString          m_connName;
    QString          m_lastError;
    QString          m_stationCallsign;
    CtyDatParser     m_cty;

    // base call → entries
    QHash<QString, QVector<CacheEntry>> m_byCall;
    // dxcc prefix → band → mode-group bits
    QHash<QString, QHash<QString, quint8>> m_dxcc;

    std::unique_ptr<QThread> m_importThread;     // non-null while importing
    std::atomic<bool> m_cancelImport{false};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::LogQso)
Q_DECLARE_METATYPE(AetherSDR::ImportResult)
