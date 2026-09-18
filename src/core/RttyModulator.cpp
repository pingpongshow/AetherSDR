#include "RttyModulator.h"

#include <QChar>

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

int findCode(const char (&table)[32], QChar ch)
{
    const char target = ch.toLatin1();
    if (target == '\0')
        return -1;
    for (int i = 0; i < 32; ++i) {
        if (table[i] == target)
            return i;
    }
    return -1;
}

} // namespace

void RttyModulator::setBaud(double baud)
{
    m_baud = std::clamp(baud, 10.0, 300.0);
    m_samplesPerBit = m_sampleRate / m_baud;
}

void RttyModulator::setMarkHz(int hz)
{
    m_markHz = std::clamp(hz, 200, 4000);
}

void RttyModulator::setShiftHz(int hz)
{
    m_shiftHz = std::clamp(hz, 10, 1000);
}

void RttyModulator::setInverted(bool on)
{
    m_inverted = on;
}

void RttyModulator::setUnshiftOnSpace(bool on)
{
    m_unshiftOnSpace = on;
}

void RttyModulator::clearQueue()
{
    m_bitQueue.clear();
}

void RttyModulator::resetStreamState()
{
    m_phase = 0.0;
    m_bitSamplePos = 0.0;
    m_currentBit = 1;
    m_currentBitFromQueue = false;
    m_lettersMode = true;
}

bool RttyModulator::hasPendingData() const
{
    return m_currentBitFromQueue || !m_bitQueue.empty();
}

int RttyModulator::pendingBits() const
{
    return static_cast<int>(m_bitQueue.size()) + (m_currentBitFromQueue ? 1 : 0);
}

void RttyModulator::appendCode(quint8 code)
{
    m_bitQueue.push_back(0);
    for (int i = 0; i < 5; ++i)
        m_bitQueue.push_back((code >> i) & 0x01);
    m_bitQueue.push_back(1);
    m_bitQueue.push_back(1);
}

void RttyModulator::appendPrintableCharacter(QChar ch)
{
    if (ch == '\r')
        return;

    if (ch == '\n') {
        appendCode(8);
        appendCode(2);
        m_lettersMode = true;
        return;
    }

    if (ch == ' ') {
        appendCode(4);
        if (m_unshiftOnSpace)
            m_lettersMode = true;
        return;
    }

    const QChar upper = ch.toUpper();
    const int lettersCode = findCode(kLettersTable, upper);
    const int figuresCode = findCode(kFiguresTable, upper);

    if (lettersCode < 0 && figuresCode < 0) {
        appendCode(4);
        if (m_unshiftOnSpace)
            m_lettersMode = true;
        return;
    }

    if (lettersCode >= 0 && figuresCode < 0) {
        if (!m_lettersMode) {
            appendCode(kLettersShift);
            m_lettersMode = true;
        }
        appendCode(static_cast<quint8>(lettersCode));
        return;
    }

    if (figuresCode >= 0 && lettersCode < 0) {
        if (m_lettersMode) {
            appendCode(kFiguresShift);
            m_lettersMode = false;
        }
        appendCode(static_cast<quint8>(figuresCode));
        return;
    }

    appendCode(static_cast<quint8>(m_lettersMode ? lettersCode : figuresCode));
}

void RttyModulator::queueText(const QString& text)
{
    for (const QChar ch : text)
        appendPrintableCharacter(ch);
}

QByteArray RttyModulator::renderAudio(int frames)
{
    frames = std::max(frames, 1);
    QByteArray pcm(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());

    const double spaceHz = std::max(10, m_markHz - m_shiftHz);

    for (int i = 0; i < frames; ++i) {
        while (m_bitSamplePos >= m_samplesPerBit || (!m_currentBitFromQueue && !m_bitQueue.empty())) {
            if (m_bitSamplePos >= m_samplesPerBit)
                m_bitSamplePos -= m_samplesPerBit;

            if (!m_bitQueue.empty()) {
                m_currentBit = m_bitQueue.front();
                m_bitQueue.pop_front();
                m_currentBitFromQueue = true;
            } else {
                m_currentBit = 1;
                m_currentBitFromQueue = false;
            }
        }

        const int logicalBit = m_inverted ? (m_currentBit ? 0 : 1) : m_currentBit;
        const double freq = logicalBit ? static_cast<double>(m_markHz) : spaceHz;

        m_phase += kTwoPi * freq / m_sampleRate;
        if (m_phase >= kTwoPi)
            m_phase -= kTwoPi;

        const float sample = 0.75f * static_cast<float>(std::sin(m_phase));
        dst[i * 2] = sample;
        dst[i * 2 + 1] = sample;
        m_bitSamplePos += 1.0;

        if (m_currentBitFromQueue && m_bitSamplePos >= m_samplesPerBit && m_bitQueue.empty())
            m_currentBitFromQueue = false;
    }

    return pcm;
}

} // namespace AetherSDR
