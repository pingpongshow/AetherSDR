#include "core/Scanner.h"

#include "models/BandDefs.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// Minimum time on a channel after the settle window, so at least one fresh
// meter sample (radio meter rate ~10 Hz) can arrive before we move on.
constexpr int kMinPostSettleMs = 100;

// Frequencies closer than this are considered equal when restoring.
constexpr double kFreqEpsilonMhz = 0.5e-6;

const QStringList& knownModes()
{
    static const QStringList modes = {
        QStringLiteral("USB"),  QStringLiteral("LSB"),  QStringLiteral("CW"),
        QStringLiteral("CWL"),  QStringLiteral("AM"),   QStringLiteral("SAM"),
        QStringLiteral("FM"),   QStringLiteral("NFM"),  QStringLiteral("DFM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("RTTY"),
        QStringLiteral("FDV"),
    };
    return modes;
}

} // namespace

Scanner::Scanner(QObject* parent, bool useInternalTimer)
    : QObject(parent)
{
    qRegisterMetaType<AetherSDR::ScanChannel>("AetherSDR::ScanChannel");
    qRegisterMetaType<AetherSDR::ScanHit>("AetherSDR::ScanHit");

    QElapsedTimer et;
    et.start();
    m_clock = [et]() { return et.elapsed(); };

    if (useInternalTimer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(kTickMs);
        connect(m_timer, &QTimer::timeout, this, &Scanner::tick);
    }
}

Scanner::~Scanner() = default;

QString Scanner::stateName(State s)
{
    switch (s) {
    case State::Idle:     return QStringLiteral("Idle");
    case State::Scanning: return QStringLiteral("Scanning");
    case State::Stopped:  return QStringLiteral("Stopped on signal");
    case State::Paused:   return QStringLiteral("Paused");
    case State::PausedTx: return QStringLiteral("Paused (TX)");
    }
    return {};
}

void Scanner::setClock(Clock clock)
{
    if (clock) {
        m_clock = std::move(clock);
    }
}

qint64 Scanner::now() const
{
    return m_clock();
}

// ─── Configuration ───────────────────────────────────────────────────────────

bool Scanner::setTarget(std::unique_ptr<ScanTarget> target)
{
    if (m_state != State::Idle) {
        return false;
    }
    m_pendingRestore = false;
    m_target = std::move(target);
    return true;
}

void Scanner::setChannels(const QVector<ScanChannel>& channels)
{
    // Keep the scan position stable when the list is edited mid-scan.
    const double currentFreq = (m_index >= 0 && m_index < m_channels.size())
        ? m_channels[m_index].freqMhz : 0.0;
    m_channels = channels;
    if (m_state == State::Idle) {
        m_index = -1;
        return;
    }
    m_index = -1;
    for (int i = 0; i < m_channels.size(); ++i) {
        if (channelKey(m_channels[i].freqMhz) == channelKey(currentFreq)) {
            m_index = i;
            break;
        }
    }
    if (m_channels.isEmpty()) {
        emit errorOccurred(QStringLiteral("Scan list is empty"));
        stop(true);
        return;
    }
    if (m_index < 0) {
        m_index = 0;   // current channel vanished: continue from the top
    }
}

void Scanner::setConfig(const Config& cfg)
{
    m_cfg = cfg;
    m_cfg.dwellMs            = std::max(50, m_cfg.dwellMs);
    m_cfg.settleMs           = std::max(0, m_cfg.settleMs);
    m_cfg.resumeMs           = std::max(0, m_cfg.resumeMs);
    m_cfg.carrierDropMs      = std::max(0, m_cfg.carrierDropMs);
    m_cfg.priorityIntervalMs = std::max(500, m_cfg.priorityIntervalMs);
    m_cfg.priorityDwellMs    = std::max(50, m_cfg.priorityDwellMs);
    m_cfg.hysteresisDb       = std::max(0.0f, m_cfg.hysteresisDb);
}

