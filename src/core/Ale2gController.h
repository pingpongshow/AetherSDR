#pragma once

// Client-side 2G ALE link controller (MIL-STD-188-141B App. A, A.5.3-A.5.5,
// A.5.7.2) for amateur HFLink / PCALE / MARS-ALE interoperation.
//
// The controller never touches the radio directly.  It drives the outside
// world only through signals, which the owner wires to the slice / transmit
// model / audio engine:
//   requestTune(frequencyHz)   - tune the ALE slice (USB/DIGU dial frequency)
//   requestTransmit(bool)      - PTT (MOX) on/off
//   txAudio(pcm)               - 24 kHz stereo float32 modem audio, paced in
//                                real time while keyed
// and the owner feeds back:
//   feedRxAudio(pcm)           - 24 kHz stereo float32 receive audio
//   setRadioTransmitting(bool) - interlock / TX state from the radio
//
// SAFETY: transmit is disabled by default.  No PTT request and no txAudio is
// ever emitted unless setTransmitEnabled(true) was called by the user.  Any
// error (key confirmation timeout, radio dropping TX, watchdog, TX disable)
// immediately emits requestTransmit(false).

#include "Ale2gFrame.h"
#include "Ale2gModem.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>
#include <vector>

class QTimer;

namespace AetherSDR {

struct Ale2gChannel {
    QString name;
    qint64 frequencyHz{0};    // USB dial (suppressed carrier) frequency
    bool sound{false};        // include in scheduled sounding ("pilot" channel)
};

struct Ale2gHeardStation {
    QString address;
    int channelIndex{-1};
    qint64 frequencyHz{0};
    QDateTime lastHeardUtc;
    float snrDb{0.0f};
    int berCode{48};          // mean non-unanimous votes (0 best .. 48 worst)
    int count{0};
    QString lastKind;         // "TWAS sound", "call", ...
};

class Ale2gController : public QObject {
    Q_OBJECT

public:
    enum class State { Idle, Scanning, Sounding, Calling, Responding, Linked };
    Q_ENUM(State)

    using Clock = std::function<qint64()>;   // monotonic milliseconds

    // useInternalTimer=false lets tests drive tick() with a fake clock.
    explicit Ale2gController(QObject* parent = nullptr, bool useInternalTimer = true);
    ~Ale2gController() override;

    static QString stateName(State s);

    void setClock(Clock clock);

    // ── Configuration ───────────────────────────────────────────────────────
    void setSelfAddresses(const QStringList& addresses);
    QStringList selfAddresses() const { return m_selfAddresses; }
    void setChannels(const QVector<Ale2gChannel>& channels);
    const QVector<Ale2gChannel>& channels() const { return m_channels; }
    void setDwellMs(int ms);
    int dwellMs() const { return m_dwellMs; }
    void setScanCallMs(int ms);            // 0 = auto from channel count x dwell
    int scanCallMs() const { return m_scanCallMs; }
    void setSoundLengthMs(int ms);         // 0 = auto; capped at 30 s (HFLink)
    void setSoundingIntervalMinutes(int minutes);   // 0 = off, else >= 30
    int soundingIntervalMinutes() const { return m_soundIntervalMin; }
    void setSoundWithTis(bool tis);        // default TWAS (HFLink)
    void setListenBeforeTransmitMs(int ms);
    void setReplyTimeoutMs(int ms);        // Twrt: start of reply after our TX
    void setLinkTimeoutMs(int ms);         // Twa: 0 = never (HFLink 360 s)
    void setMaxCallAttempts(int attempts);
    void setRespondToCalls(bool respond);
    void setAcceptAllCalls(bool accept);
    void setAcceptAnyCalls(bool accept);
    void setTxAmplitude(float amplitude);
    void setRequireTxConfirmation(bool require);
    void setKeyTimeoutMs(int ms);
    void setPttDelayMs(int ms);
    void setTxTailMs(int ms);
    void setMaxTransmitMs(int ms);
    void setTuneSettleMs(int ms);

    // ── Master transmit switch (default OFF) ────────────────────────────────
    void setTransmitEnabled(bool enabled);
    bool transmitEnabled() const { return m_txEnabled; }

    // ── Status ──────────────────────────────────────────────────────────────
    State state() const { return m_state; }
    QString remoteAddress() const { return m_remote; }
    bool isRunning() const { return m_running; }
    bool isKeyed() const { return m_keyRequested; }
    int currentChannelIndex() const { return m_channelIndex; }
    const QVector<Ale2gHeardStation>& heardStations() const { return m_heard; }
    void clearHeardStations();

public slots:
    void start();                          // available state: scan (or listen)
    void stop();                           // idle; aborts TX
    bool soundNow();
    bool call(const QString& address, const QString& amdText = QString());
    bool allCall(const QString& amdText = QString());
    bool sendAmd(const QString& text);     // within an established link
    void terminateLink();
    void abortTransmit(const QString& reason = QString());

