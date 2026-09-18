#pragma once

#include <QDialog>

#include <array>

class QComboBox;
class QGroupBox;
class QLabel;

namespace AetherSDR {

// Settings dialog for the DAX audio backend and the device-mapped DAX
// channel → audio device assignment (Windows virtual cables, or BlackHole /
// Loopback on macOS, ALSA loopback / null sinks on Linux).
//
// Reads and, on OK, writes AppSettings:
//   DaxBackend       "Native" | "DeviceMapped"
//   DaxRxDevice1..4  output device description ("" = unmapped)
//   DaxTxDevice      input device description  ("" = unmapped)
//
// The caller decides how to apply the change (see backendChanged()).
class DaxDeviceMappingDialog : public QDialog {
    Q_OBJECT

public:
    explicit DaxDeviceMappingDialog(QWidget* parent = nullptr);

    // Valid after exec() returned Accepted: true when DaxBackend changed
    // (the DAX bridge must be restarted), false when only devices changed
    // (VirtualAudioBridge::reloadDeviceMapping() is enough).
    bool backendChanged() const { return m_backendChanged; }

protected:
    void accept() override;

private:
    void populateDevices();
    void autoDetect();
    void updateHints();
    void updateEnabled();
    static void selectByData(QComboBox* combo, const QString& name);

    QComboBox* m_backendCombo{nullptr};
    QGroupBox* m_mappingGroup{nullptr};
    std::array<QComboBox*, 4> m_rxCombo{};
    std::array<QLabel*, 4> m_rxHint{};
    QComboBox* m_txCombo{nullptr};
    QLabel* m_txHint{nullptr};
    QLabel* m_notes{nullptr};
    QStringList m_outputs;
    QStringList m_inputs;
    QString m_suggestionNotes;
    bool m_backendChanged{false};
};

} // namespace AetherSDR
