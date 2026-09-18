#include "RttyDecoder.h"

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr quint8 kLettersShift = 31;
constexpr quint8 kFiguresShift = 27;

constexpr char kLettersTable[32] = {
    '\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U',
    '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
    'O', 'B', 'G', '\0', 'M', 'X', 'V', '\0'
};

constexpr char kFiguresTable[32] = {
    '\0', '3', '\n', '-', ' ', '\a', '8', '7',
    '\r', '$', '4', '\'', ',', '!', ':', '(',
    '5', '"', ')', '2', '#', '6', '0', '1',
    '9', '?', '&', '\0', '.', '/', '=', '\0'
};

float powerToDb(float power)
{
    const float rms = std::sqrt(std::max(power, 1e-12f));
    return 20.0f * std::log10(rms);
}

} // namespace

RttyDecoder::RttyDecoder(QObject* parent)
    : QObject(parent)
{
    recalcTiming();
    clearState();
}

void RttyDecoder::reset()
{
    clearState();
}

void RttyDecoder::setBaud(double baud)
{
    baud = std::clamp(baud, 10.0, 300.0);
    if (std::abs(m_baud - baud) < 0.0001)
        return;
    m_baud = baud;
    recalcTiming();
    clearState();
}

void RttyDecoder::setMarkHz(int hz)
{
    hz = std::clamp(hz, 200, 4000);
    if (m_markHz == hz)
        return;
    m_markHz = hz;
    clearState();
}

void RttyDecoder::setShiftHz(int hz)
{
    hz = std::clamp(hz, 10, 1000);
    if (m_shiftHz == hz)
        return;
    m_shiftHz = hz;
    clearState();
}

void RttyDecoder::setInverted(bool on)
{
    if (m_inverted == on)
        return;
    m_inverted = on;
    clearState();
}

void RttyDecoder::setUnshiftOnSpace(bool on)
{
    m_unshiftOnSpace = on;
}

void RttyDecoder::setSquelch(float threshold)
{
    m_squelch = std::clamp(threshold, 0.0f, 0.95f);
}

void RttyDecoder::feedAudio(const QByteArray& pcm24kStereo)
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
    processAvailableWindows();
}

void RttyDecoder::clearState()
{
    m_sampleBuffer.clear();
    m_bufferStartSample = 0;
    m_totalSamples = 0;
    m_nextWindowEnd = m_windowSamples;
    m_decisions.clear();
    m_decisionBaseIndex = 0;
    m_lettersMode = true;
    m_prevDecisionBit = true;
    m_receivingChar = false;
    m_locked = false;
    m_idleRun = 0;
    m_dataBitPos = 0;
    m_lockScore = 0;
    m_metricDecimate = 0;
    m_currentChar = 0;
    m_nextBitSample = 0.0;
    emit metricsUpdated(-120.0f, -120.0f, 0.0f, false);
    emit syncStateChanged(false);
}

void RttyDecoder::recalcTiming()
{
    m_windowSamples = std::max(64, static_cast<int>(std::lround(m_sampleRate / m_baud)));
    m_hopSamples = std::max(8, m_windowSamples / 4);
    m_oversample = static_cast<double>(m_windowSamples) / static_cast<double>(m_hopSamples);
}

float RttyDecoder::goertzelPower(const float* samples, int count, double freqHz) const
{
    freqHz = std::clamp(freqHz, 10.0, (m_sampleRate * 0.5) - 10.0);
    const double omega = kTwoPi * freqHz / m_sampleRate;
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
    return static_cast<float>(std::max(power, 0.0) / static_cast<double>(count * count));
}

void RttyDecoder::processAvailableWindows()
{
    while (m_totalSamples >= m_nextWindowEnd) {
        const qint64 windowStartAbs = m_nextWindowEnd - m_windowSamples;
        const qint64 localStart = windowStartAbs - m_bufferStartSample;
        if (localStart < 0 || localStart + m_windowSamples > static_cast<qint64>(m_sampleBuffer.size()))
            break;

        const auto* window = m_sampleBuffer.data() + static_cast<size_t>(localStart);
        const float markPower = goertzelPower(window, m_windowSamples, m_markHz);
        const float spacePower = goertzelPower(window, m_windowSamples, std::max(10, m_markHz - m_shiftHz));
        const float sum = markPower + spacePower + 1e-12f;
        const float confidence = std::abs(markPower - spacePower) / sum;

        bool bit = (markPower >= spacePower);
        if (m_inverted)
            bit = !bit;

        const int absoluteIndex = m_decisionBaseIndex + m_decisions.size();
        m_decisions.append({bit, confidence});
        handleDecision(absoluteIndex, bit, confidence);

        if ((++m_metricDecimate % 2) == 0)
            emit metricsUpdated(powerToDb(markPower), powerToDb(spacePower), confidence, m_locked);

        m_nextWindowEnd += m_hopSamples;
    }

    trimBuffers();
}

