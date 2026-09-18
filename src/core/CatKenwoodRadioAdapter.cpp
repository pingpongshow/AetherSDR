#include "CatKenwoodRadioAdapter.h"

#include "models/CwxModel.h"
#include "models/MeterModel.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

CatKenwoodRadioAdapter::CatKenwoodRadioAdapter(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
    // Coalesce bursts of model signals (a status line can touch a dozen
    // properties) into one AI poll per event-loop pass.
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(0);
    connect(&m_coalesce, &QTimer::timeout, this, &CatKenwoodRadioAdapter::stateChanged);

    if (!m_model) {
        return;
    }
    for (SliceModel* s : m_model->slices()) {
        watchSlice(s);
    }
    connect(m_model, &RadioModel::sliceAdded, this, [this](SliceModel* s) {
        watchSlice(s);
        scheduleChanged();
    });
    connect(m_model, &RadioModel::sliceRemoved, this, [this](int id) {
        m_sLevelDbm.remove(id);
        scheduleChanged();
    });
    connect(m_model, &RadioModel::connectionStateChanged, this, [this](bool) {
        m_sLevelDbm.clear();
        scheduleChanged();
    });
    connect(&m_model->transmitModel(), &TransmitModel::stateChanged,
            this, &CatKenwoodRadioAdapter::scheduleChanged);
    connect(&m_model->transmitModel(), &TransmitModel::moxChanged,
            this, &CatKenwoodRadioAdapter::scheduleChanged);
    connect(&m_model->transmitModel(), &TransmitModel::tuneChanged,
            this, &CatKenwoodRadioAdapter::scheduleChanged);
    // S-meter: MeterModel only keeps the most recent slice's level, so cache
    // per slice. Not a state change for AI purposes (no scheduleChanged()).
    connect(&m_model->meterModel(), &MeterModel::sLevelChanged,
            this, [this](int sliceIndex, float dbm) { m_sLevelDbm.insert(sliceIndex, dbm); });
}

void CatKenwoodRadioAdapter::watchSlice(SliceModel* s)
{
    if (!s) {
        return;
    }
    connect(s, &SliceModel::frequencyChanged, this, &CatKenwoodRadioAdapter::scheduleChanged);
    connect(s, &SliceModel::modeChanged,      this, &CatKenwoodRadioAdapter::scheduleChanged);
    connect(s, &SliceModel::txSliceChanged,   this, &CatKenwoodRadioAdapter::scheduleChanged);
    connect(s, &SliceModel::activeChanged,    this, &CatKenwoodRadioAdapter::scheduleChanged);
}

void CatKenwoodRadioAdapter::scheduleChanged()
{
    if (!m_coalesce.isActive()) {
        m_coalesce.start();
    }
}

// ── State ───────────────────────────────────────────────────────────────────

bool CatKenwoodRadioAdapter::isConnected() const
{
    return m_model && m_model->isConnected();
}

