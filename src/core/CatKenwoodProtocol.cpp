#include "CatKenwoodProtocol.h"

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>

// References (see also tests/cat_kenwood_test.cpp):
//  [SSC]  FlexRadio "SmartSDR CAT User Guide", FLEX-6000/8000/Aurora,
//         rev v4.1.5 (March 2026), ch. 3 "CAT Commands".
//  [HL]   Hamlib rigs/kenwood/flex6xxx.c + kenwood.c (FlexRadio 6xxx model
//         2036 = WSJT-X / Log4OM "FlexRadio 6xxx"; TS-2000 model 2014).

namespace AetherSDR {

namespace {

// [HL] flex6xxx.c dsp_bw_* — ZZFI/ZZFJ index → passband, descending.
// Matches [SSC] 3.3.9 example: LSB 2.7k < bw <= 2.9k → ZZFI02.
constexpr std::array<int, 8> kBwSsb{4000, 3300, 2900, 2700, 2400, 2100, 1800, 1600};
constexpr std::array<int, 8> kBwAm {20000, 16000, 14000, 12000, 10000, 8000, 6000, 5600};
constexpr std::array<int, 8> kBwCw {3000, 1500, 1000, 800, 400, 250, 100, 50};
constexpr std::array<int, 8> kBwDig{3000, 2000, 1500, 1000, 600, 300, 150, 100};

// [SSC] 3.4.21 / 3.4.22 — SH / SL cut-frequency index tables.
constexpr std::array<int, 12> kShSsb{1400, 1600, 1800, 2000, 2200, 2400, 2600, 2800, 3000, 3400, 4000, 5000};
constexpr std::array<int, 4>  kShAm {2500, 3000, 4000, 5000};
constexpr std::array<int, 12> kSlSsb{0, 50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000};
constexpr std::array<int, 4>  kSlAm {0, 100, 200, 500};

constexpr int  kMaxOffsetHz   = 99999;   // RIT/XIT ±5 digits
constexpr int  kMaxKyChars    = 256;     // [SSC] 3.4.11
constexpr long long kMaxFreqHz = 99999999999LL;

enum class Side { Upper, Lower, Symmetric };

Side sideOf(const QString& mode)
{
    if (mode == "LSB" || mode == "DIGL" || mode == "CWL") {
        return Side::Lower;
    }
    if (mode == "AM" || mode == "SAM" || mode == "FM" || mode == "NFM" || mode == "DFM") {
        return Side::Symmetric;
    }
    return Side::Upper;
}

const int* bwTable(const QString& mode)
{
    if (mode == "USB" || mode == "LSB" || mode.startsWith("FDV")) {
        return kBwSsb.data();
    }
    if (mode == "AM" || mode == "SAM") {
        return kBwAm.data();
    }
    if (mode == "CW" || mode == "CWL") {
        return kBwCw.data();
    }
    if (mode == "DIGU" || mode == "DIGL" || mode == "RTTY") {
        return kBwDig.data();
    }
    return nullptr;  // FM family: no index table (Hamlib also leaves it unsupported)
}

bool isAmFamily(const QString& mode)
{
    return mode == "AM" || mode == "SAM";
}

template <std::size_t N>
int nearestIndex(const std::array<int, N>& table, int value)
{
    int best = 0;
    int bestDiff = std::abs(table[0] - value);
    for (int i = 1; i < static_cast<int>(N); ++i) {
        const int d = std::abs(table[static_cast<std::size_t>(i)] - value);
        if (d < bestDiff) {
            best = i;
            bestDiff = d;
        }
    }
    return best;
}

QString agcFromCode(int code)
{
    switch (code) {
    case 0: return QStringLiteral("off");
    case 2: return QStringLiteral("slow");
    case 3: return QStringLiteral("med");
    case 4: return QStringLiteral("fast");
    default: return {};
    }
}

int codeFromAgc(const QString& mode)
{
    const QString m = mode.toLower();
    if (m == "off")  { return 0; }
    if (m == "slow") { return 2; }
    if (m == "fast") { return 4; }
    return 3;  // med (and unknown)
}

} // namespace

CatKenwoodProtocol::CatKenwoodProtocol(CatRadioInterface* radio)
    : m_radio(radio)
{}

// ── Pure helpers ────────────────────────────────────────────────────────────

// [SSC] 3.4.12 MD values. CWL/RTTY/SAM/FM variants fold into the Kenwood set.
int CatKenwoodProtocol::modeToMd(const QString& mode)
{
    if (mode == "LSB")                   { return 1; }
    if (mode == "USB")                   { return 2; }
    if (mode == "CW" || mode == "CWL")   { return 3; }
    if (mode == "FM" || mode == "NFM" || mode == "DFM"
        || mode.startsWith("FDV") || mode == "DSTR") {
        return 4;
    }
    if (mode == "AM" || mode == "SAM")   { return 5; }
    if (mode == "DIGL" || mode == "RTTY") { return 6; }
    if (mode == "DIGU")                  { return 9; }
    return 2;
}

QString CatKenwoodProtocol::mdToMode(int md)
{
    switch (md) {
    case 1: return QStringLiteral("LSB");
    case 2: return QStringLiteral("USB");
    case 3: return QStringLiteral("CW");
    case 4: return QStringLiteral("FM");
    case 5: return QStringLiteral("AM");
    case 6: return QStringLiteral("DIGL");
    case 7: return QStringLiteral("CW");    // TS-2000 CW-R: SmartSDR has one CW mode
    case 9: return QStringLiteral("DIGU");
    default: return {};
    }
}

// [SSC] 3.3.20 ZZMD values.
int CatKenwoodProtocol::modeToZzmd(const QString& mode)
{
    if (mode == "LSB")  { return 0; }
    if (mode == "USB")  { return 1; }
    if (mode == "CWL")  { return 3; }
    if (mode == "CW")   { return 4; }   // FlexRadio CW is upper-sideband style
    if (mode == "FM")   { return 5; }
    if (mode == "AM")   { return 6; }
    if (mode == "DIGU") { return 7; }
    if (mode == "DIGL") { return 9; }
    if (mode == "SAM")  { return 10; }
    if (mode == "NFM")  { return 11; }
    if (mode == "DFM")  { return 12; }
    if (mode.startsWith("FDV")) { return 20; }
    if (mode == "RTTY") { return 30; }
    if (mode == "DSTR") { return 40; }
    return 1;
}

QString CatKenwoodProtocol::zzmdToMode(int code)
{
    switch (code) {
    case 0:  return QStringLiteral("LSB");
    case 1:  return QStringLiteral("USB");
    case 3:  return QStringLiteral("CW");   // CWL: radio has a single CW mode
    case 4:  return QStringLiteral("CW");
    case 5:  return QStringLiteral("FM");
    case 6:  return QStringLiteral("AM");
    case 7:  return QStringLiteral("DIGU");
    case 9:  return QStringLiteral("DIGL");
    case 10: return QStringLiteral("SAM");
    case 11: return QStringLiteral("NFM");
    case 12: return QStringLiteral("DFM");
    case 20: return QStringLiteral("FDV");
    case 30: return QStringLiteral("RTTY");
    case 40: return QStringLiteral("DSTR");
    default: return {};
    }
}

int CatKenwoodProtocol::filterIndexForWidth(const QString& mode, int widthHz)
{
    const int* table = bwTable(mode);
    if (!table) {
        return 0;
    }
    // Largest index whose passband still covers widthHz ([SSC] 3.3.9 example).
    int idx = 0;
    for (int i = 0; i < 8; ++i) {
        if (table[i] >= widthHz) {
            idx = i;
        }
    }
    return idx;
}

int CatKenwoodProtocol::filterWidthForIndex(const QString& mode, int index)
{
    const int* table = bwTable(mode);
    if (!table || index < 0 || index > 7) {
        return -1;
    }
    return table[index];
}

// Kenwood scale: 0-15 = S0-S9 (6 dB/S-unit, S9 = -73 dBm), 15-30 = S9..S9+60.
// [SSC] 3.4.23: "S9 reads approximately 015".
int CatKenwoodProtocol::sMeterKenwood(float dbm)
{
    double v;
    if (dbm <= -73.0f) {
        v = (static_cast<double>(dbm) + 127.0) / 54.0 * 15.0;
    } else {
        v = 15.0 + (static_cast<double>(dbm) + 73.0) / 60.0 * 15.0;
    }
    return std::clamp(static_cast<int>(std::lround(v)), 0, 30);
}

// [SSC] 3.3.36: value = (dBm + 140) * 2, range 000-260.
int CatKenwoodProtocol::sMeterZz(float dbm)
{
    const long v = std::lround((static_cast<double>(dbm) + 140.0) * 2.0);
    return static_cast<int>(std::clamp(v, 0L, 260L));
}

// [SSC] 3.4.8 ID table.
int CatKenwoodProtocol::radioIdForModel(const QString& model)
{
    QString m = model.trimmed().toUpper();
    if (m.startsWith("FLEX-")) {
        m = m.mid(5);
    }
    if (m.endsWith('M')) {
        m.chop(1);             // 6400M / 6600M / 8400M / 8600M / AU-5x0M share the ID
    }
    if (m == "6700")   { return 904; }
    if (m == "6500")   { return 905; }
    if (m == "6700R")  { return 906; }
    if (m == "6300")   { return 907; }
    if (m == "6400")   { return 908; }
    if (m == "6600")   { return 909; }
    if (m == "8400")   { return 910; }
    if (m == "8600")   { return 911; }
    if (m == "AU-510") { return 930; }
    if (m == "AU-520") { return 931; }
    return 909;  // unknown: a 6000-series ID every Hamlib Kenwood backend accepts
}

QByteArray CatKenwoodProtocol::pad(long long v, int width)
{
    return QByteArray::number(v).rightJustified(width, '0', true);
}

QByteArray CatKenwoodProtocol::formatFreq(long long hz, int digits)
{
    return pad(std::max(0LL, hz), digits);
}

QByteArray CatKenwoodProtocol::formatOffset(int hz)
{
    const int clamped = std::clamp(hz, -kMaxOffsetHz, kMaxOffsetHz);
    return (clamped < 0 ? QByteArrayLiteral("-") : QByteArrayLiteral("+"))
           + pad(std::abs(clamped), 5);
}

bool CatKenwoodProtocol::parseUInt(const QByteArray& s, int& out)
{
    if (s.isEmpty() || s.size() > 9) {
        return false;
    }
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

bool CatKenwoodProtocol::parseSignedOffset(const QByteArray& s, int& out)
{
    if (s.size() < 2 || (s[0] != '+' && s[0] != '-')) {
        return false;
    }
    int v = 0;
    if (!parseUInt(s.mid(1), v) || v > kMaxOffsetHz) {
        return false;
    }
    out = (s[0] == '-') ? -v : v;
    return true;
}

// ── VFO resolution ──────────────────────────────────────────────────────────

int CatKenwoodProtocol::vfoASliceId() const
{
    if (!m_radio || !m_radio->isConnected()) {
        return -1;
    }
    const std::vector<int> ids = m_radio->sliceIds();
    if (ids.empty()) {
        return -1;
    }
    if (m_vfoABinding == kActiveSlice) {
        const int active = m_radio->activeSliceId();
        if (std::find(ids.begin(), ids.end(), active) != ids.end()) {
            return active;
        }
        return ids.front();
    }
    // Fixed binding: SmartSDR shows "Slice Not Present" and the port answers
    // ?; rather than silently driving some other slice.
    if (std::find(ids.begin(), ids.end(), m_vfoABinding) != ids.end()) {
        return m_vfoABinding;
    }
    return -1;
}

// Logical VFO B = the split slice this port created / adopted via FT1 or
// ZZSW1 ([SSC] 3.3.37: "logical VFO B is not necessarily Slice B").
// Another client's or another port's TX slice is deliberately NOT treated
// as VFO B, so a second port never retunes a slice it does not own.
int CatKenwoodProtocol::vfoBSliceId() const
{
    const int a = vfoASliceId();
    if (a < 0 || m_lastVfoB < 0 || m_lastVfoB == a) {
        return -1;
    }
    const std::vector<int> ids = m_radio->sliceIds();
    if (std::find(ids.begin(), ids.end(), m_lastVfoB) == ids.end()) {
        return -1;
    }
    return m_lastVfoB;
}

std::optional<CatSliceState> CatKenwoodProtocol::vfoA() const
{
    const int id = vfoASliceId();
    return id < 0 ? std::nullopt : m_radio->slice(id);
}

std::optional<CatSliceState> CatKenwoodProtocol::vfoB() const
{
    const int id = vfoBSliceId();
    return id < 0 ? std::nullopt : m_radio->slice(id);
}

bool CatKenwoodProtocol::splitActive() const
{
    const auto b = vfoB();
    return b && b->tx;
}

bool CatKenwoodProtocol::transmitting() const
{
    return m_radio && (m_radio->isTransmitting() || m_radio->isTuning());
}

int CatKenwoodProtocol::defaultRitStep(const QString& mode) const
{
    // [SSC] 3.3.29: 10 Hz in CW/DIGU/DIGL, 50 Hz in USB/LSB/AM.
    if (mode == "CW" || mode == "CWL" || mode == "DIGU" || mode == "DIGL" || mode == "RTTY") {
        return 10;
    }
    return 50;
}

// After FT1/ZZSW1 asked MainWindow for a split, adopt the new slice as this
// port's VFO B once it exists and carries the TX flag.
void CatKenwoodProtocol::adoptSplitSlice()
{
    if (!m_splitRequested || !m_radio || !m_radio->isConnected()) {
        return;
    }
    const int a = vfoASliceId();
    for (int id : m_radio->sliceIds()) {
        const auto s = m_radio->slice(id);
        if (s && s->tx && id != a) {
            m_lastVfoB = id;
            m_splitRequested = false;
            return;
        }
    }
}

// ── Framing ─────────────────────────────────────────────────────────────────

QByteArray CatKenwoodProtocol::feed(const QByteArray& bytes)
{
    m_buffer.append(bytes);
    QByteArray out;
    while (true) {
        const qsizetype semi = m_buffer.indexOf(';');
        if (semi < 0) {
            break;
        }
        const QByteArray cmd = m_buffer.left(semi);
        m_buffer.remove(0, semi + 1);
        out += handleCommand(cmd);
    }
    // A peer that never sends ';' must not grow the buffer without bound.
    if (m_buffer.size() > kMaxBufferBytes) {
        m_buffer.clear();
        out += err();
    }
    return out;
}

// ── Dispatch ────────────────────────────────────────────────────────────────

QByteArray CatKenwoodProtocol::handleCommand(const QByteArray& rawIn)
{
    // Strip leading whitespace / line noise (terminal programs add CR/LF).
    qsizetype start = 0;
    while (start < rawIn.size()
           && (rawIn[start] == '\r' || rawIn[start] == '\n' || rawIn[start] == ' '
               || rawIn[start] == '\t' || rawIn[start] == '\0')) {
        ++start;
    }
    QByteArray raw = rawIn.mid(start);
    while (!raw.isEmpty() && (raw.endsWith('\r') || raw.endsWith('\n'))) {
        raw.chop(1);
    }
    if (raw.isEmpty()) {
        return {};
    }
    if (raw.size() < 2) {
        return err();
    }

    adoptSplitSlice();

    const bool zz = raw.size() >= 4 && raw.left(2).toUpper() == "ZZ";
    const QByteArray name = raw.left(zz ? 4 : 2).toUpper();
    const QByteArray rawArg = raw.mid(zz ? 4 : 2);
    const QByteArray arg = rawArg.trimmed().toUpper();

    // Commands that work without a connected radio.
    if (name == "ID") {
        if (!arg.isEmpty()) { return err(); }
        const QString model = m_radio ? m_radio->radioModelName() : QString();
        return "ID" + pad(radioIdForModel(model), 3) + ";";
    }
    if (name == "PS") {
        // Never power the radio off from CAT; PS0/PS1 are accepted as no-ops.
        if (arg.isEmpty()) { return QByteArrayLiteral("PS1;"); }
        return (arg == "0" || arg == "1") ? QByteArray() : err();
    }
    if (name == "AI" || name == "ZZAI") {
        return cmdAi(name, arg);
    }
    if (!m_radio || !m_radio->isConnected()) {
        return err();
    }

    auto sliceIdA = [this]() { return vfoASliceId(); };
    auto sliceIdB = [this]() { return vfoBSliceId(); };

    // ── Frequency / mode / info ─────────────────────────────────────────
    if (name == "FA" || name == "ZZFA") { return cmdFreq(name, arg, false); }
    if (name == "FB" || name == "ZZFB") { return cmdFreq(name, arg, true); }
    if (name == "MD")   { return cmdMode(name, arg, false, false); }
    if (name == "ZZMD") { return cmdMode(name, arg, true, false); }
    if (name == "ZZME") { return cmdMode(name, arg, true, true); }
    if (name == "IF")   { return arg.isEmpty() ? cmdInfo(false) : err(); }
    if (name == "ZZIF") { return arg.isEmpty() ? cmdInfo(true) : err(); }

    // ── VFO / split / PTT ───────────────────────────────────────────────
    if (name == "FR")   { return cmdRxVfo(arg); }
    if (name == "FT" || name == "ZZSW") { return cmdTxVfo(name, arg); }
    if (name == "ZZFR") { return arg.isEmpty() ? cmdToggleRx() : err(); }
    if (name == "ZZFT") { return arg.isEmpty() ? cmdToggleTx() : err(); }
    if (name == "TX")   { return cmdTx(arg); }
    if (name == "ZZTX") { return cmdZzTx(arg); }
    if (name == "RX")   { return arg.isEmpty() ? cmdRx() : err(); }
    if (name == "ZZRX") {
        // [SSC] 3.3.34: get = inverse of MOX; only ZZRX1 may be set.
        if (arg.isEmpty()) { return QByteArrayLiteral("ZZRX") + (transmitting() ? "0" : "1") + ";"; }
        return arg == "1" ? cmdRx() : err();
    }

    // ── Filters ─────────────────────────────────────────────────────────
    if (name == "ZZFI") { return cmdFilterIndex(name, arg, false); }
    if (name == "ZZFJ") { return cmdFilterIndex(name, arg, true); }
    if (name == "SH")   { return cmdFilterCut(arg, true); }
    if (name == "SL")   { return cmdFilterCut(arg, false); }

    // ── Audio ───────────────────────────────────────────────────────────
    if (name == "AG") { return cmdAudioGain(arg); }
    if (name == "ZZAG" || name == "ZZLE") {
        auto idFn = (name == "ZZAG") ? std::function<int()>(sliceIdA) : std::function<int()>(sliceIdB);
        return numeric(name, arg, 3, 0, 100,
            [this, idFn]() -> std::optional<int> {
                const auto s = m_radio->slice(idFn());
                return s ? std::optional<int>(s->audioGain) : std::nullopt;
            },
            [this, idFn](int v) {
                const int id = idFn();
                if (id < 0) { return false; }
                m_radio->setAudioGain(id, v);
                return true;
            });
    }
    if (name == "ZZLB" || name == "ZZLF") {
        auto idFn = (name == "ZZLB") ? std::function<int()>(sliceIdA) : std::function<int()>(sliceIdB);
        return numeric(name, arg, 3, 0, 100,
            [this, idFn]() -> std::optional<int> {
                const auto s = m_radio->slice(idFn());
                return s ? std::optional<int>(s->audioPan) : std::nullopt;
            },
            [this, idFn](int v) {
                const int id = idFn();
                if (id < 0) { return false; }
                m_radio->setAudioPan(id, v);
                return true;
            });
    }
    if (name == "ZZMA" || name == "ZZMB") {
        auto idFn = (name == "ZZMA") ? std::function<int()>(sliceIdA) : std::function<int()>(sliceIdB);
        return boolean(name, arg,
            [this, idFn]() -> std::optional<bool> {
                const auto s = m_radio->slice(idFn());
                return s ? std::optional<bool>(s->mute) : std::nullopt;
            },
            [this, idFn](bool on) {
                const int id = idFn();
                if (id < 0) { return false; }
                m_radio->setMute(id, on);
                return true;
            });
    }
    if (name == "ZZBI") {
        return boolean(name, arg,
            [this]() -> std::optional<bool> { return m_radio->binaural(); },
            [this](bool on) { m_radio->setBinaural(on); return true; });
    }

    // ── AGC ─────────────────────────────────────────────────────────────
    if (name == "GT" || name == "ZZGT") { return cmdAgc(name, arg); }
    if (name == "ZZAR" || name == "ZZAS") {
        auto idFn = (name == "ZZAR") ? std::function<int()>(sliceIdA) : std::function<int()>(sliceIdB);
        return numeric(name, arg, 3, 0, 100,
            [this, idFn]() -> std::optional<int> {
                const auto s = m_radio->slice(idFn());
                return s ? std::optional<int>(s->agcThreshold) : std::nullopt;
            },
            [this, idFn](int v) {
                const int id = idFn();
                if (id < 0) { return false; }
                m_radio->setAgcThreshold(id, v);
                return true;
            });
    }

    // ── DSP ─────────────────────────────────────────────────────────────
    if (name == "NB") {
        return boolean(name, arg,
            [this]() -> std::optional<bool> {
                const auto s = vfoA();
                return s ? std::optional<bool>(s->wnb) : std::nullopt;
            },
            [this](bool on) {
                const int id = vfoASliceId();
                if (id < 0) { return false; }
                m_radio->setWnb(id, on);
                return true;
            });
    }
    if (name == "ZZNL") {
        return numeric(name, arg, 3, 0, 100,
            [this]() -> std::optional<int> {
                const auto s = vfoA();
                return s ? std::optional<int>(s->wnbLevel) : std::nullopt;
            },
            [this](int v) {
                const int id = vfoASliceId();
                if (id < 0) { return false; }
                m_radio->setWnbLevel(id, v);
                return true;
            });
    }
    if (name == "ZZNR" || name == "NR") {
        // TS-2000 NR takes 0/1/2 (NR1/NR2); both "on" values map to slice NR.
        QByteArray a = arg;
        if (name == "NR" && a == "2") { a = "1"; }
        return boolean(name, a,
            [this]() -> std::optional<bool> {
                const auto s = vfoA();
                return s ? std::optional<bool>(s->nr) : std::nullopt;
            },
            [this](bool on) {
                const int id = vfoASliceId();
                if (id < 0) { return false; }
                m_radio->setNr(id, on);
                return true;
            });
    }
    if (name == "ZZDE") {
        return boolean(name, arg,
            [this]() -> std::optional<bool> {
                const auto s = vfoA();
                return s ? std::optional<bool>(s->diversity) : std::nullopt;
            },
            [this](bool on) {
                const int id = vfoASliceId();
                if (id < 0) { return false; }
                m_radio->setDiversity(id, on);
                return true;
            });
    }
    if (name == "SQ") { return cmdSquelch(arg); }

    // ── RIT / XIT ───────────────────────────────────────────────────────
    if (name == "RT" || name == "ZZRT" || name == "ZZRY") {
        auto idFn = (name == "ZZRY") ? std::function<int()>(sliceIdB) : std::function<int()>(sliceIdA);
        return boolean(name, arg,
            [this, idFn]() -> std::optional<bool> {
                const auto s = m_radio->slice(idFn());
                return s ? std::optional<bool>(s->ritOn) : std::nullopt;
            },
            [this, idFn](bool on) {
                const auto s = m_radio->slice(idFn());
                if (!s) { return false; }
                m_radio->setRit(s->id, on, s->ritHz);
                return true;
            });
    }
    if (name == "XT" || name == "ZZXS") {
        return boolean(name, arg,
            [this]() -> std::optional<bool> {
                const auto s = vfoA();
                return s ? std::optional<bool>(s->xitOn) : std::nullopt;
            },
            [this](bool on) {
                const auto s = vfoA();
                if (!s) { return false; }
                m_radio->setXit(s->id, on, s->xitHz);
                return true;
            });
    }
    if (name == "RC")   { return arg.isEmpty() ? cmdRitClear() : err(); }
    if (name == "ZZRC") {
        const auto s = vfoA();
        if (!s || !arg.isEmpty()) { return err(); }
        m_radio->setRit(s->id, s->ritOn, 0);
        return {};
    }
    if (name == "ZZXC") {
        const auto s = vfoA();
        if (!s || !arg.isEmpty()) { return err(); }
        m_radio->setXit(s->id, s->xitOn, 0);
        return {};
    }
    if (name == "RU" || name == "ZZRU") { return cmdRitStep(arg, +1); }
    if (name == "RD" || name == "ZZRD") { return cmdRitStep(arg, -1); }
    if (name == "ZZRG") { return cmdOffsetValue(name, arg, false, false); }
    if (name == "ZZRW") { return cmdOffsetValue(name, arg, true, false); }
    if (name == "ZZXG") { return cmdOffsetValue(name, arg, false, true); }

    // ── Metering ────────────────────────────────────────────────────────
    if (name == "SM")   { return cmdSMeter(arg, false); }
    if (name == "ZZSM") { return cmdSMeter(arg, true); }

    // ── Transmitter ─────────────────────────────────────────────────────
    if (name == "PC" || name == "ZZPC") {
        return numeric(name, arg, 3, 0, 100,
            [this]() -> std::optional<int> { return m_radio->rfPower(); },
            [this](int v) { m_radio->setRfPower(v); return true; });
    }
    if (name == "ZZMG") {
        return numeric(name, arg, 3, 0, 100,
            [this]() -> std::optional<int> { return m_radio->micLevel(); },
            [this](int v) { m_radio->setMicLevel(v); return true; });
    }
    if (name == "VX") {
        return boolean(name, arg,
            [this]() -> std::optional<bool> { return m_radio->voxEnabled(); },
            [this](bool on) { m_radio->setVox(on); return true; });
    }
    if (name == "KS") {
        return numeric(name, arg, 3, 5, 100,
            [this]() -> std::optional<int> { return m_radio->cwSpeed(); },
            [this](int v) { m_radio->setCwSpeed(v); return true; });
    }
    if (name == "PT") {
        // [SSC] 3.4.15: 100-999; values below 100 are silently ignored.
        return numeric(name, arg, 3, 0, 999,
            [this]() -> std::optional<int> { return m_radio->cwPitch(); },
            [this](int v) {
                if (v >= 100) { m_radio->setCwPitch(v); }
                return true;
            });
    }
    if (name == "KY") { return cmdKey(rawArg); }

    return err();
}

// ── Generic numeric / boolean handlers ──────────────────────────────────────

template <typename Getter, typename Setter>
QByteArray CatKenwoodProtocol::numeric(const QByteArray& name, const QByteArray& arg, int width,
                                       int minVal, int maxVal, Getter get, Setter set)
{
    if (arg.isEmpty()) {
        const std::optional<int> v = get();
        if (!v) {
            return err();
        }
        return name + pad(std::clamp(*v, minVal, maxVal), width) + ";";
    }
    int v = 0;
    if (arg.size() > width || !parseUInt(arg, v) || v < minVal || v > maxVal) {
        return err();
    }
    return set(v) ? QByteArray() : err();
}

template <typename Getter, typename Setter>
QByteArray CatKenwoodProtocol::boolean(const QByteArray& name, const QByteArray& arg,
                                       Getter get, Setter set)
{
    if (arg.isEmpty()) {
        const std::optional<bool> v = get();
        if (!v) {
            return err();
        }
        return name + (*v ? "1" : "0") + ";";
    }
    if (arg != "0" && arg != "1") {
        return err();
    }
    return set(arg == "1") ? QByteArray() : err();
}

// ── Command implementations ─────────────────────────────────────────────────

QByteArray CatKenwoodProtocol::cmdAi(const QByteArray& name, const QByteArray& arg)
{
    if (arg.isEmpty()) {
        return name + QByteArray::number(m_aiMode) + ";";
    }
    int v = 0;
    // AI0 off; AI1..AI3 on (TS-2000 AI2/AI3 are "on" variants).
    if (arg.size() != 1 || !parseUInt(arg, v) || v > 3) {
        return err();
    }
    const bool wasOn = m_aiMode != 0;
    m_aiMode = v;
    if (v != 0 && !wasOn) {
        m_aiLast = takeSnapshot();   // report changes from now on, not a dump
    }
    return {};
}

// FA / FB / ZZFA / ZZFB — [SSC] 3.3.7, 3.3.8, 3.4.3, 3.4.4.
QByteArray CatKenwoodProtocol::cmdFreq(const QByteArray& name, const QByteArray& arg, bool vfoBCmd)
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    const auto b = vfoB();

    if (arg.isEmpty()) {
        long long hz = a->freqHz;
        if (vfoBCmd) {
            // No split slice yet: report the frequency a pending FB set asked
            // for, else mirror VFO A (a real TS-2000 always has a VFO B, and
            // pollers like N1MM read FB unconditionally).
            hz = b ? b->freqHz : (m_pendingVfoBHz > 0 ? m_pendingVfoBHz : a->freqHz);
        }
        return name + formatFreq(hz, m_freqDigits) + ";";
    }

    bool digitsOnly = true;
    for (char c : arg) {
        if (c < '0' || c > '9') {
            digitsOnly = false;
            break;
        }
    }
    if (!digitsOnly || arg.size() > 11) {
        return err();
    }
    const long long hz = arg.toLongLong();
    if (hz <= 0 || hz > kMaxFreqHz) {
        return err();
    }
    if (arg.size() == 8) {
        m_freqDigits = 8;
    } else if (arg.size() == 11) {
        m_freqDigits = 11;
    }

    if (!vfoBCmd) {
        m_radio->setFrequency(a->id, hz);
        return {};
    }
    if (b) {
        m_radio->setFrequency(b->id, hz);
        return {};
    }
    m_pendingVfoBHz = hz;
    if (m_splitRequested) {
        // FT1 already asked for a split slice that is still being created:
        // retarget it (same pattern as RigctlProtocol::cmdSetSplitFreq).
        m_radio->requestSplit(a->id, true, static_cast<double>(hz) / 1e6);
    }
    return {};
}

// MD / ZZMD / ZZME — [SSC] 3.3.20, 3.3.21, 3.4.12.
QByteArray CatKenwoodProtocol::cmdMode(const QByteArray& name, const QByteArray& arg, bool zz, bool vfoBCmd)
{
    const auto s = vfoBCmd ? vfoB() : vfoA();
    if (!s) {
        return err();
    }
    if (arg.isEmpty()) {
        if (zz) {
            return name + pad(modeToZzmd(s->mode), 2) + ";";
        }
        return name + QByteArray::number(modeToMd(s->mode)) + ";";
    }
    int code = 0;
    if (arg.size() != (zz ? 2 : 1) || !parseUInt(arg, code)) {
        return err();
    }
    QString mode = zz ? zzmdToMode(code) : mdToMode(code);
    if (mode.isEmpty()) {
        return err();
    }
    if (mode == "FDV" && !s->modeList.isEmpty() && !s->modeList.contains("FDV")) {
        // FreeDV waveform registers FDVU/FDVL on some firmware.
        mode = s->modeList.contains("FDVU") ? QStringLiteral("FDVU") : mode;
    }
    // Keep "same family" modes: MD4 on NFM, MD5 on SAM, MD6 on RTTY must not
    // bounce the slice to the base mode on every poll-and-set cycle.
    if (!zz && modeToMd(s->mode) == code) {
        return {};
    }
    if (!s->modeList.isEmpty() && !s->modeList.contains(mode)) {
        return err();   // e.g. DSTR/FDV without the waveform installed
    }
    if (s->mode != mode) {
        m_radio->setMode(s->id, mode);
    }
    return {};
}

// IF / ZZIF — [SSC] 3.3.14 / 3.4.9.
// IF P1(11) P2(4) P3(6) P4 P5 P6 P7(2) P8 P9 P10 P11 P12 P13 P14(2) P15 ;
// Hamlib reads PTT from offset 28 and split from offset 32 of this string.
QByteArray CatKenwoodProtocol::cmdInfo(bool zz)
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    const int offset = a->xitOn ? a->xitHz : a->ritHz;   // [SSC] note under 3.3.14
    QByteArray r = zz ? QByteArrayLiteral("ZZIF") : QByteArrayLiteral("IF");
    r += formatFreq(a->freqHz, 11);                        // P1
    r += "0000";                                           // P2 step (unused by clients)
    r += formatOffset(offset);                             // P3
    r += a->ritOn ? "1" : "0";                             // P4
    r += a->xitOn ? "1" : "0";                             // P5
    r += "0";                                              // P6
    r += "00";                                             // P7
    r += transmitting() ? "1" : "0";                       // P8
    r += zz ? pad(modeToZzmd(a->mode), 2)                  // P9
            : QByteArray::number(modeToMd(a->mode));
    r += (m_rxOnVfoB && vfoBSliceId() >= 0) ? "1" : "0";   // P10 = FR
    r += "0";                                              // P11
    r += splitActive() ? "1" : "0";                        // P12 = FT
    r += "0";                                              // P13
    r += "00";                                             // P14
    r += "0";                                              // P15
    r += ";";
    return r;
}

