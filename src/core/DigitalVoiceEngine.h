#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

namespace AetherSDR {

// Digital voice modes a slice can run.  Exactly one engine is active per
// application (one slice at a time), selected by this enum.
enum class DigitalVoiceMode {
    None = 0,
    RadeV1,       // FreeDV RADE V1 (bundled third_party/radae, HAVE_RADE)
    FreeDv700D,   // codec2 OFDM 700D (HAVE_CODEC2)
    FreeDv700E,   // codec2 OFDM 700E (HAVE_CODEC2)
    FreeDv1600,   // codec2 FDMDV 1600 (HAVE_CODEC2)
    FreeDv700C,   // codec2 COHPSK 700C (HAVE_CODEC2)
};

// Human-readable name shown in the UI and VFO indicator.
inline QString digitalVoiceModeName(DigitalVoiceMode m)
{
    switch (m) {
    case DigitalVoiceMode::RadeV1:     return QStringLiteral("RADE V1");
    case DigitalVoiceMode::FreeDv700D: return QStringLiteral("700D");
    case DigitalVoiceMode::FreeDv700E: return QStringLiteral("700E");
    case DigitalVoiceMode::FreeDv1600: return QStringLiteral("1600");
    case DigitalVoiceMode::FreeDv700C: return QStringLiteral("700C");
    case DigitalVoiceMode::None:       break;
    }
    return QString();
}

// Short tag for the VFO status label (kept "RADE" for RADE so the existing
// indicator text is unchanged).
inline QString digitalVoiceModeTag(DigitalVoiceMode m)
{
    return m == DigitalVoiceMode::RadeV1 ? QStringLiteral("RADE") : digitalVoiceModeName(m);
}

// Mode string as used by freedv-gui on FreeDV Reporter (transmit/rx_report
// "mode" field).  freedv-gui src/main.cpp uses "RADEV1" for RADE and the bare
// codec2 mode name ("700D", "700E", "1600", "700C") otherwise.
inline QString digitalVoiceReporterModeName(DigitalVoiceMode m)
{
    return m == DigitalVoiceMode::RadeV1 ? QStringLiteral("RADEV1") : digitalVoiceModeName(m);
}

// Stable settings token (AppSettings "DigitalVoiceMode").
inline QString digitalVoiceModeKey(DigitalVoiceMode m)
{
    switch (m) {
    case DigitalVoiceMode::RadeV1:     return QStringLiteral("RADEV1");
    case DigitalVoiceMode::FreeDv700D: return QStringLiteral("700D");
    case DigitalVoiceMode::FreeDv700E: return QStringLiteral("700E");
    case DigitalVoiceMode::FreeDv1600: return QStringLiteral("1600");
    case DigitalVoiceMode::FreeDv700C: return QStringLiteral("700C");
    case DigitalVoiceMode::None:       break;
    }
    return QString();
}

inline DigitalVoiceMode digitalVoiceModeFromKey(const QString& key)
{
    for (DigitalVoiceMode m : {DigitalVoiceMode::RadeV1, DigitalVoiceMode::FreeDv700D,
                               DigitalVoiceMode::FreeDv700E, DigitalVoiceMode::FreeDv1600,
                               DigitalVoiceMode::FreeDv700C}) {
        if (digitalVoiceModeKey(m) == key) {
            return m;
        }
    }
    return DigitalVoiceMode::None;
}

inline bool isCodec2Mode(DigitalVoiceMode m)
{
    return m == DigitalVoiceMode::FreeDv700D || m == DigitalVoiceMode::FreeDv700E
        || m == DigitalVoiceMode::FreeDv1600 || m == DigitalVoiceMode::FreeDv700C;
}

// Modes compiled into this build, in UI order.
inline QList<DigitalVoiceMode> availableDigitalVoiceModes()
{
    QList<DigitalVoiceMode> modes;
#ifdef HAVE_RADE
    modes.append(DigitalVoiceMode::RadeV1);
#endif
#ifdef HAVE_CODEC2
    modes.append(DigitalVoiceMode::FreeDv700E);
    modes.append(DigitalVoiceMode::FreeDv700D);
    modes.append(DigitalVoiceMode::FreeDv1600);
    modes.append(DigitalVoiceMode::FreeDv700C);
#endif
    return modes;
}

// Common interface for digital voice codecs (RADEEngine, FreeDvEngine).
// Instances live on their own worker QThread; every slot is invoked queued
// from the main/audio threads.  Audio format on both sides is 24 kHz stereo
// float32 (what AudioEngine::txRawPcmReady emits, what DAX RX delivers and
// what AudioEngine::feedDecodedSpeech / sendModemTxAudio consume).
class DigitalVoiceEngine : public QObject {
    Q_OBJECT

public:
    explicit DigitalVoiceEngine(QObject* parent = nullptr) : QObject(parent) {}
    ~DigitalVoiceEngine() override = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isActive() const = 0;
    virtual bool isSynced() const = 0;

public slots:
    // DAX RX audio for the digital voice slice (24 kHz stereo float32).
    virtual void feedRxAudio(int channel, const QByteArray& pcm) = 0;
    // Mic audio (24 kHz stereo float32).
    virtual void feedTxAudio(const QByteArray& pcm) = 0;
    // Flush TX state (MOX release).
    virtual void resetTx() = 0;
    // Callsign / free text sent in the modem text channel.  RADE V1 carries
    // it (max 8 chars, see RadeEooText) in the End-of-Over frame.
    virtual void setTxText(const QString& text) { Q_UNUSED(text); }
    // End of transmission requested (PTT release).  Engines that send an
    // end-of-over marker (RADE V1 EOO) emit the remaining modem audio via
    // txModemReady and then endOfOverSent(); the caller must keep the radio
    // keyed until then.  Default: nothing to send, signal immediately.
    virtual void requestEndOfOver() { emit endOfOverSent(); }
    // Decoder squelch.  No-op for RADE.
    virtual void setSquelch(bool enabled, float snrThresholdDb)
    {
        Q_UNUSED(enabled);
        Q_UNUSED(snrThresholdDb);
    }

signals:
    void rxSpeechReady(const QByteArray& pcm);   // decoded speech, 24 kHz stereo float32
    void txModemReady(const QByteArray& pcm);    // modem audio, 24 kHz stereo float32
    void syncChanged(bool synced);
    void snrChanged(float snrDb);
    void freqOffsetChanged(float hz);
    void textReceived(const QString& text);      // decoded text-channel string (callsign)
    void endOfOverSent();                        // all end-of-over modem audio emitted
};

} // namespace AetherSDR
