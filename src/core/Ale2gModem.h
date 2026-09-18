#pragma once

// 2G ALE 8-FSK modem (MIL-STD-188-141B App. A, A.5.1 / A.5.2.6).
//
// Modulator: redundant words -> 147 on-air bits each -> 49 tri-bit symbols ->
// phase-continuous tones 750..2500 Hz (250 Hz step, Gray-coded per A.5.1.2)
// at 125 baud, rendered at 24 kHz (192 samples/symbol, integer cycles per
// symbol so every symbol starts at a cosine peak: "slope zero" transitions).
//
// Demodulator: 8 symbol-timing phases (24-sample steps) with Goertzel tone
// detectors, 2/3 majority vote over three word copies, Golay decode and the
// A.5.2.6.3 synchronisation criteria (unanimous-vote threshold, Golay, Basic-38
// characters, redundant-word phase).  Emits words on the transmitter's
// Trw grid together with BER (non-unanimous votes) and SNR estimates.
//
// No Qt event loop, no locking: call from a single thread.

#include "Ale2gWord.h"

#include <QByteArray>
#include <QVector>

#include <array>
#include <cstdint>
#include <vector>

namespace AetherSDR::Ale2g {

class Modulator {
public:
    struct Options {
        float amplitude{0.5f};     // peak, linear full scale
        int leadSilenceMs{0};
        int tailSilenceMs{0};
        int rampSamples{24};       // 1 ms raised-cosine at frame edges
    };

    // Tone index (0 = 750 Hz .. 7 = 2500 Hz) for a 3-bit value (MSB first).
    static int toneIndexForBits(int bits3);
    static int bitsForToneIndex(int toneIndex);

    // Every entry is one redundant word (sent 3 times, A.5.2.2.4).
    static std::vector<std::uint8_t> symbolsForWords(const QVector<std::uint32_t>& words);
    static std::vector<float> modulate(const QVector<std::uint32_t>& words, const Options& options);
    static QByteArray toStereoFloat32(const float* mono, int count);
};

class Demodulator {
public:
    struct Word {
        std::uint32_t word{0};
        int golayErrors{0};       // corrected bits, both halves
        int nonUnanimous{0};      // 0..48
        float snrDb{0.0f};        // mean per-symbol tone SNR over the word
        qint64 endSample{0};      // absolute input sample index of word end
        bool repeat{false};       // identical word on the next Trw of a run
    };

    struct Event {
        enum class Type { Word, SyncLost };
        Type type{Type::Word};
        Word word;
    };

    Demodulator();

    void reset();
    // Mono float samples at 24 kHz.
    void process(const float* samples, int count, std::vector<Event>* events);
    void processStereoFloat32(const QByteArray& pcm, std::vector<Event>* events);

    qint64 samplesProcessed() const { return m_totalSamples; }
    bool isLocked() const { return m_locked; }

    // Tunables (A.5.2.6.3: manufacturer-chosen thresholds)
    static constexpr int kAcquireMaxNonUnanimous = 14;
    static constexpr int kAcquireMaxGolayErrorsPerHalf = 1;
    static constexpr int kTrackMaxNonUnanimous = 26;
    static constexpr int kMissesBeforeSyncLost = 3;   // one Trw (= Tlww)

private:
    static constexpr int kPhaseCount = 8;
    static constexpr int kPhaseStep = kSamplesPerSymbol / kPhaseCount;   // 24
    static constexpr int kMaxBits = 4096;

    struct Phase {
        qint64 nextSymbolStart{0};
        qint64 bitBase{0};                 // absolute index of bits[0]
        std::vector<std::uint8_t> bits;
        std::vector<float> symbolSnrDb;    // one per symbol (parallel to bits/3)
        float quality{0.0f};               // EMA of tone concentration
    };

    struct Candidate {
        bool valid{false};
        std::uint32_t word{0};
        int golayErrors{0};
        int nonUnanimous{0};
        float snrDb{0.0f};
        qint64 bitIndex{0};
        qint64 endSample{0};
    };

    Candidate evaluate(const Phase& phase, int phaseIndex, qint64 endBit, bool acquiring) const;
    void onNewBit(int phaseIndex, qint64 bitIndex, std::vector<Event>* events);
    void finalizePending(std::vector<Event>* events);
    void emitWord(const Candidate& c, bool repeat, std::vector<Event>* events);
    void loseSync(std::vector<Event>* events);
    int bestPhase() const;
    void trim();

    std::vector<float> m_buffer;
    qint64 m_bufferStart{0};
    qint64 m_totalSamples{0};
    std::array<Phase, kPhaseCount> m_phases;

    // Word sync state
    bool m_locked{false};
    int m_lockPhase{-1};
    qint64 m_gridBit{0};          // a bit index on the Tw (49-bit) grid
    int m_trwPhase{-1};           // grid-boundary index modulo 3 of the Trw grid
    int m_misses{0};
    bool m_hasPending{false};
    Candidate m_pendingBest;
    int m_pendingCount{0};
    bool m_hasLast{false};
    std::uint32_t m_lastWord{0};
    qint64 m_lastBoundary{0};
};

} // namespace AetherSDR::Ale2g