// ─── Skip list ───────────────────────────────────────────────────────────────

qint64 Scanner::channelKey(double mhz)
{
    return std::llround(mhz * 1.0e6);
}

void Scanner::setSkipList(const QSet<qint64>& hz)
{
    if (m_skip == hz) {
        return;
    }
    m_skip = hz;
    emit skipListChanged();
}

void Scanner::addSkip(double mhz)
{
    const qint64 key = channelKey(mhz);
    if (key <= 0 || m_skip.contains(key)) {
        return;
    }
    m_skip.insert(key);
    emit skipListChanged();
}

void Scanner::removeSkip(double mhz)
{
    if (m_skip.remove(channelKey(mhz))) {
        emit skipListChanged();
    }
}

void Scanner::clearSkipList()
{
    if (m_skip.isEmpty()) {
        return;
    }
    m_skip.clear();
    emit skipListChanged();
}

// ─── Control ─────────────────────────────────────────────────────────────────

bool Scanner::start()
{
    if (m_state != State::Idle) {
        return false;
    }
    if (!m_target || !m_target->isValid()) {
        emit errorOccurred(QStringLiteral("No slice selected for scanning"));
        return false;
    }
    if (m_target->isLocked()) {
        emit errorOccurred(QStringLiteral("Unlock the slice before scanning"));
        return false;
    }
    if (txActive()) {
        emit errorOccurred(QStringLiteral("Cannot start scanning while transmitting"));
        return false;
    }
    if (m_channels.isEmpty()) {
        emit errorOccurred(QStringLiteral("Scan list is empty"));
        return false;
    }
    const int first = nextEligible(-1);
    if (first < 0) {
        emit errorOccurred(QStringLiteral("All channels are locked out"));
        return false;
    }

    // A restore still pending from a stop during TX keeps the older original.
    if (!m_pendingRestore) {
        m_original.freqMhz    = m_target->frequencyMhz();
        m_original.mode       = m_target->mode();
        m_original.filterLow  = m_target->filterLow();
        m_original.filterHigh = m_target->filterHigh();
    }
    m_pendingRestore = false;

    m_onPriority     = false;
    m_stopIsPriority = false;
    m_currentHitId   = 0;
    m_lastPriorityMs = now();

    setState(State::Scanning);
    enterChannel(first);
    if (m_timer) {
        m_timer->start();
    }
    return true;
}

void Scanner::stop(bool restore)
{
    finish(restore);
}

void Scanner::abort()
{
    m_pendingRestore = false;
    finish(false);
}

void Scanner::pause()
{
    if (m_state == State::Scanning || m_state == State::Stopped) {
        m_stateBeforeTx = m_state;   // remembers "paused while stopped"
        setState(State::Paused);
    } else if (m_state == State::PausedTx) {
        m_stateBeforeTx = State::Paused;
    }
}

void Scanner::resume()
{
    if (m_state == State::Stopped) {
        resumeScan();
        return;
    }
    if (m_state != State::Paused) {
        return;
    }
    if (m_onPriority) {
        m_priorityReturnState = State::Scanning;
        returnFromPriority();
        return;
    }
    if (m_stateBeforeTx == State::Stopped) {
        resumeScan();
        return;
    }
    setState(State::Scanning);
    m_channelStartMs = now();
    beginSettle();
}

void Scanner::skipCurrent()
{
    if (m_state == State::Idle || m_onPriority) {
        return;
    }
    if (m_state == State::Stopped && m_stopIsPriority) {
        // The priority channel cannot be locked out — just move on.
        resumeScan();
        return;
    }
    if (m_index >= 0 && m_index < m_channels.size()) {
        addSkip(m_channels[m_index].freqMhz);
    }
    if (m_state == State::Scanning || m_state == State::Stopped) {
        m_currentHitId = 0;
        setState(State::Scanning);
        advance();
    }
}

