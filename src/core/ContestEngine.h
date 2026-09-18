#pragma once

// ---------------------------------------------------------------------------
// ContestEngine — contest profiles, exchange handling, serial numbers,
// dupe checking, running score, band-map spot annotations, F1–F8 message
// macros, and SO2R slice tracking.
//
// All state lives on the GUI thread.  The engine reads QSOs from a Logbook
// and recomputes the score whenever the log changes (O(n) over this
// contest's QSOs only).
//
// TX SAFETY: sendCwMacro() / sendVoiceMacro() key the transmitter through
// CwxModel / DvkModel.  They refuse unless the caller has explicitly armed
// macro TX (setMacroTxArmed(true)) AND the optional TX guard callback
// returns true.  Nothing is armed by default.
// ---------------------------------------------------------------------------

#include "models/CwxModel.h"
#include "models/DvkModel.h"
#include "models/Logbook.h"
#include "models/SliceModel.h"

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <functional>

namespace AetherSDR {

struct SpotData;

enum class ContestType {
    None,           // plain logging, no contest
    GenericSerial,  // RST + serial
    CqWw,           // RST + CQ zone
    ArrlDx,         // W/VE: RST + state/prov; DX: RST + power
    ArrlFieldDay,   // class + ARRL/RAC section
    Naqp,           // name + state/prov/DXCC
    NaSprint,       // serial + name + state/prov/DXCC
    Custom,         // user template
};

// One received-exchange field.
struct ExchangeField {
    QString key;         // token: RST, SN, ZONE, STATE, NAME, CLASS, SECT, PWR, QTH, or custom
    QString label;       // UI label
    QString adifField;   // where it is stored in LogQso (RST_RCVD, SRX, CQZ, STATE, NAME, CLASS, ARRL_SECT, RX_PWR, QTH, SRX_STRING)
    bool    numeric{false};
    bool    isMult{false};   // Custom templates: '*' prefix marks the multiplier field
};

struct ContestProfile {
    ContestType type{ContestType::None};
    QString     key;             // stable id used in AppSettings ("CQWW", …)
    QString     name;            // display
    QString     adifIdBase;      // CONTEST_ID prefix ("CQ-WW", "ARRL-DX" …)
    bool        modeInId{true};  // append -CW / -SSB / -RTTY to CONTEST_ID
    QVector<ExchangeField> rcvd;
    QString     sentTemplate;    // macro template for {EXCH}
    bool        dupePerBand{true};
    bool        dupePerMode{true};
    bool        usesSerial{false};
};

struct ContestScore {
    int  qsos{0};
    int  dupes{0};
    int  points{0};
    int  mults{0};
    long long score{0};
    QMap<QString, int> qsosPerBand;
    QMap<QString, int> multsByCategory;   // "Zones", "Countries", "States", …
};

// Band-map annotation for one spot.
struct SpotAnnotation {
    enum class Kind { Unknown, Dupe, NewMult, NewDxcc, NewBand, NewMode, NewCall, Worked };
    Kind    kind{Kind::Unknown};
    QString label;      // short tag ("DUPE", "MULT", "NEW" …), empty for none
    QColor  color;      // suggested marker colour (invalid → leave default)
    QString tooltip;
};

// Context for macro expansion.
struct MacroContext {
    QString myCall;
    QString call;        // his call
    QString rst{"599"};
    int     serial{0};
    QString exchange;    // expanded sent exchange (without RST unless template has it)
    QString name;        // his name (for "TU NAME")
    QString myName;
    QString myState;
    QString myZone;
    QString myClass;
    QString mySection;
    QString myPower;
    bool    cutNumbers{false};   // 0→T, 9→N in RST/serial
};

class ContestEngine : public QObject {
    Q_OBJECT

public:
    explicit ContestEngine(Logbook* log, QObject* parent = nullptr);

