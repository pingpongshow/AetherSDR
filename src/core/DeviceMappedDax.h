#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <array>
#include <memory>

class QMediaDevices;
class QTimer;

namespace AetherSDR {

// "Device-mapped" DAX backend.
//
// Instead of a bundled virtual audio driver (macOS HAL plug-in) or
// PulseAudio pipe modules (Linux), DAX RX 1–4 are played to user-selected
// output devices and DAX TX is captured from a user-selected input device.
// Intended for third-party virtual cables (VB-Audio Virtual Cable, VAC,
// Voicemeeter, BlackHole, Loopback): AetherSDR plays into "CABLE Input",
// WSJT-X/VARA/fldigi record from "CABLE Output", and vice versa for TX.
//
// This is the default (and only) DAX backend on Windows, where a signed
// WDM driver is not available; on macOS/Linux it is an opt-in fallback
// (AppSettings DaxBackend=DeviceMapped).
//
// Formats (identical to what the native bridges receive/emit):
//   feedDaxAudio(): float32 stereo interleaved, 24 kHz (PanadapterStream DAX)
//   txAudioReady(): float32 stereo interleaved, 24 kHz (AudioEngine::feedDaxTxAudio)
// Each device runs at its own preferred rate / channel count / sample
// format; conversion, resampling and clock-drift compensation happen here.
//
// Threading: lives on the GUI thread like the other DAX bridges.  RX data is
// handed to each QAudioSink through a lock-free SPSC playout buffer, so it
// is safe whichever thread the Qt backend pulls from.
//
// Settings (AppSettings):
//   DaxRxDevice1..4  output device description, empty = channel unmapped
//   DaxTxDevice      input device description, empty = DAX TX unmapped
class DeviceMappedDax : public QObject {
    Q_OBJECT

public:
    static constexpr int NUM_CHANNELS = 4;
    static constexpr int kDaxSampleRate = 24000;  // radio-native DAX rate (fw v1.4.0.0+)

    explicit DeviceMappedDax(QObject* parent = nullptr);
    ~DeviceMappedDax() override;

    // Opens every mapped device.  Returns false (with lastError()) only when
    // nothing at all could be opened; a partially mapped/opened set is OK and
    // is reported through lastError() as a warning.
    bool open();
    void close();
    bool isOpen() const { return m_open; }
    QString lastError() const { return m_lastError; }

    void setGain(float g);
    void setChannelGain(int channel, float g);
    void setTxGain(float g);
    float gain() const { return m_gain; }

    // Device actually opened for a channel (empty = unmapped / failed).
    QString rxDeviceName(int channel) const;
    QString txDeviceName() const;
    // Human-readable per-device format/latency/underrun summary.
    QStringList statusLines() const;

    // Settings helpers (shared with DaxDeviceMappingDialog).
    static QString rxDeviceSettingKey(int channel);   // "DaxRxDevice<n>"
    static QString txDeviceSettingKey();              // "DaxTxDevice"
    static bool hasAnyMapping();

public slots:
    // pcm: float32 stereo interleaved, 24 kHz.
    void feedDaxAudio(int channel, const QByteArray& pcm);
    // Parity with the native bridges.  The device clocks keep running
    // during TX on their own: an empty playout buffer plays silence, so no
    // explicit silence fill is required.
    void setTransmitting(bool tx);
    // Re-read the device mapping from AppSettings and reopen devices.
    void reloadDeviceMapping();

signals:
    void txAudioReady(const QByteArray& pcm);
    void daxRxLevel(int channel, float rms);
    void daxTxLevel(float rms);

private:
    struct RxChannel;
    struct TxChannel;

    bool openRx(int index, const QString& deviceName);
    bool openTx(const QString& deviceName);
    void closeRx(int index);
    void closeTx();
    void readTx();
    void scheduleReload(int delayMs);
    void onDevicesChanged();

    bool m_open{false};
    bool m_transmitting{false};
    QString m_lastError;
    float m_gain{0.5f};
    std::array<float, NUM_CHANNELS> m_channelGain{0.5f, 0.5f, 0.5f, 0.5f};
    float m_txGain{0.5f};

    std::array<std::unique_ptr<RxChannel>, NUM_CHANNELS> m_rx;
    std::unique_ptr<TxChannel> m_tx;
    std::array<int, NUM_CHANNELS> m_rxMeterCount{};
    int m_txMeterCount{0};

    QTimer* m_reloadTimer{nullptr};        // Qt parent-owned
    QMediaDevices* m_mediaDevices{nullptr}; // Qt parent-owned (Windows/macOS)
};

} // namespace AetherSDR
