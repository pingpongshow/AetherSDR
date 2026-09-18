#pragma once

#include "core/CwSkimmer.h"

#include <QHash>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace AetherSDR {

// CW skimmer panel: one row per decoded carrier (frequency, last callsign,
// WPM, SNR, running text).  Clicking a row requests a tune to that carrier.
//
// Client-side settings (AppSettings):
//   CwSkimmerEnabled        "True"/"False"  (default False)
//   CwSkimmerDaxIqChannel   1-4             (default 1)
class CwSkimmerPanel : public QWidget {
    Q_OBJECT

public:
    explicit CwSkimmerPanel(QWidget* parent = nullptr);

    bool isSkimmerEnabled() const;
    int  daxIqChannel() const;

public slots:
    void setChannels(const QVector<AetherSDR::CwSkimmerChannelInfo>& channels);
    void addSpot(const AetherSDR::CwSkimmerSpot& spot);
    void removeChannel(int channelId);
    void setRunning(bool running);

signals:
    void enableToggled(bool on);
    void daxIqChannelChanged(int channel);
    void tuneRequested(double freqMhz);

private:
    void rebuildRows();
    void onCellClicked(int row, int column);

    QPushButton*  m_enableBtn{nullptr};
    QComboBox*    m_channelCombo{nullptr};
    QLabel*       m_status{nullptr};
    QTableWidget* m_table{nullptr};

    QVector<CwSkimmerChannelInfo> m_channels;
    QHash<int, CwSkimmerSpot>     m_lastSpot;   // channel id → latest spot
};

} // namespace AetherSDR
