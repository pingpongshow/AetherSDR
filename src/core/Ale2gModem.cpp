#include "Ale2gModem.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace AetherSDR::Ale2g {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

struct ToneTables {
    // cos() for each tone over one symbol.  Every tone has an integer number
    // of cycles per 8 ms symbol (6..20), so consecutive symbols are phase
    // continuous and each transition sits on a waveform maximum (A.5.1.2).
    std::array<std::array<float, kSamplesPerSymbol>, 8> wave{};
    // Goertzel coefficients for the demodulator.
    std::array<double, 8> coeff{};

    ToneTables()
    {
        for (int t = 0; t < 8; ++t) {
            const double f = kToneBaseHz + kToneStepHz * t;
            const double w = kTwoPi * f / kSampleRate;
            coeff[static_cast<size_t>(t)] = 2.0 * std::cos(w);
            for (int n = 0; n < kSamplesPerSymbol; ++n) {
                wave[static_cast<size_t>(t)][static_cast<size_t>(n)] =
                    static_cast<float>(std::cos(w * n));
            }
        }
    }
};

const ToneTables& toneTables()
{
    static const ToneTables t;
    return t;
}

} // namespace

// ─── Modulator ──────────────────────────────────────────────────────────────

int Modulator::toneIndexForBits(int bits3)
{
    // A.5.1.2 mapping is the 3-bit Gray code of the tone index.
    const int g = bits3 & 0x7;
    return g ^ (g >> 1) ^ (g >> 2);
}

int Modulator::bitsForToneIndex(int toneIndex)
{
    const int i = toneIndex & 0x7;
    return i ^ (i >> 1);
}

std::vector<std::uint8_t> Modulator::symbolsForWords(const QVector<std::uint32_t>& words)
{
    std::vector<std::uint8_t> symbols;
    symbols.reserve(static_cast<size_t>(words.size()) * kSymbolsPerRedundantWord);
    for (std::uint32_t word : words) {
        const auto one = encodeOnAirWord(word);
        std::array<std::uint8_t, kBitsPerRedundantWord> bits{};
        for (int copy = 0; copy < 3; ++copy) {
            std::copy(one.begin(), one.end(), bits.begin() + copy * kBitsPerWord);
        }
        for (int s = 0; s < kSymbolsPerRedundantWord; ++s) {
            const int v = (bits[static_cast<size_t>(s * 3)] << 2)
                          | (bits[static_cast<size_t>(s * 3 + 1)] << 1)
                          | bits[static_cast<size_t>(s * 3 + 2)];
            symbols.push_back(static_cast<std::uint8_t>(toneIndexForBits(v)));
        }
    }
    return symbols;
}

std::vector<float> Modulator::modulate(const QVector<std::uint32_t>& words, const Options& options)
{
    const auto& tt = toneTables();
    const std::vector<std::uint8_t> symbols = symbolsForWords(words);
    const int lead = std::max(0, options.leadSilenceMs) * kSampleRate / 1000;
    const int tail = std::max(0, options.tailSilenceMs) * kSampleRate / 1000;
    const int body = static_cast<int>(symbols.size()) * kSamplesPerSymbol;

    std::vector<float> out(static_cast<size_t>(lead + body + tail), 0.0f);
    const float amp = std::clamp(options.amplitude, 0.0f, 1.0f);
    for (size_t s = 0; s < symbols.size(); ++s) {
        const auto& wave = tt.wave[symbols[s]];
        const size_t base = static_cast<size_t>(lead) + s * kSamplesPerSymbol;
        for (int n = 0; n < kSamplesPerSymbol; ++n) {
            out[base + static_cast<size_t>(n)] = amp * wave[static_cast<size_t>(n)];
        }
    }

    const int ramp = std::min(std::max(0, options.rampSamples), body / 2);
    for (int n = 0; n < ramp; ++n) {
        const float g = 0.5f - 0.5f * static_cast<float>(std::cos(kTwoPi * 0.5 * n / ramp));
        out[static_cast<size_t>(lead + n)] *= g;
        out[static_cast<size_t>(lead + body - 1 - n)] *= g;
    }
    return out;
}

