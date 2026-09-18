#pragma once

#include <QObject>
#include <QVector>

#include <vector>

class QByteArray;

namespace AetherSDR {

class RttyDecoder : public QObject {
    Q_OBJECT

public:
    explicit RttyDecoder(QObject* parent = nullptr);

    double baud() const { return m_baud; }
    int markHz() const { return m_markHz; }
    int shiftHz() const { return m_shiftHz; }
    bool inverted() const { return m_inverted; }
    bool unshiftOnSpace() const { return m_unshiftOnSpace; }
    float squelch() const { return m_squelch; }

    void reset();

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);
    void setBaud(double baud);
    void setMarkHz(int hz);
    void setShiftHz(int hz);
    void setInverted(bool on);
    void setUnshiftOnSpace(bool on);
    void setSquelch(float threshold);

signals:
    void textDecoded(const QString& text);
    void metricsUpdated(float markLevelDb, float spaceLevelDb, float confidence, bool locked);
    void syncStateChanged(bool locked);

private:
    struct Decision {
        bool bit{true};
        float confidence{0.0f};
    };

    void clearState();
    void decodeCharacter(quint8 code);
    float goertzelPower(const float* samples, int count, double freqHz) const;
    void handleDecision(int absoluteIndex, bool bit, float confidence);
    void processAvailableWindows();
    void recalcTiming();
    bool sampleDecision(double decisionIndex) const;
    void trimBuffers();
    void updateLockState(bool goodDecision);

    double m_sampleRate{24000.0};
    double m_baud{45.45};
    int m_markHz{2125};
    int m_shiftHz{170};
    bool m_inverted{false};
    bool m_unshiftOnSpace{true};
    float m_squelch{0.12f};

    int m_windowSamples{528};
    int m_hopSamples{132};
    double m_oversample{4.0};

    std::vector<float> m_sampleBuffer;
    qint64 m_bufferStartSample{0};
    qint64 m_totalSamples{0};
    qint64 m_nextWindowEnd{0};

    QVector<Decision> m_decisions;
    int m_decisionBaseIndex{0};

    bool m_lettersMode{true};
    bool m_prevDecisionBit{true};
    bool m_receivingChar{false};
    bool m_locked{false};
    int m_idleRun{0};
    int m_dataBitPos{0};
    int m_lockScore{0};
    int m_metricDecimate{0};
    quint8 m_currentChar{0};
    double m_nextBitSample{0.0};
};

} // namespace AetherSDR
