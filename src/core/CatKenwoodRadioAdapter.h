#pragma once

#include "CatKenwoodProtocol.h"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <functional>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Binds the pure CatRadioInterface to AetherSDR's RadioModel / SliceModel /
// TransmitModel. Lives on the main thread with the models; every CAT server
// and PTY shares one instance.
//
// TX semantics are identical to RigctlProtocol: the TX flag is moved with
// SliceModel::setTxSlice() and keying goes through RadioModel::setTransmit().
// Split requests go through RadioModel::splitRequested → MainWindow::
// enableSplit()/disableSplit(), the same path rigctld and TCI use.
class CatKenwoodRadioAdapter : public QObject, public CatRadioInterface {
    Q_OBJECT

public:
    explicit CatKenwoodRadioAdapter(RadioModel* model, QObject* parent = nullptr);

    // Active-slice bridge to MainWindow (RadioModel has no active-slice API;
    // CLAUDE.md: never send "slice set <id> active=1" from here).
    void setActiveSliceProvider(std::function<int()> provider) { m_activeProvider = std::move(provider); }

    // ── CatRadioInterface ────────────────────────────────────────────────
    bool isConnected() const override;
    std::vector<int> sliceIds() const override;
    std::optional<CatSliceState> slice(int id) const override;
    int  activeSliceId() const override;
    bool isTransmitting() const override;
    bool isTuning() const override;
    int  rfPower() const override;
    int  micLevel() const override;
    int  cwSpeed() const override;
    int  cwPitch() const override;
    bool voxEnabled() const override;
    bool binaural() const override;
    QString radioModelName() const override;

    void setFrequency(int sliceId, long long hz) override;
    void setMode(int sliceId, const QString& mode) override;
    void setFilter(int sliceId, int low, int high) override;
    void setAudioGain(int sliceId, int gain) override;
    void setAudioPan(int sliceId, int pan) override;
    void setMute(int sliceId, bool mute) override;
    void setRit(int sliceId, bool on, int hz) override;
    void setXit(int sliceId, bool on, int hz) override;
    void setAgcMode(int sliceId, const QString& mode) override;
    void setAgcThreshold(int sliceId, int value) override;
    void setNr(int sliceId, bool on) override;
    void setWnb(int sliceId, bool on) override;
    void setWnbLevel(int sliceId, int level) override;
    void setDiversity(int sliceId, bool on) override;
    void setSquelch(int sliceId, bool on, int level) override;
    void setTxSlice(int sliceId, bool on) override;
    void setActiveSlice(int sliceId) override;
    void requestSplit(int rxSliceId, bool on, double txFreqMhz) override;

    void setTransmit(bool tx) override;
    void setTune(bool on) override;
    void setRfPower(int power) override;
    void setMicLevel(int level) override;
    void setCwSpeed(int wpm) override;
    void setCwPitch(int hz) override;
    void setVox(bool on) override;
    void setBinaural(bool on) override;
    void sendCw(const QString& text) override;
    void clearCw() override;

signals:
    // Coalesced (≤ 1 per event-loop pass): something a CAT client may care
    // about changed. Servers call CatKenwoodProtocol::pollAutoInfo() on it.
    void stateChanged();
    // FR1;/ZZFR asked for a different active slice. MainWindow should
    // connect this to setActiveSlice(int).
    void activeSliceRequested(int sliceId);

private:
    void watchSlice(SliceModel* s);
    void scheduleChanged();

    RadioModel*          m_model;
    std::function<int()> m_activeProvider;
    QHash<int, float>    m_sLevelDbm;   // slice id → latest S-meter dBm
    QTimer               m_coalesce;
};

} // namespace AetherSDR
