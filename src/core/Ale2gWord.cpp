#include "Ale2gWord.h"

#include <algorithm>
#include <bit>

namespace AetherSDR::Ale2g {

namespace {

// Parity matrix P from MIL-STD-188-141B figure A-6 (generator matrix
// G = [I12 : P]).  Row i is added when data bit W(i+1) (MSB first) is 1.
// Cross-checked against LinuxALE golay.c encode_table (e.g. data 0x001 ->
// 0x5C7, data 0x800 -> 0xAE3).
constexpr std::array<std::uint16_t, 12> kParityRows{{
    0xAE3, // 101 011 100 011
    0xF92, // 111 110 010 010
    0xD2B, // 110 100 101 011
    0xC76, // 110 001 110 110
    0xCD9, // 110 011 011 001
    0x66D, // 011 001 101 101
    0x337, // 001 100 110 111
    0xB78, // 101 101 111 000
    0x5BC, // 010 110 111 100
    0x2DE, // 001 011 011 110
    0xB8D, // 101 110 001 101
    0x5C7, // 010 111 000 111
}};

struct GolayTables {
    std::array<std::uint16_t, 4096> parity{};
    // syndrome -> 24-bit minimum-weight error pattern, or 0xFFFFFFFF if the
    // syndrome belongs to a weight>=4 coset (uncorrectable).
    std::array<std::uint32_t, 4096> errorPattern{};
    std::array<std::uint8_t, 4096> errorWeight{};

