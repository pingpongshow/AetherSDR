#include "Ale2gDecoder.h"
#include "Ale2gWord.h"

#include <QByteArray>
#include <QDateTime>

#include <algorithm>
#include <array>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr std::array<double, 8> kToneFrequencies{{750.0, 1000.0, 1250.0, 1500.0,
                                                   1750.0, 2000.0, 2250.0, 2500.0}};
constexpr std::array<std::array<unsigned char, 3>, 8> kToneBits{{
    {{0, 0, 0}}, {{0, 0, 1}}, {{0, 1, 1}}, {{0, 1, 0}},
    {{1, 1, 0}}, {{1, 1, 1}}, {{1, 0, 1}}, {{1, 0, 0}},
}};
int mod49(qint64 value)
{
    const int mod = static_cast<int>(value % 49);
    return mod < 0 ? mod + 49 : mod;
}

int popcount32(unsigned int value)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(value);
#else
    int count = 0;
    while (value) {
        value &= (value - 1);
        ++count;
    }
    return count;
#endif
}

} // namespace

Ale2gDecoder::Ale2gDecoder(QObject* parent)
    : QObject(parent)
{
    buildGolayTable();
    clearState();
}

void Ale2gDecoder::reset()
{
    clearState();
    emit statusChanged(QStringLiteral("2G ALE decoder reset."));
}

void Ale2gDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    const int floatCount = pcm24kStereo.size() / static_cast<int>(sizeof(float));
    if (floatCount < 2)
        return;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int frames = floatCount / 2;
    m_sampleBuffer.reserve(m_sampleBuffer.size() + static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const float mono = (src[i * 2] + src[i * 2 + 1]) * 0.5f;
        m_sampleBuffer.push_back(mono);
    }

    m_totalSamples += frames;
    for (int i = 0; i < kPhaseCount; ++i)
        processPhase(i);
    trimBuffers();
}

QString Ale2gDecoder::preambleName(int code)
{
    switch (code & 0x7) {
    case 0:
        return QStringLiteral("DATA");
    case 1:
        return QStringLiteral("THRU");
    case 2:
        return QStringLiteral("TO");
    case 3:
        return QStringLiteral("TWAS");
    case 4:
        return QStringLiteral("FROM");
    case 5:
        return QStringLiteral("TIS");
    case 6:
        return QStringLiteral("CMD");
    case 7:
    default:
        return QStringLiteral("REP");
    }
}

float Ale2gDecoder::powerToDb(float power)
{
    const float rms = std::sqrt(std::max(power, 1e-12f));
    return 20.0f * std::log10(rms);
}

bool Ale2gDecoder::isReasonableAleChar(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z')
           || (ch >= '0' && ch <= '9')
           || ch == ' '
           || ch == '?'
           || ch == '@'
           || ch == '-'
           || ch == '/'
           || ch == '.';
}

QString Ale2gDecoder::sanitizedText(unsigned char c1, unsigned char c2, unsigned char c3)
{
    QString out;
    out.reserve(3);
    for (unsigned char ch : {c1, c2, c3}) {
        if (ch >= 0x20 && ch <= 0x7e)
            out += QChar::fromLatin1(static_cast<char>(ch));
        else
            out += QLatin1Char('.');
    }
    return out;
}

int Ale2gDecoder::bitsToIntMsbFirst(const unsigned char* bits, int count)
{
    int value = 0;
    for (int i = 0; i < count; ++i)
        value = (value << 1) | (bits[i] ? 1 : 0);
    return value;
}

float Ale2gDecoder::goertzelPower(const float* samples, int count, double freqHz) const
{
    if (count <= 0)
        return 0.0f;

    const double omega = kTwoPi * freqHz / static_cast<double>(kSampleRate);
    const double coeff = 2.0 * std::cos(omega);
    double q0 = 0.0;
    double q1 = 0.0;
    double q2 = 0.0;

    for (int i = 0; i < count; ++i) {
        q0 = coeff * q1 - q2 + samples[i];
        q2 = q1;
        q1 = q0;
    }

    const double power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
    return static_cast<float>(std::max(0.0, power) / static_cast<double>(count * count));
}

void Ale2gDecoder::buildGolayTable()
{
    // Parity bits from the MIL-STD-188-141B figure A-6 generator matrix
    // (shared with the transmit path in Ale2gWord).  The previous local
    // parity rows did not match the standard (or LinuxALE/PCALE), so real
    // on-air words were rejected by the distance check below.
    for (int data = 0; data < 4096; ++data) {
        m_golayCodewords[static_cast<size_t>(data)] =
            static_cast<int>(Ale2g::golayEncode(static_cast<std::uint16_t>(data)));
    }
}

