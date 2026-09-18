#pragma once

#include "core/SstvModes.h"

#include <QObject>
#include <QImage>

#include <array>
#include <vector>

class QByteArray;

namespace AetherSDR {

class SstvDecoder : public QObject {
    Q_OBJECT

public:
    explicit SstvDecoder(QObject* parent = nullptr);

    void clear();
    QImage currentImage() const;

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);
    void setAutoMode(bool autoMode);
    void setManualMode(const QString& modeName);

signals:
    void imageUpdated(const QImage& image);
    void detectedModeChanged(const QString& modeName);
    void lineProgressChanged(int line, int totalLines);
    void statusChanged(const QString& text);
    void syncStateChanged(bool locked, float quality);

private:
    struct HilbertTap {
        int delay;
        float coeff;
    };

    static constexpr int kInputRate = 24000;
    static constexpr int kToneDecimation = 2;
    static constexpr int kToneRate = kInputRate / kToneDecimation;
    static constexpr int kHilbertTaps = 31;

    static std::array<HilbertTap, kHilbertTaps / 2> makeHilbertTaps();

    void processToneBuffer();
    void pushTone(float toneHz);
    bool tryDetectVisHeader();
    bool decodeLineAtSync(qint64 syncIndexAbs, float* qualityOut);
    bool findSyncNear(qint64 expectedAbs, qint64* syncIndexAbs, float* qualityOut) const;
    bool findSyncWide(qint64 fromAbs, qint64* syncIndexAbs, float* qualityOut) const;
    float averageTone(qint64 absStart, int count) const;
    float sampleTone(double absCenter, double span) const;
    float sampleLevel(const SstvModeDefinition& mode,
                      qint64 syncIndexAbs,
                      double beginMs,
                      double endMs,
                      int pixelIndex,
                      int totalPixels,
                      float offsetHz) const;
    int decodeVisCode(qint64 visStartAbs) const;
    void startFrame(const SstvModeDefinition* mode, const QString& reason);
    void trimBuffers();
    int msToToneSamples(double ms) const;
    qint64 toLocalIndex(qint64 absIndex) const;

    const SstvModeDefinition* m_manualMode{nullptr};
    const SstvModeDefinition* m_activeMode{nullptr};
    bool m_autoMode{true};

    std::array<HilbertTap, kHilbertTaps / 2> m_hilbertTaps;
    std::array<float, kHilbertTaps> m_history{};
    int m_historyPos{0};
    int m_samplesSeen{0};
    bool m_havePrevPhase{false};
    float m_prevPhase{0.0f};
    float m_freqAccum{1900.0f};
    int m_decimationCounter{0};

    std::vector<float> m_tones;
    std::vector<double> m_prefixSums{0.0};
    qint64 m_toneStartIndex{0};
    qint64 m_totalToneSamples{0};
    qint64 m_headerSearchIndex{0};
    qint64 m_searchIndex{0};
    qint64 m_expectedSyncIndex{-1};
    bool m_syncLocked{false};
    float m_lastSyncQuality{0.0f};

    QImage m_image;
    int m_nextLine{0};
};

} // namespace AetherSDR
