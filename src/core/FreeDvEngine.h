#pragma once

#include "DigitalVoiceEngine.h"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <memory>
#include <vector>

struct freedv;
struct MODEM_STATS;

namespace AetherSDR {

class Resampler;

// Wraps codec2's freedv_api for the classic FreeDV voice modes
// (700D, 700E, 1600, 700C).  Same threading/signal shape as RADEEngine:
// constructed on the main thread, moved to a worker QThread, all slots
// invoked queued.
//
// RX: DAX (24k stereo f32) → mono → modem rate (8k) → freedv_rx()
//     → speech rate (8k) → 24k stereo f32 → rxSpeechReady
// TX: mic (24k stereo f32) → mono → speech rate → freedv_tx()
//     → modem rate → 24k stereo f32 → txModemReady
//
// Text channel: 700D/700E use codec2 reliable_text (FEC-protected callsign,
// what freedv-gui sends/decodes for these modes); 1600/700C use the plain
// varicode text callbacks (freedv_set_callback_txt).
//
// Without HAVE_CODEC2 the class compiles to a stub whose start() fails.
class FreeDvEngine : public DigitalVoiceEngine {
    Q_OBJECT

public:
    explicit FreeDvEngine(DigitalVoiceMode mode, QObject* parent = nullptr);
    ~FreeDvEngine() override;

    bool start() override;
    void stop() override;
    bool isActive() const override { return m_active.load(); }
    bool isSynced() const override { return m_synced.load(); }

    DigitalVoiceMode mode() const { return m_mode; }
    int modemSampleRate() const { return m_modemRate; }
    int speechSampleRate() const { return m_speechRate; }

    // Build-time availability check.
    static bool isSupported();

public slots:
    void feedRxAudio(int channel, const QByteArray& pcm) override;
    void feedTxAudio(const QByteArray& pcm) override;
    void resetTx() override;
    void setTxText(const QString& text) override;
    void setSquelch(bool enabled, float snrThresholdDb) override;

private:
#ifdef HAVE_CODEC2
    static void onReliableTextRx(void* rt, const char* txt, int length, void* state);
    static void onPlainTextRx(void* state, char c);
    static char onPlainTextTx(void* state);

    void applySquelch();
    void applyTxText();
    void publishStats(bool force);
    void emitText(const QString& text);

    struct freedv*                m_fdv{nullptr};
    void*                         m_reliableText{nullptr};  // reliable_text_t
    std::unique_ptr<MODEM_STATS>  m_stats;

    std::unique_ptr<Resampler> m_rxDown;   // 24k → modem rate
    std::unique_ptr<Resampler> m_rxUp;     // speech rate → 24k
    std::unique_ptr<Resampler> m_txDown;   // 24k → speech rate
    std::unique_ptr<Resampler> m_txUp;     // modem rate → 24k

    std::vector<float>   m_rxAccum;        // modem-rate mono float [-1,1]
    std::vector<float>   m_txAccum;        // speech-rate mono float [-1,1]
    std::vector<short>   m_demodIn;
    std::vector<short>   m_speechOut;
    std::vector<short>   m_speechIn;
    std::vector<short>   m_modOut;

    QByteArray m_txTextBytes;              // plain-text TX cycle (callsign + "\r")
    int        m_txTextPos{0};
    QByteArray m_rxTextLine;

    int  m_statsSampleCounter{0};
#endif
    DigitalVoiceMode  m_mode;
    int               m_modemRate{8000};
    int               m_speechRate{8000};
    QString           m_txText;
    bool              m_squelchEnabled{true};
    float             m_squelchThreshDb{2.0f};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_synced{false};
};

} // namespace AetherSDR