void Ale2gDecoder::clearState()
{
    m_sampleBuffer.clear();
    m_bufferStartSample = 0;
    m_totalSamples = 0;
    m_activePhase = -1;
    m_totalWords = 0;
    m_lastFrameWallMs = 0;
    m_currentFrameWords.clear();

    for (int i = 0; i < kPhaseCount; ++i) {
        auto& phase = m_phases[static_cast<size_t>(i)];
        phase.offsetSamples = i * kPhaseStepSamples;
        phase.nextSymbolStart = phase.offsetSamples;
        phase.bitBaseIndex = 0;
        phase.bits.clear();
        phase.bitConfidence.clear();
        phase.boundaryScores.fill(0.0f);
        phase.bestBoundary = 0;
        phase.bestBoundaryScore = 0.0f;
        phase.validHits = 0;
        phase.lastAcceptedEndBit = -100000;
    }
}

bool Ale2gDecoder::decodeGolayWord(int received, int* decodedData, int* distance) const
{
    int bestData = -1;
    int bestDistance = 25;
    for (int data = 0; data < 4096; ++data) {
        const unsigned int diff = static_cast<unsigned int>(received ^ m_golayCodewords[static_cast<size_t>(data)]);
        const int d = popcount32(diff);
        if (d < bestDistance) {
            bestDistance = d;
            bestData = data;
            if (d == 0)
                break;
        }
    }

    if (bestDistance > 3 || bestData < 0)
        return false;

    if (decodedData)
        *decodedData = bestData;
    if (distance)
        *distance = bestDistance;
    return true;
}

bool Ale2gDecoder::decodeCandidateWord(const PhaseState& phase, qint64 endBit, WordCandidate* candidate) const
{
    if (!candidate)
        return false;

    if (endBit - 98 < phase.bitBaseIndex)
        return false;

    unsigned char wordBits[49]{};
    float confidenceSum = 0.0f;

    auto loadBit = [&phase](qint64 absoluteBit, unsigned char* bit, float* confidence) -> bool {
        const qint64 offset = absoluteBit - phase.bitBaseIndex;
        if (offset < 0 || offset >= static_cast<qint64>(phase.bits.size()))
            return false;
        *bit = phase.bits[static_cast<size_t>(offset)];
        *confidence = phase.bitConfidence[static_cast<size_t>(offset)];
        return true;
    };

    for (int i = 0; i < 49; ++i) {
        const qint64 absolute = endBit - 48 + i;
        unsigned char b0 = 0;
        unsigned char b1 = 0;
        unsigned char b2 = 0;
        float c0 = 0.0f;
        float c1 = 0.0f;
        float c2 = 0.0f;
        if (!loadBit(absolute, &b0, &c0)
            || !loadBit(absolute - 49, &b1, &c1)
            || !loadBit(absolute - 98, &b2, &c2)) {
            return false;
        }

        wordBits[i] = (b0 + b1 + b2) >= 2 ? 1 : 0;
        confidenceSum += (c0 + c1 + c2) / 3.0f;
    }

    unsigned char aBits[24]{};
    unsigned char bBits[24]{};
    for (int i = 0; i < 24; ++i) {
        aBits[i] = wordBits[i * 2];
        bBits[i] = wordBits[(i * 2) + 1];
    }

    const int aReceived = (bitsToIntMsbFirst(aBits, 12) << 12) | bitsToIntMsbFirst(aBits + 12, 12);
    unsigned char bNormal[24]{};
    for (int i = 0; i < 12; ++i)
        bNormal[i] = bBits[i];
    for (int i = 0; i < 12; ++i)
        bNormal[12 + i] = bBits[12 + i] ? 0 : 1;
    const int bReceived = (bitsToIntMsbFirst(bNormal, 12) << 12) | bitsToIntMsbFirst(bNormal + 12, 12);

    int aData = 0;
    int bData = 0;
    int aDistance = 0;
    int bDistance = 0;
    if (!decodeGolayWord(aReceived, &aData, &aDistance)
        || !decodeGolayWord(bReceived, &bData, &bDistance)) {
        return false;
    }

    if ((aDistance + bDistance) > 4)
        return false;

    unsigned char wordData[24]{};
    for (int i = 0; i < 12; ++i)
        wordData[i] = (aData >> (11 - i)) & 1;
    for (int i = 0; i < 12; ++i)
        wordData[12 + i] = (bData >> (11 - i)) & 1;

    const int preamble = bitsToIntMsbFirst(wordData, 3);
    const unsigned char c1 = static_cast<unsigned char>(bitsToIntMsbFirst(wordData + 3, 7));
    const unsigned char c2 = static_cast<unsigned char>(bitsToIntMsbFirst(wordData + 10, 7));
    const unsigned char c3 = static_cast<unsigned char>(bitsToIntMsbFirst(wordData + 17, 7));

    const float confidence = confidenceSum / 49.0f;
    if (confidence < 0.08f)
        return false;

    const QString preambleText = preambleName(preamble);
    const QString text = sanitizedText(c1, c2, c3);

    const int basicCount = (isReasonableAleChar(c1) ? 1 : 0)
                           + (isReasonableAleChar(c2) ? 1 : 0)
                           + (isReasonableAleChar(c3) ? 1 : 0);

    if (preamble == 0 || preamble == 7) {
        // DATA / REP also carry AMD text from the expanded-64 set (A.5.7.2).
        if (!Ale2g::isExpanded64(c1) || !Ale2g::isExpanded64(c2) || !Ale2g::isExpanded64(c3))
            return false;
    } else if (preamble != 6) {
        if (basicCount < 2)
            return false;
        if (!isReasonableAleChar(c1) || !isReasonableAleChar(c2) || !isReasonableAleChar(c3))
            return false;
    } else if (basicCount == 0 && (aDistance + bDistance) > 1) {
        return false;
    }

    candidate->valid = true;
    candidate->preamble = preambleText;
    candidate->text = (preamble == 6 && basicCount < 2)
                          ? QStringLiteral("0x%1%2%3")
                                .arg(c1, 2, 16, QLatin1Char('0'))
                                .arg(c2, 2, 16, QLatin1Char('0'))
                                .arg(c3, 2, 16, QLatin1Char('0'))
                          : text;
    candidate->errors = aDistance + bDistance;
    candidate->confidence = confidence;
    candidate->summary = QStringLiteral("%1 %2").arg(candidate->preamble, candidate->text.trimmed());
    return true;
}

