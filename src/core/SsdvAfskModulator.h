#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

#include <vector>

namespace AetherSDR {

class SsdvAfskModulator {
public:
    bool startTransmission(const QImage& image,
                           const QString& callsign,
                           int imageId,
                           QString* errorMessage);
    void clear();
    bool hasPendingData() const;
    qsizetype pendingFrames() const;
    int packetCount() const { return m_packetCount; }
    QByteArray renderAudio(int frames);

private:
    static quint16 crc16Ccitt(const QByteArray& payload);

    void appendBit(bool bit);
    void appendFlag(int repeatCount = 1);
    void appendFrame(const QByteArray& payload, bool firstFrame);
    void appendToneSamples(double frequencyHz, int samples);

    double m_sampleRate{24000.0};
    int m_samplesPerBit{20};
    double m_phase{0.0};
    bool m_markState{true};
    int m_packetCount{0};
    std::vector<float> m_audio;
    qsizetype m_readPos{0};
};

} // namespace AetherSDR