void RttyDecoder::updateLockState(bool goodDecision)
{
    if (goodDecision)
        m_lockScore = std::min(m_lockScore + 1, 24);
    else
        m_lockScore = std::max(m_lockScore - 1, 0);

    const bool locked = (m_lockScore >= 4);
    if (locked == m_locked)
        return;

    m_locked = locked;
    emit syncStateChanged(m_locked);
}

bool RttyDecoder::sampleDecision(double decisionIndex) const
{
    const int center = static_cast<int>(std::floor(decisionIndex));
    double score = 0.0;
    double weightSum = 0.0;

    for (int i = center - 1; i <= center + 1; ++i) {
        const int offset = i - m_decisionBaseIndex;
        if (offset < 0 || offset >= m_decisions.size())
            continue;

        const double distance = std::abs(decisionIndex - static_cast<double>(i));
        const double proximity = std::max(0.0, 1.25 - distance);
        if (proximity <= 0.0)
            continue;

        const Decision& d = m_decisions.at(offset);
        const double weight = proximity * std::max(0.05f, d.confidence);
        score += d.bit ? weight : -weight;
        weightSum += weight;
    }

    if (weightSum <= 0.0)
        return true;
    return score >= 0.0;
}

void RttyDecoder::handleDecision(int absoluteIndex, bool bit, float confidence)
{
    const bool goodDecision = confidence >= m_squelch;
    updateLockState(goodDecision);

    if (!m_receivingChar) {
        if (bit) {
            if (goodDecision)
                ++m_idleRun;
            else
                m_idleRun = std::max(m_idleRun - 1, 0);
        } else {
            const bool fallingEdge = m_prevDecisionBit && !bit;
            if (goodDecision && fallingEdge && m_idleRun >= std::max(2, static_cast<int>(std::lround(m_oversample * 0.75)))) {
                m_receivingChar = true;
                m_dataBitPos = 0;
                m_currentChar = 0;
                m_nextBitSample = static_cast<double>(absoluteIndex) + (m_oversample * 1.5);
            }
            m_idleRun = 0;
        }
    } else {
        while (static_cast<double>(absoluteIndex) + 0.0001 >= m_nextBitSample) {
            const bool sampledBit = sampleDecision(m_nextBitSample);
            if (m_dataBitPos < 5) {
                if (sampledBit)
                    m_currentChar |= static_cast<quint8>(1u << m_dataBitPos);
                ++m_dataBitPos;
                m_nextBitSample += m_oversample;
                continue;
            }

            if (sampledBit)
                decodeCharacter(m_currentChar);

            m_receivingChar = false;
            m_dataBitPos = 0;
            m_currentChar = 0;
            m_idleRun = sampledBit ? 1 : 0;
            break;
        }
    }

    m_prevDecisionBit = bit;
}

void RttyDecoder::decodeCharacter(quint8 code)
{
    if (code == kLettersShift) {
        m_lettersMode = true;
        return;
    }

    if (code == kFiguresShift) {
        m_lettersMode = false;
        return;
    }

    const char ch = m_lettersMode ? kLettersTable[code & 0x1F] : kFiguresTable[code & 0x1F];
    if (ch == '\0' || ch == '\r' || ch == '\a')
        return;

    if (ch == ' ' && m_unshiftOnSpace)
        m_lettersMode = true;

    if (ch == '\n')
        emit textDecoded(QStringLiteral("\n"));
    else
        emit textDecoded(QString(QChar::fromLatin1(ch)));
}

void RttyDecoder::trimBuffers()
{
    const qint64 keepFromSample = std::max<qint64>(0, m_nextWindowEnd - (m_windowSamples * 2));
    if (keepFromSample > m_bufferStartSample) {
        const qint64 drop = keepFromSample - m_bufferStartSample;
        if (drop > 0 && drop <= static_cast<qint64>(m_sampleBuffer.size())) {
            m_sampleBuffer.erase(m_sampleBuffer.begin(),
                                 m_sampleBuffer.begin() + static_cast<std::ptrdiff_t>(drop));
            m_bufferStartSample = keepFromSample;
        }
    }

    int minDecisionNeeded = std::max(0, static_cast<int>(std::floor(m_nextBitSample)) - 4);
    if (!m_receivingChar)
        minDecisionNeeded = std::max(0, m_decisionBaseIndex + static_cast<int>(m_decisions.size()) - 64);

    const int removable = std::max(0, minDecisionNeeded - m_decisionBaseIndex);
    if (removable > 0 && removable <= m_decisions.size()) {
        m_decisions.remove(0, removable);
        m_decisionBaseIndex += removable;
    }
}

} // namespace AetherSDR
