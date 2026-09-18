#include "SpectrumPlayer.h"
#include "LogManager.h"

#include <algorithm>

namespace AetherSDR {

namespace {
constexpr int    kTickMs            = 15;       // ~66 Hz scheduler; frames are timestamp-driven
constexpr qint64 kPositionEmitUs    = 100000;   // positionChanged at most every 100 ms
constexpr double kMinSpeed          = 0.125;
constexpr double kMaxSpeed          = 64.0;
}

SpectrumPlayer::SpectrumPlayer(QObject* parent)
    : QObject(parent)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &SpectrumPlayer::onTick);
}

SpectrumPlayer::~SpectrumPlayer() = default;  // attach() restores the widget via destroyed()

bool SpectrumPlayer::open(const QString& path, QString* err)
{
    close();
    if (!m_reader.open(path, err)) {
        return false;
    }
    if (m_reader.chunks().isEmpty()) {
        if (err) { *err = tr("Recording contains no data"); }
        m_reader.close();
        return false;
    }
    if (m_reader.hadTruncation()) {
        qCWarning(lcDsp) << "SpectrumPlayer: recording has a torn tail (crash?) — playing valid part:"
                         << path;
    }
    m_path = path;
    m_posUs = m_reader.startUs();
    setState(State::Stopped);
    loadChunk(0);
    dispatchUntil(m_posUs, true);   // emit the initial keyframe (pan state)
    emit positionChanged(0);
    return true;
}

void SpectrumPlayer::close()
{
    m_timer.stop();
    m_reader.close();
    m_path.clear();
    m_records.clear();
    m_chunkIndex = -1;
    m_recIndex = 0;
    m_havePan = false;
    m_slices.clear();
    m_posUs = 0;
    setState(State::Closed);
}

void SpectrumPlayer::setState(State s)
{
    if (s == m_state) { return; }
    m_state = s;
    emit stateChanged(s);
    emit labelChanged(playbackLabel());
}

QString SpectrumPlayer::playbackLabel() const
{
    const QString speed = (m_speed == static_cast<int>(m_speed))
        ? QString::number(static_cast<int>(m_speed))
        : QString::number(m_speed, 'g', 3);
    switch (m_state) {
    case State::Playing: return tr("PLAYBACK %1x").arg(speed);
    case State::Paused:
    case State::Stopped: return tr("PLAYBACK paused");
    case State::Closed:  return {};
    }
    return {};
}

void SpectrumPlayer::play()
{
    if (!isOpen()) { return; }
    if (m_posUs >= m_reader.endUs()) {
        seek(0);
    }
    m_wallBaseUs = m_posUs;
    m_wall.restart();
    m_timer.start();
    setState(State::Playing);
}

void SpectrumPlayer::pause()
{
    if (m_state != State::Playing) { return; }
    m_timer.stop();
    setState(State::Paused);
}

void SpectrumPlayer::stop()
{
    if (!isOpen()) { return; }
    m_timer.stop();
    seek(0);
    setState(State::Stopped);
}

void SpectrumPlayer::setSpeed(double factor)
{
    factor = std::clamp(factor, kMinSpeed, kMaxSpeed);
    if (factor == m_speed) { return; }
    if (m_state == State::Playing) {
        m_wallBaseUs = wallPositionUs();   // rebase so position is continuous
        m_wall.restart();
    }
    m_speed = factor;
    emit labelChanged(playbackLabel());
}

qint64 SpectrumPlayer::wallPositionUs() const
{
    return m_wallBaseUs + static_cast<qint64>(static_cast<double>(m_wall.nsecsElapsed()) / 1000.0 * m_speed);
}

bool SpectrumPlayer::loadChunk(int index)
{
    m_records.clear();
    m_recIndex = 0;
    m_chunkIndex = index;
    QString err;
    if (!m_reader.readChunk(index, m_records, &err)) {
        // A damaged chunk in the middle is skipped, not fatal.
        qCWarning(lcDsp) << "SpectrumPlayer: skipping chunk" << index << err;
        m_records.clear();
        return false;
    }
    return true;
}