// FR — [SSC] 3.4.5.
QByteArray CatKenwoodProtocol::cmdRxVfo(const QByteArray& arg)
{
    const int a = vfoASliceId();
    const int b = vfoBSliceId();
    if (a < 0) {
        return err();
    }
    if (arg.isEmpty()) {
        return (m_rxOnVfoB && b >= 0) ? QByteArrayLiteral("FR1;") : QByteArrayLiteral("FR0;");
    }
    if (arg == "1") {
        if (b < 0) {
            return err();   // "Setting FR1; without VFO B will result in ?;"
        }
        if (!m_rxOnVfoB && m_vfoABinding != kActiveSlice) {
            m_radio->setActiveSlice(b);
        }
        m_rxOnVfoB = true;
        return {};
    }
    if (arg == "0") {
        // Only move GUI focus on a real B→A transition; Hamlib sends FR0 on
        // every set_vfo and we must not steal focus from the operator.
        if (m_rxOnVfoB && m_vfoABinding != kActiveSlice) {
            m_radio->setActiveSlice(a);
        }
        m_rxOnVfoB = false;
        return {};
    }
    return err();
}

// FT / ZZSW — [SSC] 3.3.37 / 3.4.6.
QByteArray CatKenwoodProtocol::cmdTxVfo(const QByteArray& name, const QByteArray& arg)
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    if (arg.isEmpty()) {
        return name + (splitActive() ? "1" : "0") + ";";
    }
    if (arg == "1") {
        const int b = vfoBSliceId();
        if (b >= 0) {
            m_radio->setTxSlice(b, true);
            return {};
        }
        // Create VFO B "near VFO A" via the GUI split path (MainWindow::
        // enableSplit), adopting the new TX slice when it shows up.
        m_splitRequested = true;
        const double txMhz = m_pendingVfoBHz > 0 ? static_cast<double>(m_pendingVfoBHz) / 1e6 : 0.0;
        m_radio->requestSplit(a->id, true, txMhz);
        return {};
    }
    if (arg == "0") {
        const int b = vfoBSliceId();
        m_splitRequested = false;
        if (!a->tx) {
            m_radio->setTxSlice(a->id, true);
        }
        if (m_splitMode == SplitMode::So2r && b >= 0) {
            // SO2R: close the split slice.
            m_radio->requestSplit(a->id, false, 0.0);
            m_lastVfoB = -1;
            m_rxOnVfoB = false;
        }
        m_pendingVfoBHz = 0;
        return {};
    }
    return err();
}

