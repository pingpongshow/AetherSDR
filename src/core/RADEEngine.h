#pragma once

#include "DigitalVoiceEngine.h"

#include <QByteArray>
#include <QString>
#include <memory>

#ifdef HAVE_RADE
struct rade;
struct LPCNetEncState;
// FARGANState is defined in fargan.h (opus), included only in .cpp
// Use void* here to avoid pulling in opus headers
#endif

namespace AetherSDR {

class Resampler;

// Wraps the RADE v1 (Radio Autoencoder) codec for FreeDV digital voice.
// The radio is set to DIGU (SSB passthrough). RADE handles the encoding
// and decoding locally:
//
// TX: Mic(24kHz stereo) → 16kHz mono → LPCNet features → rade_tx() → RADE_COMP(8kHz) → 24kHz stereo → DAX TX
// RX: DAX RX(24kHz stereo) → 8kHz mono → RADE_COMP → rade_rx() → features → FARGAN → 16kHz mono → 24kHz stereo
//
// End of over (EOO): freedv-gui 2.x sends the station callsign in the EOO
// frame (RadeEooText).  RX: every EOO frame rade_rx() flags is decoded and a
// valid callsign is emitted through textReceived().  TX: setTxText() loads
// the callsign into rade_tx_set_eoo_bits(); requestEndOfOver() appends the
// EOO frame (rade_tx_eoo) plus a short silence tail, then endOfOverSent().
//
// Resamplers use Resampler::Profile::LowLatencyVoice (~16 ms per 8 kHz
// stage instead of ~297 ms with the high-quality r8brain defaults).
class RADEEngine : public DigitalVoiceEngine {
    Q_OBJECT

public:
    explicit RADEEngine(QObject* parent = nullptr);
    ~RADEEngine() override;

    bool start() override;
    void stop() override;
    bool isActive() const override;
    bool isSynced() const override;

    // Silence appended after the EOO frame so the TX upsampler's FIR tail
    // (and the DAX TX path) carries the whole EOO before unkey.
    static constexpr int kEooSilenceTailMs = 60;

    // Callsign as it will be transmitted in EOO (sanitised, <= 8 chars).
    QString txCallsign() const { return m_txCallsign; }

public slots:
    // Feed DAX RX audio (24kHz stereo float32) for decoding.  The caller
    // (MainWindow) forwards only the DV slice's DAX channel.
    void feedRxAudio(int channel, const QByteArray& pcm) override;

    // Feed mic audio (24kHz stereo float32) for encoding.
    void feedTxAudio(const QByteArray& pcm) override;

    // Flush TX encoder state (call on MOX release to prevent stale audio)
    void resetTx() override;

    // Station callsign sent in the EOO frame (freedv-gui "RADE callsign").
    void setTxText(const QString& text) override;

    // Emit the EOO frame now (see DigitalVoiceEngine::requestEndOfOver).
    void requestEndOfOver() override;

    // Signals (rxSpeechReady, txModemReady, syncChanged, snrChanged,
    // freqOffsetChanged, textReceived, endOfOverSent) are inherited.

private:
    QString m_txCallsign;
#ifdef HAVE_RADE
    void applyEooBits();

    struct rade*         m_rade{nullptr};
    LPCNetEncState*      m_lpcnetEnc{nullptr};
    void*                m_fargan{nullptr};  // actually FARGANState*, opaque here
    bool                 m_synced{false};
    bool                 m_eooSent{false};   // TX gated until resetTx()

    // TX accumulation: 12 frames of NB_TOTAL_FEATURES = 432 floats
    QByteArray m_txAccum;
    QByteArray m_txFeatAccum;

    // RX accumulation: rade_nin() RADE_COMP samples
    QByteArray m_rxAccum;
    QByteArray m_rxFeatAccum;
    QByteArray m_rxOutAccum;

    bool m_farganWarmedUp{false};

    // Resamplers (r8brain, low-latency voice profile)
    std::unique_ptr<Resampler> m_down24to8;   // 24k→8k (modem RX input)
    std::unique_ptr<Resampler> m_up8to24;     // 8k→24k (modem TX output)
    std::unique_ptr<Resampler> m_down24to16;  // 24k→16k (LPCNet TX input)
    std::unique_ptr<Resampler> m_up16to24;    // 16k→24k (FARGAN RX output)
#endif
};

} // namespace AetherSDR
