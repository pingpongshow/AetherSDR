#include "SsdvAfskDecoder.h"

#include "core/SsdvCodec.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kMarkHz = 1200.0;
constexpr double kSpaceHz = 2200.0;

float powerToDb(float power)
{
    const float rms = std::sqrt(std::max(power, 1.0e-12f));
    return 20.0f * std::log10(rms);
}

} // namespace

SsdvAfskDecoder::SsdvAfskDecoder(QObject* parent)
    : QObject(parent)
{
    emit statusChanged(QStringLiteral("SSDV decoder ready."));
}

void SsdvAfskDecoder::reset()
{
    m_sampleBuffer.clear();
    m_bufferStartSample = 0;
    m_totalSamples = 0;
    m_nextWindowEnd = m_windowSamples;
    m_decisionCount = 0;
    m_metricDecimate = 0;
    m_phases = {};
    m_locked = false;
    m_lastConfidence = 0.0f;
    m_currentCallsign.clear();
    m_currentImageId = -1;
    m_packets.clear();
    emit imageUpdated(QImage());
    emit packetStatsChanged(0, -1, -1, QString());
    emit metricsUpdated(-120.0f, -120.0f, 0.0f, false);
    emit statusChanged(QStringLiteral("SSDV decoder ready."));
}

void SsdvAfskDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    const int floatCount = pcm24kStereo.size() / static_cast<int>(sizeof(float));
    if (floatCount < 2)
        return;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int frames = floatCount / 2;

    m_sampleBuffer.reserve(m_sampleBuffer.size() + static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
        m_sampleBuffer.push_back(0.5f * (src[i * 2] + src[i * 2 + 1]));

    m_totalSamples += frames;
    processAvailableWindows();
}

quint16 SsdvAfskDecoder::crc16Ccitt(const QByteArray& payload)
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