// ZZFR — [SSC] 3.3.11.
QByteArray CatKenwoodProtocol::cmdToggleRx()
{
    const int a = vfoASliceId();
    if (a < 0) {
        return err();
    }
    if (vfoBSliceId() < 0) {
        m_radio->setActiveSlice(a);
        m_rxOnVfoB = false;
        return {};
    }
    return cmdRxVfo(m_rxOnVfoB ? "0" : "1");
}

// ZZFT — [SSC] 3.3.12.
QByteArray CatKenwoodProtocol::cmdToggleTx()
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    const int b = vfoBSliceId();
    if (b < 0) {
        m_radio->setTxSlice(a->id, !a->tx);
        return {};
    }
    return cmdTxVfo("FT", splitActive() ? "0" : "1");
}

// Key the transmitter. Mirrors RigctlProtocol::cmdSetPtt: move the TX flag to
// this port's slice first (SmartSDR "Auto Switch TX Slice"), then
// RadioModel::setTransmit(true). A port in split keeps TX on VFO B.
void CatKenwoodProtocol::keyTransmit()
{
    const auto a = vfoA();
    if (m_autoSwitchTx && a && !a->tx && !splitActive()) {
        m_radio->setTxSlice(a->id, true);
    }
    m_radio->setTransmit(true);
}