    void feedRxAudio(const QByteArray& pcm24kStereoFloat32);
    void feedRxMono(const float* samples, int count);
    void setRadioTransmitting(bool transmitting);
    void tick();

signals:
    void requestTune(qint64 frequencyHz);
    void requestTransmit(bool on);
    void txAudio(const QByteArray& pcm24kStereoFloat32);

    void stateChanged(AetherSDR::Ale2gController::State state, const QString& remote);
    void channelChanged(int index, qint64 frequencyHz);
    void wordReceived(const QString& text, int nonUnanimous, float snrDb);
    void frameReceived(const QString& summary);
    void amdReceived(const QString& from, const QString& text);
    void heardStationsChanged();
    void linkEstablished(const QString& remote);
    void linkTerminated(const QString& remote, const QString& reason);
    void logMessage(const QString& text);
    void errorOccurred(const QString& text);
    void transmitActiveChanged(bool active);

private:
    enum class JobKind { Sound, Call, AllCall, Response, Ack, Amd, Terminate };
    enum class TxPhase { None, Pending, Tuning, Listening, WaitKey, PreAudio, Sending };

    struct TxJob {
        JobKind kind{JobKind::Sound};
        QVector<std::uint32_t> words;
        QString remote;
        int channelIndex{-1};              // -1: stay on current frequency
        int lbtMs{0};
        qint64 notBeforeMs{0};
        QString description;
    };

    qint64 now() const;
    void setState(State s, const QString& remote = QString());
    void resumeAvailable();
    void log(const QString& text);
    void fail(const QString& text);

    // Transmit machinery
    bool queueJob(TxJob job);
    void processTx(qint64 t);
    void startKeying(qint64 t);
    void finishJob(qint64 t);
    void jobBusy(qint64 t);
    void emergencyUnkey(const QString& reason);
    bool radioBusyWithForeignTx(qint64 t) const;
    void abandonJobRadioBusy();

    // Receive
    void handleDemodEvents(const std::vector<Ale2g::Demodulator::Event>& events);
    void handleFrame(const Ale2g::Frame& frame);
    void updateHeard(const Ale2g::Frame& frame, const QString& kind);
    QString matchSelf(const QStringList& toAddresses) const;
    bool rxActive() const;

    // Scanning / sounding / calling helpers
    void tuneToChannel(int index);
    void scanTick(qint64 t);
    void startScheduledSounding(qint64 t);
    bool queueNextSound();
    bool queueNextCallAttempt();
    int scanWordsForCall() const;
    int soundWords() const;
    QVector<int> callChannelOrder(const QString& address) const;

    QTimer* m_timer{nullptr};
    Clock m_clock;

    // Configuration
    QStringList m_selfAddresses;
    QVector<Ale2gChannel> m_channels;
    int m_dwellMs{500};
    int m_scanCallMs{0};
    int m_soundLengthMs{0};
    int m_soundIntervalMin{0};
    bool m_soundTis{false};
    int m_lbtMs{2000};
    int m_replyTimeoutMs{6000};
    int m_linkTimeoutMs{360000};
    int m_maxCallAttempts{1};
    bool m_respondToCalls{true};
    bool m_acceptAllCalls{true};
    bool m_acceptAnyCalls{true};
    float m_txAmplitude{0.5f};
    bool m_requireTxConfirm{true};
    int m_keyTimeoutMs{2500};
    int m_pttDelayMs{100};
    int m_txTailMs{150};
    int m_maxTransmitMs{60000};
    int m_tuneSettleMs{250};

    // State
    bool m_txEnabled{false};
    bool m_running{false};
    State m_state{State::Idle};
    QString m_remote;
    QString m_linkSelf;                    // self address used in current exchange
    int m_channelIndex{-1};
    qint64 m_dwellEndMs{0};
    qint64 m_holdUntilMs{0};
    qint64 m_deadlineMs{0};                // reply / ack deadline
    qint64 m_lastActivityMs{0};
    qint64 m_nextSoundMs{0};
    QVector<int> m_soundQueue;
    QVector<int> m_callQueue;
    QString m_callAddress;
    QString m_callAmd;
    bool m_callIsAllCall{false};

    // Transmit
    TxPhase m_txPhase{TxPhase::None};
    TxJob m_job;
    bool m_keyRequested{false};
    bool m_radioTransmitting{false};
    qint64 m_phaseDeadlineMs{0};
    qint64 m_keyRequestMs{0};
    qint64 m_lastUnkeyMs{-3600000};        // last time we released our own PTT
    qint64 m_txStartMs{0};
    qint64 m_rxBlankUntilMs{0};
    qint64 m_lastRxWordMs{0};
    std::vector<float> m_txSamples;
    size_t m_txPos{0};

    // Receive
    Ale2g::Demodulator m_demod;
    Ale2g::FrameAssembler m_assembler;
    QVector<Ale2gHeardStation> m_heard;
};

} // namespace AetherSDR