void SpectrumPlayer::dispatchUntil(qint64 targetUs, bool emitFrames)
{
    const bool coalesce = m_speed > 1.0;
    QVector<float> pendingFft;
    bool havePendingFft = false;
    const int nChunks = static_cast<int>(m_reader.chunks().size());

    for (;;) {
        if (m_recIndex >= m_records.size()) {
            const int next = m_chunkIndex + 1;
            if (next >= nChunks || m_reader.chunks()[next].t0Us > targetUs) {
                break;
            }
            loadChunk(next);
            continue;
        }
        const SpecRec::Record& r = m_records[m_recIndex];
        if (r.tUs > targetUs) { break; }
        ++m_recIndex;

        switch (r.type) {
        case SpecRec::RecordType::PanState:
            if (!m_havePan || !(r.pan == m_pan)) {
                m_pan = r.pan;
                m_havePan = true;
                emit panStateChanged(m_pan.centerMhz, m_pan.bandwidthMhz, m_pan.minDbm, m_pan.maxDbm);
            }
            break;
        case SpecRec::RecordType::SliceMarkers:
            if (r.slices != m_slices) {
                m_slices = r.slices;
                emit sliceMarkersChanged(m_slices);
            }
            break;
        case SpecRec::RecordType::FftFrame:
            if (!emitFrames) {
                pendingFft = r.bins;        // seek: keep only the last one
                havePendingFft = true;
            } else if (coalesce) {
                pendingFft = r.bins;
                havePendingFft = true;
            } else {
                emit spectrumFrame(r.bins);
            }
            break;
        case SpecRec::RecordType::WaterfallRow:
            if (emitFrames) {
                emit waterfallRow(r.bins, r.lowMhz, r.highMhz, r.timecode);
            }
            break;
        case SpecRec::RecordType::Note:
            if (emitFrames) {
                emit noteReached(r.text);
            }
            break;
        }
    }
    if (havePendingFft) {
        emit spectrumFrame(pendingFft);
    }
    m_posUs = std::max(m_posUs, targetUs);
}

void SpectrumPlayer::advanceTo(qint64 ms)
{
    if (!isOpen()) { return; }
    const qint64 target = std::min(m_reader.startUs() + ms * 1000, m_reader.endUs());
    dispatchUntil(target, true);
    emit positionChanged(positionMs());
}

void SpectrumPlayer::seek(qint64 ms)
{
    if (!isOpen()) { return; }
    const qint64 target = std::clamp(m_reader.startUs() + ms * 1000,
                                     m_reader.startUs(), m_reader.endUs());
    emit seeked((target - m_reader.startUs()) / 1000);
    loadChunk(m_reader.chunkForTime(target));
    m_havePan = false;      // force re-emit of the keyframe state
    m_slices.clear();
    m_posUs = m_reader.chunks()[m_chunkIndex].t0Us;
    dispatchUntil(target, false);
    m_posUs = target;
    if (m_state == State::Playing) {
        m_wallBaseUs = m_posUs;
        m_wall.restart();
    }
    m_lastPosEmitUs = m_posUs;
    emit positionChanged(positionMs());
}

void SpectrumPlayer::onTick()
{
    if (!isOpen()) {
        m_timer.stop();
        return;
    }
    const qint64 endUs = m_reader.endUs();
    qint64 target = wallPositionUs();
    if (target >= endUs) {
        dispatchUntil(endUs, true);
        if (m_loop) {
            seek(0);
            m_wallBaseUs = m_posUs;
            m_wall.restart();
            return;
        }
        m_timer.stop();
        m_posUs = endUs;
        emit positionChanged(positionMs());
        setState(State::Paused);
        emit finished();
        return;
    }
    dispatchUntil(target, true);
    if (m_posUs - m_lastPosEmitUs >= kPositionEmitUs || m_posUs < m_lastPosEmitUs) {
        m_lastPosEmitUs = m_posUs;
        emit positionChanged(positionMs());
    }
}

} // namespace AetherSDR