// TX — [SSC] 3.4.24: TX / TX0 = MIC, TX1 = DATA, TX2 = TUNE. Set only.
QByteArray CatKenwoodProtocol::cmdTx(const QByteArray& arg)
{
    if (arg.isEmpty() || arg == "0" || arg == "1") {
        keyTransmit();
        return {};
    }
    if (arg == "2") {
        const auto a = vfoA();
        if (m_autoSwitchTx && a && !a->tx && !splitActive()) {
            m_radio->setTxSlice(a->id, true);
        }
        m_radio->setTune(true);
        return {};
    }
    return err();
}

// ZZTX — [SSC] 3.3.38. Hamlib flex6k_set_ptt sends "ZZTX1;ZZTX;" and
// expects the read-back to match immediately; RadioModel::setTransmit updates
// TransmitModel::isTransmitting() synchronously, so it does.
QByteArray CatKenwoodProtocol::cmdZzTx(const QByteArray& arg)
{
    if (arg.isEmpty()) {
        return QByteArrayLiteral("ZZTX") + (transmitting() ? "1" : "0") + ";";
    }
    if (arg == "1") {
        keyTransmit();
        return {};
    }
    if (arg == "0") {
        return cmdRx();
    }
    return err();
}

QByteArray CatKenwoodProtocol::cmdRx()
{
    if (m_radio->isTuning()) {
        m_radio->setTune(false);
    }
    m_radio->setTransmit(false);
    return {};
}

