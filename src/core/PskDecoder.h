#pragma once

#include <QObject>

#include <array>
#include <complex>
#include <vector>

class QByteArray;

namespace AetherSDR {

class PskDecoder : public QObject {
    Q_OBJECT

public:
    explicit PskDecoder(QObject* parent = nullptr);

    void reset();

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);
    void setBaud(double baud);
    void setQpsk(bool on);
    void setRxOffsetHz(int hz);
    void setSquelch(float threshold);

signals:
    void textDecoded(const QString& text);
    void metricsUpdated(float signalLevelDb, float confidence, float timingQuality, bool locked);

private:
    std::complex<float> integrateWindow(qint64 windowStartAbs, int windowSamples) const;
    void emitBitsFromAngle(float angle, float confidence);
    void handleBit(int bit);
    void processAvailableWindows();
    void trimBuffers();
    void updateLock(bool good);

    double m_sampleRate{24000.0};
    double m_baud{31.25};
    bool m_qpsk{false};
    int m_rxOffsetHz{2000};
    float m_squelch{0.18f};

    int m_oversample{4};
    int m_windowSamples{768};
    int m_hopSamples{192};

    std::vector<float> m_sampleBuffer;
    qint64 m_bufferStartSample{0};
    qint64 m_totalSamples{0};
    qint64 m_nextWindowEnd{768};

    int m_windowIndex{0};
    int m_bestPhase{0};
    std::array<float, 4> m_phaseScores{{0.0f, 0.0f, 0.0f, 0.0f}};

    bool m_havePrevSymbol{false};
    std::complex<float> m_prevSymbol{1.0f, 0.0f};
    bool m_pendingZero{false};
    QByteArray m_currentCode;
    float m_levelAverage{0.0f};
    float m_confidenceAverage{0.0f};
    int m_lockScore{0};
    bool m_locked{false};
};

} // namespace AetherSDR
