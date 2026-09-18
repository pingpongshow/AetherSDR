#pragma once
#ifdef HAVE_WEBSOCKETS

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <memory>

class QWebSocketServer;
class QWebSocket;
class QTimer;

namespace AetherSDR {

class RadioModel;
class AudioEngine;
class SliceModel;
class TciProtocol;
class Resampler;

// TCI WebSocket server — exposes radio state and audio over the TCI protocol.
// Phase 1: text commands (VFO, mode, filter, TX, RIT/XIT, CW, spots)
// Phase 2: binary RX/TX audio streaming
class TciServer : public QObject {
    Q_OBJECT

public:
    explicit TciServer(RadioModel* model, QObject* parent = nullptr);
    ~TciServer() override;

    bool start(quint16 port = 50001);
    void stop();

    bool isRunning() const;
    quint16 port() const;
    int clientCount() const { return m_clients.size(); }

    void setAudioEngine(AudioEngine* audio) { m_audio = audio; }

    // TCI TX gain (0.0–1.0). Applied to outbound TX audio from WSJT-X/JTDX
    // before the radio.  Decoupled from DaxTxGain (#1627) — the DAX bridge
    // and TCI maintain independent gain settings.  Persists to TciTxGain.
    void setTxGain(float gain);
    float txGain() const { return m_txGain; }

    // Per-channel TCI RX gain (0.0–1.0), applied to outbound DAX audio before
    // resampling and sending to TCI clients.  Decoupled from DaxRxGain<n> so
    // DAX bridge and TCI maintain independent per-channel gains.
    // Channel is 1-based (1–4).  Persists to TciRxGain<channel>.
    void setRxChannelGain(int channel, float gain);
    float rxChannelGain(int channel) const;

    // Wire slice signals for state change broadcasts
    void wireSlice(int trx, SliceModel* slice);
    void wireSpotModel();

public slots:
    // RX audio from main audio pipeline (float32 stereo, 24 kHz)
    void onRxAudioReady(const QByteArray& pcm);
    // RX audio from DAX pipeline (float32 stereo, 24 kHz)
    void onDaxAudioReady(int channel, const QByteArray& pcm);
    // IQ data from DAX IQ stream (big-endian float32 I/Q pairs)
    void onIqDataReady(int channel, const QByteArray& rawPayload, int sampleRate);

signals:
    void clientCountChanged(int count);
    void rxLevel(int channel, float rms);  // 1-based channel, RMS of TCI-gained RX audio
    void txLevel(float rms);                // RMS of post-gain TCI TX audio

private slots:
    void onNewConnection();
    void onClientDisconnected();
    void onTextMessage(const QString& msg);
    void onBinaryMessage(const QByteArray& data);
    void broadcastStatus();

private:
    void sendInitBurst(QWebSocket* client);
    void broadcast(const QString& msg);
    // Broadcast msg only if it differs from the last notification sent under
    // key. Slice/transmit status signals re-fire on every radio status line
    // containing the key, so this keeps GUI-originated sync from storming.
    void broadcastIfChanged(const QString& key, const QString& msg);
    void broadcastSplitState();
    void broadcastDriveState();
    void broadcastBinary(const QByteArray& data);
    void startTxChrono(QWebSocket* client, int trx);
    void stopTxChrono();
    void sendTxChronoFrame(QWebSocket* client);
    void logTxAudioSummary(const char* reason);

    // Build a TCI binary audio frame (64-byte header + float32 samples)
    static QByteArray buildAudioFrame(int receiver, int type,
                                      int sampleRate, int channels,
                                      const float* samples, int sampleCount);