// RU / RD / ZZRU / ZZRD — [SSC] 3.3.29, 3.3.32, 3.4.17, 3.4.19.
// When only XIT is on, the shared Kenwood offset is the XIT offset (Hamlib
// kenwood_set_xit drives XIT through RU/RD and reads it back from IF P3).
QByteArray CatKenwoodProtocol::cmdRitStep(const QByteArray& arg, int sign)
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    int step = defaultRitStep(a->mode);
    if (!arg.isEmpty()) {
        if (arg.size() > 5 || !parseUInt(arg, step)) {
            return err();
        }
    }
    const bool useXit = a->xitOn && !a->ritOn;
    const int cur = useXit ? a->xitHz : a->ritHz;
    const int next = std::clamp(cur + sign * step, -kMaxOffsetHz, kMaxOffsetHz);
    if (useXit) {
        m_radio->setXit(a->id, true, next);
    } else {
        m_radio->setRit(a->id, a->ritOn, next);
    }
    return {};
}

// RC — [SSC] 3.4.16 (same shared-offset rule as RU/RD).
QByteArray CatKenwoodProtocol::cmdRitClear()
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    if (a->xitOn && !a->ritOn) {
        m_radio->setXit(a->id, true, 0);
    } else {
        m_radio->setRit(a->id, a->ritOn, 0);
    }
    return {};
}