    // ── Profiles ─────────────────────────────────────────────────────────
    static QVector<ContestProfile> builtinProfiles();
    static ContestProfile profileFor(ContestType t);
    // Build a profile from a custom template like "RST SN *STATE NAME".
    static ContestProfile customProfile(const QString& templ, const QString& adifId);

    void setProfile(const ContestProfile& p);
    void setProfile(ContestType t) { setProfile(profileFor(t)); }
    const ContestProfile& profile() const { return m_profile; }
    bool isActive() const { return m_profile.type != ContestType::None; }

    // Mode variant for CONTEST_ID ("CW", "SSB", "RTTY").
    void setModeVariant(const QString& v);
    QString modeVariant() const { return m_modeVariant; }
    QString contestId() const;

    // Only QSOs on/after this time count (dupes, score, serials).
    void setStartTime(const QDateTime& utc);
    QDateTime startTime() const { return m_start; }

    // My exchange values keyed by token (STATE, ZONE, NAME, CLASS, SECT, PWR, QTH).
    void setMyExchange(const QMap<QString, QString>& ex) { m_myExchange = ex; }
    QMap<QString, QString> myExchange() const { return m_myExchange; }
    void setFieldDayPowerMultiplier(int m) { m_fdPowerMult = std::max(1, m); recompute(); }

    // Load / save profile key, mode variant, my exchange, start time.
    void loadSettings();
    void saveSettings() const;

    // ── Serial numbers ───────────────────────────────────────────────────
    int nextSerial() const;         // max(STX in contest) + 1

    // ── Exchange ─────────────────────────────────────────────────────────
    // Parse a free-typed received exchange ("599 14" / "1A EMA") into field
    // tokens using the profile's field order.  Unmatched text is kept in
    // the "_REST" key.
    QMap<QString, QString> parseExchange(const QString& text) const;
    // Apply field values to a QSO: RST_RCVD, SRX, CQZ, STATE, NAME, …
    // Also stamps CONTEST_ID, STX, STX_STRING, SRX_STRING.
    void applyExchange(LogQso& q, const QMap<QString, QString>& rcvd, int serialSent) const;
    // Sent exchange text for this QSO (template with {SN} etc. expanded).
    QString sentExchange(int serial) const;

    // ── Dupes & score ────────────────────────────────────────────────────
    DupeRules dupeRules() const;
    bool isDupe(const QString& call, const QString& band, const QString& mode) const;
    const ContestScore& score() const { return m_score; }
    // Would this contact be a new multiplier?  exchangeHint may carry the
    // received field values if known (e.g. STATE).
    bool isNewMult(const QString& call, const QString& band,
                   const QMap<QString, QString>& exchangeHint = {}) const;
    // Points this contact would earn (0 for dupes / invalid).
    int qsoPoints(const LogQso& q) const;
    // Pure scoring over a set of QSOs (exposed for tests).
    ContestScore computeScore(const QVector<LogQso>& qsos) const;

    // ── Band map ─────────────────────────────────────────────────────────
    SpotAnnotation annotateSpot(const QString& call, double freqMhz,
                                const QString& spotMode) const;
    SpotAnnotation annotateSpot(const SpotData& spot) const;
    // Colours (client-side display preference; maintainer review needed).
    QColor colorDupe{0x70, 0x70, 0x70};
    QColor colorNewMult{0xFF, 0x30, 0x30};
    QColor colorNewDxcc{0xFF, 0x30, 0x30};
    QColor colorNewBand{0xFF, 0x8C, 0x00};
    QColor colorNewMode{0xFF, 0xD7, 0x00};
    QColor colorNewCall{0x40, 0xC0, 0xFF};
    QColor colorWorked{0x60, 0x60, 0x60};