bool Scanner::tuneTo(const ScanChannel& ch)
{
    if (txActive()) {
        emit errorOccurred(QStringLiteral("Cannot retune while transmitting"));
        return false;
    }
    if (!m_target || !m_target->isValid() || ch.freqMhz <= 0.0) {
        return false;
    }
    if (m_target->isLocked()) {
        emit errorOccurred(QStringLiteral("Unlock the slice before tuning"));
        return false;
    }
    finish(false);
    m_pendingRestore = false;
    applyChannel(ch);
    return true;
}

ScanChannel Scanner::currentChannel() const
{
    if (m_onPriority || (m_state == State::Stopped && m_stopIsPriority)) {
        return m_cfg.priorityChannel;
    }
    if (m_index >= 0 && m_index < m_channels.size()) {
        return m_channels[m_index];
    }
    return {};
}

// ─── Inputs ──────────────────────────────────────────────────────────────────

void Scanner::onSignalLevel(int sliceId, float dbm)
{
    if (m_state == State::Idle || !m_target || sliceId != m_target->sliceId()) {
        return;
    }
    // Meter samples straddling a retune belong to the previous channel.
    if (now() < m_settleUntilMs) {
        return;
    }
    m_levelDbm   = dbm;
    m_levelValid = true;
    emit levelUpdated(dbm);

    if (m_state == State::Stopped && m_currentHitId != 0 && dbm > m_currentPeak) {
        m_currentPeak = dbm;
        emit hitPeakUpdated(m_currentHitId, dbm);
    }
    if (m_state == State::Scanning || m_state == State::Stopped) {
        tick();
    }
}

void Scanner::setSquelchOpen(bool open)
{
    if (m_state == State::Idle || now() < m_settleUntilMs) {
        return;
    }
    m_squelchOpen  = open;
    m_squelchValid = true;
    if (m_state == State::Scanning || m_state == State::Stopped) {
        tick();
    }
}

void Scanner::setTransmitting(bool tx)
{
    if (m_tx == tx) {
        return;
    }
    m_tx = tx;
    updateTx();
}

void Scanner::setMox(bool mox)
{
    if (m_mox == mox) {
        return;
    }
    m_mox = mox;
    updateTx();
}

void Scanner::updateTx()
{
    if (txActive()) {
        if (m_state == State::Scanning || m_state == State::Stopped) {
            m_stateBeforeTx = m_state;
            setState(State::PausedTx);
        } else if (m_state == State::Paused) {
            m_stateBeforeTx = State::Paused;
            setState(State::PausedTx);
        }
        return;
    }

    // TX ended.
    if (m_state == State::Idle) {
        if (m_pendingRestore) {
            m_pendingRestore = false;
            restoreOriginal();
            emit finished(true);
        }
        return;
    }
    if (m_state != State::PausedTx) {
        return;
    }
    if (!m_cfg.resumeAfterTx || m_stateBeforeTx == State::Paused) {
        setState(State::Paused);
        return;
    }
    if (m_onPriority) {
        returnFromPriority();
        return;
    }
    // Meter readings during TX are meaningless; settle before trusting them.
    m_channelStartMs = now();
    beginSettle();
    if (m_stateBeforeTx == State::Stopped) {
        m_stopStartMs   = now();
        m_lastPresentMs = now();
        setState(State::Stopped);
    } else {
        setState(State::Scanning);
    }
}

// ─── State machine ───────────────────────────────────────────────────────────