// ZZRG / ZZRW / ZZXG — [SSC] 3.3.30, 3.3.33, 3.3.40.
QByteArray CatKenwoodProtocol::cmdOffsetValue(const QByteArray& name, const QByteArray& arg,
                                              bool vfoBCmd, bool xit)
{
    const auto s = vfoBCmd ? vfoB() : vfoA();
    if (!s) {
        return err();
    }
    if (arg.isEmpty()) {
        return name + formatOffset(xit ? s->xitHz : s->ritHz) + ";";
    }
    int hz = 0;
    if (!parseSignedOffset(arg, hz)) {
        return err();
    }
    if (xit) {
        m_radio->setXit(s->id, s->xitOn, hz);
    } else {
        m_radio->setRit(s->id, s->ritOn, hz);
    }
    return {};
}

// AG — SmartSDR form "AGnnn" (000-100, [SSC] 3.4.2) and the TS-2000 form
// "AG<rx>nnn" (000-255, rx 0 = main/VFO A, 1 = sub/VFO B) used by Hamlib's
// generic Kenwood backends.
QByteArray CatKenwoodProtocol::cmdAudioGain(const QByteArray& arg)
{
    auto sliceFor = [this](int rx) { return rx == 0 ? vfoA() : vfoB(); };

    if (arg.isEmpty()) {
        const auto a = vfoA();
        return a ? "AG" + pad(a->audioGain, 3) + ";" : err();
    }
    int v = 0;
    if (!parseUInt(arg, v)) {
        return err();
    }
    if (arg.size() == 1) {            // TS-2000 read: AG0; / AG1;
        if (v > 1) { return err(); }
        const auto s = sliceFor(v);
        if (!s) { return err(); }
        const int scaled = static_cast<int>(std::lround(s->audioGain * 255.0 / 100.0));
        return "AG" + arg + pad(scaled, 3) + ";";
    }
    if (arg.size() == 3) {            // SmartSDR set: AGnnn (0-100)
        const auto a = vfoA();
        if (!a || v > 100) { return err(); }
        m_radio->setAudioGain(a->id, v);
        return {};
    }
    if (arg.size() == 4) {            // TS-2000 set: AG<rx>nnn (0-255)
        const int rx = arg[0] - '0';
        int level = 0;
        if (rx > 1 || !parseUInt(arg.mid(1), level) || level > 255) { return err(); }
        const auto s = sliceFor(rx);
        if (!s) { return err(); }
        m_radio->setAudioGain(s->id, static_cast<int>(std::lround(level * 100.0 / 255.0)));
        return {};
    }
    return err();
}

