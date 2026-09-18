#include "PskModulator.h"

#include "PskVaricode.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kHalfPi = 1.57079632679489661923;
constexpr double kTwoPi = 6.28318530717958647692;

double wrapPi(double angle)
{
    while (angle <= -kPi)
        angle += kTwoPi;
    while (angle > kPi)
        angle -= kTwoPi;
    return angle;
}

} // namespace

void PskModulator::reset()
{
    m_symbolQueue.clear();
    m_carrierPhase = 0.0;
    m_symbolPhase = 0.0;
    m_symbolProgress = 0.0;
    m_symbolStartPhase = 0.0;
    m_symbolDiff = 0.0;
    m_symbolLoaded = false;
}

void PskModulator::setBaud(double baud)
{
    m_baud = std::clamp(baud, 10.0, 250.0);
    m_samplesPerSymbol = m_sampleRate / m_baud;
}

void PskModulator::setQpsk(bool on)
{
    m_qpsk = on;
}

void PskModulator::setTxOffsetHz(int hz)
{
    m_txOffsetHz = std::clamp(hz, 95, 5000);
}

void PskModulator::clearQueue()
{
    m_symbolQueue.clear();
}

bool PskModulator::hasPendingData() const
{
    return m_symbolLoaded || !m_symbolQueue.empty();
}

int PskModulator::pendingSymbols() const
{
    return static_cast<int>(m_symbolQueue.size()) + (m_symbolLoaded ? 1 : 0);
}

void PskModulator::queueText(const QString& text)
{
    QByteArray bits;
    bits.append(QByteArray(32, '0'));
    bits.append(PskVaricode::encodeText(text));
    bits.append(QByteArray(32, '0'));

    if (!m_qpsk) {
        for (const char bit : bits)
            m_symbolQueue.push_back(bit == '1' ? 0 : 2);
        return;
    }

    if (bits.size() & 1)
        bits.append('0');

    for (int i = 0; i < bits.size(); i += 2) {
        const char b0 = bits[i];
        const char b1 = bits[i + 1];
        int symbol = 0;
        if (b0 == '1' && b1 == '1')
            symbol = 0;
        else if (b0 == '0' && b1 == '1')
            symbol = 1;
        else if (b0 == '0' && b1 == '0')
            symbol = 2;
        else
            symbol = 3;
        m_symbolQueue.push_back(symbol);
    }
}

QByteArray PskModulator::renderAudio(int frames)
{
    frames = std::max(frames, 1);
    QByteArray pcm(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());

    const double carrierInc = kTwoPi * static_cast<double>(m_txOffsetHz) / m_sampleRate;
    static constexpr double kSymbolAngles[4] = {0.0, kHalfPi, kPi, -kHalfPi};

    for (int i = 0; i < frames; ++i) {
        if (!m_symbolLoaded) {
            m_symbolStartPhase = m_symbolPhase;
            const int symbol = m_symbolQueue.empty() ? 0 : m_symbolQueue.front();
            if (!m_symbolQueue.empty())
                m_symbolQueue.pop_front();
            m_symbolDiff = kSymbolAngles[symbol];
            m_symbolLoaded = true;
        }

        const double t = std::clamp(m_symbolProgress / m_samplesPerSymbol, 0.0, 1.0);
        const double smooth = 0.5 - 0.5 * std::cos(kPi * t);
        const double phaseShape = m_symbolStartPhase + (m_symbolDiff * smooth);

        m_carrierPhase += carrierInc;
        if (m_carrierPhase >= kTwoPi)
            m_carrierPhase -= kTwoPi;

        const float sample = 0.78f * static_cast<float>(std::cos(m_carrierPhase + phaseShape));
        dst[i * 2] = sample;
        dst[i * 2 + 1] = sample;

        m_symbolProgress += 1.0;
        if (m_symbolProgress >= m_samplesPerSymbol) {
            m_symbolProgress -= m_samplesPerSymbol;
            m_symbolPhase = wrapPi(m_symbolStartPhase + m_symbolDiff);
            m_symbolLoaded = false;
        }
    }

    return pcm;
}

} // namespace AetherSDR