    GolayTables()
    {
        for (int d = 0; d < 4096; ++d) {
            std::uint16_t p = 0;
            for (int bit = 0; bit < 12; ++bit) {
                if (d & (1 << (11 - bit))) {
                    p ^= kParityRows[static_cast<size_t>(bit)];
                }
            }
            parity[static_cast<size_t>(d)] = p;
        }
        errorPattern.fill(0xFFFFFFFFu);
        errorWeight.fill(0xFF);
        auto consider = [this](std::uint32_t e) {
            const std::uint16_t eData = static_cast<std::uint16_t>((e >> 12) & 0xFFF);
            const std::uint16_t ePar = static_cast<std::uint16_t>(e & 0xFFF);
            const std::uint16_t syn = parity[eData] ^ ePar;
            const int w = std::popcount(e);
            if (w < errorWeight[syn]) {
                errorWeight[syn] = static_cast<std::uint8_t>(w);
                errorPattern[syn] = e;
            }
        };
        consider(0);
        for (int a = 0; a < 24; ++a) {
            consider(1u << a);
            for (int b = a + 1; b < 24; ++b) {
                consider((1u << a) | (1u << b));
                for (int c = b + 1; c < 24; ++c) {
                    consider((1u << a) | (1u << b) | (1u << c));
                }
            }
        }
    }
};

const GolayTables& tables()
{
    static const GolayTables t;
    return t;
}

// Tone index (0 = 750 Hz) for each on-air bit triple is handled in the modem;
// this file stays waveform-agnostic.

} // namespace

QString preambleName(Preamble p)
{
    switch (p) {
    case Preamble::Data: return QStringLiteral("DATA");
    case Preamble::Thru: return QStringLiteral("THRU");
    case Preamble::To:   return QStringLiteral("TO");
    case Preamble::Twas: return QStringLiteral("TWAS");
    case Preamble::From: return QStringLiteral("FROM");
    case Preamble::Tis:  return QStringLiteral("TIS");
    case Preamble::Cmd:  return QStringLiteral("CMD");
    case Preamble::Rep:  return QStringLiteral("REP");
    }
    return QStringLiteral("?");
}

std::uint32_t makeWord(Preamble p, char c1, char c2, char c3)
{
    const std::uint32_t data = ((static_cast<std::uint32_t>(c1) & 0x7F) << 14)
                               | ((static_cast<std::uint32_t>(c2) & 0x7F) << 7)
                               | (static_cast<std::uint32_t>(c3) & 0x7F);
    return makeWordBits(p, data);
}

std::uint32_t makeWordBits(Preamble p, std::uint32_t data21)
{
    return (static_cast<std::uint32_t>(p) << 21) | (data21 & 0x1FFFFF);
}

Preamble wordPreamble(std::uint32_t word)
{
    return static_cast<Preamble>((word >> 21) & 0x7);
}

unsigned char wordChar(std::uint32_t word, int index)
{
    const int shift = 14 - 7 * std::clamp(index, 0, 2);
    return static_cast<unsigned char>((word >> shift) & 0x7F);
}

QString wordChars(std::uint32_t word)
{
    QString s;
    s.reserve(3);
    for (int i = 0; i < 3; ++i) {
        s += QChar::fromLatin1(static_cast<char>(wordChar(word, i)));
    }
    return s;
}

QString wordToString(std::uint32_t word)
{
    QString chars;
    for (int i = 0; i < 3; ++i) {
        const unsigned char c = wordChar(word, i);
        chars += (c >= 0x20 && c < 0x7F) ? QChar::fromLatin1(static_cast<char>(c))
                                         : QChar::fromLatin1('.');
    }
    return QStringLiteral("%1 %2").arg(preambleName(wordPreamble(word)), chars);
}

bool isBasic38(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '@' || c == '?';
}

bool isExpanded64(unsigned char c)
{
    // A.5.7.2.1: b7 b6 = "01" or "10"  ->  0x20..0x5F
    return c >= 0x20 && c <= 0x5F;
}

bool allBasic38(std::uint32_t word)
{
    return isBasic38(wordChar(word, 0)) && isBasic38(wordChar(word, 1))
           && isBasic38(wordChar(word, 2));
}

bool allExpanded64(std::uint32_t word)
{
    return isExpanded64(wordChar(word, 0)) && isExpanded64(wordChar(word, 1))
           && isExpanded64(wordChar(word, 2));
}

std::uint16_t golayParity(std::uint16_t data12)
{
    return tables().parity[data12 & 0xFFF];
}

std::uint32_t golayEncode(std::uint16_t data12)
{
    const std::uint16_t d = data12 & 0xFFF;
    return (static_cast<std::uint32_t>(d) << 12) | golayParity(d);
}

bool golayDecode(std::uint32_t received24, std::uint16_t* data12, int* errors)
{
    const auto& t = tables();
    const std::uint16_t d = static_cast<std::uint16_t>((received24 >> 12) & 0xFFF);
    const std::uint16_t p = static_cast<std::uint16_t>(received24 & 0xFFF);
    const std::uint16_t syn = t.parity[d] ^ p;
    const std::uint32_t e = t.errorPattern[syn];
    if (e == 0xFFFFFFFFu) {
        if (errors) {
            *errors = 4;
        }
        return false;
    }
    if (data12) {
        *data12 = static_cast<std::uint16_t>(d ^ ((e >> 12) & 0xFFF));
    }
    if (errors) {
        *errors = t.errorWeight[syn];
    }
    return true;
}

std::array<std::uint8_t, kBitsPerWord> encodeOnAirWord(std::uint32_t word24)
{
    const std::uint32_t a = golayEncode(static_cast<std::uint16_t>((word24 >> 12) & 0xFFF));
    // Check bits G13..G24 of the B half are inverted before interleaving.
    const std::uint32_t b = golayEncode(static_cast<std::uint16_t>(word24 & 0xFFF)) ^ 0xFFFu;
    std::array<std::uint8_t, kBitsPerWord> bits{};
    for (int i = 0; i < 24; ++i) {
        bits[static_cast<size_t>(2 * i)] = static_cast<std::uint8_t>((a >> (23 - i)) & 1u);
        bits[static_cast<size_t>(2 * i + 1)] = static_cast<std::uint8_t>((b >> (23 - i)) & 1u);
    }
    bits[48] = 0;   // S49 stuff bit
    return bits;
}

MajorityDecode decodeRedundantWord(const std::uint8_t* bits147)
{
    MajorityDecode out;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    for (int i = 0; i < 48; ++i) {
        const int sum = (bits147[i] ? 1 : 0) + (bits147[i + kBitsPerWord] ? 1 : 0)
                        + (bits147[i + 2 * kBitsPerWord] ? 1 : 0);
        if (sum == 1 || sum == 2) {
            ++out.nonUnanimous;
        }
        const std::uint32_t bit = sum >= 2 ? 1u : 0u;
        if ((i & 1) == 0) {
            a = (a << 1) | bit;
        } else {
            b = (b << 1) | bit;
        }
    }
    b ^= 0xFFFu;
    std::uint16_t da = 0;
    std::uint16_t db = 0;
    const bool okA = golayDecode(a, &da, &out.errorsA);
    const bool okB = golayDecode(b, &db, &out.errorsB);
    out.ok = okA && okB;
    out.word = (static_cast<std::uint32_t>(da) << 12) | db;
    return out;
}

QString normalizeAddress(const QString& address)
{
    QString out;
    const QString up = address.trimmed().toUpper();
    for (QChar ch : up) {
        const char16_t u = ch.unicode();
        if (u < 0x80 && isBasic38(static_cast<unsigned char>(u))) {
            out += ch;
        }
        if (out.size() >= kMaxAddressChars) {
            break;
        }
    }
    return out;
}

bool isValidAddress(const QString& address)
{
    if (address.isEmpty() || address.size() > kMaxAddressChars) {
        return false;
    }
    for (QChar ch : address) {
        const char16_t u = ch.unicode();
        if (u >= 0x80 || !isBasic38(static_cast<unsigned char>(u))) {
            return false;
        }
    }
    return true;
}

QVector<std::uint32_t> addressWords(Preamble first, const QString& address)
{
    QVector<std::uint32_t> words;
    const QString addr = normalizeAddress(address);
    if (addr.isEmpty()) {
        return words;
    }
    const int count = static_cast<int>((addr.size() + 2) / 3);
    for (int w = 0; w < count; ++w) {
        char chars[3] = {'@', '@', '@'};
        for (int i = 0; i < 3; ++i) {
            const int idx = w * 3 + i;
            if (idx < addr.size()) {
                chars[i] = addr.at(idx).toLatin1();
            }
        }
        Preamble p = first;
        if (w > 0) {
            p = (w % 2 == 1) ? Preamble::Data : Preamble::Rep;
        }
        words.push_back(makeWord(p, chars[0], chars[1], chars[2]));
    }
    return words;
}

std::uint32_t addressFirstWord(Preamble first, const QString& address)
{
    const QVector<std::uint32_t> words = addressWords(first, address);
    return words.isEmpty() ? makeWord(first, '@', '@', '@') : words.first();
}

QString unstuffAddress(const QString& raw)
{
    if (raw.startsWith(QLatin1Char('@'))) {
        return raw;
    }
    QString out = raw;
    while (out.endsWith(QLatin1Char('@'))) {
        out.chop(1);
    }
    return out;
}

bool isAllCallAddress(const QString& raw)
{
    // "@?@" global, "@A@" selective (A alphanumeric)
    if (raw.size() != 3 || raw.at(0) != QLatin1Char('@') || raw.at(2) != QLatin1Char('@')) {
        return false;
    }
    const QChar mid = raw.at(1);
    return mid == QLatin1Char('?') || (mid != QLatin1Char('@') && isBasic38(static_cast<unsigned char>(mid.toLatin1())));
}

bool isAnyCallAddress(const QString& raw)
{
    // "@@?" global, "@@A" selective, "@AB" / "@A?" double selective
    if (raw.size() != 3 || raw.at(0) != QLatin1Char('@') || raw.at(2) == QLatin1Char('@')) {
        return false;
    }
    return !isAllCallAddress(raw);
}

bool addressMatchesSelf(const QString& received, const QString& self)
{
    if (received.isEmpty() || self.isEmpty()) {
        return false;
    }
    if (isAllCallAddress(received)) {
        const QChar sel = received.at(1);
        return sel == QLatin1Char('?') || self.endsWith(sel);
    }
    if (isAnyCallAddress(received)) {
        const QChar c1 = received.at(1);
        const QChar c2 = received.at(2);
        if (c1 == QLatin1Char('@')) {
            return c2 == QLatin1Char('?') || self.endsWith(c2);
        }
        if (self.size() < 2) {
            return false;
        }
        const QChar s1 = self.at(self.size() - 2);
        const QChar s2 = self.at(self.size() - 1);
        return s1 == c1 && (c2 == QLatin1Char('?') || s2 == c2);
    }
    if (received.size() != self.size()) {
        return false;
    }
    for (int i = 0; i < received.size(); ++i) {
        const QChar r = received.at(i);
        if (r == QLatin1Char('?')) {
            if (!self.at(i).isLetterOrNumber()) {
                return false;
            }
            continue;
        }
        if (r != self.at(i)) {
            return false;
        }
    }
    return true;
}

QString normalizeAmdText(const QString& text)
{
    QString out;
    const QString up = text.toUpper();
    for (QChar ch : up) {
        char16_t u = ch.unicode();
        if (u == '\t' || u == '\n' || u == '\r') {
            u = ' ';
        }
        if (u < 0x80 && isExpanded64(static_cast<unsigned char>(u))) {
            out += QChar(u);
        }
        if (out.size() >= kMaxAmdChars) {
            break;
        }
    }
    return out;
}

QVector<std::uint32_t> amdWords(const QString& text)
{
    QVector<std::uint32_t> words;
    const QString msg = normalizeAmdText(text);
    if (msg.isEmpty()) {
        return words;
    }
    const int count = static_cast<int>((msg.size() + 2) / 3);
    for (int w = 0; w < count; ++w) {
        char chars[3] = {' ', ' ', ' '};
        for (int i = 0; i < 3; ++i) {
            const int idx = w * 3 + i;
            if (idx < msg.size()) {
                chars[i] = msg.at(idx).toLatin1();
            }
        }
        Preamble p = Preamble::Cmd;
        if (w > 0) {
            p = (w % 2 == 1) ? Preamble::Data : Preamble::Rep;
        }
        words.push_back(makeWord(p, chars[0], chars[1], chars[2]));
    }
    return words;
}

} // namespace AetherSDR::Ale2g