std::vector<int> CatKenwoodRadioAdapter::sliceIds() const
{
    std::vector<int> ids;
    if (!m_model) {
        return ids;
    }
    for (SliceModel* s : m_model->slices()) {
        ids.push_back(s->sliceId());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::optional<CatSliceState> CatKenwoodRadioAdapter::slice(int id) const
{
    if (!m_model || id < 0) {
        return std::nullopt;
    }
    SliceModel* s = m_model->slice(id);
    if (!s) {
        return std::nullopt;
    }
    CatSliceState st;
    st.id           = s->sliceId();
    st.freqHz       = std::llround(s->frequency() * 1e6);
    st.mode         = s->mode();
    st.modeList     = s->modeList();
    st.filterLow    = s->filterLow();
    st.filterHigh   = s->filterHigh();
    st.active       = s->isActive();
    st.tx           = s->isTxSlice();
    st.audioGain    = static_cast<int>(std::lround(s->audioGain()));
    st.audioPan     = s->audioPan();
    st.mute         = s->audioMute();
    st.ritOn        = s->ritOn();
    st.ritHz        = s->ritFreq();
    st.xitOn        = s->xitOn();
    st.xitHz        = s->xitFreq();
    st.agcMode      = s->agcMode();
    st.agcThreshold = s->agcThreshold();
    st.nr           = s->nrOn();
    st.diversity    = s->diversity();
    st.squelchOn    = s->squelchOn();
    st.squelchLevel = s->squelchLevel();
    if (PanadapterModel* pan = m_model->panadapter(s->panId())) {
        st.wnb      = pan->wnbActive();
        st.wnbLevel = pan->wnbLevel();
    }
    st.sMeterDbm = m_sLevelDbm.value(id, -127.0f);
    return st;
}

int CatKenwoodRadioAdapter::activeSliceId() const
{
    if (m_activeProvider) {
        return m_activeProvider();
    }
    if (!m_model) {
        return -1;
    }
    for (SliceModel* s : m_model->slices()) {
        if (s->isActive()) {
            return s->sliceId();
        }
    }
    return -1;
}

// fw v1.4.0.0: the radio never sends mox= — use the interlock state.
bool CatKenwoodRadioAdapter::isTransmitting() const
{
    return m_model && m_model->transmitModel().isTransmitting();
}

bool CatKenwoodRadioAdapter::isTuning() const
{
    return m_model && m_model->transmitModel().isTuning();
}

int CatKenwoodRadioAdapter::rfPower() const
{
    return m_model ? m_model->transmitModel().rfPower() : 0;
}

int CatKenwoodRadioAdapter::micLevel() const
{
    return m_model ? m_model->transmitModel().micLevel() : 0;
}

int CatKenwoodRadioAdapter::cwSpeed() const
{
    return m_model ? m_model->transmitModel().cwSpeed() : 20;
}

int CatKenwoodRadioAdapter::cwPitch() const
{
    return m_model ? m_model->transmitModel().cwPitch() : 600;
}

bool CatKenwoodRadioAdapter::voxEnabled() const
{
    return m_model && m_model->transmitModel().voxEnable();
}

bool CatKenwoodRadioAdapter::binaural() const
{
    return m_model && m_model->binauralRx();
}

QString CatKenwoodRadioAdapter::radioModelName() const
{
    return m_model ? m_model->model() : QString();
}

// ── Slice actions ───────────────────────────────────────────────────────────

void CatKenwoodRadioAdapter::setFrequency(int sliceId, long long hz)
{
    SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr;
    if (!s) {
        return;
    }
    const double mhz = static_cast<double>(hz) / 1e6;
    // In-pan tunes (spot clicks, N1MM band map) keep the pan still
    // (autopan=0); a jump outside the visible span recenters so cross-band
    // changes from loggers follow the slice (same reason as rigctld #536).
    bool inPan = false;
    if (PanadapterModel* pan = m_model->panadapter(s->panId())) {
        const double half = pan->bandwidthMhz() / 2.0;
        inPan = half > 0.0 && std::abs(mhz - pan->centerMhz()) < half;
    }
    if (inPan) {
        s->setFrequency(mhz);
    } else {
        s->tuneAndRecenter(mhz);
    }
}

void CatKenwoodRadioAdapter::setMode(int sliceId, const QString& mode)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setMode(mode);
    }
}

void CatKenwoodRadioAdapter::setFilter(int sliceId, int low, int high)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setFilterWidth(low, high);
    }
}

void CatKenwoodRadioAdapter::setAudioGain(int sliceId, int gain)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setAudioGain(static_cast<float>(gain));
    }
}

void CatKenwoodRadioAdapter::setAudioPan(int sliceId, int pan)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setAudioPan(pan);
    }
}

void CatKenwoodRadioAdapter::setMute(int sliceId, bool mute)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setAudioMute(mute);
    }
}

void CatKenwoodRadioAdapter::setRit(int sliceId, bool on, int hz)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setRit(on, hz);
    }
}

void CatKenwoodRadioAdapter::setXit(int sliceId, bool on, int hz)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setXit(on, hz);
    }
}

