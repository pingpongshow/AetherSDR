#pragma once

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QThread>

#include <array>
#include <atomic>
#include <vector>

namespace AetherSDR {

class HfFaxDecoder : public QObject {
    Q_OBJECT

public:
    static constexpr int kImageWidth = 800;
    static constexpr int kLineRateLpm = 120;
    static constexpr int kIoc = 576;

    explicit HfFaxDecoder(QObject* parent = nullptr);
    ~HfFaxDecoder() override;

    void start();
    void stop();
    bool isRunning() const { return m_running; }

public slots:
    void clear();
    void feedAudio(const QByteArray& pcm24kStereo);
    void setPaused(bool paused) { m_paused = paused; }
    void setAutoStart(bool on) { m_autoStart = on; }
    void setClockAdjustPpm(int ppm) { m_clockAdjustPpm = ppm; }

signals:
    void rawLineDecoded(const QByteArray& grayLine);
    void syncStateChanged(bool locked, float quality);
    void toneChanged(float toneHz);
    void statusChanged(const QString& text);
    void transmissionStarted();
    void transmissionEnded();

private:
    static constexpr int kInputRate = 24000;
    static constexpr int kOutputRate = 6000;
    static constexpr int kDecimation = kInputRate / kOutputRate;
    static constexpr int kHilbertTaps = 31;
    static constexpr int kSyncPulseMs = 25;
    static constexpr int kInputQueueCapacity = kInputRate * static_cast<int>(sizeof(float)) * 8;

    struct HilbertTap {
        int delay;
        float coeff;
    };

    void decodeLoop();
    void processMonoBlock(const float* mono, int sampleCount);
    void handleDecimatedTone(float toneHz);
    bool maybeAcquireSync();
    bool consumeLockedLine();
    int lineSamples() const;
    int syncSamples() const;
    void resetSyncState(bool keepTransmissionState = false);
    QByteArray buildGrayLine(const std::vector<float>& line, int syncSampleCount) const;

    static std::array<HilbertTap, kHilbertTaps / 2> makeHilbertTaps();

    QMutex m_bufMutex;
    QByteArray m_inputBuf;
    QThread* m_workerThread{nullptr};

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_autoStart{true};
    std::atomic<bool> m_clearRequested{false};
    std::atomic<int>  m_clockAdjustPpm{0};

    std::array<float, kHilbertTaps> m_history{};
    std::array<HilbertTap, kHilbertTaps / 2> m_hilbertTaps;
    int m_historyPos{0};
    int m_samplesSeen{0};
    bool m_havePrevPhase{false};
    float m_prevPhase{0.0f};
    float m_freqEma{1900.0f};
    int m_decimationCounter{0};
    int m_toneEmitCounter{0};

    std::vector<float> m_brightnessBuf;
    bool m_locked{false};
    bool m_transmissionActive{false};
    int m_weakSyncLines{0};
    int m_startToneCount{0};
    int m_stopToneCount{0};
};

} // namespace AetherSDR
