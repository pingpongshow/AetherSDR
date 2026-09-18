#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QHBoxLayout;
class QComboBox;

namespace AetherSDR {

class RotatorClient;
class CompassRose;

// Rotator applet: compass rose (heading + target), "Turn to" entry that
// accepts a callsign, Maidenhead grid or bearing, SP/LP, STOP/PARK, preset
// headings and a collapsible settings section (backend/host/port/poll/range).
//
// Styling reuses the applet constants from AntennaGeniusApplet/TunerApplet
// (#1a2a3a / #203040 / #c8d8e8 / #00b4d8) — no new theme.
class RotatorWidget : public QWidget {
    Q_OBJECT

public:
    explicit RotatorWidget(QWidget* parent = nullptr);

    // Attach the (MainWindow-owned) client.  Safe to call once.
    void setRotatorClient(RotatorClient* client);

    // Load AppSettings into the client and start it if RotatorEnabled=True.
    // MainWindow calls this once at startup after setRotatorClient().
    void applySavedSettings();

private:
    void buildUI();
    QWidget* buildSettingsSection();
    void rebuildPresetButtons();
    void refreshStatus();
    void refreshTargetInfo();
    void turnFromEntry(bool longPath);
    void applySettingsFromForm();
    void loadSettingsIntoForm();
    void setEnabledFromUi(bool on);

    RotatorClient* m_client{nullptr};

    CompassRose* m_rose{nullptr};
    QLabel*      m_statusLabel{nullptr};
    QPushButton* m_enableBtn{nullptr};
    QLabel*      m_headingLabel{nullptr};
    QLineEdit*   m_turnEdit{nullptr};
    QPushButton* m_spBtn{nullptr};
    QPushButton* m_lpBtn{nullptr};
    QLabel*      m_pathLabel{nullptr};
    QPushButton* m_stopBtn{nullptr};
    QPushButton* m_parkBtn{nullptr};
    QWidget*     m_presetRow{nullptr};
    QHBoxLayout* m_presetLayout{nullptr};

    // Settings section
    QPushButton* m_settingsToggle{nullptr};
    QWidget*     m_settingsBody{nullptr};
    QComboBox*   m_backendCombo{nullptr};
    QLineEdit*   m_hostEdit{nullptr};
    QLineEdit*   m_portEdit{nullptr};
    QLineEdit*   m_pollEdit{nullptr};
    QLineEdit*   m_minAzEdit{nullptr};
    QLineEdit*   m_maxAzEdit{nullptr};
    QLineEdit*   m_n1mmNameEdit{nullptr};
    QLineEdit*   m_presetsEdit{nullptr};

    QString m_lastError;
};

} // namespace AetherSDR
