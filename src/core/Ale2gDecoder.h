#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <array>
#include <vector>

class QByteArray;

namespace AetherSDR {

class Ale2gDecoder : public QObject {
    Q_OBJECT

public:
    explicit Ale2gDecoder(QObject* parent = nullptr);

    void reset();

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);

signals:
    void frameDecoded(const QString& utcText, const QString& summary);
    void metricsUpdated(bool locked,
                        int phaseSamples,
                        int boundary,
                        float score,
                        int totalWords);
    void statusChanged(const QString& text);
    void wordDecoded(const QString& utcText,
                     const QString& preamble,
                     const QString& text,
                     int errors,
                     float confidence,
                     const QString& summary);

private:
    struct WordCandidate {
        bool valid{false};
        QString preamble;
        QString text;
        QString summary;
        int errors{0};
        float confidence{0.0f};
    };

    struct PhaseState {
        int offsetSamples{0};
        qint64 nextSymbolStart{0};
        qint64 bitBaseIndex{0};
        std::vector<unsigned char> bits;
        std::vector<float> bitConfidence;
        std::array<float, 49> boundaryScores{};
        int bestBoundary{0};
        float bestBoundaryScore{0.0f};
        int validHits{0};
        qint64 lastAcceptedEndBit{-100000};
    };

    static constexpr int kSampleRate = 24000;
    static constexpr int kSymbolSamples = 192;
    static constexpr int kPhaseStepSamples = 24;
    static constexpr int kPhaseCount = 8;

    static QString preambleName(int code);
    static float powerToDb(float power);
    static bool isReasonableAleChar(unsigned char ch);
    static QString sanitizedText(unsigned char c1, unsigned char c2, unsigned char c3);
    static int bitsToIntMsbFirst(const unsigned char* bits, int count);

    float goertzelPower(const float* samples, int count, double freqHz) const;
    void buildGolayTable();
    void clearState();
    bool decodeCandidateWord(const PhaseState& phase, qint64 endBit, WordCandidate* candidate) const;
    bool decodeGolayWord(int received, int* decodedData, int* distance) const;
    void emitAcceptedWord(const PhaseState& phase, qint64 endBit, const WordCandidate& candidate);
    void finalizeFrame();
    void processPhase(int phaseIndex);
    void recomputeBestBoundary(PhaseState* phase);
    void refreshActivePhase();
    void trimBuffers();
    void trimPhaseBits(PhaseState* phase);

    std::vector<float> m_sampleBuffer;
    qint64 m_bufferStartSample{0};
    qint64 m_totalSamples{0};
    std::array<PhaseState, kPhaseCount> m_phases;
    std::array<int, 4096> m_golayCodewords{};

    int m_activePhase{-1};
    int m_totalWords{0};
    qint64 m_lastFrameWallMs{0};
    QStringList m_currentFrameWords;
};

} // namespace AetherSDR
