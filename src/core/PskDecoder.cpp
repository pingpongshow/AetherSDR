#include "PskDecoder.h"

#include "PskVaricode.h"

#include <QByteArray>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace AetherSDR {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr double kTwoPi = 6.28318530717958647692;

float dbFromMag(float mag)
{
    return 20.0f * std::log10(std::max(mag, 1e-9f));
}

float wrapPi(float angle)
{
    while (angle <= -kPi)
        angle += static_cast<float>(kTwoPi);
    while (angle > kPi)
        angle -= static_cast<float>(kTwoPi);
    return angle;
}

} // namespace

PskDecoder::PskDecoder(QObject* parent)
    : QObject(parent)
{
    reset();
}

void PskDecoder::reset()
{
    m_sampleBuffer.clear();
    m_bufferStartSample = 0;
    m_totalSamples = 0;
    m_nextWindowEnd = m_windowSamples;
    m_windowIndex = 0;
    m_bestPhase = 0;
    m_phaseScores = {0.0f, 0.0f, 0.0f, 0.0f};
    m_havePrevSymbol = false;
    m_prevSymbol = {1.0f, 0.0f};
    m_pendingZero = false;
    m_currentCode.clear();
    m_levelAverage = 0.0f;
    m_confidenceAverage = 0.0f;
    m_lockScore = 0;
    m_locked = false;
    emit metricsUpdated(-120.0f, 0.0f, 0.0f, false);
}

void PskDecoder::setBaud(double baud)
{
    baud = std::clamp(baud, 10.0, 250.0);
    if (std::abs(m_baud - baud) < 0.001)
        return;

    m_baud = baud;
    m_windowSamples = std::max(32, static_cast<int>(std::lround(m_sampleRate / m_baud)));
    m_hopSamples = std::max(8, m_windowSamples / m_oversample);
    reset();
}

void PskDecoder::setQpsk(bool on)
{
    if (m_qpsk == on)
        return;
    m_qpsk = on;
    reset();
}

void PskDecoder::setRxOffsetHz(int hz)
{
    hz = std::clamp(hz, 95, 5000);
    if (m_rxOffsetHz == hz)
        return;
    m_rxOffsetHz = hz;
    reset();
}

void PskDecoder::setSquelch(float threshold)
{
    m_squelch = std::clamp(threshold, 0.0f, 0.95f);
}

void PskDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    const int floatCount = pcm24kStereo.size() / static_cast<int>(sizeof(float));
    if (floatCount < 2)
        return;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int frames = floatCount / 2;
    m_sampleBuffer.reserve(m_sampleBuffer.size() + static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const float mono = 0.5f * (src[i * 2] + src[i * 2 + 1]);
        m_sampleBuffer.push_back(mono);
    }

    m_totalSamples += frames;
    processAvailableWindows();
}

std::complex<float> PskDecoder::integrateWindow(qint64 windowStartAbs, int windowSamples) const
{
    const qint64 localStart = windowStartAbs - m_bufferStartSample;
    if (localStart < 0 || localStart + windowSamples > static_cast<qint64>(m_sampleBuffer.size()))
        return {};

    const double phaseInc = -kTwoPi * static_cast<double>(m_rxOffsetHz) / m_sampleRate;
    double phase = phaseInc * static_cast<double>(windowStartAbs);
    std::complex<float> accum(0.0f, 0.0f);

    for (int i = 0; i < windowSamples; ++i) {
        const float s = m_sampleBuffer[static_cast<size_t>(localStart + i)];
        accum += std::complex<float>(s * std::cos(phase), s * std::sin(phase));
        phase += phaseInc;
    }

    return accum / static_cast<float>(windowSamples);
}

void PskDecoder::updateLock(bool good)
{
    if (good)
        m_lockScore = std::min(m_lockScore + 1, 30);
    else
        m_lockScore = std::max(m_lockScore - 1, 0);

    m_locked = (m_lockScore >= 5);
}

