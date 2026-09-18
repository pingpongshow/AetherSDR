#pragma once

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <memory>

class QTimer;

namespace AetherSDR {

// Shared memory layout for one DAX audio channel.
// Used by both AetherSDR (writer) and the HAL plugin (reader).
// Sample rate matches FLEX-8600 fw v1.4.0.0 native DAX rate: 24 kHz stereo.
struct DaxShmBlock {
    std::atomic<uint32_t> writePos{0};
    std::atomic<uint32_t> readPos{0};
    uint32_t sampleRate{24000};
    uint32_t channels{2};           // stereo
    uint32_t active{0};             // 1 = AetherSDR is feeding data
    uint32_t reserved[3]{};
    // Ring buffer: 2 seconds @ 24kHz stereo = 96000 float samples
    static constexpr uint32_t RING_SIZE = 24000 * 2 * 2;
    float ringBuffer[RING_SIZE]{};
};

class DeviceMappedDax;
class PipeWireAudioBridge;

// Platform DAX audio bridge (the type MainWindow uses as DaxBridge).
//
// Two backends, chosen from AppSettings "DaxBackend" when constructed:
//
//  Native        macOS: POSIX shared memory → bundled CoreAudio HAL plug-in.
//                  Creates 4 RX segments (/aethersdr-dax-1 … -4) for radio →
//                  apps and 1 TX segment (/aethersdr-dax-tx) for apps → radio.
//                Linux (HAVE_PIPEWIRE): delegates to PipeWireAudioBridge
//                  (PulseAudio pipe modules), unchanged.
//                Windows: not available (no signed WDM driver).
//  DeviceMapped  DeviceMappedDax: DAX RX 1–4 played to user-selected output
//                  devices, DAX TX captured from a user-selected input device
//                  (VB-Cable, VAC, Voicemeeter, BlackHole, Loopback …).
//
// Default: Native on macOS/Linux, DeviceMapped on Windows.
//
// Public API and data formats are the same for every backend:
//   feedDaxAudio(): float32 stereo interleaved, 24 kHz
//   txAudioReady(): float32 stereo interleaved, 24 kHz
class VirtualAudioBridge : public QObject {
    Q_OBJECT

public:
    static constexpr int NUM_CHANNELS = 4;

    enum class Backend { Native, DeviceMapped };

    // AppSettings key and values: DaxBackend = "Native" | "DeviceMapped".
    static QString backendSettingKey() { return QStringLiteral("DaxBackend"); }
    static QString backendName(Backend b);
    static Backend backendFromName(const QString& name, Backend fallback);
    static bool nativeBackendAvailable();
    static Backend defaultBackend();
    // Backend the next bridge will use (setting, else platform default;
    // Native is coerced to DeviceMapped where it is unavailable).
    static Backend configuredBackend();

    explicit VirtualAudioBridge(QObject* parent = nullptr);   // configuredBackend()
    VirtualAudioBridge(Backend backend, QObject* parent);
    ~VirtualAudioBridge() override;

    bool open();
    void close();
    bool isOpen() const { return m_open; }

    Backend backend() const { return m_backend; }
    // Human-readable reason the last open() failed or partially failed.
    QString lastError() const { return m_lastError; }
    // Non-null only for the DeviceMapped backend while open.
    DeviceMappedDax* deviceMapped() const { return m_mapped.get(); }

    // DAX output gain (0.0–1.0). Default 0.25 (≈ -12 dB) to avoid
    // overloading digital-mode software like WSJT-X.
    void setGain(float g);
    void setChannelGain(int channel, float g);
    void setTxGain(float g);
    float gain() const { return m_gain; }

    // Read TX audio from shared memory (apps → radio).  macOS Native only;
    // other backends push TX through txAudioReady and return empty here.
    // Returns float32 stereo PCM, or empty if no data available.
    QByteArray readTxAudio(int maxFrames = 480);

public slots:
    // Feed decoded DAX audio for a channel (1-4).
    // pcm format: float32 stereo, 24 kHz (PanadapterStream DAX output).
    void feedDaxAudio(int channel, const QByteArray& pcm);

    // When transmitting, feed silence to all RX channels so apps don't starve.
    void setTransmitting(bool tx);

    // DeviceMapped: re-read DaxRxDevice1..4 / DaxTxDevice and reopen the
    // devices that changed.  No-op for the Native backend.
    void reloadDeviceMapping();

signals:
    void txAudioReady(const QByteArray& pcm);
    void daxRxLevel(int channel, float rms);
    void daxTxLevel(float rms);

private:
    bool openSharedMemory();    // macOS Native
    bool openDelegate();        // DeviceMapped, or Linux Native (PipeWire)
    void closeSharedMemory();

    Backend m_backend{Backend::Native};
    QString m_lastError;
    std::unique_ptr<DeviceMappedDax> m_mapped;
    // Linux Native delegate.  Declared unconditionally (Qt parent-owned raw
    // pointer) so the class layout never depends on HAVE_PIPEWIRE.
    PipeWireAudioBridge* m_pipewire{nullptr};

    bool m_open{false};
    float m_gain{0.5f};  // -6 dB default
    float m_channelGain[NUM_CHANNELS]{0.5f, 0.5f, 0.5f, 0.5f};
    float m_txGain{0.5f};

    // RX channels (radio → apps)
    int  m_shmFds[NUM_CHANNELS]{-1, -1, -1, -1};
    DaxShmBlock* m_blocks[NUM_CHANNELS]{};

    // TX channel (apps → radio)
    int m_txShmFd{-1};
    DaxShmBlock* m_txBlock{nullptr};
    ::QTimer* m_txPollTimer{nullptr};

    void feedSilenceToAllChannels();
    static QString shmName(int channel);

    bool     m_transmitting{false};
    QTimer*  m_silenceTimer{nullptr};
    QElapsedTimer m_silenceElapsed;   // tracks wall-clock time for accurate silence fill
    qint64   m_silenceFramesWritten{0};  // stereo frames fed since m_silenceElapsed started
};

} // namespace AetherSDR