void Ale2gDecoder::emitAcceptedWord(const PhaseState& phase, qint64 endBit, const WordCandidate& candidate)
{
    Q_UNUSED(phase);

    const QString utcText = QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss"));
    emit wordDecoded(utcText,
                     candidate.preamble,
                     candidate.text,
                     candidate.errors,
                     candidate.confidence,
                     candidate.summary);

    ++m_totalWords;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastFrameWallMs > 0 && (nowMs - m_lastFrameWallMs) > 700)
        finalizeFrame();

    m_currentFrameWords.append(candidate.summary);
    m_lastFrameWallMs = nowMs;

    Q_UNUSED(endBit);
}

void Ale2gDecoder::finalizeFrame()
{
    if (m_currentFrameWords.isEmpty())
        return;

    const QString summary = m_currentFrameWords.join(QStringLiteral(" | "));
    emit frameDecoded(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")),
                      summary);
    m_currentFrameWords.clear();
}

void Ale2gDecoder::recomputeBestBoundary(PhaseState* phase)
{
    if (!phase)
        return;

    int bestBoundary = 0;
    float bestScore = phase->boundaryScores[0];
    for (int i = 1; i < 49; ++i) {
        if (phase->boundaryScores[static_cast<size_t>(i)] > bestScore) {
            bestScore = phase->boundaryScores[static_cast<size_t>(i)];
            bestBoundary = i;
        }
    }
    phase->bestBoundary = bestBoundary;
    phase->bestBoundaryScore = bestScore;
}

void Ale2gDecoder::refreshActivePhase()
{
    int bestPhase = -1;
    float bestScore = 0.0f;

    for (int i = 0; i < kPhaseCount; ++i) {
        const auto& phase = m_phases[static_cast<size_t>(i)];
        if (phase.validHits < 2)
            continue;
        if (phase.bestBoundaryScore > bestScore) {
            bestScore = phase.bestBoundaryScore;
            bestPhase = i;
        }
    }

    m_activePhase = (bestScore >= 2.0f) ? bestPhase : -1;

    const int phaseSamples = m_activePhase >= 0 ? m_phases[static_cast<size_t>(m_activePhase)].offsetSamples : -1;
    const int boundary = m_activePhase >= 0 ? m_phases[static_cast<size_t>(m_activePhase)].bestBoundary : -1;
    emit metricsUpdated(m_activePhase >= 0,
                        phaseSamples,
                        boundary,
                        bestScore,
                        m_totalWords);
}