    struct ClientState {
        QWebSocket*  socket{nullptr};
        TciProtocol* protocol{nullptr};
        bool         audioEnabled{false};   // client sent AUDIO_START
        int          audioSampleRate{48000}; // requested output rate (48kHz for WSJT-X compat)
        int          audioChannels{2};       // 1=mono, 2=stereo
        int          audioFormat{3};         // 0=int16, 3=float32
        // RX resamplers (24 kHz → audioSampleRate), one per DAX channel plus
        // kMainRxResamplerKey for onRxAudioReady. r8brain keeps filter history
        // between calls, so sharing one instance across channels bled each
        // slice's delayed tail into the others. Empty when rate == 24000.
        // shared_ptr because ClientState is copied into a QList.
        QHash<int, std::shared_ptr<Resampler>> rxResamplers;
        // Per-DAX-channel accumulation buffers. Concatenating multi-channel
        // packets into a shared buffer would interleave audio from different
        // slices and destroy the resampler output, so each channel maintains
        // its own staging area. QHash over QMap: channel count is tiny (1-4)
        // and we never iterate in key order.
        QHash<int, QByteArray> rxAccumBuf;
        bool         rxSensorsEnabled{false};
        bool         txSensorsEnabled{false};
        bool         iqEnabled{false};       // client sent IQ_START
        int          iqChannel{0};           // TCI TRX → DAX IQ channel (0-based)
    };

    // Minimum frames to accumulate before flushing to r8brain.
    // ~21ms at 24kHz — large enough for clean resampling, small enough
    // for acceptable latency in digital modes.
    static constexpr int kAccumMinFrames = 512;
    static constexpr int kMainRxResamplerKey = -1;

    // Returns the RX resampler for this client/channel, creating it lazily.
    // nullptr when the client uses the native 24 kHz rate.
    static Resampler* rxResamplerFor(ClientState& cs, int key);

    void ensureDaxForTci();
    void releaseDaxForTci();

    RadioModel*       m_model;
    AudioEngine*      m_audio{nullptr};
    QWebSocketServer* m_server{nullptr};
    QList<ClientState> m_clients;
    // Per-slice connection context: deleting it drops that slice's TCI
    // broadcast connections, so re-wiring a slice never duplicates them.
    QHash<SliceModel*, QObject*> m_sliceContexts;
    // Last state notification sent per key (e.g. "agc_mode:0"). Cleared when
    // a client connects, since the init burst resynchronises everyone.
    QHash<QString, QString> m_lastNotified;
    QSet<int>         m_tciDaxSlices;   // slice IDs where we auto-assigned DAX (#1331)
    QMap<int, quint32> m_tciDaxStreamIds;      // DAX channel → stream ID created or borrowed by TCI
    QSet<int>          m_tciDaxBorrowedChannels; // channels where TCI reused an existing stream
    QTimer*           m_meterTimer{nullptr};  // 200ms status broadcast
    QTimer*           m_txChronoTimer{nullptr}; // TX_CHRONO frame cadence
    QWebSocket*       m_txChronoClient{nullptr};
    int               m_txChronoTrx{0};
    std::unique_ptr<Resampler> m_txResampler; // 48kHz→24kHz TX downsampler
    QElapsedTimer     m_txChronoClock;
    QElapsedTimer     m_txChronoSessionClock;
    qint64            m_txChronoAccumNs{0};
    qint64            m_txChronoRequestedFrames{0};
    bool              m_txUseRadioRoute{true};
    float             m_txGain{1.0f};
    float             m_rxChannelGain[4]{1.0f, 1.0f, 1.0f, 1.0f};
    qint64            m_txAudioBlocks{0};
    qint64            m_txInputFrames{0};
    qint64            m_txOutputFrames{0};
    qint64            m_txClipSamples{0};
    qint64            m_txAudioSampleCount{0};
    double            m_txAudioSumSq{0.0};
    float             m_txAudioPeak{0.0f};
    bool              m_txSawDuplicatedStereo{false};
    bool              m_lastTx{false};
    float             m_cachedSLevel[8]{-130,-130,-130,-130,-130,-130,-130,-130};
    float             m_cachedFwdPower{0};
    float             m_cachedSwr{1.0f};
    float             m_cachedMicLevel{-50.0f};
};

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
