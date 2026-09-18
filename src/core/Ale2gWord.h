#pragma once

// 2G ALE (MIL-STD-188-141B Appendix A) word layer: word packing, Golay (24,12)
// FEC, interleaving / triple redundancy, character sets and address / AMD
// word sequences.  Pure functions, no Qt event loop, safe on any thread.
//
// References (see also docs in the implementation file):
//  - MIL-STD-188-141B App. A, A.5.1 (tones/timing), A.5.2.2 (Golay, fig. A-6
//    generator matrix, fig. A-10 interleaving, B check bits inverted, S49 = 0),
//    A.5.2.3 (preambles), A.5.2.4 (Basic-38 addressing, '@' stuffing),
//    A.5.7.2 (AMD, expanded-64 set, space stuffing).
//    Copy used: https://hflink.com/standards/ALE_standard_188_141B.pdf
//  - LinuxALE golay.c / modem.c (C. Brain G4GUO & I. Toivanen): identical
//    parity table, B-word check-bit inversion, 48-bit majority + stuff bit.

#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <cstdint>

namespace AetherSDR::Ale2g {

// ── Waveform constants (A.5.1) ──────────────────────────────────────────────
constexpr int kSampleRate          = 24000;   // AetherSDR audio rate
constexpr int kSymbolRate          = 125;     // 8 ms per tone
constexpr int kSamplesPerSymbol    = kSampleRate / kSymbolRate;   // 192
constexpr int kBitsPerSymbol       = 3;
constexpr int kBitsPerWord         = 49;      // 48 interleaved FEC bits + S49
constexpr int kBitsPerRedundantWord = kBitsPerWord * 3;             // 147
constexpr int kSymbolsPerRedundantWord = kBitsPerRedundantWord / kBitsPerSymbol; // 49
constexpr int kSamplesPerRedundantWord = kSymbolsPerRedundantWord * kSamplesPerSymbol; // 9408
constexpr int kTrwMs               = 392;     // redundant word period (3 Tw)
constexpr int kToneBaseHz          = 750;
constexpr int kToneStepHz          = 250;

// ── Preambles (A.5.2.3.1.3, table A-VIII) ───────────────────────────────────
enum class Preamble : std::uint8_t {
    Data = 0,
    Thru = 1,
    To   = 2,
    Twas = 3,
    From = 4,
    Tis  = 5,
    Cmd  = 6,
    Rep  = 7,
};

QString preambleName(Preamble p);

// ── 24-bit basic word: W1..W3 preamble, then 3 x 7-bit characters ──────────
std::uint32_t makeWord(Preamble p, char c1, char c2, char c3);
std::uint32_t makeWordBits(Preamble p, std::uint32_t data21);
Preamble wordPreamble(std::uint32_t word);
unsigned char wordChar(std::uint32_t word, int index);   // index 0..2
QString wordChars(std::uint32_t word);                    // raw 3 chars (latin1)
QString wordToString(std::uint32_t word);                 // "TO K1A"

bool isBasic38(unsigned char c);      // A-Z 0-9 @ ?
bool isExpanded64(unsigned char c);   // 0x20..0x5F
bool allBasic38(std::uint32_t word);
bool allExpanded64(std::uint32_t word);

// ── Golay (24,12) extended code (A.5.2.2.2, figure A-6) ─────────────────────
std::uint16_t golayParity(std::uint16_t data12);
std::uint32_t golayEncode(std::uint16_t data12);          // data<<12 | parity
// Corrects up to 3 bit errors.  Returns false for >=4 (detected, uncorrectable).
bool golayDecode(std::uint32_t received24, std::uint16_t* data12, int* errors);

// ── On-air word (A.5.2.2.3, figure A-10) ───────────────────────────────────
// 49 bits in transmit order: A1 B1 A2 B2 ... A24 B24 S49.  A = Golay(W1..W12),
// B = Golay(W13..W24) with its 12 check bits inverted.
std::array<std::uint8_t, kBitsPerWord> encodeOnAirWord(std::uint32_t word24);

struct MajorityDecode {
    bool ok{false};              // both Golay halves decodable
    std::uint32_t word{0};
    int errorsA{0};
    int errorsB{0};
    int nonUnanimous{0};         // 0..48, the A.5.4.1.1 BER measure
};

// `bits` points at 147 hard bits (three consecutive 49-bit copies, oldest
// first).  2/3 majority vote per bit, S49 ignored, de-interleave, Golay.
MajorityDecode decodeRedundantWord(const std::uint8_t* bits147);

// ── Addressing (A.5.2.4) ────────────────────────────────────────────────────
constexpr int kMaxAddressChars = 15;
QString normalizeAddress(const QString& address);  // upper-case, Basic-38 only, <=15
bool isValidAddress(const QString& address);
// Whole address in first-preamble / DATA / REP sequence, '@'-stuffed.
QVector<std::uint32_t> addressWords(Preamble first, const QString& address);
// First word only (scanning call), '@'-stuffed if shorter than 3 chars.
std::uint32_t addressFirstWord(Preamble first, const QString& address);
// Strip trailing '@' stuffing (special addresses beginning with '@' are kept).
QString unstuffAddress(const QString& raw);

bool isAllCallAddress(const QString& raw);   // "@?@" or selective "@A@"
bool isAnyCallAddress(const QString& raw);   // "@@?", "@@A" or "@AB"
// True when a received (possibly wildcard) address designates `self`.
bool addressMatchesSelf(const QString& received, const QString& self);

// ── AMD (A.5.7.2) ───────────────────────────────────────────────────────────
constexpr int kMaxAmdChars = 90;
QString normalizeAmdText(const QString& text);   // upper-case, expanded-64, <=90
// CMD, DATA, REP, DATA ... words, last word space-stuffed.
QVector<std::uint32_t> amdWords(const QString& text);

} // namespace AetherSDR::Ale2g
