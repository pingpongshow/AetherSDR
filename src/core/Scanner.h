#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class QTimer;

namespace AetherSDR {

struct BandDef;

// ─── Receive-only tuning target ──────────────────────────────────────────────
//
// The scanner drives one slice through this interface. It deliberately has NO
// transmit, MOX, tune-carrier or ATU method: the scanner cannot key the radio
// by construction. The GUI adapter (SliceScanTarget in ScannerPanel.cpp) wraps
// a SliceModel; tests use a fake.
class ScanTarget {
public:
    virtual ~ScanTarget() = default;

    virtual bool    isValid() const = 0;       // false once the slice is gone
    virtual int     sliceId() const = 0;
    virtual bool    isLocked() const = 0;
    virtual double  frequencyMhz() const = 0;
    virtual QString mode() const = 0;
    virtual int     filterLow() const = 0;
    virtual int     filterHigh() const = 0;

    virtual void tune(double mhz) = 0;
    virtual void setMode(const QString& mode) = 0;
    virtual void setFilter(int low, int high) = 0;
};

// One stop in a scan list. Empty mode / zero filter mean "leave as is".
struct ScanChannel {
    double  freqMhz{0.0};
    QString mode;
    int     filterLow{0};
    int     filterHigh{0};
    QString label;
    QString group;
    int     memoryIndex{-1};     // radio memory index, -1 for band/list channels

    bool hasFilter() const { return filterLow < filterHigh; }
};

// Activity-log entry emitted when the scanner stops on a signal.
struct ScanHit {
    int       id{0};             // monotonically increasing, for peak updates
    QDateTime timeUtc;
    double    freqMhz{0.0};
    QString   mode;
    QString   label;
    float     peakDbm{-150.0f};
    bool      priority{false};
};

// Memory / band / frequency-list scanner state machine.
//
// Radio-authoritative policy: the scanner only changes slice frequency, mode
// and filter while a user-started scan is running, and restores the slice's
// original frequency/mode/filter when the user stops the scan. It never
// persists radio state and never retunes while the radio is transmitting.
//
// Time comes from an injectable monotonic clock; tests drive tick() directly
// (useInternalTimer=false) with a virtual clock.
class Scanner : public QObject {
    Q_OBJECT

public:
    enum class State { Idle, Scanning, Stopped, Paused, PausedTx };
    Q_ENUM(State)

    enum class DetectSource { SMeter, Squelch, Either };
    Q_ENUM(DetectSource)

    // Carrier: resume once the signal has been gone for carrierDropMs.
    // Time:    resume resumeMs after stopping, even if the signal remains.
    // Hold:    stay on the channel until the user resumes / skips.
    enum class ResumeMode { Carrier, Time, Hold };
    Q_ENUM(ResumeMode)

    enum class MemorySelect { All, Group, Selected };
    Q_ENUM(MemorySelect)

    using Clock = std::function<qint64()>;   // monotonic milliseconds

    struct Config {
        int          dwellMs{400};           // time on a quiet channel
        int          settleMs{200};          // ignore meter/squelch right after a retune
        float        thresholdDbm{-100.0f};  // S-meter stop threshold
        float        hysteresisDb{3.0f};     // carrier-present hysteresis while stopped
        DetectSource detect{DetectSource::SMeter};
        ResumeMode   resume{ResumeMode::Time};
        int          resumeMs{5000};         // Time mode
        int          carrierDropMs{2000};    // Carrier mode
        bool         priorityEnabled{false};
        ScanChannel  priorityChannel;
        int          priorityIntervalMs{5000};
        int          priorityDwellMs{400};
        bool         resumeAfterTx{true};    // auto-resume when TX ends
    };

    static constexpr int kTickMs = 50;
    static constexpr int kMaxBandChannels = 20000;

    explicit Scanner(QObject* parent = nullptr, bool useInternalTimer = true);
    ~Scanner() override;

    static QString stateName(State s);

    void setClock(Clock clock);

    // ── Configuration ────────────────────────────────────────────────────────
    // Returns false (and does nothing) while a scan is running.
    bool setTarget(std::unique_ptr<ScanTarget> target);
    ScanTarget* target() const { return m_target.get(); }
    void setChannels(const QVector<ScanChannel>& channels);
    const QVector<ScanChannel>& channels() const { return m_channels; }
    void setConfig(const Config& cfg);   // may change while running
    const Config& config() const { return m_cfg; }