// GT / ZZGT — [SSC] 3.3.13 / 3.4.7: 0 off, 2 slow, 3 med, 4 fast.
// Also accepts the TS-2000 3-digit form GTnnn (000 = off).
QByteArray CatKenwoodProtocol::cmdAgc(const QByteArray& name, const QByteArray& arg)
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    if (arg.isEmpty()) {
        return name + QByteArray::number(codeFromAgc(a->agcMode)) + ";";
    }
    int v = 0;
    if (!parseUInt(arg, v)) {
        return err();
    }
    QString mode;
    if (arg.size() == 1) {
        mode = agcFromCode(v);
    } else if (arg.size() == 3 && name == "GT") {
        if (v == 0)       { mode = QStringLiteral("off"); }
        else if (v <= 1)  { mode = QStringLiteral("fast"); }
        else if (v <= 4)  { mode = agcFromCode(v); }
        else if (v <= 20) { mode = QStringLiteral("slow"); }
    }
    if (mode.isEmpty()) {
        return err();
    }
    m_radio->setAgcMode(a->id, mode);
    return {};
}

// ZZFI / ZZFJ — [SSC] 3.3.9 / 3.3.10, index tables from [HL] flex6xxx.c.
QByteArray CatKenwoodProtocol::cmdFilterIndex(const QByteArray& name, const QByteArray& arg, bool vfoBCmd)
{
    const auto s = vfoBCmd ? vfoB() : vfoA();
    if (!s) {
        return err();
    }
    const int width = std::abs(s->filterHigh - s->filterLow);
    if (arg.isEmpty()) {
        return name + pad(filterIndexForWidth(s->mode, width), 2) + ";";
    }
    int idx = 0;
    if (arg.size() != 2 || !parseUInt(arg, idx)) {
        return err();
    }
    const int w = filterWidthForIndex(s->mode, idx);
    if (w <= 0) {
        return err();
    }
    int lo = 0;
    int hi = 0;
    if (isAmFamily(s->mode)) {
        lo = -w / 2;
        hi = w / 2;
    } else if (s->mode == "USB" || s->mode.startsWith("FDV")) {
        // Keep the low edge (voice low-cut) and move the high edge.
        lo = std::max(0, s->filterLow);
        hi = lo + w;
    } else if (s->mode == "LSB") {
        hi = std::min(0, s->filterHigh);
        lo = hi - w;
    } else {
        // CW / DIGx / RTTY: narrow around the current passband centre.
        const int centre = (s->filterLow + s->filterHigh) / 2;
        lo = centre - w / 2;
        hi = lo + w;
        if (s->mode == "DIGU" && lo < 0) {
            hi -= lo;
            lo = 0;
        } else if (s->mode == "DIGL" && hi > 0) {
            lo -= hi;
            hi = 0;
        }
    }
    m_radio->setFilter(s->id, lo, hi);
    return {};
}

