#pragma once

#include <QString>

namespace AetherSDR::RadeEooText {

// FreeDV RADE V1 End-of-Over (EOO) callsign codec.
//
// freedv-gui 2.x sends the station callsign in the RADE EOO frame: the
// rade_n_eoo_bits() soft bits handed to rade_tx_set_eoo_bits() carry
//   CRC-8 (poly 0x1D) + up to 8 chars of a 6-bit OTA charset
//   → 56 info bits → LDPC(112,56) "HRA_56_56" (codec2)
//   → golden-prime symbol interleave (b = 37 for 56 QPSK symbols)
//   → QPSK 00=(1,0) 01=(0,1) 10=(0,-1) 11=(-1,0); remaining bits are
//     filled with the known symbol (1,0).
//
// Wire-compatible with:
//   drowe67/freedv-gui src/pipeline/rade_text.c (freedv-gui 2.0, codec2 LDPC)
//   tmiw/freedv-backend src/pipeline/rade_text.cpp @ 8990bd09 (C++ port,
//     BP decoder + dense HRA_56_56 matrix) — BSD-2-Clause, Mooneer Salem.
// This is a self-contained re-implementation (no codec2 dependency, since
// RADE builds without codec2).  The H matrix is codec2's HRA_56_56_H_rows.
//
// Pure functions, no state — safe to call from the RADE worker thread.

constexpr int kMaxChars = 8;       // RADE_TEXT_MAX_LENGTH
constexpr int kInfoBits = 56;      // LDPC k
constexpr int kCodeBits = 112;     // LDPC n (= float count consumed from EOO bits)
constexpr int kSymbols = kCodeBits / 2;

// Upper-cases and drops characters the OTA charset cannot carry, then
// truncates to kMaxChars (what will actually be heard at the far end).
QString sanitize(const QString& callsign);

// Fills eooBits[0..nEooBits) (nEooBits = rade_n_eoo_bits(), must be
// >= kCodeBits) with +/-1/0 soft bits for rade_tx_set_eoo_bits().
// Returns false (and leaves eooBits untouched) if nEooBits is too small.
bool encode(const QString& callsign, float* eooBits, int nEooBits);

struct DecodeResult {
    bool    ok{false};        // LDPC converged/accepted AND CRC matched
    QString callsign;         // valid only when ok
    int     iterations{0};    // BP iterations used
    int     unsatisfiedChecks{0};
};

// Decodes the soft eoo_out[] from rade_rx() (IQIQ... order, nEooBits floats).
DecodeResult decode(const float* eooSoft, int nEooBits);

} // namespace AetherSDR::RadeEooText
