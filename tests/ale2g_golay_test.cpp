// 2G ALE word layer tests: Golay (24,12) per MIL-STD-188-141B figure A-6,
// error correction / detection, interleaving + B check-bit inversion,
// address and AMD word construction.
//
// Build: `ale2g_golay_test` (Qt6::Core).  Exit code 0 on all pass.

#include "core/Ale2gWord.h"

#include <QRandomGenerator>

#include <bit>
#include <cstdio>

using namespace AetherSDR::Ale2g;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
}

} // namespace

int main()
{
    // ── Reference parity values (MIL-STD-188-141B fig. A-6 rows; identical to
    //    LinuxALE golay.c encode_table[1], [2], [0x800]).
    check(golayParity(0x001) == 0x5C7, "parity(0x001) == 0x5C7 (fig. A-6 row 12)");
    check(golayParity(0x002) == 0xB8D, "parity(0x002) == 0xB8D (fig. A-6 row 11)");
    check(golayParity(0x800) == 0xAE3, "parity(0x800) == 0xAE3 (fig. A-6 row 1)");

    // ── Linear code, minimum distance 8 (extended Golay).
    int minWeight = 24;
    for (int d = 1; d < 4096; ++d) {
        minWeight = std::min(minWeight, std::popcount(golayEncode(static_cast<std::uint16_t>(d))));
    }
    check(minWeight == 8, "minimum codeword weight is 8");

    // ── Correct all error patterns of weight <= 3 for sampled data words;
    //    detect all weight-4 patterns tried.
    bool allCorrected = true;
    bool allDetected = true;
    auto* rng = QRandomGenerator::global();
    for (int trial = 0; trial < 40; ++trial) {
        const auto data = static_cast<std::uint16_t>(rng->bounded(4096));
        const std::uint32_t code = golayEncode(data);
        for (int a = 0; a < 24; ++a) {
            for (int b = a; b < 24; ++b) {
                for (int c = b; c < 24; ++c) {
                    std::uint32_t e = (1u << a) | (1u << b) | (1u << c);
                    std::uint16_t out = 0;
                    int errs = -1;
                    const bool ok = golayDecode(code ^ e, &out, &errs);
                    if (!ok || out != data || errs != std::popcount(e)) {
                        allCorrected = false;
                    }
                }
            }
        }
        for (int n = 0; n < 200; ++n) {
            std::uint32_t e = 0;
            while (std::popcount(e) < 4) {
                e |= 1u << rng->bounded(24);
            }
            std::uint16_t out = 0;
            int errs = 0;
            if (golayDecode(code ^ e, &out, &errs)) {
                allDetected = false;
            }
        }
    }
    check(allCorrected, "all 1..3-bit errors corrected (40 random data words)");
    check(allDetected, "4-bit errors detected as uncorrectable");

    // ── On-air word: interleave order A1 B1 .. A24 B24 S49, B check bits inverted.
    const std::uint32_t word = makeWord(Preamble::To, 'K', '1', 'A');
    const auto bits = encodeOnAirWord(word);
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    for (int i = 0; i < 24; ++i) {
        a = (a << 1) | bits[static_cast<size_t>(2 * i)];
        b = (b << 1) | bits[static_cast<size_t>(2 * i + 1)];
    }
    check(bits[48] == 0, "stuff bit S49 is 0");
    check(a == golayEncode(static_cast<std::uint16_t>(word >> 12)), "A half = Golay(W1..W12)");
    check(b == (golayEncode(static_cast<std::uint16_t>(word & 0xFFF)) ^ 0xFFF),
          "B half = Golay(W13..W24) with check bits inverted");
    check((a >> 21) == static_cast<std::uint32_t>(Preamble::To), "preamble TO = 010 in W1..W3");

    // ── Majority decode with independent errors in each copy.
    std::uint8_t triple[147];
    for (int copy = 0; copy < 3; ++copy) {
        for (int i = 0; i < 49; ++i) {
            triple[copy * 49 + i] = bits[static_cast<size_t>(i)];
        }
    }
    // Different single-bit errors in each copy: fully fixed by 2/3 vote.
    triple[0 * 49 + 5] ^= 1;
    triple[1 * 49 + 17] ^= 1;
    triple[2 * 49 + 40] ^= 1;
    MajorityDecode md = decodeRedundantWord(triple);
    check(md.ok && md.word == word && md.errorsA + md.errorsB == 0 && md.nonUnanimous == 3,
          "majority vote removes one error per copy (3 non-unanimous votes)");
    // Same bits wrong in two copies -> Golay has to correct them.
    triple[1 * 49 + 2] ^= 1;
    triple[2 * 49 + 2] ^= 1;   // A2 wrong after vote
    triple[1 * 49 + 9] ^= 1;
    triple[2 * 49 + 9] ^= 1;   // B5 wrong after vote
    md = decodeRedundantWord(triple);
    check(md.ok && md.word == word && md.errorsA == 1 && md.errorsB == 1,
          "Golay corrects residual errors after voting");

    // ── Addressing (A.5.2.4).
    QVector<std::uint32_t> w = addressWords(Preamble::To, QStringLiteral("k1abc"));
    check(w.size() == 2 && wordToString(w[0]) == QStringLiteral("TO K1A")
              && wordToString(w[1]) == QStringLiteral("DATA BC@"),
          "TO K1A / DATA BC@ for K1ABC");
    w = addressWords(Preamble::Tis, QStringLiteral("MISSISSIPPI"));
    check(w.size() == 4 && wordToString(w[1]) == QStringLiteral("DATA SIS")
              && wordToString(w[2]) == QStringLiteral("REP SIP")
              && wordToString(w[3]) == QStringLiteral("DATA PI@"),
          "MISSISSIPPI -> TIS MIS, DATA SIS, REP SIP, DATA PI@ (A.5.2.4.4.2 note 2)");
    check(unstuffAddress(QStringLiteral("K1ABC@")) == QStringLiteral("K1ABC"), "unstuff trailing @");
    check(isAllCallAddress(QStringLiteral("@?@")) && !isAnyCallAddress(QStringLiteral("@?@")), "@?@ is AllCall");
    check(isAnyCallAddress(QStringLiteral("@@?")) && !isAllCallAddress(QStringLiteral("@@?")), "@@? is AnyCall");
    check(addressMatchesSelf(QStringLiteral("K1A?C"), QStringLiteral("K1ABC")), "wildcard ? matches");
    check(!addressMatchesSelf(QStringLiteral("K1ABD"), QStringLiteral("K1ABC")), "different address rejected");
    check(addressMatchesSelf(QStringLiteral("@C@"), QStringLiteral("K1ABC")), "selective AllCall on last char");

    // ── AMD (A.5.7.2): CMD, DATA, REP ..., space stuffed, expanded-64 only.
    w = amdWords(QStringLiteral("Hello, world!"));
    check(w.size() == 5 && wordPreamble(w[0]) == Preamble::Cmd && wordPreamble(w[1]) == Preamble::Data
              && wordPreamble(w[2]) == Preamble::Rep && wordChars(w[0]) == QStringLiteral("HEL")
              && wordChars(w[4]) == QStringLiteral("!  "),
          "AMD words CMD/DATA/REP with upper-casing and space stuffing");
    bool all64 = true;
    for (std::uint32_t x : w) {
        all64 = all64 && allExpanded64(x);
    }
    check(all64, "AMD characters in expanded-64 subset");

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
