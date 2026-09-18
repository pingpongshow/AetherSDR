#pragma once

#include "CtyDatParser.h"

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>
#include <complex>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace AetherSDR {

// ─────────────────────────────────────────────────────────────────────────────
// CW skimmer: decodes every CW signal inside a DAX IQ stream.
//
//   DAX IQ (24/48/96/192 kHz complex float32, big-endian on the wire)
//     → FFT detector (≈12 Hz bins, smoothed, median noise floor, SNR
//       hysteresis, persistence) → up to maxChannels tracked carriers
//     → per carrier: NCO mix → integrate-and-dump ↓ to ~8 kHz → 4th-order Butterworth
//       LPF (±75 Hz) → envelope @ 500 Hz → adaptive-timing Morse decoder
//     → words → callsign regex + CQ/DE/TEST heuristics (+ cty.dat check)
//     → CwSkimmerSpot
//
// CwSkimmerEngine is pure (no threads, no timers, sample-count clock) and is
// what tests/cw_skimmer_test.cpp drives.  CwSkimmer wraps it in a QObject
// worker on its own QThread — no DSP ever runs on the GUI thread.
//
// Why not one CwDecoder per carrier: CwDecoder spawns a thread per instance,
// expects 24 kHz stereo float audio and runs ggmorse's own pitch search;
// 16–32 of those is 16–32 threads of redundant FFT work.  Here the
// channelizer already knows each carrier's frequency, so a light timing
// decoder per channel keeps CPU bounded on a single worker thread.
// ─────────────────────────────────────────────────────────────────────────────

struct CwSkimmerSpot {
    double  freqHz{0.0};       // absolute RF (center + offset)
    double  offsetHz{0.0};     // relative to IQ center
    QString callsign;
    int     wpm{0};
    float   snrDb{0.0f};
    bool    isCq{false};
    QString dxccPrefix;        // cty.dat primary prefix ("" if cty not loaded)
    int     channelId{-1};
    double  timeSec{0.0};      // engine clock (seconds of IQ processed)
};

struct CwSkimmerChannelInfo {
    int     id{-1};
    double  offsetHz{0.0};
    double  freqHz{0.0};
    float   snrDb{0.0f};
    float   wpm{0.0f};
    QString text;              // running decoded text (tail)
    QString lastCall;          // last spotted callsign on this channel
};

struct CwSkimmerConfig {
    int    maxChannels{24};        // hard cap on simultaneously decoded carriers
    double onSnrDb{10.0};          // carrier detection threshold (per bin)
    double offSnrDb{5.0};          // carrier considered gone below this
    double minSeparationHz{80.0};  // closer peaks are merged into one carrier
    double idleTimeoutSec{20.0};   // drop a channel after this long below offSnrDb
    double respotIntervalSec{600.0};
    double dcGuardHz{150.0};       // ignore carriers this close to IQ center (DC spur)
    int    textTailChars{160};
};

class CwSkimmerEngine {
public:
    using SpotCallback = std::function<void(const CwSkimmerSpot&)>;
    using TextCallback = std::function<void(int channelId, double offsetHz, const QString& text)>;
    using RemovedCallback = std::function<void(int channelId)>;

    explicit CwSkimmerEngine(int sampleRate, CwSkimmerConfig cfg = {});
    ~CwSkimmerEngine();

    CwSkimmerEngine(const CwSkimmerEngine&) = delete;
    CwSkimmerEngine& operator=(const CwSkimmerEngine&) = delete;

    int sampleRate() const { return m_sampleRate; }
    const CwSkimmerConfig& config() const { return m_cfg; }

    void setCenterFrequencyHz(double hz) { m_centerHz = hz; }
    double centerFrequencyHz() const { return m_centerHz; }

    // Non-owning; must outlive the engine (or be reset to nullptr).
    void setCtyParser(const CtyDatParser* cty) { m_cty = cty; }

    void onSpot(SpotCallback cb) { m_onSpot = std::move(cb); }
    void onText(TextCallback cb) { m_onText = std::move(cb); }
    void onChannelRemoved(RemovedCallback cb) { m_onRemoved = std::move(cb); }

    // Feed native-endian complex samples.
    void process(const std::complex<float>* iq, std::size_t count);

    std::vector<CwSkimmerChannelInfo> channels() const;
    double timeSec() const { return static_cast<double>(m_samples) / m_sampleRate; }

    // Exposed for tests.
    static bool looksLikeCallsign(const QString& word);

private:
    class Channel;
    struct Candidate {
        double offsetHz{0.0};
        int    hits{0};
        int    lastFrame{0};
        float  snrDb{0.0f};
    };

