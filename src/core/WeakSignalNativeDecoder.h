#pragma once

#include "Resampler.h"
#include "WeakSignalDecode.h"

#include <QHash>
#include <QObject>

#include <array>
#include <vector>

namespace AetherSDR {

class WeakSignalNativeDecoder : public QObject {
    Q_OBJECT

public:
    enum class Mode {
        FT8 = 0,
        FT4 = 1,
        JT65 = 2,
        WSPR = 3
    };

    explicit WeakSignalNativeDecoder(QObject* parent = nullptr);
    ~WeakSignalNativeDecoder() override;

    bool isListening() const { return m_running; }

    void startListening();
    void stopListening();

    void setMode(Mode mode);
    void setDialFrequencyKhz(double khz);

    void feedAudio(const QByteArray& pcm24kStereo);
    void requestReplay();
    void requestClear(quint8 window = 0);

    // Exposed for ft8_lib's hash callback bridge.
    bool lookupHash(int hashType, quint32 hash, char* callsign);
    void saveHash(const char* callsign, quint32 hash);

signals:
    void listening();
    void stopped();
    void decodeReceived(const AetherSDR::WeakSignalDecode& decode);
    void cleared(quint8 window);
    void statusMessage(const QString& text);

private:
    struct HashEntry {
        char callsign[12]{};
        quint32 hash{0};
    };

    void resetCaptureState();
    void appendAudio(const QByteArray& pcm24kStereo);
    void pruneAudioBuffer();
    std::vector<float> audioWindowForTimes(qint64 startMs, qint64 endMs) const;

    void maybeDecode();
    void decodeLatestCompletedFtxSlot(bool force);
    void decodeLatestCompletedJt65Slot(bool force);
    void decodeLatestCompletedWsprSlot(bool force);
    void emitFtxDecodes(const std::vector<float>& samples, qint64 slotStartMs, Mode mode);
    void emitJt65Decodes(const std::vector<float>& samples, qint64 slotStartMs);
    void emitWsprDecodes(const std::vector<float>& samples, qint64 slotStartMs);

    QString extractCallsign(const QString& message) const;
    QString extractGrid(const QString& message) const;

    void cleanupHashTable(quint8 maxAge);

    bool hasSeenDecodeKey(const QString& key, qint64 timestampMs) const;
    void rememberDecodeKey(const QString& key, qint64 timestampMs);
    void pruneRecentDecodeKeys(qint64 nowMs);

    Mode m_mode{Mode::FT8};
    double m_dialFrequencyKhz{14074.0};
    bool m_running{false};
    Resampler m_resampler;
    std::vector<float> m_audio12k;
    double m_bufferEndTimeMs{0.0};
    qint64 m_lastFtxSlotStartMs{-1};
    qint64 m_lastJt65SlotStartMs{-1};
    qint64 m_lastWsprSlotStartMs{-1};
    qint64 m_lastJt65NoticeMs{0};
    std::array<HashEntry, 256> m_hashTable{};
    QHash<QString, qint64> m_recentDecodeKeys;
};

} // namespace AetherSDR
