#pragma once

#include "core/SstvModes.h"

#include <QByteArray>
#include <QImage>
#include <QString>

#include <vector>

namespace AetherSDR {

class SstvModulator {
public:
    bool startTransmission(const QImage& image, const SstvModeDefinition& mode);
    void clear();
    bool hasPendingData() const;
    qsizetype pendingFrames() const;
    QByteArray renderAudio(int frames);
    QString activeModeName() const { return m_activeModeName; }

private:
    enum class Channel {
        Red,
        Green,
        Blue,
        Gray
    };

    int msToSamples(double ms) const;
    void appendTone(double frequencyHz, double durationMs);
    void appendToneSamples(double frequencyHz, int samples);
    void appendVisHeader(int visCode);
    void appendMartinImage(const QImage& image, const SstvModeDefinition& mode);
    void appendScottieImage(const QImage& image, const SstvModeDefinition& mode);
    void appendPasokonImage(const QImage& image, const SstvModeDefinition& mode);
    void appendWraaseImage(const QImage& image, const SstvModeDefinition& mode);
    void appendRobotBwImage(const QImage& image, const SstvModeDefinition& mode);
    void appendChannelScan(const QImage& image,
                           int y,
                           Channel channel,
                           double durationMs);
    int channelLevel(const QImage& image, int x, int y, Channel channel) const;

    double m_sampleRate{24000.0};
    double m_phase{0.0};
    QString m_activeModeName;
    std::vector<float> m_audio;
    qsizetype m_readPos{0};
};

} // namespace AetherSDR
