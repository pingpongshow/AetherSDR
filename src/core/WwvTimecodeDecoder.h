#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

#include <array>
#include <vector>

class QByteArray;

namespace AetherSDR {

class WwvTimecodeDecoder : public QObject {
    Q_OBJECT

public:
    enum class Profile {
        Wwv,
        Chu,
        Bpm
    };

    explicit WwvTimecodeDecoder(QObject* parent = nullptr);

    void reset();
    void setProfile(Profile profile);
    Profile profile() const { return m_profile; }
    static QString profileName(Profile profile);

public slots:
    void feedAudio(const QByteArray& pcm24kStereo);

signals:
    void currentTimeUpdated(const QString& utcText);
    void frameDecoded(const QString& utcText,
                      const QString& frameText,
                      const QString& flagsText,
                      const QString& bitsText);
    void metricsUpdated(const QString& station,
                        float tickLevelDb,
                        float codeLevelDb,
                        bool locked,
                        int secondOfMinute,
                        bool minuteMarker,
                        bool hourMarker,
                        const QString& pulseClass);
    void statusChanged(const QString& text);

private:
    enum class PulseClass {
        Unknown,
        Zero,
        One,
        Marker
    };

    struct MinuteFrame {
        qint64 startSample{0};
        QString station;
        QVector<PulseClass> pulses;
        QDateTime startUtc;
        bool startUtcValid{false};
    };

    struct DecodedMinute {
        QDateTime startUtc;
        int year{0};
        int dayOfYear{0};
        int hour{0};
        int minute{0};
        int ut1Tenths{0};
        bool ut1Positive{true};
        bool leapSecond{false};
        bool dstCurrent{false};
        bool dstTransitionToday{false};
        QString station;
        QString bitsText;
    };

    struct AnalysisPoint {
        qint64 startSample{0};
        float p1000{0.0f};
        float p1200{0.0f};
        float p1500{0.0f};
    };

    struct SecondSummary {
        QString station;
        float tickLevelDb{-120.0f};
        float codeLevelDb{-120.0f};
        bool minuteMarker{false};
        bool hourMarker{false};
        PulseClass pulse{PulseClass::Unknown};
        QString pulseLabel;
    };

    struct ChuBurst {
        bool valid{false};
        bool formatA{false};
        std::array<quint8, 5> payload{};
        QString rawHex;
        int dayOfYear{-1};
        int hour{-1};
        int minute{-1};
        int second{-1};
        int year{-1};
        int dutTenths{-1};
        bool dutPositive{true};
        bool leapSecond{false};
        int taiMinusUtc{-1};
        quint8 dstCode{0};
    };

    struct ChuMinuteState {
        bool haveFormatA{false};
        bool haveFormatB{false};
        std::array<quint8, 5> formatA{};
        std::array<quint8, 5> formatB{};
        QString rawAHex;
        QString rawBHex;
        int dayOfYear{-1};
        int hour{-1};
        int minute{-1};
        int second{-1};
        int year{-1};
        int dutTenths{-1};
        bool dutPositive{true};
        bool leapSecond{false};
        int taiMinusUtc{-1};
        quint8 dstCode{0};
    };

    float goertzelPower(const float* samples, int count, double freqHz) const;
    static float powerToDb(float power);
    static QString formatHexBytes(const std::array<quint8, 5>& bytes);
    static QString formatUtcLabel(const QDateTime& utc);
    static QString pulseClassLabel(PulseClass pulse);
    static QString pulsePatternText(const QVector<PulseClass>& pulses);
    void clearState();
    int decodeWeighted(const QVector<PulseClass>& pulses,
                       const std::initializer_list<std::pair<int, int>>& mapping) const;
    QString dstSummary(bool current, bool transitionToday) const;
    void finalizeCurrentMinute();
    PulseClass classifyTimecodePulse(const float* second) const;
    int estimateToneDurationMs(const float* second, double freqHz, int blockMs) const;
    QString waitingDisplayText() const;
    QString readyStatusText() const;
    QString lockStatusText() const;
    void processAnalysisWindows();
    void processLockedSeconds();
    void processSecond(qint64 secondStart, const float* second);
    void processSecondWwv(qint64 secondStart, const float* second);
    void processSecondChu(const float* second);
    void processSecondBpm(const float* second);
    void registerPeak(qint64 startSample);
    bool tryDecodeMinute(const MinuteFrame& frame, DecodedMinute* decoded) const;
    bool decodeChuBurst(const float* second, ChuBurst* burst) const;
    void handleChuBurst(const ChuBurst& burst);
    void trimBuffers();

    static constexpr int kSampleRate = 24000;
    static constexpr int kSecondSamples = 24000;
    static constexpr int kAnalysisWindowSamples = 480;
    static constexpr int kAnalysisHopSamples = 120;

    Profile m_profile{Profile::Wwv};
    std::vector<float> m_sampleBuffer;
    qint64 m_bufferStartSample{0};
    qint64 m_totalSamples{0};
    qint64 m_nextAnalysisStart{0};

    QVector<AnalysisPoint> m_recentAnalysis;
    float m_tickNoiseFloor{1e-8f};
    qint64 m_lastPeakStart{-1};
    bool m_locked{false};
    qint64 m_nextSecondStart{-1};
    int m_lockScore{0};

    QString m_lastStation;
    int m_profileSecondOfMinute{-1};
    bool m_profileMinuteAligned{false};
    QDateTime m_profileMinuteStartUtc;
    bool m_profileMinuteStartUtcValid{false};
    MinuteFrame m_currentMinute;
    bool m_haveCurrentMinute{false};
    QDateTime m_nextMinuteUtc;
    bool m_nextMinuteUtcValid{false};
    ChuMinuteState m_chuMinute;
    bool m_chuHourSilenceActive{false};
};

} // namespace AetherSDR