void Scanner::tick()
{
    if (m_state == State::Idle) {
        return;
    }
    if (!m_target || !m_target->isValid()) {
        emit errorOccurred(QStringLiteral("Scanned slice is no longer available"));
        abort();
        return;
    }
    if (m_state == State::Paused || m_state == State::PausedTx) {
        return;
    }
    if (m_target->isLocked()) {
        emit errorOccurred(QStringLiteral("Slice locked — scan paused"));
        pause();
        return;
    }

    const qint64 t = now();
    const bool priorityConfigured = m_cfg.priorityEnabled && m_cfg.priorityChannel.freqMhz > 0.0;
    const bool priorityDue = priorityConfigured && !m_onPriority
        && (t - m_lastPriorityMs) >= m_cfg.priorityIntervalMs;

    if (m_onPriority) {
        if (detected()) {
            stopOnSignal();
            return;
        }
        const qint64 dwell = std::max<qint64>(m_cfg.priorityDwellMs,
                                              m_cfg.settleMs + kMinPostSettleMs);
        if (t - m_channelStartMs >= dwell) {
            returnFromPriority();
        }
        return;
    }

    if (m_state == State::Scanning) {
        if (priorityDue) {
            if (channelKey(currentChannel().freqMhz) == channelKey(m_cfg.priorityChannel.freqMhz)) {
                m_lastPriorityMs = t;   // already listening to it
            } else {
                startPriorityCheck();
                return;
            }
        }
        if (detected()) {
            stopOnSignal();
            return;
        }
        const qint64 dwell = std::max<qint64>(m_cfg.dwellMs, m_cfg.settleMs + kMinPostSettleMs);
        if (t - m_channelStartMs >= dwell) {
            advance();
        }
        return;
    }

    if (m_state == State::Stopped) {
        const bool isPresent = present();
        if (isPresent) {
            m_lastPresentMs = t;
        }
        switch (m_cfg.resume) {
        case ResumeMode::Time:
            if (t - m_stopStartMs >= m_cfg.resumeMs) {
                resumeScan();
                return;
            }
            break;
        case ResumeMode::Carrier:
            if (!isPresent && t - m_lastPresentMs >= m_cfg.carrierDropMs) {
                resumeScan();
                return;
            }
            break;
        case ResumeMode::Hold:
            break;
        }
        if (priorityDue && !m_stopIsPriority) {
            startPriorityCheck();
        }
    }
}

void Scanner::setState(State s)
{
    if (m_state == s) {
        return;
    }
    m_state = s;
    emit stateChanged(s);
}

void Scanner::applyChannel(const ScanChannel& in)
{
    // Never retune (and so never move the TX frequency) while transmitting.
    if (txActive() || !m_target || !m_target->isValid()) {
        return;
    }
    // While scanning, a channel without its own mode means "the slice's mode
    // at scan start" — not whatever the previous (memory/priority) channel
    // left behind.
    ScanChannel ch = in;
    if (m_state != State::Idle && ch.mode.isEmpty() && !m_original.mode.isEmpty()) {
        ch.mode = m_original.mode;
        if (!ch.hasFilter()) {
            ch.filterLow  = m_original.filterLow;
            ch.filterHigh = m_original.filterHigh;
        }
    }
    if (!ch.mode.isEmpty() && ch.mode.compare(m_target->mode(), Qt::CaseInsensitive) != 0) {
        m_target->setMode(ch.mode.toUpper());
    }
    if (ch.hasFilter()
        && (ch.filterLow != m_target->filterLow() || ch.filterHigh != m_target->filterHigh())) {
        m_target->setFilter(ch.filterLow, ch.filterHigh);
    }
    if (std::abs(ch.freqMhz - m_target->frequencyMhz()) > kFreqEpsilonMhz) {
        m_target->tune(ch.freqMhz);
    }
}

void Scanner::beginSettle()
{
    m_settleUntilMs = now() + m_cfg.settleMs;
    m_levelValid    = false;
    m_squelchValid  = false;
    m_squelchOpen   = false;
}

void Scanner::enterChannel(int index)
{
    m_index = index;
    applyChannel(m_channels[index]);
    m_channelStartMs = now();
    beginSettle();
    emit channelChanged(index, m_channels[index], false);
}

int Scanner::nextEligible(int from) const
{
    const int n = static_cast<int>(m_channels.size());
    for (int k = 1; k <= n; ++k) {
        const int j = ((from + k) % n + n) % n;
        if (!isSkipped(m_channels[j].freqMhz)) {
            return j;
        }
    }
    return -1;
}