// SH / SL — [SSC] 3.4.21 / 3.4.22.
QByteArray CatKenwoodProtocol::cmdFilterCut(const QByteArray& arg, bool high)
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    const Side side = sideOf(a->mode);
    const bool am = isAmFamily(a->mode) || side == Side::Symmetric;

    // Distances from the carrier.
    int lowCut = 0;
    int highCut = 0;
    if (side == Side::Lower) {
        lowCut = -a->filterHigh;
        highCut = -a->filterLow;
    } else if (side == Side::Symmetric) {
        lowCut = 0;
        highCut = a->filterHigh;
    } else {
        lowCut = a->filterLow;
        highCut = a->filterHigh;
    }

    const QByteArray name = high ? QByteArrayLiteral("SH") : QByteArrayLiteral("SL");
    if (arg.isEmpty()) {
        int idx = 0;
        if (high) {
            idx = am ? nearestIndex(kShAm, highCut) : nearestIndex(kShSsb, highCut);
        } else {
            idx = am ? nearestIndex(kSlAm, lowCut) : nearestIndex(kSlSsb, lowCut);
        }
        return name + pad(idx, 2) + ";";
    }
    int idx = 0;
    if (arg.size() != 2 || !parseUInt(arg, idx)) {
        return err();
    }
    const int maxIdx = am ? 3 : 11;
    if (idx > maxIdx) {
        return err();
    }
    const auto tableValue = [am, high](int i) {
        if (high) {
            return am ? kShAm[static_cast<std::size_t>(i)] : kShSsb[static_cast<std::size_t>(i)];
        }
        return am ? kSlAm[static_cast<std::size_t>(i)] : kSlSsb[static_cast<std::size_t>(i)];
    };
    if (high) {
        highCut = tableValue(idx);
    } else {
        lowCut = tableValue(idx);
    }
    int lo = 0;
    int hi = 0;
    if (side == Side::Symmetric) {
        if (!high) {
            return {};     // low cut has no meaning for a symmetric AM/FM filter
        }
        lo = -highCut;
        hi = highCut;
    } else if (side == Side::Lower) {
        lo = -highCut;
        hi = -lowCut;
    } else {
        lo = lowCut;
        hi = highCut;
    }
    if (lo >= hi) {
        return err();
    }
    m_radio->setFilter(a->id, lo, hi);
    return {};
}

// SM / ZZSM — [SSC] 3.3.36 / 3.4.23. "SM0;" is the TS-2000 form (4 digits).
QByteArray CatKenwoodProtocol::cmdSMeter(const QByteArray& arg, bool zz)
{
    const auto a = vfoA();
    if (!a) {
        return err();
    }
    if (zz) {
        return arg.isEmpty() ? "ZZSM" + pad(sMeterZz(a->sMeterDbm), 3) + ";" : err();
    }
    if (arg.isEmpty()) {
        return "SM" + pad(sMeterKenwood(a->sMeterDbm), 3) + ";";
    }
    if (arg == "0" || arg == "1") {
        const auto s = (arg == "0") ? a : vfoB();
        if (!s) {
            return err();
        }
        return "SM" + arg + pad(sMeterKenwood(s->sMeterDbm), 4) + ";";
    }
    return err();
}

// KY — [SSC] 3.4.11. "KY;" → "KY0;" (buffer always available). Set form is
// "KY" + P1 + text; SmartSDR is lenient about the 24-char padding. An empty
// text ("KY0;", Hamlib stop_morse) clears the CWX buffer.
QByteArray CatKenwoodProtocol::cmdKey(const QByteArray& rawArg)
{
    if (rawArg.isEmpty()) {
        return QByteArrayLiteral("KY0;");
    }
    QByteArray text = rawArg;
    const char p1 = text[0];
    if (p1 == ' ' || (p1 >= '0' && p1 <= '2')) {
        text.remove(0, 1);
    }
    while (!text.isEmpty() && (text.endsWith(' ') || text.endsWith('\r') || text.endsWith('\n'))) {
        text.chop(1);
    }
    if (text.size() > kMaxKyChars) {
        return err();
    }
    for (char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
            return err();
        }
    }
    if (text.isEmpty()) {
        m_radio->clearCw();
        return {};
    }
    m_radio->sendCw(QString::fromLatin1(text));
    return {};
}

// SQ — TS-2000 "SQ<rx>nnn" (000-255) mapped onto slice squelch level 0-100.
QByteArray CatKenwoodProtocol::cmdSquelch(const QByteArray& arg)
{
    if (arg.isEmpty() || (arg[0] != '0' && arg[0] != '1')) {
        return err();
    }
    const auto s = (arg[0] == '0') ? vfoA() : vfoB();
    if (!s) {
        return err();
    }
    if (arg.size() == 1) {
        const int scaled = static_cast<int>(std::lround(s->squelchLevel * 255.0 / 100.0));
        return "SQ" + arg.left(1) + pad(scaled, 3) + ";";
    }
    int level = 0;
    if (arg.size() != 4 || !parseUInt(arg.mid(1), level) || level > 255) {
        return err();
    }
    m_radio->setSquelch(s->id, s->squelchOn, static_cast<int>(std::lround(level * 100.0 / 255.0)));
    return {};
}

// ── Auto-information ────────────────────────────────────────────────────────

CatKenwoodProtocol::AiSnapshot CatKenwoodProtocol::takeSnapshot() const
{
    AiSnapshot snap;
    const auto a = vfoA();
    if (!a) {
        return snap;
    }
    snap.valid = true;
    snap.freqA = a->freqHz;
    const auto b = vfoB();
    snap.freqB = b ? b->freqHz : -1;
    snap.md = modeToMd(a->mode);
    snap.tx = transmitting();
    return snap;
}

// [SSC] 3.3.2 / 3.4.1: with AI on, frequency changes of VFO A or B are sent
// as FA/FB answers. Like a TS-2000 we also push MD on mode changes and IF on
// TX/RX transitions so AI-driven loggers see band/mode/PTT without polling.
QByteArray CatKenwoodProtocol::pollAutoInfo()
{
    if (m_aiMode == 0) {
        return {};
    }
    adoptSplitSlice();   // pick up a split slice that appeared since the last command
    const AiSnapshot now = takeSnapshot();
    if (!now.valid) {
        return {};
    }
    if (!m_aiLast.valid) {
        m_aiLast = now;
        return {};
    }
    QByteArray out;
    if (now.freqA != m_aiLast.freqA) {
        out += "FA" + formatFreq(now.freqA, m_freqDigits) + ";";
    }
    if (now.freqB >= 0 && now.freqB != m_aiLast.freqB) {
        out += "FB" + formatFreq(now.freqB, m_freqDigits) + ";";
    }
    if (now.md != m_aiLast.md) {
        out += "MD" + QByteArray::number(now.md) + ";";
    }
    if (now.tx != m_aiLast.tx) {
        out += cmdInfo(false);
    }
    m_aiLast = now;
    return out;
}

} // namespace AetherSDR
