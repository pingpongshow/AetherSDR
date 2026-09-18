#pragma once

#include <QWidget>

class QPushButton;
class QLabel;
class QSlider;
class QComboBox;

namespace AetherSDR {

class TransmitModel;
class TunerModel;

// TX applet — transmit controls matching the SmartSDR TX panel.
//
// Layout (top to bottom):
//  - Title bar: "TX"
//  - Forward Power horizontal gauge (0–120 W, red > 100 W)
//  - SWR horizontal gauge (1.0–3.0, red > 2.5)
//  - RF Power slider (0–100%)
//  - Tune Power slider (0–100%)
//  - TX Profile dropdown + Success/Byp/Mem indicators
//  - TUNE / MOX / ATU / MEM buttons
//  - Active / Cal / Avail indicators
//  - APD button
class TxApplet : public QWidget {
    Q_OBJECT

public:
    explicit TxApplet(QWidget* parent = nullptr);

    void setTransmitModel(TransmitModel* model);
    void setTunerModel(TunerModel* tuner);

public slots:
    void updateMeters(float fwdPower, float swr);
    void setPowerScale(int maxWatts, bool hasAmplifier);

private:
    void buildUI();
    void syncFromModel();
    void syncAtuIndicators();
    void syncPermissions();

    TransmitModel* m_model{nullptr};

    // Gauges (HGauge*)
    QWidget* m_fwdGauge{nullptr};
    QWidget* m_swrGauge{nullptr};

    // Sliders
    QSlider* m_rfPowerSlider{nullptr};
    QSlider* m_tunePowerSlider{nullptr};
    QLabel*  m_rfPowerLabel{nullptr};
    QLabel*  m_tunePowerLabel{nullptr};

    // Profile dropdown
    QComboBox* m_profileCombo{nullptr};

    // ATU status indicators
    QLabel* m_successInd{nullptr};
    QLabel* m_bypInd{nullptr};
    QLabel* m_memInd{nullptr};
    QLabel* m_activeInd{nullptr};
    QLabel* m_calInd{nullptr};
    QLabel* m_availInd{nullptr};

    // Buttons
    QPushButton* m_tuneBtn{nullptr};
    QPushButton* m_moxBtn{nullptr};
    QPushButton* m_atuBtn{nullptr};
    QPushButton* m_memBtn{nullptr};
    QPushButton* m_apdBtn{nullptr};
    QWidget*     m_apdRow{nullptr};
public:
    void setApdVisible(bool v) { if (m_apdRow) m_apdRow->setVisible(v); }
private:

    bool m_updatingFromModel{false};
};

} // namespace AetherSDR