bool Scanner::advance()
{
    const int next = nextEligible(m_index);
    if (next < 0) {
        emit errorOccurred(QStringLiteral("All channels are locked out"));
        stop(true);
        return false;
    }
    enterChannel(next);
    return true;
}

bool Scanner::detected() const
{
    const bool sm = m_levelValid && m_levelDbm >= m_cfg.thresholdDbm;
    const bool sq = m_squelchValid && m_squelchOpen;
    switch (m_cfg.detect) {
    case DetectSource::SMeter:  return sm;
    case DetectSource::Squelch: return sq;
    case DetectSource::Either:  return sm || sq;
    }
    return false;
}

bool Scanner::present() const
{
    const bool sm = m_levelValid && m_levelDbm >= (m_cfg.thresholdDbm - m_cfg.hysteresisDb);
    const bool sq = m_squelchValid && m_squelchOpen;
    switch (m_cfg.detect) {
    case DetectSource::SMeter:  return sm;
    case DetectSource::Squelch: return sq;
    case DetectSource::Either:  return sm || sq;
    }
    return false;
}

void Scanner::stopOnSignal()
{
    const qint64 t = now();
    m_stopIsPriority = m_onPriority;
    if (m_onPriority) {
        m_onPriority     = false;
        m_lastPriorityMs = t;
    }
    m_stopStartMs   = t;
    m_lastPresentMs = t;

    const ScanChannel ch = currentChannel();
    ScanHit hit;
    hit.id       = m_nextHitId++;
    hit.timeUtc  = QDateTime::currentDateTimeUtc();
    hit.freqMhz  = ch.freqMhz;
    hit.mode     = ch.mode.isEmpty() && m_target ? m_target->mode() : ch.mode;
    hit.label    = ch.label;
    hit.peakDbm  = m_levelValid ? m_levelDbm : -150.0f;
    hit.priority = m_stopIsPriority;
    m_currentHitId = hit.id;
    m_currentPeak  = hit.peakDbm;

    setState(State::Stopped);
    emit hitAdded(hit);
}

void Scanner::resumeScan()
{
    m_stopIsPriority = false;
    m_currentHitId   = 0;
    setState(State::Scanning);
    advance();
}

void Scanner::startPriorityCheck()
{
    m_priorityReturnState = m_state;
    m_onPriority = true;
    applyChannel(m_cfg.priorityChannel);
    m_channelStartMs = now();
    beginSettle();
    setState(State::Scanning);
    emit channelChanged(-1, m_cfg.priorityChannel, true);
}

void Scanner::returnFromPriority()
{
    m_onPriority     = false;
    m_lastPriorityMs = now();
    if (m_index < 0 || m_index >= m_channels.size()) {
        setState(State::Scanning);
        advance();
        return;
    }
    applyChannel(m_channels[m_index]);
    m_channelStartMs = now();
    beginSettle();
    if (m_priorityReturnState == State::Stopped) {
        m_lastPresentMs = now();   // grace period: meter is settling again
        setState(State::Stopped);
    } else {
        setState(State::Scanning);
    }
    emit channelChanged(m_index, m_channels[m_index], false);
}

void Scanner::restoreOriginal()
{
    if (!m_target || !m_target->isValid() || m_original.freqMhz <= 0.0) {
        return;
    }
    ScanChannel orig;
    orig.freqMhz    = m_original.freqMhz;
    orig.mode       = m_original.mode;
    orig.filterLow  = m_original.filterLow;
    orig.filterHigh = m_original.filterHigh;
    applyChannel(orig);
}

void Scanner::finish(bool restore)
{
    if (m_state == State::Idle) {
        return;
    }
    if (m_timer) {
        m_timer->stop();
    }
    m_onPriority     = false;
    m_stopIsPriority = false;
    m_currentHitId   = 0;

    bool restored = false;
    if (restore) {
        if (txActive()) {
            m_pendingRestore = true;   // restore when TX ends, never retune mid-TX
        } else {
            restoreOriginal();
            restored = true;
        }
    }
    setState(State::Idle);
    if (!m_pendingRestore) {
        emit finished(restored);
    }
}