QByteArray Modulator::toStereoFloat32(const float* mono, int count)
{
    QByteArray out(count * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(out.data());
    for (int i = 0; i < count; ++i) {
        dst[2 * i] = mono[i];
        dst[2 * i + 1] = mono[i];
    }
    return out;
}

// ─── Demodulator ────────────────────────────────────────────────────────────

Demodulator::Demodulator()
{
    reset();
}

void Demodulator::reset()
{
    m_buffer.clear();
    m_bufferStart = 0;
    m_totalSamples = 0;
    for (int p = 0; p < kPhaseCount; ++p) {
        Phase& ph = m_phases[static_cast<size_t>(p)];
        ph.nextSymbolStart = p * kPhaseStep;
        ph.bitBase = 0;
        ph.bits.clear();
        ph.symbolSnrDb.clear();
        ph.quality = 0.0f;
    }
    m_locked = false;
    m_lockPhase = -1;
    m_gridBit = 0;
    m_trwPhase = -1;
    m_misses = 0;
    m_hasPending = false;
    m_pendingCount = 0;
    m_hasLast = false;
}

void Demodulator::processStereoFloat32(const QByteArray& pcm, std::vector<Event>* events)
{
    const int frames = pcm.size() / static_cast<int>(2 * sizeof(float));
    if (frames <= 0) {
        return;
    }
    const auto* src = reinterpret_cast<const float*>(pcm.constData());
    std::vector<float> mono(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        mono[static_cast<size_t>(i)] = 0.5f * (src[2 * i] + src[2 * i + 1]);
    }
    process(mono.data(), frames, events);
}

int Demodulator::bestPhase() const
{
    int best = 0;
    for (int p = 1; p < kPhaseCount; ++p) {
        if (m_phases[static_cast<size_t>(p)].quality > m_phases[static_cast<size_t>(best)].quality) {
            best = p;
        }
    }
    return best;
}

void Demodulator::process(const float* samples, int count, std::vector<Event>* events)
{
    if (count <= 0) {
        return;
    }
    m_buffer.insert(m_buffer.end(), samples, samples + count);
    m_totalSamples += count;

    const auto& tt = toneTables();
    for (;;) {
        // Process symbols in chronological order across timing phases.
        int p = -1;
        for (int i = 0; i < kPhaseCount; ++i) {
            const Phase& ph = m_phases[static_cast<size_t>(i)];
            if (ph.nextSymbolStart + kSamplesPerSymbol > m_totalSamples) {
                continue;
            }
            if (p < 0 || ph.nextSymbolStart < m_phases[static_cast<size_t>(p)].nextSymbolStart) {
                p = i;
            }
        }
        if (p < 0) {
            break;
        }

        Phase& ph = m_phases[static_cast<size_t>(p)];
        const qint64 local = ph.nextSymbolStart - m_bufferStart;
        const float* w = m_buffer.data() + local;

        std::array<double, 8> power{};
        int bestTone = 0;
        double sum = 0.0;
        for (int t = 0; t < 8; ++t) {
            const double c = tt.coeff[static_cast<size_t>(t)];
            double q1 = 0.0;
            double q2 = 0.0;
            for (int n = 0; n < kSamplesPerSymbol; ++n) {
                const double q0 = c * q1 - q2 + w[n];
                q2 = q1;
                q1 = q0;
            }
            const double pw = std::max(0.0, q1 * q1 + q2 * q2 - c * q1 * q2);
            power[static_cast<size_t>(t)] = pw;
            sum += pw;
            if (pw > power[static_cast<size_t>(bestTone)]) {
                bestTone = t;
            }
        }

        const double best = power[static_cast<size_t>(bestTone)];
        float snrDb = -99.0f;
        float concentration = 0.0f;
        if (best > 1e-9) {
            const double noise = std::max((sum - best) / 7.0, best * 1e-6);
            snrDb = static_cast<float>(10.0 * std::log10(best / noise));
            concentration = static_cast<float>(best / sum);
        }
        ph.quality = 0.92f * ph.quality + 0.08f * concentration;
        ph.symbolSnrDb.push_back(snrDb);

        const int bits = Modulator::bitsForToneIndex(bestTone);
        const qint64 firstBit = ph.bitBase + static_cast<qint64>(ph.bits.size());
        ph.bits.push_back(static_cast<std::uint8_t>((bits >> 2) & 1));
        ph.bits.push_back(static_cast<std::uint8_t>((bits >> 1) & 1));
        ph.bits.push_back(static_cast<std::uint8_t>(bits & 1));
        ph.nextSymbolStart += kSamplesPerSymbol;

        for (int b = 0; b < 3; ++b) {
            onNewBit(p, firstBit + b, events);
        }
        trim();
    }
}

Demodulator::Candidate Demodulator::evaluate(const Phase& phase, int phaseIndex,
                                             qint64 endBit, bool acquiring) const
{
    Candidate c;
    const qint64 start = endBit - (kBitsPerRedundantWord - 1) - phase.bitBase;
    if (start < 0 || endBit - phase.bitBase >= static_cast<qint64>(phase.bits.size())) {
        return c;
    }
    const MajorityDecode d = decodeRedundantWord(phase.bits.data() + start);
    if (!d.ok) {
        return c;
    }

    // Mean per-symbol SNR over the 49 symbols spanned by this redundant word.
    const qint64 lastSym = endBit / kBitsPerSymbol;
    const qint64 firstSym = lastSym - (kSymbolsPerRedundantWord - 1);
    const qint64 symBase = phase.bitBase / kBitsPerSymbol;
    double snrSum = 0.0;
    int snrCount = 0;
    for (qint64 s = std::max(firstSym, symBase); s <= lastSym; ++s) {
        const qint64 idx = s - symBase;
        if (idx >= 0 && idx < static_cast<qint64>(phase.symbolSnrDb.size())) {
            snrSum += phase.symbolSnrDb[static_cast<size_t>(idx)];
            ++snrCount;
        }
    }
    const float snr = snrCount > 0 ? static_cast<float>(snrSum / snrCount) : -99.0f;

    const Preamble pre = wordPreamble(d.word);
    const bool routing = pre == Preamble::To || pre == Preamble::Tis || pre == Preamble::Twas
                         || pre == Preamble::Thru || pre == Preamble::From;
    if (acquiring) {
        if (d.errorsA > kAcquireMaxGolayErrorsPerHalf || d.errorsB > kAcquireMaxGolayErrorsPerHalf
            || d.nonUnanimous > kAcquireMaxNonUnanimous || !allBasic38(d.word) || snr < 1.0f) {
            return c;
        }
    } else {
        if (d.nonUnanimous > kTrackMaxNonUnanimous) {
            return c;
        }
        if (routing && !allBasic38(d.word)) {
            return c;
        }
    }

    c.valid = true;
    c.word = d.word;
    c.golayErrors = d.errorsA + d.errorsB;
    c.nonUnanimous = d.nonUnanimous;
    c.snrDb = snr;
    c.bitIndex = endBit;
    c.endSample = phaseIndex * kPhaseStep + (lastSym + 1) * kSamplesPerSymbol;
    return c;
}

void Demodulator::onNewBit(int phaseIndex, qint64 bitIndex, std::vector<Event>* events)
{
    const Phase& ph = m_phases[static_cast<size_t>(phaseIndex)];

    if (!m_locked) {
        if (phaseIndex != bestPhase()) {
            return;
        }
        const Candidate c = evaluate(ph, phaseIndex, bitIndex, true);
        if (!c.valid) {
            return;
        }
        m_locked = true;
        m_lockPhase = phaseIndex;
        m_gridBit = bitIndex;
        m_trwPhase = -1;
        m_misses = 0;
        m_hasLast = false;
        m_hasPending = true;
        m_pendingBest = c;
        m_pendingCount = 1;
        return;
    }

    if (phaseIndex != m_lockPhase) {
        return;
    }
    const qint64 delta = bitIndex - m_gridBit;
    if (delta <= 0 || (delta % kBitsPerWord) != 0) {
        return;
    }
    const qint64 k = delta / kBitsPerWord;
    const Candidate c = evaluate(ph, phaseIndex, bitIndex, false);

    if (!c.valid) {
        if (m_hasPending) {
            finalizePending(events);
        }
        if (++m_misses >= kMissesBeforeSyncLost) {
            loseSync(events);
        }
        return;
    }
    m_misses = 0;

    if (m_hasPending) {
        if (c.word == m_pendingBest.word) {
            if (c.nonUnanimous < m_pendingBest.nonUnanimous) {
                m_pendingBest = c;
            }
            if (++m_pendingCount >= 3) {
                finalizePending(events);
            }
            return;
        }
        finalizePending(events);
    }

    if (m_hasLast && c.word == m_lastWord) {
        if (m_trwPhase >= 0 && (k % 3) == m_trwPhase) {
            emitWord(c, true, events);
            m_lastBoundary = k;
        }
        return;
    }

    m_hasPending = true;
    m_pendingBest = c;
    m_pendingCount = 1;
}

void Demodulator::finalizePending(std::vector<Event>* events)
{
    if (!m_hasPending) {
        return;
    }
    m_hasPending = false;
    const Candidate best = m_pendingBest;
    const qint64 k = (best.bitIndex - m_gridBit) / kBitsPerWord;
    m_trwPhase = static_cast<int>(((k % 3) + 3) % 3);
    m_hasLast = true;
    m_lastWord = best.word;
    m_lastBoundary = k;
    emitWord(best, false, events);
}

void Demodulator::emitWord(const Candidate& c, bool repeat, std::vector<Event>* events)
{
    if (!events) {
        return;
    }
    Event e;
    e.type = Event::Type::Word;
    e.word.word = c.word;
    e.word.golayErrors = c.golayErrors;
    e.word.nonUnanimous = c.nonUnanimous;
    e.word.snrDb = c.snrDb;
    e.word.endSample = c.endSample;
    e.word.repeat = repeat;
    events->push_back(e);
}

void Demodulator::loseSync(std::vector<Event>* events)
{
    if (m_hasPending) {
        finalizePending(events);
    }
    m_locked = false;
    m_lockPhase = -1;
    m_trwPhase = -1;
    m_misses = 0;
    m_hasLast = false;
    if (events) {
        Event e;
        e.type = Event::Type::SyncLost;
        events->push_back(e);
    }
}

void Demodulator::trim()
{
    qint64 keepFrom = m_totalSamples;
    for (auto& ph : m_phases) {
        keepFrom = std::min(keepFrom, ph.nextSymbolStart);
        if (static_cast<int>(ph.bits.size()) > kMaxBits) {
            // Drop a whole number of symbols so bits and SNR stay aligned.
            const int dropSymbols = (static_cast<int>(ph.bits.size()) - kMaxBits / 2) / kBitsPerSymbol;
            const int dropBits = dropSymbols * kBitsPerSymbol;
            ph.bits.erase(ph.bits.begin(), ph.bits.begin() + dropBits);
            ph.symbolSnrDb.erase(ph.symbolSnrDb.begin(), ph.symbolSnrDb.begin() + dropSymbols);
            ph.bitBase += dropBits;
        }
    }
    const qint64 drop = keepFrom - m_bufferStart;
    if (drop > 4 * kSamplesPerSymbol && drop <= static_cast<qint64>(m_buffer.size())) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + drop);
        m_bufferStart = keepFrom;
    }
}

} // namespace AetherSDR::Ale2g
