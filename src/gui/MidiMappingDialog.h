#pragma once

#ifdef HAVE_MIDI

#include <QDialog>
#include <QTableWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>

namespace AetherSDR {

class MidiControlManager;

// MIDI Mapping dialog — dedicated settings window for configuring MIDI
// controller bindings. Opened from Settings → MIDI Mapping.
// Shows device selector, binding table with Learn mode, and profile
// save/load. All bindings stored in ~/.config/AetherSDR/midi.settings.
class MidiMappingDialog : public QDialog {
    Q_OBJECT

public:
    explicit MidiMappingDialog(MidiControlManager* manager, QWidget* parent = nullptr);

private:
    void refreshPortList();
    void refreshBindingTable();
    void refreshProfileList();
    void setPortState(bool open, const QString& status, const QString& style);

    MidiControlManager* m_manager;

    QComboBox*    m_portCombo;
    QPushButton*  m_connectBtn;
    QLabel*       m_statusLabel;
    QLabel*       m_activityLabel;
    QTableWidget* m_bindingTable;
    QComboBox*    m_paramCombo;
    QComboBox*    m_categoryCombo;
    QComboBox*    m_profileCombo;
    bool          m_portOpen{false};
    bool          m_userConnectPending{false};
};

} // namespace AetherSDR

#endif // HAVE_MIDI