    void runDetector();
    void drainChannels();
    void updateTracks(const std::vector<double>& peakHz, const std::vector<float>& peakSnr);
    float snrAtOffset(double offsetHz) const;
    void handleWord(Channel& ch, const QString& word);
    void flushWords(Channel& ch);
    void evaluateWord(Channel& ch, int index, const QString& next);

    int             m_sampleRate;
    CwSkimmerConfig m_cfg;
    double          m_centerHz{0.0};
    const CtyDatParser* m_cty{nullptr};

    // Detector
    int                 m_fftSize{4096};
    int                 m_hop{2048};
    std::vector<float>  m_window;
    std::vector<std::complex<float>> m_fftIn;
    std::vector<std::complex<float>> m_fftBuf;
    std::vector<std::complex<float>> m_twiddle;
    std::vector<int>    m_bitrev;
    std::vector<float>  m_avgPower;     // EMA of |X|^2 (fftshifted, bin 0 = -fs/2)
    std::vector<float>  m_heldPower;    // peak-hold with slow decay of m_avgPower
    float               m_noiseFloor{0.0f};
    int                 m_fftFill{0};
    int                 m_frame{0};
    std::vector<Candidate> m_candidates;

    // IQ pre-roll (kHistorySec) used to prime newly created channels.
    std::vector<std::complex<float>> m_history;
    std::size_t         m_historyPos{0};
    std::size_t         m_historyFilled{0};

    std::vector<std::unique_ptr<Channel>> m_channels;
    int           m_nextChannelId{1};
    std::uint64_t m_samples{0};

    SpotCallback    m_onSpot;
    TextCallback    m_onText;
    RemovedCallback m_onRemoved;
};

// ── Threaded wrapper ────────────────────────────────────────────────────────

class CwSkimmerWorker : public QObject {
    Q_OBJECT

public:
    explicit CwSkimmerWorker(QObject* parent = nullptr);
    ~CwSkimmerWorker() override;

    // Packets queued to this worker but not yet processed (backlog guard).
    std::atomic<int>& pendingCounter() { return m_pending; }

public slots:
    void configure(int maxChannels);
    void setSwapIq(bool swap);
    void loadCty(const QString& path);
    void setCenterFrequencyHz(double hz);
    // rawPayload: VITA-49 payload as emitted by PanadapterStream::iqDataReady
    // (big-endian float32 I/Q pairs).
    void processPacket(const QByteArray& rawPayload, int sampleRate);
    void reset();

signals:
    void spotDetected(const AetherSDR::CwSkimmerSpot& spot);
    void channelsUpdated(const QVector<AetherSDR::CwSkimmerChannelInfo>& channels);
    void channelRemoved(int channelId);

private:
    void ensureEngine(int sampleRate);

    std::unique_ptr<CwSkimmerEngine> m_engine;
    CtyDatParser m_cty;
    CwSkimmerConfig m_cfg;
    double m_centerHz{0.0};
    double m_lastPublishSec{0.0};
    bool m_swapIq{false};
    std::atomic<int> m_pending{0};
    std::vector<std::complex<float>> m_scratch;
};

// Owner / facade — lives on the GUI thread, owns the worker thread.
class CwSkimmer : public QObject {
    Q_OBJECT

public:
    explicit CwSkimmer(QObject* parent = nullptr);
    ~CwSkimmer() override;

    bool isRunning() const { return m_running.load(); }
    int  daxIqChannel() const { return m_daxIqChannel.load(); }
    int  backlog() const { return m_worker ? m_worker->pendingCounter().load() : 0; }

    // Max packets queued to the worker before new ones are dropped.  Keeps
    // latency and memory bounded if the worker falls behind at 192 kHz.
    static constexpr int kMaxBacklogPackets = 32;

public slots:
    void start();
    void stop();
    void setDaxIqChannel(int channel);          // 1-4
    void setCenterFrequencyMhz(double mhz);     // pan / DAX IQ center
    void setMaxChannels(int n);
    void setSwapIq(bool swap);                  // spectrum inversion fix-up
    void setCtyDatPath(const QString& path);    // default ":/cty.dat"

    // Thread-safe: connect PanadapterStream::iqDataReady with
    // Qt::DirectConnection so packets hop straight to the skimmer thread.
    void feedRawIqPacket(int channel, const QByteArray& rawPayload, int sampleRate);

signals:
    void spotDetected(const AetherSDR::CwSkimmerSpot& spot);
    void channelsUpdated(const QVector<AetherSDR::CwSkimmerChannelInfo>& channels);
    void channelRemoved(int channelId);
    void runningChanged(bool running);

private:
    QThread m_thread;
    CwSkimmerWorker* m_worker{nullptr};   // owned by m_thread (deleteLater on finish)
    std::atomic<bool> m_running{false};
    std::atomic<int>  m_daxIqChannel{1};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::CwSkimmerSpot)
Q_DECLARE_METATYPE(AetherSDR::CwSkimmerChannelInfo)