// ─── Channel list builders ───────────────────────────────────────────────────

QVector<ScanChannel> Scanner::bandChannels(double startMhz, double stopMhz, int stepHz,
                                           const QString& mode, int maxChannels)
{
    QVector<ScanChannel> out;
    if (stepHz <= 0 || startMhz <= 0.0 || stopMhz <= 0.0 || maxChannels <= 0) {
        return out;
    }
    qint64 lo = channelKey(startMhz);
    qint64 hi = channelKey(stopMhz);
    if (hi < lo) {
        std::swap(lo, hi);
    }
    const qint64 count = std::min<qint64>((hi - lo) / stepHz + 1, maxChannels);
    out.reserve(static_cast<int>(count));
    for (qint64 i = 0; i < count; ++i) {
        ScanChannel ch;
        ch.freqMhz = static_cast<double>(lo + i * stepHz) / 1.0e6;
        ch.mode    = mode;
        out.append(ch);
    }
    return out;
}

const BandDef* Scanner::bandForFrequency(double mhz)
{
    for (const BandDef& b : kBands) {
        if (mhz >= b.lowMhz && mhz <= b.highMhz) {
            return &b;
        }
    }
    return nullptr;
}

QVector<ScanChannel> Scanner::selectMemories(const QVector<ScanChannel>& all,
                                             MemorySelect select,
                                             const QString& group,
                                             const QSet<int>& indices)
{
    QVector<ScanChannel> out;
    for (const ScanChannel& ch : all) {
        if (ch.freqMhz <= 0.0) {
            continue;
        }
        bool keep = true;
        if (select == MemorySelect::Group) {
            keep = ch.group.compare(group, Qt::CaseInsensitive) == 0;
        } else if (select == MemorySelect::Selected) {
            keep = indices.contains(ch.memoryIndex);
        }
        if (keep) {
            out.append(ch);
        }
    }
    return out;
}

QVector<ScanChannel> Scanner::parseFrequencyList(const QString& text, QStringList* errors)
{
    static const QRegularExpression kSplit(QStringLiteral("[\\n;]"));
    static const QRegularExpression kWs(QStringLiteral("\\s+"));
    static const QRegularExpression kFreq(
        QStringLiteral("^([0-9]+(?:\\.[0-9]+)?)(k|khz|m|mhz)?$"),
        QRegularExpression::CaseInsensitiveOption);

    QVector<ScanChannel> out;
    const QStringList entries = text.split(kSplit);
    for (const QString& raw : entries) {
        const QString entry = raw.trimmed();
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('#'))) {
            continue;
        }
        QStringList tok = entry.split(kWs, Qt::SkipEmptyParts);
        const QRegularExpressionMatch m = kFreq.match(tok.first());
        if (!m.hasMatch()) {
            if (errors) {
                errors->append(entry);
            }
            continue;
        }
        double value = m.captured(1).toDouble();
        QString unit = m.captured(2).toLower();
        tok.removeFirst();
        if (unit.isEmpty() && !tok.isEmpty()) {
            const QString u = tok.first().toLower();
            if (u == QStringLiteral("khz") || u == QStringLiteral("mhz")) {
                unit = u;
                tok.removeFirst();
            }
        }
        if (unit.startsWith(QLatin1Char('k'))) {
            value /= 1000.0;
        }
        if (value <= 0.0 || value > 10000.0) {
            if (errors) {
                errors->append(entry);
            }
            continue;
        }
        ScanChannel ch;
        ch.freqMhz = value;
        if (!tok.isEmpty() && knownModes().contains(tok.first().toUpper())) {
            ch.mode = tok.first().toUpper();
            tok.removeFirst();
        }
        ch.label = tok.join(QLatin1Char(' '));
        out.append(ch);
    }
    return out;
}

} // namespace AetherSDR