float SsdvAfskDecoder::goertzelPower(const float* samples, int count, double freqHz) const
{
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

bool SsdvAfskDecoder::decodeFrameBits(const std::vector<quint8>& rawBits, QByteArray* payload) const
{
    if (!payload || rawBits.size() < 32)
        return false;

    std::vector<quint8> bits;
    bits.reserve(rawBits.size());

    int ones = 0;
    for (quint8 rawBit : rawBits) {
        const bool bit = rawBit != 0;
        if (bit) {
            bits.push_back(1);
            ++ones;
            if (ones > 6)
                return false;
        } else {
            if (ones == 5) {
                ones = 0;
                continue;
            }
            ones = 0;
            bits.push_back(0);
        }
    }

    if ((bits.size() % 8) != 0 || bits.size() < 24)
        return false;

    QByteArray frame(static_cast<int>(bits.size() / 8), '\0');
    for (size_t i = 0; i < bits.size(); ++i) {
        if (!bits[i])
            continue;
        frame[static_cast<int>(i / 8)] =
            static_cast<char>(static_cast<quint8>(frame[static_cast<int>(i / 8)]) | (1u << (i % 8)));
    }

    if (frame.size() < 3)
        return false;

    const QByteArray framePayload = frame.left(frame.size() - 2);
    const quint16 receivedCrc =
        static_cast<quint8>(frame.at(frame.size() - 2))
        | (static_cast<quint16>(static_cast<quint8>(frame.at(frame.size() - 1))) << 8);
    if (crc16Ccitt(framePayload) != receivedCrc)
        return false;

    *payload = framePayload;
    return true;
}

void SsdvAfskDecoder::handleDecision(int decisionIndex, bool markLevel)
{
    PhaseState& phase = m_phases[decisionIndex % static_cast<int>(m_phases.size())];
    if (!phase.havePrevLevel) {
        phase.havePrevLevel = true;
        phase.prevLevel = markLevel;
        return;
    }

    const bool bit = (markLevel == phase.prevLevel);
    phase.prevLevel = markLevel;
    processPhaseBit(phase, bit);
}

void SsdvAfskDecoder::handlePacket(const QByteArray& packet)
{
    SsdvPacketInfo info;
    QString error;
    if (!SsdvCodec::inspectPacket(packet, &info, &error))
        return;

    if (info.callsign != m_currentCallsign || info.imageId != m_currentImageId) {
        m_currentCallsign = info.callsign;
        m_currentImageId = info.imageId;
        m_packets.clear();
        emit imageUpdated(QImage());
    }

    if (m_packets.contains(info.packetId))
        return;

    m_packets.insert(info.packetId, packet);
    emit packetStatsChanged(m_packets.size(), info.imageId, info.packetId, info.callsign);

    QByteArray ordered;
    ordered.reserve(m_packets.size() * packet.size());
    for (auto it = m_packets.cbegin(); it != m_packets.cend(); ++it)
        ordered.append(it.value());

    QImage image;
    QString callsign;
    int imageId = 0;
    QString decodeError;
    if (SsdvCodec::decodePackets(ordered, &image, &callsign, &imageId, &decodeError) && !image.isNull())
        emit imageUpdated(image);

    emit statusChanged(QStringLiteral("SSDV packet %1 received from %2 image %3 (%4 packets).")
                       .arg(info.packetId)
                       .arg(info.callsign.isEmpty() ? QStringLiteral("UNKNOWN") : info.callsign)
                       .arg(info.imageId)
                       .arg(m_packets.size()));
}

void SsdvAfskDecoder::processAvailableWindows()
{
    while (m_totalSamples >= m_nextWindowEnd) {
        const qint64 windowStartAbs = m_nextWindowEnd - m_windowSamples;
        const qint64 localStart = windowStartAbs - m_bufferStartSample;
        if (localStart < 0 || localStart + m_windowSamples > static_cast<qint64>(m_sampleBuffer.size()))
            break;

        const auto* window = m_sampleBuffer.data() + static_cast<size_t>(localStart);
        const float markPower = goertzelPower(window, m_windowSamples, kMarkHz);
        const float spacePower = goertzelPower(window, m_windowSamples, kSpaceHz);
        const float sum = markPower + spacePower + 1.0e-12f;
        m_lastConfidence = std::abs(markPower - spacePower) / sum;

        handleDecision(m_decisionCount++, markPower >= spacePower);

        for (auto& phase : m_phases) {
            if (phase.recentFlagHold > 0)
                --phase.recentFlagHold;
        }
        updateLockState();

        if ((++m_metricDecimate % 2) == 0)
            emit metricsUpdated(powerToDb(markPower), powerToDb(spacePower), m_lastConfidence, m_locked);

        m_nextWindowEnd += m_hopSamples;
    }

    trimBuffers();
}

void SsdvAfskDecoder::processPhaseBit(PhaseState& phase, bool bit)
{
    phase.flagReg = static_cast<quint8>(((phase.flagReg << 1) | (bit ? 1 : 0)) & 0xFF);

    if (!phase.inFrame) {
        if (phase.flagReg == 0x7E) {
            phase.inFrame = true;
            phase.rawBits.clear();
            phase.recentFlagHold = 1200;
        }
        return;
    }

    phase.rawBits.push_back(bit ? 1 : 0);
    if (phase.rawBits.size() > 32768) {
        phase.inFrame = false;
        phase.rawBits.clear();
        return;
    }

    if (phase.flagReg != 0x7E)
        return;

    phase.recentFlagHold = 1200;
    if (phase.rawBits.size() >= 8)
        phase.rawBits.resize(phase.rawBits.size() - 8);

    QByteArray payload;
    if (decodeFrameBits(phase.rawBits, &payload))
        handlePacket(payload);

    phase.rawBits.clear();
    phase.inFrame = true;
}

void SsdvAfskDecoder::trimBuffers()
{
    const qint64 keepWindowStarts = std::max<qint64>(0, m_nextWindowEnd - (m_windowSamples * 8));
    if (keepWindowStarts <= m_bufferStartSample)
        return;

    const qint64 drop = keepWindowStarts - m_bufferStartSample;
    if (drop <= 0 || drop >= static_cast<qint64>(m_sampleBuffer.size()))
        return;

    m_sampleBuffer.erase(m_sampleBuffer.begin(),
                         m_sampleBuffer.begin() + static_cast<std::ptrdiff_t>(drop));
    m_bufferStartSample = keepWindowStarts;
}

void SsdvAfskDecoder::updateLockState()
{
    bool locked = false;
    for (const auto& phase : m_phases) {
        if (phase.recentFlagHold > 0) {
            locked = true;
            break;
        }
    }

    if (locked == m_locked)
        return;

    m_locked = locked;
    emit metricsUpdated(-120.0f, -120.0f, m_lastConfidence, m_locked);
}

} // namespace AetherSDR
