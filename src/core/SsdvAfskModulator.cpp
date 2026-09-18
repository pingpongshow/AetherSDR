#include "SsdvAfskModulator.h"

#include "core/SsdvCodec.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kMarkHz = 1200.0;
constexpr double kSpaceHz = 2200.0;
constexpr float kAmplitude = 0.78f;
constexpr int kPacketBytes = 256;

} // namespace

bool SsdvAfskModulator::startTransmission(const QImage& image,
                                          const QString& callsign,
                                          int imageId,
                                          QString* errorMessage)
{
    clear();

    QByteArray packets;
    if (!SsdvCodec::encodeImage(image, callsign, imageId, &packets, errorMessage))
        return false;

    m_packetCount = packets.size() / kPacketBytes;
    if (m_packetCount <= 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("No SSDV packets were produced for transmission.");
        clear();
        return false;
    }

    const qsizetype estimatedBits =
        (48 + (m_packetCount * (8 + ((kPacketBytes + 2) * 10)))) * 8;
    m_audio.reserve(static_cast<size_t>(estimatedBits * m_samplesPerBit));

    for (int packetIndex = 0; packetIndex < m_packetCount; ++packetIndex) {
        const QByteArray payload = packets.mid(packetIndex * kPacketBytes, kPacketBytes);
        appendFrame(payload, packetIndex == 0);
    }

    appendFlag(16);
    return !m_audio.empty();
}

void SsdvAfskModulator::clear()
{
    m_phase = 0.0;
    m_markState = true;
    m_packetCount = 0;
    m_audio.clear();
    m_readPos = 0;
}

bool SsdvAfskModulator::hasPendingData() const
{
    return m_readPos < static_cast<qsizetype>(m_audio.size());
}

qsizetype SsdvAfskModulator::pendingFrames() const
{
    return std::max<qsizetype>(0, static_cast<qsizetype>(m_audio.size()) - m_readPos);
}

QByteArray SsdvAfskModulator::renderAudio(int frames)
{
    frames = std::max(frames, 1);
    QByteArray pcm(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());

    for (int i = 0; i < frames; ++i) {
        const float sample = hasPendingData() ? m_audio[static_cast<size_t>(m_readPos++)] : 0.0f;
        dst[i * 2] = sample;
        dst[i * 2 + 1] = sample;
    }

    return pcm;
}

quint16 SsdvAfskModulator::crc16Ccitt(const QByteArray& payload)
{
    quint16 crc = 0xFFFFu;
    for (const char ch : payload) {
        crc ^= static_cast<quint8>(ch);
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x0001u)
                crc = (crc >> 1) ^ 0x8408u;
            else
                crc >>= 1;
        }
    }
    return static_cast<quint16>(~crc);
}

void SsdvAfskModulator::appendBit(bool bit)
{
    if (!bit)
        m_markState = !m_markState;
    appendToneSamples(m_markState ? kMarkHz : kSpaceHz, m_samplesPerBit);
}

void SsdvAfskModulator::appendFlag(int repeatCount)
{
    constexpr quint8 flag = 0x7E;
    for (int repeat = 0; repeat < repeatCount; ++repeat) {
        for (int bit = 0; bit < 8; ++bit)
            appendBit((flag >> bit) & 0x01);
    }
}

void SsdvAfskModulator::appendFrame(const QByteArray& payload, bool firstFrame)
{
    appendFlag(firstFrame ? 48 : 2);

    QByteArray frame = payload;
    const quint16 fcs = crc16Ccitt(payload);
    frame.append(static_cast<char>(fcs & 0xFF));
    frame.append(static_cast<char>((fcs >> 8) & 0xFF));

    int ones = 0;
    for (const char ch : frame) {
        const quint8 byte = static_cast<quint8>(ch);
        for (int bit = 0; bit < 8; ++bit) {
            const bool value = ((byte >> bit) & 0x01) != 0;
            appendBit(value);
            if (value) {
                ++ones;
                if (ones == 5) {
                    appendBit(false);
                    ones = 0;
                }
            } else {
                ones = 0;
            }
        }
    }

    appendFlag();
}

void SsdvAfskModulator::appendToneSamples(double frequencyHz, int samples)
{
    if (samples <= 0)
        return;

    m_audio.reserve(m_audio.size() + static_cast<size_t>(samples));
    const double phaseInc = kTwoPi * frequencyHz / m_sampleRate;
    for (int i = 0; i < samples; ++i) {
        m_phase += phaseInc;
        if (m_phase >= kTwoPi)
            m_phase = std::fmod(m_phase, kTwoPi);
        m_audio.push_back(kAmplitude * static_cast<float>(std::sin(m_phase)));
    }
}

} // namespace AetherSDR
