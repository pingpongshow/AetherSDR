#pragma once

#include <QObject>
#include <QByteArray>
#include <QImage>
#include <QMap>

#include <array>
#include <vector>

namespace AetherSDR {

class SsdvAfskDecoder : public QObject {
    Q_OBJECT

public:
    explicit SsdvAfskDecoder(QObject* parent = nullptr);

    void reset();

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);

signals:
    void imageUpdated(const QImage& image);
    void metricsUpdated(float markLevelDb, float spaceLevelDb, float confidence, bool locked);
    void packetStatsChanged(int packetCount, int imageId, int lastPacketId, const QString& callsign);
    void statusChanged(const QString& text);

private:
    struct PhaseState {
        bool havePrevLevel{false};
        bool prevLevel{true};
        quint8 flagReg{0};
        bool inFrame{false};
        int recentFlagHold{0};
        std::vector<quint8> rawBits;
    };

    static quint16 crc16Ccitt(const QByteArray& payload);

    bool decodeFrameBits(const std::vector<quint8>& rawBits, QByteArray* payload) const;
    float goertzelPower(const float* samples, int count, double freqHz) const;
    void handleDecision(int decisionIndex, bool markLevel);
    void handlePacket(const QByteArray& packet);
    void processAvailableWindows();
    void processPhaseBit(PhaseState& phase, bool bit);
    void trimBuffers();
    void updateLockState();

    double m_sampleRate{24000.0};
    int m_windowSamples{20};
    int m_hopSamples{5};
    int m_metricDecimate{0};

    std::vector<float> m_sampleBuffer;
    qint64 m_bufferStartSample{0};
    qint64 m_totalSamples{0};
    qint64 m_nextWindowEnd{20};
    int m_decisionCount{0};

    std::array<PhaseState, 4> m_phases;
    bool m_locked{false};
    float m_lastConfidence{0.0f};

    QString m_currentCallsign;
    int m_currentImageId{-1};
    QMap<int, QByteArray> m_packets;
};

} // namespace AetherSDR