    // ── Macros (F1–F8) ───────────────────────────────────────────────────
    static constexpr int kMacroCount = 8;
    static QString defaultCwMacro(ContestType t, int fkey);   // 1-based
    QString cwMacro(int fkey) const;                         // 1-based
    void    setCwMacro(int fkey, const QString& templ);
    int     voiceMacroSlot(int fkey) const;                  // DVK recording id, -1 none
    void    setVoiceMacroSlot(int fkey, int dvkId);
    // Expand {MYCALL} {CALL} {RST} {SN} {NR} {EXCH} {NAME} {MYNAME}
    // {STATE} {ZONE} {CLASS} {SECT} {PWR} and "!" (= his call, N1MM style).
    static QString expandMacro(const QString& templ, const MacroContext& ctx);
    MacroContext macroContext(const QString& hisCall, const QString& hisName,
                              int serial) const;

    void setCwxModel(CwxModel* m) { m_cwx = m; }
    void setDvkModel(DvkModel* m) { m_dvk = m; }
    void setMacroTxArmed(bool on) { m_macroTxArmed = on; }
    bool macroTxArmed() const { return m_macroTxArmed; }
    // Extra guard, e.g. [this]{ return !m_rxOnlyMode && radio connected; }
    void setTxGuard(std::function<bool()> g) { m_txGuard = std::move(g); }
    // Returns false (and sends nothing) unless armed + guard passes.
    bool sendCwMacro(int fkey, const QString& hisCall, const QString& hisName, int serial);
    bool sendVoiceMacro(int fkey);

signals:
    void scoreChanged(const AetherSDR::ContestScore& s);
    void profileChanged();
    void macroRefused(const QString& reason);
    void macroSent(int fkey, const QString& text);

private:
    void recompute();
    bool    iAmWve() const;
    QString myContinent() const;
    QString myDxcc() const;
    bool    txAllowed(QString* why) const;

    Logbook*          m_log{nullptr};
    ContestProfile    m_profile;
    QString           m_modeVariant{"CW"};
    QDateTime         m_start;
    QMap<QString, QString> m_myExchange;
    int               m_fdPowerMult{2};
    ContestScore      m_score;
    // category|band|value strings already worked (for isNewMult)
    QSet<QString>     m_multsWorked;

    QPointer<CwxModel> m_cwx;
    QPointer<DvkModel> m_dvk;
    bool               m_macroTxArmed{false};
    std::function<bool()> m_txGuard;
};

// ---------------------------------------------------------------------------
// So2rController — tracks two slices (radio A / radio B), their run/S&P
// role, which one has focus (keyboard entry) and which is the TX slice.
// swapTx() moves TX to the other slice via SliceModel::setTxSlice(true)
// (a radio command, NOT keying).  MainWindow binds hotkeys to swapTx() /
// swapFocus() / toggleRole() via ShortcutManager.
// ---------------------------------------------------------------------------
class So2rController : public QObject {
    Q_OBJECT

public:
    enum class Role { Run, SearchPounce };
    explicit So2rController(QObject* parent = nullptr);

    void setSlices(SliceModel* a, SliceModel* b);
    SliceModel* slice(int radio) const;            // 0 = A, 1 = B
    int  focusRadio() const { return m_focus; }
    SliceModel* focusedSlice() const { return slice(m_focus); }
    int  txRadio() const;                          // -1 unknown
    Role role(int radio) const { return m_role[radio & 1]; }
    void setRole(int radio, Role r);
    static QString roleTag(Role r) { return r == Role::Run ? QStringLiteral("RUN") : QStringLiteral("S&P"); }

    // Stamp sliceId + so2rRole on a QSO being logged from the focused radio.
    void stampQso(LogQso& q) const;

public slots:
    void swapFocus();
    void setFocus(int radio);
    bool swapTx();            // TX → other slice; false if not possible
    bool setTxRadio(int radio);
    void toggleRole();        // on focused radio

signals:
    void focusChanged(int radio);
    void roleChanged(int radio, AetherSDR::So2rController::Role role);
    void txRequested(int radio);

private:
    QPointer<SliceModel> m_slice[2];
    Role m_role[2]{Role::Run, Role::SearchPounce};
    int  m_focus{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ContestScore)