void CatKenwoodRadioAdapter::setAgcMode(int sliceId, const QString& mode)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setAgcMode(mode);
    }
}

void CatKenwoodRadioAdapter::setAgcThreshold(int sliceId, int value)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setAgcThreshold(value);
    }
}

void CatKenwoodRadioAdapter::setNr(int sliceId, bool on)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setNr(on);
    }
}

// SmartSDR CAT NB/ZZNL drive the Wide Noise Blanker, which is a panadapter
// property (FlexLib Panadapter.WNBOn → "display pan set <pan> wnb=").
void CatKenwoodRadioAdapter::setWnb(int sliceId, bool on)
{
    SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr;
    if (!s || s->panId().isEmpty()) {
        return;
    }
    m_model->sendCmdPublic(QString("display pan set %1 wnb=%2").arg(s->panId()).arg(on ? 1 : 0),
                           nullptr);
}

void CatKenwoodRadioAdapter::setWnbLevel(int sliceId, int level)
{
    SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr;
    if (!s || s->panId().isEmpty()) {
        return;
    }
    m_model->sendCmdPublic(QString("display pan set %1 wnb_level=%2")
                               .arg(s->panId()).arg(std::clamp(level, 0, 100)),
                           nullptr);
}

void CatKenwoodRadioAdapter::setDiversity(int sliceId, bool on)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setDiversity(on);
    }
}

void CatKenwoodRadioAdapter::setSquelch(int sliceId, bool on, int level)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setSquelch(on, std::clamp(level, 0, 100));
    }
}

void CatKenwoodRadioAdapter::setTxSlice(int sliceId, bool on)
{
    if (SliceModel* s = m_model ? m_model->slice(sliceId) : nullptr) {
        s->setTxSlice(on);
    }
}

void CatKenwoodRadioAdapter::setActiveSlice(int sliceId)
{
    emit activeSliceRequested(sliceId);
}

void CatKenwoodRadioAdapter::requestSplit(int rxSliceId, bool on, double txFreqMhz)
{
    if (m_model) {
        emit m_model->splitRequested(rxSliceId, on, txFreqMhz);
    }
}

// ── Radio actions ───────────────────────────────────────────────────────────

void CatKenwoodRadioAdapter::setTransmit(bool tx)
{
    if (m_model) {
        m_model->setTransmit(tx);
    }
}

void CatKenwoodRadioAdapter::setTune(bool on)
{
    if (!m_model) {
        return;
    }
    if (on) {
        m_model->transmitModel().startTune();
    } else {
        m_model->transmitModel().stopTune();
    }
}

void CatKenwoodRadioAdapter::setRfPower(int power)
{
    if (m_model) {
        m_model->transmitModel().setRfPower(power);
    }
}

void CatKenwoodRadioAdapter::setMicLevel(int level)
{
    if (m_model) {
        m_model->transmitModel().setMicLevel(level);
    }
}

void CatKenwoodRadioAdapter::setCwSpeed(int wpm)
{
    if (m_model) {
        m_model->transmitModel().setCwSpeed(wpm);
    }
}

void CatKenwoodRadioAdapter::setCwPitch(int hz)
{
    if (m_model) {
        m_model->transmitModel().setCwPitch(hz);
    }
}

void CatKenwoodRadioAdapter::setVox(bool on)
{
    if (m_model) {
        m_model->transmitModel().setVoxEnable(on);
    }
}

void CatKenwoodRadioAdapter::setBinaural(bool on)
{
    if (m_model) {
        m_model->sendCmdPublic(QString("radio set binaural_rx=%1").arg(on ? 1 : 0), nullptr);
    }
}

void CatKenwoodRadioAdapter::sendCw(const QString& text)
{
    // CwxModel encodes spaces/blocks the way the radio expects (see
    // RigctlProtocol::cmdSendMorse).
    if (m_model) {
        m_model->cwxModel().send(text);
    }
}

void CatKenwoodRadioAdapter::clearCw()
{
    if (m_model) {
        m_model->sendCmdPublic("cwx clear", nullptr);
    }
}

} // namespace AetherSDR
