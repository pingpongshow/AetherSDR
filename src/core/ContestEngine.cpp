#include "ContestEngine.h"

#include "core/AppSettings.h"
#include "models/CwxModel.h"
#include "models/DvkModel.h"
#include "models/SliceModel.h"
#include "models/SpotModel.h"

#include <QRegularExpression>

#include <algorithm>

namespace AetherSDR {

namespace {

bool isWveDxcc(const QString& pfx)
{
    // cty.dat primary prefixes: K = United States (incl. KL7/KH6? no — those
    // are separate entities), VE = Canada.  ARRL DX treats 48 states + DC and
    // VE as "W/VE"; KL7/KH6 are DX.
    return pfx == "K" || pfx == "VE";
}

bool isAllDigits(const QString& s)
{
    if (s.isEmpty()) {
        return false;
    }
    return std::all_of(s.cbegin(), s.cend(), [](QChar c) { return c.isDigit(); });
}

bool hasLetter(const QString& s)
{
    return std::any_of(s.cbegin(), s.cend(), [](QChar c) { return c.isLetter(); });
}

bool looksLikeRst(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("^[1-5][1-9N][1-9N]?$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(s).hasMatch();
}

QString cut(const QString& s)
{
    QString o = s;
    o.replace('0', 'T');
    o.replace('9', 'N');
    return o;
}

ExchangeField fld(const char* key, const char* label, const char* adif, bool numeric = false)
{
    ExchangeField f;
    f.key = QString::fromLatin1(key);
    f.label = QString::fromLatin1(label);
    f.adifField = QString::fromLatin1(adif);
    f.numeric = numeric;
    return f;
}

// Read an exchange value back from a stored QSO.
QString fieldValue(const LogQso& q, const QString& adif)
{
    if (adif == "RST_RCVD") { return q.rstRcvd; }
    if (adif == "SRX") { return q.srx > 0 ? QString::number(q.srx) : QString(); }
    if (adif == "CQZ") { return q.cqZone > 0 ? QString::number(q.cqZone) : QString(); }
    if (adif == "STATE") { return q.state; }
    if (adif == "NAME") { return q.name; }
    if (adif == "QTH") { return q.qth; }
    if (adif == "SRX_STRING") { return q.srxString; }
    return q.extra.value(adif);
}

void setFieldValue(LogQso& q, const QString& adif, const QString& v)
{
    if (adif == "RST_RCVD") { q.rstRcvd = v; }
    else if (adif == "SRX") { q.srx = v.toInt(); }
    else if (adif == "CQZ") { if (v.toInt() > 0) { q.cqZone = v.toInt(); } }
    else if (adif == "STATE") { q.state = v.toUpper(); }
    else if (adif == "NAME") { q.name = v; }
    else if (adif == "QTH") { q.qth = v; }
    else if (adif == "SRX_STRING") { q.srxString = v; }
    else if (!v.isEmpty()) { q.extra.insert(adif, v.toUpper()); }
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Profiles
// ═══════════════════════════════════════════════════════════════════════════

ContestProfile ContestEngine::profileFor(ContestType t)
{
    ContestProfile p;
    p.type = t;
    switch (t) {
    case ContestType::None:
        p.key = "NONE"; p.name = tr("No contest"); p.modeInId = false;
        p.sentTemplate = "{RST}";
        break;
    case ContestType::GenericSerial:
        p.key = "SERIAL"; p.name = tr("Generic serial"); p.adifIdBase = "";
        p.modeInId = false; p.usesSerial = true;
        p.rcvd = {fld("RST", "RST", "RST_RCVD"), fld("SN", "Serial", "SRX", true)};
        p.sentTemplate = "{RST} {SN}";
        break;
    case ContestType::CqWw:
        p.key = "CQWW"; p.name = "CQ WW DX"; p.adifIdBase = "CQ-WW";
        p.rcvd = {fld("RST", "RST", "RST_RCVD"), fld("ZONE", "Zone", "CQZ", true)};
        p.sentTemplate = "{RST} {ZONE}";
        break;
    case ContestType::ArrlDx:
        p.key = "ARRLDX"; p.name = "ARRL DX"; p.adifIdBase = "ARRL-DX";
        // W/VE receive power, DX receive state — one field covers both.
        p.rcvd = {fld("RST", "RST", "RST_RCVD"), fld("STATE", "State/Power", "STATE")};
        p.sentTemplate = "{RST} {STATE}";   // DX side: {PWR} (see sentExchange)
        break;
    case ContestType::ArrlFieldDay:
        p.key = "ARRLFD"; p.name = "ARRL Field Day"; p.adifIdBase = "ARRL-FIELD-DAY";
        p.modeInId = false;
        p.rcvd = {fld("CLASS", "Class", "CLASS"), fld("SECT", "Section", "ARRL_SECT")};
        p.sentTemplate = "{CLASS} {SECT}";
        break;
    case ContestType::Naqp:
        p.key = "NAQP"; p.name = "NAQP"; p.adifIdBase = "NAQP";
        p.dupePerMode = false;   // separate contests per mode
        p.rcvd = {fld("NAME", "Name", "NAME"), fld("STATE", "State/Prov/DXCC", "STATE")};
        p.sentTemplate = "{MYNAME} {STATE}";
        break;
    case ContestType::NaSprint:
        p.key = "SPRINT"; p.name = "NA Sprint"; p.adifIdBase = "NA-SPRINT";
        p.dupePerMode = false; p.usesSerial = true;
        p.rcvd = {fld("SN", "Serial", "SRX", true), fld("NAME", "Name", "NAME"),
                  fld("STATE", "State/Prov/DXCC", "STATE")};
        p.sentTemplate = "{SN} {MYNAME} {STATE}";
        break;
    case ContestType::Custom:
        return customProfile(QStringLiteral("RST SN"), QString());
    }
    return p;
}

QVector<ContestProfile> ContestEngine::builtinProfiles()
{
    return {profileFor(ContestType::None), profileFor(ContestType::GenericSerial),
            profileFor(ContestType::CqWw), profileFor(ContestType::ArrlDx),
            profileFor(ContestType::ArrlFieldDay), profileFor(ContestType::Naqp),
            profileFor(ContestType::NaSprint), profileFor(ContestType::Custom)};
}

ContestProfile ContestEngine::customProfile(const QString& templ, const QString& adifId)
{
    ContestProfile p;
    p.type = ContestType::Custom;
    p.key = "CUSTOM";
    p.name = tr("Custom");
    p.adifIdBase = adifId.trimmed().toUpper();
    p.modeInId = false;
    QStringList sent;
    for (QString tok : templ.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
        bool mult = false;
        if (tok.startsWith('*')) {
            mult = true;
            tok = tok.mid(1);
        }
        tok = tok.toUpper();
        ExchangeField f;
        if (tok == "RST") { f = fld("RST", "RST", "RST_RCVD"); }
        else if (tok == "SN" || tok == "NR") { f = fld("SN", "Serial", "SRX", true); p.usesSerial = true; tok = "SN"; }
        else if (tok == "ZONE") { f = fld("ZONE", "Zone", "CQZ", true); }
        else if (tok == "STATE") { f = fld("STATE", "State", "STATE"); }
        else if (tok == "NAME") { f = fld("NAME", "Name", "NAME"); }
        else if (tok == "CLASS") { f = fld("CLASS", "Class", "CLASS"); }
        else if (tok == "SECT") { f = fld("SECT", "Section", "ARRL_SECT"); }
        else if (tok == "PWR") { f = fld("PWR", "Power", "RX_PWR"); }
        else if (tok == "QTH") { f = fld("QTH", "QTH", "QTH"); }
        else {
            f.key = tok;
            f.label = tok.left(1) + tok.mid(1).toLower();
            f.adifField = QStringLiteral("APP_AETHERSDR_EX_") + tok;
        }
        f.isMult = mult;
        p.rcvd.append(f);
        sent << (tok == "NAME" ? QStringLiteral("{MYNAME}") : QStringLiteral("{%1}").arg(tok));
    }
    p.sentTemplate = sent.join(' ');
    return p;
}

// ═══════════════════════════════════════════════════════════════════════════
// ContestEngine
// ═══════════════════════════════════════════════════════════════════════════

ContestEngine::ContestEngine(Logbook* log, QObject* parent)
    : QObject(parent)
    , m_log(log)
    , m_profile(profileFor(ContestType::None))
{
    qRegisterMetaType<AetherSDR::ContestScore>();
    if (m_log) {
        connect(m_log, &Logbook::qsoAdded, this, [this](const LogQso&) { recompute(); });
        connect(m_log, &Logbook::qsoUpdated, this, [this](const LogQso&) { recompute(); });
        connect(m_log, &Logbook::qsoDeleted, this, [this](qint64) { recompute(); });
        connect(m_log, &Logbook::logReset, this, &ContestEngine::recompute);
    }
}

void ContestEngine::setProfile(const ContestProfile& p)
{
    m_profile = p;
    recompute();
    emit profileChanged();
}

void ContestEngine::setModeVariant(const QString& v)
{
    const QString u = v.trimmed().toUpper();
    m_modeVariant = (u == "PH" || u == "PHONE") ? QStringLiteral("SSB") : u;
    recompute();
    emit profileChanged();
}

QString ContestEngine::contestId() const
{
    if (m_profile.type == ContestType::None) {
        return {};
    }
    if (m_profile.adifIdBase.isEmpty()) {
        // Generic serial contest without an ADIF id: use a local tag so
        // dupes/serials are still scoped.
        return QStringLiteral("AETHER-") + m_profile.key;
    }
    return m_profile.modeInId ? m_profile.adifIdBase + '-' + m_modeVariant
                              : m_profile.adifIdBase;
}

void ContestEngine::setStartTime(const QDateTime& utc)
{
    m_start = utc;
    recompute();
}

void ContestEngine::loadSettings()
{
    auto& s = AppSettings::instance();
    const QString key = s.value("ContestProfile", "NONE").toString();
    ContestProfile p = profileFor(ContestType::None);
    for (const ContestProfile& c : builtinProfiles()) {
        if (c.key == key) {
            p = c;
        }
    }
    if (p.type == ContestType::Custom) {
        p = customProfile(s.value("ContestCustomTemplate", "RST SN").toString(),
                          s.value("ContestCustomId", QString()).toString());
    }
    m_modeVariant = s.value("ContestModeVariant", "CW").toString();
    m_start = QDateTime::fromString(s.value("ContestStartUtc", QString()).toString(), Qt::ISODate);
    m_fdPowerMult = std::max(1, s.value("ContestFdPowerMult", "2").toInt());
    m_myExchange.clear();
    for (const char* tok : {"STATE", "ZONE", "NAME", "CLASS", "SECT", "PWR", "QTH"}) {
        const QString v = s.value(QStringLiteral("ContestMyExchange_") + tok, QString()).toString();
        if (!v.isEmpty()) {
            m_myExchange.insert(QString::fromLatin1(tok), v);
        }
    }
    setProfile(p);
}

void ContestEngine::saveSettings() const
{
    auto& s = AppSettings::instance();
    s.setValue("ContestProfile", m_profile.key);
    s.setValue("ContestModeVariant", m_modeVariant);
    s.setValue("ContestStartUtc", m_start.isValid() ? m_start.toString(Qt::ISODate) : QString());
    s.setValue("ContestFdPowerMult", QString::number(m_fdPowerMult));
    for (auto it = m_myExchange.cbegin(); it != m_myExchange.cend(); ++it) {
        s.setValue(QStringLiteral("ContestMyExchange_") + it.key(), it.value());
    }
    s.save();
}

int ContestEngine::nextSerial() const
{
    if (!m_log || !isActive()) {
        return 1;
    }
    return m_log->maxSerialSent(contestId()) + 1;
}

QMap<QString, QString> ContestEngine::parseExchange(const QString& text) const
{
    QMap<QString, QString> out;
    QStringList toks = text.trimmed().toUpper().split(QRegularExpression("\\s+"),
                                                      Qt::SkipEmptyParts);
    QVector<bool> used(toks.size(), false);
    const int nonRst = static_cast<int>(std::count_if(
        m_profile.rcvd.cbegin(), m_profile.rcvd.cend(),
        [](const ExchangeField& f) { return f.key != "RST"; }));

    for (const ExchangeField& f : m_profile.rcvd) {
        int pick = -1;
        // Power ("100", "KW") and ARRL DX's state/power field take any token.
        const bool anyToken = f.key == "PWR" || f.key == "CLASS" ||
            (m_profile.type == ContestType::ArrlDx && f.key == "STATE");
        for (int i = 0; i < toks.size() && pick < 0; ++i) {
            if (used[i]) {
                continue;
            }
            const QString& t = toks[i];
            if (f.key == "RST") {
                // Only take an RST if there are more tokens than other fields
                // (so "14" in CQ WW is a zone, not an RST).
                if (looksLikeRst(t) && toks.size() > nonRst) {
                    pick = i;
                }
                break;   // RST must be the first token if present
            }
            if (f.numeric) {
                QString v = t;
                if (!isAllDigits(v)) {
                    // Cut numbers: T=0, N=9, O=0
                    v.replace('T', '0').replace('N', '9').replace('O', '0');
                    if (!isAllDigits(v)) {
                        continue;
                    }
                }
                pick = i;
                toks[i] = v;
            } else if (anyToken || hasLetter(t)) {
                pick = i;
            }
        }
        if (pick >= 0) {
            used[pick] = true;
            QString v = toks[pick];
            if (f.numeric) {
                v = QString::number(v.toInt());   // strip leading zeros
            } else if (f.key == "RST") {
                v.replace('N', '9');              // 5NN → 599
            }
            out.insert(f.key, v);
        }
    }
    QStringList rest;
    for (int i = 0; i < toks.size(); ++i) {
        if (!used[i]) {
            rest << toks[i];
        }
    }
    if (!rest.isEmpty()) {
        out.insert(QStringLiteral("_REST"), rest.join(' '));
    }
    return out;
}

QString ContestEngine::sentExchange(int serial) const
{
    QString templ = m_profile.sentTemplate;
    if (m_profile.type == ContestType::ArrlDx && !iAmWve()) {
        templ = QStringLiteral("{RST} {PWR}");
    }
    MacroContext ctx = macroContext(QString(), QString(), serial);
    ctx.cutNumbers = false;   // log the uncut form
    ctx.exchange.clear();
    return expandMacro(templ, ctx).simplified();
}

void ContestEngine::applyExchange(LogQso& q, const QMap<QString, QString>& rcvd,
                                  int serialSent) const
{
    if (!isActive()) {
        return;
    }
    q.contestId = contestId();
    QStringList parts;
    for (const ExchangeField& f : m_profile.rcvd) {
        const QString v = rcvd.value(f.key).trimmed();
        if (v.isEmpty()) {
            if (f.key == "RST" && !q.rstRcvd.isEmpty()) {
                parts << q.rstRcvd;
            }
            continue;
        }
        if (m_profile.type == ContestType::ArrlDx && f.key == "STATE" && !iAmWve()) {
            // DX side receives W/VE state; W/VE side receives DX power.
            setFieldValue(q, "STATE", v);
        } else if (m_profile.type == ContestType::ArrlDx && f.key == "STATE" && iAmWve()) {
            q.extra.insert(QStringLiteral("RX_PWR"), v);
        } else {
            setFieldValue(q, f.adifField, v);
        }
        parts << v;
    }
    if (rcvd.contains("_REST")) {
        parts << rcvd.value("_REST");
    }
    q.srxString = parts.join(' ');
    if (m_profile.usesSerial || serialSent > 0) {
        q.stx = serialSent;
    }
    q.stxString = sentExchange(serialSent);
}

DupeRules ContestEngine::dupeRules() const
{
    DupeRules r;
    r.perBand = m_profile.dupePerBand;
    r.perMode = m_profile.dupePerMode;
    r.contestId = isActive() ? contestId() : QString();
    r.since = m_start;
    return r;
}

bool ContestEngine::isDupe(const QString& call, const QString& band, const QString& mode) const
{
    if (!m_log || !isActive()) {
        return false;
    }
    return m_log->isDupe(call, band, mode, dupeRules());
}

QString ContestEngine::myDxcc() const
{
    if (!m_log) {
        return {};
    }
    return m_log->cty().resolvePrimaryPrefix(m_log->stationCallsign());
}

QString ContestEngine::myContinent() const
{
    if (!m_log) {
        return {};
    }
    const DxccEntity* e = m_log->cty().entityByPrefix(myDxcc());
    return e ? e->continent : QString();
}

bool ContestEngine::iAmWve() const
{
    return isWveDxcc(myDxcc());
}

int ContestEngine::qsoPoints(const LogQso& q) const
{
    switch (m_profile.type) {
    case ContestType::None:
        return 0;
    case ContestType::CqWw: {
        const QString mine = myDxcc();
        if (!mine.isEmpty() && q.dxccPrefix == mine) {
            return 0;
        }
        const QString myCont = myContinent();
        if (myCont.isEmpty() || q.continent.isEmpty()) {
            return 1;
        }
        if (q.continent != myCont) {
            return 3;
        }
        return myCont == "NA" ? 2 : 1;
    }
    case ContestType::ArrlDx: {
        const bool hisWve = isWveDxcc(q.dxccPrefix);
        return (iAmWve() != hisWve) ? 3 : 0;
    }
    case ContestType::ArrlFieldDay: {
        const QString g = LogUtil::modeGroup(q.mode, q.submode);
        return (g == "PHONE") ? 1 : 2;
    }
    default:
        return 1;
    }
}

namespace {

// Multiplier keys "Category|band|value" (band empty when not per band).
QStringList multKeys(const ContestProfile& p, const LogQso& q, bool iAmWve)
{
    QStringList keys;
    const QString b = q.band;
    auto isNa = [&q]() { return q.continent == "NA"; };
    switch (p.type) {
    case ContestType::CqWw:
        if (q.cqZone > 0) {
            keys << QStringLiteral("Zones|%1|%2").arg(b).arg(q.cqZone);
        }
        if (!q.dxccPrefix.isEmpty()) {
            keys << QStringLiteral("Countries|%1|%2").arg(b, q.dxccPrefix);
        }
        break;
    case ContestType::ArrlDx:
        if (iAmWve) {
            if (!q.dxccPrefix.isEmpty() && !isWveDxcc(q.dxccPrefix)) {
                keys << QStringLiteral("Countries|%1|%2").arg(b, q.dxccPrefix);
            }
        } else if (isWveDxcc(q.dxccPrefix) && !q.state.isEmpty()) {
            keys << QStringLiteral("States|%1|%2").arg(b, q.state);
        }
        break;
    case ContestType::Naqp:
    case ContestType::NaSprint: {
        const QString band = (p.type == ContestType::Naqp) ? b : QString();
        if (isWveDxcc(q.dxccPrefix)) {
            if (!q.state.isEmpty()) {
                keys << QStringLiteral("Mults|%1|%2").arg(band, q.state);
            }
        } else if (isNa() && !q.dxccPrefix.isEmpty()) {
            keys << QStringLiteral("Mults|%1|%2").arg(band, q.dxccPrefix);
        }
        break;
    }
    case ContestType::GenericSerial:
        if (!q.dxccPrefix.isEmpty()) {
            keys << QStringLiteral("Countries||%1").arg(q.dxccPrefix);
        }
        break;
    case ContestType::Custom:
        for (const ExchangeField& f : p.rcvd) {
            if (f.isMult) {
                const QString v = fieldValue(q, f.adifField).toUpper();
                if (!v.isEmpty()) {
                    keys << QStringLiteral("Mults|%1|%2").arg(b, v);
                }
            }
        }
        break;
    case ContestType::ArrlFieldDay:
    case ContestType::None:
        break;
    }
    return keys;
}

} // namespace

ContestScore ContestEngine::computeScore(const QVector<LogQso>& qsosIn) const
{
    ContestScore sc;
    if (!isActive()) {
        return sc;
    }
    QVector<LogQso> qsos = qsosIn;
    std::stable_sort(qsos.begin(), qsos.end(), [](const LogQso& a, const LogQso& b) {
        return a.timeOn < b.timeOn;
    });
    QSet<QString> seen;
    QSet<QString> mults;
    const bool wve = iAmWve();
    for (const LogQso& q : qsos) {
        const QString g = LogUtil::modeGroup(q.mode, q.submode);
        const QString dk = q.call + '|' + (m_profile.dupePerBand ? q.band : QString()) + '|' +
                           (m_profile.dupePerMode ? g : QString());
        if (seen.contains(dk)) {
            ++sc.dupes;
            continue;
        }
        seen.insert(dk);
        ++sc.qsos;
        ++sc.qsosPerBand[q.band];
        sc.points += qsoPoints(q);
        for (const QString& k : multKeys(m_profile, q, wve)) {
            if (!mults.contains(k)) {
                mults.insert(k);
                ++sc.multsByCategory[k.section('|', 0, 0)];
            }
        }
    }
    sc.mults = static_cast<int>(mults.size());
    if (m_profile.type == ContestType::ArrlFieldDay) {
        sc.score = static_cast<long long>(sc.points) * m_fdPowerMult;
    } else {
        sc.score = static_cast<long long>(sc.points) * sc.mults;
    }
    return sc;
}

void ContestEngine::recompute()
{
    m_multsWorked.clear();
    if (!m_log || !isActive()) {
        m_score = ContestScore{};
        emit scoreChanged(m_score);
        return;
    }
    LogFilter f;
    f.contestId = contestId();
    f.from = m_start;
    const QVector<LogQso> qsos = m_log->search(f);
    m_score = computeScore(qsos);
    const bool wve = iAmWve();
    for (const LogQso& q : qsos) {
        for (const QString& k : multKeys(m_profile, q, wve)) {
            m_multsWorked.insert(k);
        }
    }
    emit scoreChanged(m_score);
}

bool ContestEngine::isNewMult(const QString& call, const QString& band,
                              const QMap<QString, QString>& hint) const
{
    if (!m_log || !isActive()) {
        return false;
    }
    LogQso q;
    q.call = LogUtil::normalizeCall(call);
    q.band = band.toLower();
    m_log->fillDxcc(q);
    if (hint.contains("ZONE")) {
        q.cqZone = hint.value("ZONE").toInt();
    }
    q.state = hint.value("STATE").toUpper();
    for (const ExchangeField& f : m_profile.rcvd) {
        if (hint.contains(f.key)) {
            setFieldValue(q, f.adifField, hint.value(f.key));
        }
    }
    const QStringList keys = multKeys(m_profile, q, iAmWve());
    return std::any_of(keys.cbegin(), keys.cend(),
                       [this](const QString& k) { return !m_multsWorked.contains(k); });
}

SpotAnnotation ContestEngine::annotateSpot(const QString& call, double freqMhz,
                                           const QString& spotMode) const
{
    SpotAnnotation a;
    if (!m_log || call.trimmed().isEmpty()) {
        return a;
    }
    const QString band = LogUtil::freqToBand(freqMhz);
    QString group;
    if (!spotMode.isEmpty()) {
        QString m;
        QString sub;
        LogUtil::flexModeToAdif(spotMode, m, sub);
        group = LogUtil::modeGroup(m, sub);
    } else if (isActive() && m_profile.dupePerMode) {
        group = LogUtil::modeGroup(m_modeVariant == "SSB" ? QStringLiteral("SSB") : m_modeVariant);
    }

    if (isActive()) {
        if (isDupe(call, band, group)) {
            a.kind = SpotAnnotation::Kind::Dupe;
            a.label = QStringLiteral("DUPE");
            a.color = colorDupe;
            a.tooltip = tr("Already worked in this contest on %1").arg(band);
            return a;
        }
        if (isNewMult(call, band)) {
            a.kind = SpotAnnotation::Kind::NewMult;
            a.label = QStringLiteral("MULT");
            a.color = colorNewMult;
            a.tooltip = tr("New multiplier");
            return a;
        }
        a.kind = SpotAnnotation::Kind::NewCall;
        a.color = colorNewCall;
        a.tooltip = tr("Needed — %1 pts").arg(qsoPoints([&]() {
            LogQso q;
            q.call = LogUtil::normalizeCall(call);
            q.band = band;
            q.mode = group == "PHONE" ? QStringLiteral("SSB") : group;
            m_log->fillDxcc(q);
            return q;
        }()));
        return a;
    }

    const WorkedStatus w = m_log->workedStatus(call, band, group);
    switch (w.dxccStatus()) {
    case DxccStatus::NewDxcc:
        a.kind = SpotAnnotation::Kind::NewDxcc;
        a.label = QStringLiteral("NEW DXCC");
        a.color = colorNewDxcc;
        break;
    case DxccStatus::NewBand:
        a.kind = SpotAnnotation::Kind::NewBand;
        a.label = QStringLiteral("NEW BAND");
        a.color = colorNewBand;
        break;
    case DxccStatus::NewMode:
        a.kind = SpotAnnotation::Kind::NewMode;
        a.label = QStringLiteral("NEW MODE");
        a.color = colorNewMode;
        break;
    case DxccStatus::Worked:
    case DxccStatus::Unknown:
        if (w.callBandModeWorked) {
            a.kind = SpotAnnotation::Kind::Worked;
            a.color = colorWorked;
        } else if (!w.callWorked) {
            a.kind = SpotAnnotation::Kind::NewCall;
            a.color = colorNewCall;
        } else {
            a.kind = SpotAnnotation::Kind::Worked;
            a.label = QString();
            a.color = colorWorked;
        }
        break;
    }
    a.tooltip = w.country.isEmpty() ? QString()
                                     : tr("%1 — %2 QSO(s) with %3").arg(w.country)
                                           .arg(w.callQsoCount).arg(LogUtil::normalizeCall(call));
    return a;
}

SpotAnnotation ContestEngine::annotateSpot(const SpotData& spot) const
{
    return annotateSpot(spot.callsign, spot.rxFreqMhz, spot.mode);
}

// ── Macros ────────────────────────────────────────────────────────────────

QString ContestEngine::defaultCwMacro(ContestType t, int fkey)
{
    const bool contest = (t != ContestType::None);
    switch (fkey) {
    case 1: return contest ? QStringLiteral("CQ TEST {MYCALL} {MYCALL} TEST")
                           : QStringLiteral("CQ CQ DE {MYCALL} {MYCALL} K");
    case 2: return contest ? QStringLiteral("{EXCH}") : QStringLiteral("{CALL} DE {MYCALL} {RST} {RST} K");
    case 3: return contest ? QStringLiteral("TU {MYCALL}") : QStringLiteral("TU 73 {CALL} DE {MYCALL} SK");
    case 4: return QStringLiteral("{MYCALL}");
    case 5: return QStringLiteral("!");
    case 6: return QStringLiteral("{EXCH} {EXCH}");
    case 7: return QStringLiteral("?");
    case 8: return QStringLiteral("AGN?");
    default: return {};
    }
}

QString ContestEngine::cwMacro(int fkey) const
{
    if (fkey < 1 || fkey > kMacroCount) {
        return {};
    }
    const QString k = QStringLiteral("ContestCwMacro_%1_F%2").arg(m_profile.key).arg(fkey);
    return AppSettings::instance().value(k, defaultCwMacro(m_profile.type, fkey)).toString();
}

void ContestEngine::setCwMacro(int fkey, const QString& templ)
{
    if (fkey < 1 || fkey > kMacroCount) {
        return;
    }
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("ContestCwMacro_%1_F%2").arg(m_profile.key).arg(fkey), templ);
    s.save();
}

int ContestEngine::voiceMacroSlot(int fkey) const
{
    if (fkey < 1 || fkey > kMacroCount) {
        return -1;
    }
    return AppSettings::instance()
        .value(QStringLiteral("ContestVoiceMacroF%1").arg(fkey), "-1").toInt();
}

void ContestEngine::setVoiceMacroSlot(int fkey, int dvkId)
{
    if (fkey < 1 || fkey > kMacroCount) {
        return;
    }
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("ContestVoiceMacroF%1").arg(fkey), QString::number(dvkId));
    s.save();
}

QString ContestEngine::expandMacro(const QString& templ, const MacroContext& c)
{
    QString sn = c.serial > 0 ? QStringLiteral("%1").arg(c.serial, 3, 10, QChar('0')) : QString();
    QString rst = c.rst;
    QString zone = c.myZone;
    if (c.cutNumbers) {
        sn = cut(sn);
        rst = QString(rst).replace('9', 'N');
        zone = cut(zone);
    }
    QString out = templ;
    out.replace("{EXCH}", c.exchange, Qt::CaseInsensitive);
    out.replace("{MYCALL}", c.myCall, Qt::CaseInsensitive);
    out.replace("{CALL}", c.call, Qt::CaseInsensitive);
    out.replace("{RST}", rst, Qt::CaseInsensitive);
    out.replace("{SN}", sn, Qt::CaseInsensitive);
    out.replace("{NR}", sn, Qt::CaseInsensitive);
    out.replace("{MYNAME}", c.myName, Qt::CaseInsensitive);
    out.replace("{NAME}", c.name, Qt::CaseInsensitive);
    out.replace("{STATE}", c.myState, Qt::CaseInsensitive);
    out.replace("{ZONE}", zone, Qt::CaseInsensitive);
    out.replace("{CLASS}", c.myClass, Qt::CaseInsensitive);
    out.replace("{SECT}", c.mySection, Qt::CaseInsensitive);
    out.replace("{PWR}", c.myPower, Qt::CaseInsensitive);
    out.replace("{QTH}", c.myState, Qt::CaseInsensitive);
    // N1MM-style "!" = his call (only as a standalone token)
    static const QRegularExpression bang(QStringLiteral("(^|\\s)!(?=\\s|$)"));
    out.replace(bang, QStringLiteral("\\1") + c.call);
    return out.simplified();
}

MacroContext ContestEngine::macroContext(const QString& hisCall, const QString& hisName,
                                         int serial) const
{
    MacroContext c;
    c.myCall = m_log ? m_log->stationCallsign() : QString();
    c.call = LogUtil::normalizeCall(hisCall);
    c.name = hisName.toUpper();
    const QString g = LogUtil::modeGroup(m_modeVariant == "SSB" ? QStringLiteral("SSB") : m_modeVariant);
    c.rst = (g == "PHONE") ? QStringLiteral("59") : QStringLiteral("599");
    c.serial = serial;
    c.myName = m_myExchange.value("NAME");
    c.myState = m_myExchange.value("STATE");
    c.myZone = m_myExchange.value("ZONE");
    c.myClass = m_myExchange.value("CLASS");
    c.mySection = m_myExchange.value("SECT");
    c.myPower = m_myExchange.value("PWR");
    c.cutNumbers = AppSettings::instance().value("ContestCwCutNumbers", "True").toString() == "True";
    MacroContext plain = c;
    plain.exchange.clear();
    QString templ = m_profile.sentTemplate;
    if (m_profile.type == ContestType::ArrlDx && !iAmWve()) {
        templ = QStringLiteral("{RST} {PWR}");
    }
    c.exchange = expandMacro(templ, plain);
    return c;
}

bool ContestEngine::txAllowed(QString* why) const
{
    if (!m_macroTxArmed) {
        *why = tr("Macro TX is not armed");
        return false;
    }
    if (m_txGuard && !m_txGuard()) {
        *why = tr("TX guard refused (radio not ready / RX-only)");
        return false;
    }
    return true;
}

bool ContestEngine::sendCwMacro(int fkey, const QString& hisCall, const QString& hisName,
                                int serial)
{
    QString why;
    if (!txAllowed(&why)) {
        emit macroRefused(why);
        return false;
    }
    if (!m_cwx) {
        emit macroRefused(tr("CWX not available"));
        return false;
    }
    const QString text = expandMacro(cwMacro(fkey), macroContext(hisCall, hisName, serial));
    if (text.isEmpty()) {
        emit macroRefused(tr("F%1 is empty").arg(fkey));
        return false;
    }
    m_cwx->send(text);
    emit macroSent(fkey, text);
    return true;
}

bool ContestEngine::sendVoiceMacro(int fkey)
{
    QString why;
    if (!txAllowed(&why)) {
        emit macroRefused(why);
        return false;
    }
    const int slot = voiceMacroSlot(fkey);
    if (!m_dvk || slot < 0) {
        emit macroRefused(tr("No DVK recording assigned to F%1").arg(fkey));
        return false;
    }
    m_dvk->playbackStart(slot);
    emit macroSent(fkey, QStringLiteral("DVK %1").arg(slot));
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// So2rController
// ═══════════════════════════════════════════════════════════════════════════

So2rController::So2rController(QObject* parent)
    : QObject(parent)
{
}

void So2rController::setSlices(SliceModel* a, SliceModel* b)
{
    m_slice[0] = a;
    m_slice[1] = b;
    if (!slice(m_focus)) {
        m_focus = 0;
    }
}

SliceModel* So2rController::slice(int radio) const
{
    return (radio == 0 || radio == 1) ? m_slice[radio].data() : nullptr;
}

int So2rController::txRadio() const
{
    for (int r = 0; r < 2; ++r) {
        if (m_slice[r] && m_slice[r]->isTxSlice()) {
            return r;
        }
    }
    return -1;
}

void So2rController::setRole(int radio, Role r)
{
    radio &= 1;
    if (m_role[radio] == r) {
        return;
    }
    m_role[radio] = r;
    emit roleChanged(radio, r);
}

void So2rController::stampQso(LogQso& q) const
{
    if (SliceModel* s = focusedSlice()) {
        q.sliceId = s->sliceId();
    }
    q.so2rRole = roleTag(m_role[m_focus]);
}

void So2rController::swapFocus()
{
    setFocus(m_focus ^ 1);
}

void So2rController::setFocus(int radio)
{
    radio &= 1;
    if (!m_slice[radio] || radio == m_focus) {
        return;
    }
    m_focus = radio;
    emit focusChanged(m_focus);
}

bool So2rController::swapTx()
{
    const int cur = txRadio();
    const int target = (cur < 0) ? m_focus : (cur ^ 1);
    return setTxRadio(target);
}

bool So2rController::setTxRadio(int radio)
{
    SliceModel* s = slice(radio);
    if (!s) {
        return false;
    }
    // Assigns the TX slice only (radio moves the TX flag); does not key.
    s->setTxSlice(true);
    emit txRequested(radio);
    return true;
}

void So2rController::toggleRole()
{
    setRole(m_focus, m_role[m_focus] == Role::Run ? Role::SearchPounce : Role::Run);
}

} // namespace AetherSDR