    // ── Skip (lockout) list, keyed by frequency in Hz ────────────────────────
    static qint64 channelKey(double mhz);
    void setSkipList(const QSet<qint64>& hz);
    QSet<qint64> skipList() const { return m_skip; }
    void addSkip(double mhz);
    void removeSkip(double mhz);
    void clearSkipList();
    bool isSkipped(double mhz) const { return m_skip.contains(channelKey(mhz)); }

    // ── Control (all user actions) ───────────────────────────────────────────
    bool start();                     // false + errorOccurred() when refused
    void stop(bool restore = true);   // user stop: restore original slice state
    void abort();                     // target vanished / disconnected: no restore
    void pause();
    void resume();
    void skipCurrent();               // lock out the current channel and move on
    // Stop scanning (no restore — the user picked a frequency) and tune there.
    bool tuneTo(const ScanChannel& ch);

    // ── Inputs ───────────────────────────────────────────────────────────────
    State state() const { return m_state; }
    bool  isRunning() const { return m_state != State::Idle; }
    bool  isCheckingPriority() const { return m_onPriority; }
    int   currentIndex() const { return m_index; }
    ScanChannel currentChannel() const;
    bool  txActive() const { return m_tx || m_mox; }

    // Drive the state machine (called by the internal timer, or by tests).
    void tick();

    // ── Channel list builders (pure helpers) ─────────────────────────────────
    static QVector<ScanChannel> bandChannels(double startMhz, double stopMhz,
                                             int stepHz, const QString& mode = {},
                                             int maxChannels = kMaxBandChannels);
    static const BandDef* bandForFrequency(double mhz);
    static QVector<ScanChannel> selectMemories(const QVector<ScanChannel>& all,
                                               MemorySelect select,
                                               const QString& group,
                                               const QSet<int>& indices);
    // Parse "14.074 DIGU FT8" lines (newline or ';' separated). Frequency is in
    // MHz unless suffixed with k/kHz. Unparseable entries go to *errors.
    static QVector<ScanChannel> parseFrequencyList(const QString& text,
                                                   QStringList* errors = nullptr);

public slots:
    void onSignalLevel(int sliceId, float dbm);   // MeterModel::sLevelChanged
    void setSquelchOpen(bool open);               // squelch-based detection input
    void setTransmitting(bool tx);                // RadioModel::radioTransmittingChanged
    void setMox(bool mox);                        // TransmitModel::moxChanged

signals:
    void stateChanged(AetherSDR::Scanner::State state);
    void channelChanged(int index, const AetherSDR::ScanChannel& channel, bool priority);
    void levelUpdated(float dbm);
    void hitAdded(const AetherSDR::ScanHit& hit);
    void hitPeakUpdated(int hitId, float peakDbm);
    void skipListChanged();
    void errorOccurred(const QString& message);
    void finished(bool restored);

private:
    qint64 now() const;
    void   setState(State s);
    void   applyChannel(const ScanChannel& ch);
    void   enterChannel(int index);
    int    nextEligible(int from) const;
    bool   advance();
    void   beginSettle();
    bool   detected() const;
    bool   present() const;
    void   stopOnSignal();
    void   resumeScan();
    void   startPriorityCheck();
    void   returnFromPriority();
    void   updateTx();
    void   restoreOriginal();
    void   finish(bool restore);

    std::unique_ptr<ScanTarget> m_target;
    QVector<ScanChannel> m_channels;
    Config               m_cfg;
    QSet<qint64>         m_skip;
    Clock                m_clock;
    QTimer*              m_timer{nullptr};

    State  m_state{State::Idle};
    State  m_stateBeforeTx{State::Idle};
    int    m_index{-1};
    qint64 m_channelStartMs{0};
    qint64 m_settleUntilMs{0};
    qint64 m_stopStartMs{0};
    qint64 m_lastPresentMs{0};
    qint64 m_lastPriorityMs{0};

    bool   m_levelValid{false};
    float  m_levelDbm{-150.0f};
    bool   m_squelchValid{false};
    bool   m_squelchOpen{false};

    bool   m_onPriority{false};
    State  m_priorityReturnState{State::Scanning};
    bool   m_stopIsPriority{false};

    int    m_nextHitId{1};
    int    m_currentHitId{0};
    float  m_currentPeak{-150.0f};

    bool   m_tx{false};
    bool   m_mox{false};
    bool   m_pendingRestore{false};

    struct Original {
        double  freqMhz{0.0};
        QString mode;
        int     filterLow{0};
        int     filterHigh{0};
    } m_original;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ScanChannel)
Q_DECLARE_METATYPE(AetherSDR::ScanHit)