void Ale2gDecoder::processPhase(int phaseIndex)
{
    auto& phase = m_phases[static_cast<size_t>(phaseIndex)];
    while (phase.nextSymbolStart + kSymbolSamples <= m_totalSamples) {
        const qint64 localStart = phase.nextSymbolStart - m_bufferStartSample;
        if (localStart < 0 || localStart + kSymbolSamples > static_cast<qint64>(m_sampleBuffer.size()))
            break;

        const auto* window = m_sampleBuffer.data() + static_cast<size_t>(localStart);
        std::array<float, 8> powers{};
        int bestTone = 0;
        float bestPower = -1.0f;
        float secondPower = -1.0f;

        for (int i = 0; i < 8; ++i) {
            const float power = goertzelPower(window, kSymbolSamples, kToneFrequencies[static_cast<size_t>(i)]);
            powers[static_cast<size_t>(i)] = power;
            if (power > bestPower) {
                secondPower = bestPower;
                bestPower = power;
                bestTone = i;
            } else if (power > secondPower) {
                secondPower = power;
            }
        }

        const float confidence = std::max(0.0f, (bestPower - secondPower) / (bestPower + secondPower + 1e-12f));
        for (float& score : phase.boundaryScores)
            score *= 0.9992f;

        for (int bit = 0; bit < 3; ++bit) {
            phase.bits.push_back(kToneBits[static_cast<size_t>(bestTone)][static_cast<size_t>(bit)]);
            phase.bitConfidence.push_back(confidence);

            const qint64 absoluteBit = phase.bitBaseIndex + static_cast<qint64>(phase.bits.size()) - 1;
            WordCandidate candidate;
            if (decodeCandidateWord(phase, absoluteBit, &candidate)) {
                const int boundary = mod49(absoluteBit);
                const float scoreAdd = std::max(0.15f, candidate.confidence * 2.0f) + (candidate.errors == 0 ? 0.6f : 0.0f);
                phase.boundaryScores[static_cast<size_t>(boundary)] =
                    std::min(phase.boundaryScores[static_cast<size_t>(boundary)] + scoreAdd, 50.0f);
                ++phase.validHits;
                recomputeBestBoundary(&phase);
                refreshActivePhase();

                if (m_activePhase == phaseIndex
                    && boundary == phase.bestBoundary
                    && (absoluteBit - phase.lastAcceptedEndBit) >= 49) {
                    phase.lastAcceptedEndBit = absoluteBit;
                    emitAcceptedWord(phase, absoluteBit, candidate);
                }
            }
        }

        phase.nextSymbolStart += kSymbolSamples;
        trimPhaseBits(&phase);
    }
}

void Ale2gDecoder::trimPhaseBits(PhaseState* phase)
{
    if (!phase || phase->bits.size() < 4096)
        return;

    const size_t keep = 2048;
    const size_t drop = phase->bits.size() - keep;
    phase->bits.erase(phase->bits.begin(), phase->bits.begin() + static_cast<std::ptrdiff_t>(drop));
    phase->bitConfidence.erase(phase->bitConfidence.begin(),
                               phase->bitConfidence.begin() + static_cast<std::ptrdiff_t>(drop));
    phase->bitBaseIndex += static_cast<qint64>(drop);
}

void Ale2gDecoder::trimBuffers()
{
    qint64 keepFrom = m_totalSamples;
    for (const auto& phase : m_phases)
        keepFrom = std::min(keepFrom, phase.nextSymbolStart);
    keepFrom = std::max<qint64>(0, keepFrom - (kSymbolSamples * 4));

    if (keepFrom <= m_bufferStartSample)
        return;

    const qint64 drop = keepFrom - m_bufferStartSample;
    if (drop <= 0 || drop > static_cast<qint64>(m_sampleBuffer.size()))
        return;

    m_sampleBuffer.erase(m_sampleBuffer.begin(),
                         m_sampleBuffer.begin() + static_cast<std::ptrdiff_t>(drop));
    m_bufferStartSample = keepFrom;
}

} // namespace AetherSDR