void PskDecoder::emitBitsFromAngle(float angle, float confidence)
{
    if (m_qpsk) {
        static constexpr std::array<float, 4> kPhases = {
            0.0f,
            kHalfPi,
            kPi,
            -kHalfPi
        };
        static constexpr std::array<std::pair<int, int>, 4> kBits = {
            std::pair<int, int>{1, 1},
            std::pair<int, int>{0, 1},
            std::pair<int, int>{0, 0},
            std::pair<int, int>{1, 0},
        };

        int best = 0;
        float bestErr = std::numeric_limits<float>::max();
        for (int i = 0; i < static_cast<int>(kPhases.size()); ++i) {
            const float err = std::abs(wrapPi(angle - kPhases[i]));
            if (err < bestErr) {
                bestErr = err;
                best = i;
            }
        }

        const float phaseConfidence = 1.0f - std::min(1.0f, bestErr / kHalfPi);
        const float finalConfidence = confidence * phaseConfidence;
        updateLock(finalConfidence >= m_squelch);
        if (finalConfidence < m_squelch)
            return;

        handleBit(kBits[best].first);
        handleBit(kBits[best].second);
        return;
    }

    const float phaseConfidence = std::abs(std::cos(angle));
    const float finalConfidence = confidence * phaseConfidence;
    updateLock(finalConfidence >= m_squelch);
    if (finalConfidence < m_squelch)
        return;

    handleBit(std::cos(angle) >= 0.0f ? 1 : 0);
}

void PskDecoder::handleBit(int bit)
{
    if (bit == 0) {
        if (m_pendingZero) {
            if (!m_currentCode.isEmpty()) {
                QChar ch;
                if (PskVaricode::decodeCode(m_currentCode, &ch)) {
                    if (ch == '\r') {
                        // ignore
                    } else if (ch == '\n') {
                        emit textDecoded(QStringLiteral("\n"));
                    } else {
                        emit textDecoded(QString(ch));
                    }
                }
                m_currentCode.clear();
            }
            m_pendingZero = false;
        } else {
            m_pendingZero = true;
        }
        return;
    }

    if (m_pendingZero) {
        m_currentCode.append('0');
        m_pendingZero = false;
    }
    m_currentCode.append('1');
    if (m_currentCode.size() > 16) {
        m_currentCode.clear();
        m_pendingZero = false;
    }
}

void PskDecoder::processAvailableWindows()
{
    while (m_totalSamples >= m_nextWindowEnd) {
        const qint64 windowStartAbs = m_nextWindowEnd - m_windowSamples;
        const std::complex<float> z = integrateWindow(windowStartAbs, m_windowSamples);
        const float mag = std::abs(z);
        const int phase = m_windowIndex % m_oversample;

        for (auto& score : m_phaseScores)
            score *= 0.985f;
        m_phaseScores[phase] += mag;

        int bestPhase = 0;
        float bestScore = m_phaseScores[0];
        for (int i = 1; i < m_oversample; ++i) {
            if (m_phaseScores[i] > bestScore) {
                bestScore = m_phaseScores[i];
                bestPhase = i;
            }
        }
        m_bestPhase = bestPhase;

        m_levelAverage = (m_levelAverage * 0.97f) + (mag * 0.03f);
        const float timingQuality = bestScore > 1e-6f
            ? std::clamp((bestScore - 0.5f * (m_phaseScores[(bestPhase + 1) % m_oversample]
                                           + m_phaseScores[(bestPhase + m_oversample - 1) % m_oversample]))
                         / bestScore, 0.0f, 1.0f)
            : 0.0f;

        if (phase == m_bestPhase) {
            if (m_havePrevSymbol) {
                const std::complex<float> diff = z * std::conj(m_prevSymbol);
                const float angle = std::atan2(diff.imag(), diff.real());
                const float conf = std::clamp(mag / std::max(m_levelAverage, 1e-4f), 0.0f, 1.0f);
                m_confidenceAverage = (m_confidenceAverage * 0.96f) + (conf * 0.04f);
                emitBitsFromAngle(angle, conf);
            }
            m_prevSymbol = z;
            m_havePrevSymbol = true;
        }

        emit metricsUpdated(dbFromMag(std::max(m_levelAverage, mag)),
                            m_confidenceAverage,
                            timingQuality,
                            m_locked);

        ++m_windowIndex;
        m_nextWindowEnd += m_hopSamples;
    }

    trimBuffers();
}

void PskDecoder::trimBuffers()
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
}

} // namespace AetherSDR
