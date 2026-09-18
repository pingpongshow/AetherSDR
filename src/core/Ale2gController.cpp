#include "Ale2gController.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>

namespace AetherSDR {

using namespace Ale2g;

namespace {

constexpr int kTickMs = 10;
constexpr int kTxLeadMs = 60;             // audio queued ahead of real time
constexpr int kRxBlankAfterTxMs = 150;    // ignore our own TX tail / T/R relay
constexpr int kRxBlankAfterTuneMs = 60;
constexpr int kMaxChannelHoldMs = 45000;  // Tsc max plus message, safety cap
constexpr int kMaxReplyReadMs = 30000;    // reading a reply that started in time
constexpr int kMaxSoundWords = 30000 / kTrwMs;   // HFLink: never longer than 30 s
constexpr int kAnyCallSlotTw = 20;        // A.5.5.4.5: 20 Tw slots
constexpr int kAnyCallSlots = 16;
constexpr int kMinSoundIntervalMin = 30;  // HFLink: max 2 sounds / hour / channel
constexpr int kOwnUnkeyGraceMs = 1000;    // interlock may still report TX after our unkey

bool sameChannels(const QVector<Ale2gChannel>& a, const QVector<Ale2gChannel>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (a.at(i).frequencyHz != b.at(i).frequencyHz || a.at(i).name != b.at(i).name
            || a.at(i).sound != b.at(i).sound) {
            return false;
        }
    }
    return true;
}

// Index of the channel with the same frequency in the new list, -1 if gone.
int remapChannelIndex(int oldIndex, const QVector<Ale2gChannel>& oldList,
                      const QVector<Ale2gChannel>& newList)
{
    if (oldIndex < 0 || oldIndex >= oldList.size()) {
        return -1;
    }
    const qint64 hz = oldList.at(oldIndex).frequencyHz;
    for (int i = 0; i < newList.size(); ++i) {
        if (newList.at(i).frequencyHz == hz) {
            return i;
        }
    }
    return -1;
}

} // namespace

Ale2gController::Ale2gController(QObject* parent, bool useInternalTimer)
    : QObject(parent)
{
    QElapsedTimer et;
    et.start();
    m_clock = [et]() { return et.elapsed(); };

    if (useInternalTimer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        m_timer->setInterval(kTickMs);
        connect(m_timer, &QTimer::timeout, this, &Ale2gController::tick);
        m_timer->start();
    }
}

Ale2gController::~Ale2gController()
{
    if (m_keyRequested) {
        m_keyRequested = false;
        emit requestTransmit(false);
    }
}

QString Ale2gController::stateName(State s)
{
    switch (s) {
    case State::Idle:       return QStringLiteral("IDLE");
    case State::Scanning:   return QStringLiteral("SCANNING");
    case State::Sounding:   return QStringLiteral("SOUNDING");
    case State::Calling:    return QStringLiteral("CALLING");
    case State::Responding: return QStringLiteral("RESPONDING");
    case State::Linked:     return QStringLiteral("LINKED");
    }
    return QStringLiteral("?");
}

void Ale2gController::setClock(Clock clock)
{
    if (clock) {
        m_clock = std::move(clock);
    }
}

qint64 Ale2gController::now() const
{
    return m_clock();
}

// ─── Configuration ─────────────────────────────────────────────────────────

void Ale2gController::setSelfAddresses(const QStringList& addresses)
{
    m_selfAddresses.clear();
    for (const QString& a : addresses) {
        const QString n = normalizeAddress(a);
        if (isValidAddress(n) && !n.startsWith(QLatin1Char('@')) && !m_selfAddresses.contains(n)) {
            m_selfAddresses << n;
        }
    }
}

void Ale2gController::setChannels(const QVector<Ale2gChannel>& channels)
{
    QVector<Ale2gChannel> filtered;
    for (const Ale2gChannel& c : channels) {
        if (c.frequencyHz > 0) {
            filtered << c;
        }
    }
    if (sameChannels(filtered, m_channels)) {
        return;   // the dialog re-applies settings on every Call/Sound click
    }
    const QVector<Ale2gChannel> old = m_channels;
    m_channels = filtered;

    // Everything that holds a channel index refers to the old list: remap by
    // frequency so a queued call/sound can neither index past the end
    // (QVector::at in queueNextCallAttempt) nor transmit on a different
    // frequency than the one it was queued for.
    m_channelIndex = remapChannelIndex(m_channelIndex, old, m_channels);
    auto remapQueue = [&](QVector<int>& queue) {
        QVector<int> out;
        for (int idx : queue) {
            const int n = idx < 0 ? -1 : remapChannelIndex(idx, old, m_channels);
            if ((idx < 0 || n >= 0) && !out.contains(n)) {
                out << n;
            }
        }
        queue = out;
    };
    remapQueue(m_callQueue);
    remapQueue(m_soundQueue);
    for (Ale2gHeardStation& h : m_heard) {
        h.channelIndex = remapChannelIndex(h.channelIndex, old, m_channels);
    }

    if (m_txPhase != TxPhase::None && m_job.channelIndex >= 0) {
        const int n = remapChannelIndex(m_job.channelIndex, old, m_channels);
        if (n >= 0 || m_keyRequested) {
            m_job.channelIndex = n;   // keyed: already on air where it was tuned
        } else {
            const QString what = m_job.description;
            const JobKind kind = m_job.kind;
            m_txPhase = TxPhase::None;
            fail(QStringLiteral("Channel list changed; %1 cancelled.").arg(what));
            if (kind != JobKind::Amd && m_state != State::Linked) {
                resumeAvailable();
            }
        }
    }
}

void Ale2gController::setDwellMs(int ms) { m_dwellMs = std::clamp(ms, 200, 10000); }
void Ale2gController::setScanCallMs(int ms) { m_scanCallMs = std::clamp(ms, 0, 30000); }
void Ale2gController::setSoundLengthMs(int ms) { m_soundLengthMs = std::clamp(ms, 0, 30000); }

void Ale2gController::setSoundingIntervalMinutes(int minutes)
{
    m_soundIntervalMin = minutes <= 0 ? 0 : std::max(kMinSoundIntervalMin, minutes);
    if (m_soundIntervalMin > 0) {
        m_nextSoundMs = now() + static_cast<qint64>(m_soundIntervalMin) * 60000;
    }
}

void Ale2gController::setSoundWithTis(bool tis) { m_soundTis = tis; }
void Ale2gController::setListenBeforeTransmitMs(int ms) { m_lbtMs = std::clamp(ms, 0, 10000); }
void Ale2gController::setReplyTimeoutMs(int ms) { m_replyTimeoutMs = std::clamp(ms, 1000, 60000); }
void Ale2gController::setLinkTimeoutMs(int ms) { m_linkTimeoutMs = std::max(0, ms); }
void Ale2gController::setMaxCallAttempts(int attempts) { m_maxCallAttempts = std::clamp(attempts, 1, 100); }
void Ale2gController::setRespondToCalls(bool respond) { m_respondToCalls = respond; }
void Ale2gController::setAcceptAllCalls(bool accept) { m_acceptAllCalls = accept; }
void Ale2gController::setAcceptAnyCalls(bool accept) { m_acceptAnyCalls = accept; }
void Ale2gController::setTxAmplitude(float amplitude) { m_txAmplitude = std::clamp(amplitude, 0.0f, 1.0f); }
void Ale2gController::setRequireTxConfirmation(bool require) { m_requireTxConfirm = require; }
void Ale2gController::setKeyTimeoutMs(int ms) { m_keyTimeoutMs = std::clamp(ms, 100, 10000); }
void Ale2gController::setPttDelayMs(int ms) { m_pttDelayMs = std::clamp(ms, 0, 2000); }
void Ale2gController::setTxTailMs(int ms) { m_txTailMs = std::clamp(ms, 0, 2000); }
void Ale2gController::setMaxTransmitMs(int ms) { m_maxTransmitMs = std::clamp(ms, 5000, 120000); }
void Ale2gController::setTuneSettleMs(int ms) { m_tuneSettleMs = std::clamp(ms, 0, 5000); }

void Ale2gController::setTransmitEnabled(bool enabled)
{
    if (m_txEnabled == enabled) {
        return;
    }
    m_txEnabled = enabled;
    log(enabled ? QStringLiteral("Transmit ENABLED by user.")
                : QStringLiteral("Transmit disabled."));
    if (!enabled && m_txPhase != TxPhase::None) {
        emergencyUnkey(QStringLiteral("Transmit disabled by user during TX; aborted."));
        if (m_state == State::Calling || m_state == State::Responding
            || m_state == State::Sounding) {
            resumeAvailable();
        }
    }
}

void Ale2gController::clearHeardStations()
{
    m_heard.clear();
    emit heardStationsChanged();
}

// ─── Helpers ───────────────────────────────────────────────────────────────

void Ale2gController::setState(State s, const QString& remote)
{
    if (s == m_state && remote == m_remote) {
        return;
    }
    m_state = s;
    m_remote = remote;
    emit stateChanged(m_state, m_remote);
}

void Ale2gController::resumeAvailable()
{
    m_callQueue.clear();
    m_soundQueue.clear();
    m_holdUntilMs = 0;
    m_dwellEndMs = now() + m_dwellMs;
    setState(m_running ? State::Scanning : State::Idle, QString());
}

void Ale2gController::log(const QString& text)
{
    emit logMessage(text);
}

void Ale2gController::fail(const QString& text)
{
    emit logMessage(text);
    emit errorOccurred(text);
}

bool Ale2gController::rxActive() const
{
    return m_demod.isLocked() || m_assembler.inFrame()
           || (m_lastRxWordMs > 0 && now() - m_lastRxWordMs < 2 * kTrwMs);
}

int Ale2gController::scanWordsForCall() const
{
    if (m_scanCallMs > 0) {
        return Frames::wordsForDurationMs(m_scanCallMs);
    }
    return Frames::scanWordsForScan(std::max<int>(1, m_channels.size()), m_dwellMs);
}

int Ale2gController::soundWords() const
{
    const int words = m_soundLengthMs > 0
                          ? Frames::wordsForDurationMs(m_soundLengthMs)
                          : Frames::scanWordsForScan(std::max<int>(1, m_channels.size()), m_dwellMs);
    return std::min(words, kMaxSoundWords);
}

QString Ale2gController::matchSelf(const QStringList& toAddresses) const
{
    for (const QString& to : toAddresses) {
        if (isAllCallAddress(to) || isAnyCallAddress(to)) {
            continue;
        }
        for (const QString& self : m_selfAddresses) {
            if (addressMatchesSelf(to, self)) {
                return self;
            }
        }
    }
    return {};
}

void Ale2gController::tuneToChannel(int index)
{
    if (index < 0 || index >= m_channels.size()) {
        return;
    }
    m_channelIndex = index;
    m_demod.reset();
    m_assembler.reset();
    m_lastRxWordMs = 0;
    m_rxBlankUntilMs = now() + kRxBlankAfterTuneMs;
    const qint64 hz = m_channels.at(index).frequencyHz;
    emit requestTune(hz);
    emit channelChanged(index, hz);
}

// ─── Public operations ─────────────────────────────────────────────────────

void Ale2gController::start()
{
    m_running = true;
    if (!m_channels.isEmpty()) {
        const int idx = (m_channelIndex >= 0 && m_channelIndex < m_channels.size()) ? m_channelIndex : 0;
        tuneToChannel(idx);
    }
    m_dwellEndMs = now() + m_dwellMs;
    if (m_soundIntervalMin > 0) {
        m_nextSoundMs = now() + static_cast<qint64>(m_soundIntervalMin) * 60000;
    }
    if (m_selfAddresses.isEmpty()) {
        log(QStringLiteral("Warning: no self address configured; calls cannot be answered."));
    }
    log(QStringLiteral("ALE started: %1 channel(s), dwell %2 ms.")
            .arg(m_channels.size())
            .arg(m_dwellMs));
    if (m_state == State::Idle) {
        setState(State::Scanning);
    }
}

void Ale2gController::stop()
{
    m_running = false;
    if (m_txPhase != TxPhase::None) {
        emergencyUnkey(QStringLiteral("ALE stopped; transmission aborted."));
    }
    if (m_state == State::Linked) {
        emit linkTerminated(m_remote, QStringLiteral("stopped"));
    }
    resumeAvailable();
    log(QStringLiteral("ALE stopped."));
}

bool Ale2gController::soundNow()
{
    if (!m_txEnabled) {
        log(QStringLiteral("Transmit disabled: sound not sent."));
        return false;
    }
    if (m_selfAddresses.isEmpty()) {
        fail(QStringLiteral("Cannot sound: no self address."));
        return false;
    }
    if (m_txPhase != TxPhase::None || m_state == State::Calling || m_state == State::Responding
        || m_state == State::Linked || m_state == State::Sounding) {
        log(QStringLiteral("Busy: sound not sent."));
        return false;
    }
    TxJob job;
    job.kind = JobKind::Sound;
    job.words = Frames::buildSound(m_selfAddresses.first(), m_soundTis, soundWords());
    job.channelIndex = -1;
    job.lbtMs = m_lbtMs;
    job.notBeforeMs = now();
    job.description = QStringLiteral("%1 sound %2")
                          .arg(m_soundTis ? QStringLiteral("TIS") : QStringLiteral("TWAS"),
                               m_selfAddresses.first());
    m_soundQueue.clear();
    if (!queueJob(job)) {
        return false;
    }
    m_deadlineMs = 0;
    setState(State::Sounding);
    return true;
}

QVector<int> Ale2gController::callChannelOrder(const QString& address) const
{
    QVector<int> order;
    if (m_channels.isEmpty()) {
        order << -1;
        return order;
    }
    if (m_channelIndex < 0) {
        order << -1;   // manually tuned: try where we are first
    }

    // LQA: best recent score for this station first (A.5.4.5.1, simplified to
    // our own receive measurements).
    QVector<const Ale2gHeardStation*> entries;
    for (const Ale2gHeardStation& h : m_heard) {
        if (h.address == address && h.channelIndex >= 0 && h.channelIndex < m_channels.size()) {
            entries << &h;
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Ale2gHeardStation* a, const Ale2gHeardStation* b) {
        return (a->snrDb - 0.5f * a->berCode) > (b->snrDb - 0.5f * b->berCode);
    });
    for (const Ale2gHeardStation* h : entries) {
        if (!order.contains(h->channelIndex)) {
            order << h->channelIndex;
        }
    }
    if (m_channelIndex >= 0 && !order.contains(m_channelIndex)) {
        order << m_channelIndex;
    }
    for (int i = 0; i < m_channels.size(); ++i) {
        if (!order.contains(i)) {
            order << i;
        }
    }
    while (order.size() > m_maxCallAttempts) {
        order.removeLast();
    }
    return order;
}

bool Ale2gController::call(const QString& address, const QString& amdText)
{
    const QString addr = normalizeAddress(address);
    if (!m_txEnabled) {
        log(QStringLiteral("Transmit disabled: call to %1 not sent.").arg(addr));
        return false;
    }
    if (!isValidAddress(addr) || addr.startsWith(QLatin1Char('@')) || addr.contains(QLatin1Char('?'))) {
        fail(QStringLiteral("Invalid ALE address \"%1\".").arg(address));
        return false;
    }
    if (m_selfAddresses.isEmpty()) {
        fail(QStringLiteral("Cannot call: no self address."));
        return false;
    }
    if (m_txPhase != TxPhase::None || m_state == State::Calling || m_state == State::Responding
        || m_state == State::Linked || m_state == State::Sounding) {
        log(QStringLiteral("Busy: call to %1 not sent.").arg(addr));
        return false;
    }
    m_callAddress = addr;
    m_callAmd = amdText;
    m_callIsAllCall = false;
    m_callQueue = callChannelOrder(addr);
    m_linkSelf = m_selfAddresses.first();
    setState(State::Calling, addr);
    if (!queueNextCallAttempt()) {
        resumeAvailable();
        return false;
    }
    return true;
}

bool Ale2gController::allCall(const QString& amdText)
{
    if (!m_txEnabled) {
        log(QStringLiteral("Transmit disabled: AllCall not sent."));
        return false;
    }
    if (m_selfAddresses.isEmpty()) {
        fail(QStringLiteral("Cannot AllCall: no self address."));
        return false;
    }
    if (m_txPhase != TxPhase::None || m_state == State::Calling || m_state == State::Responding
        || m_state == State::Linked || m_state == State::Sounding) {
        log(QStringLiteral("Busy: AllCall not sent."));
        return false;
    }
    m_callAddress = QStringLiteral("@?@");
    m_callAmd = amdText;
    m_callIsAllCall = true;
    m_callQueue.clear();
    m_callQueue << (m_channelIndex >= 0 ? m_channelIndex : -1);
    m_linkSelf = m_selfAddresses.first();
    setState(State::Calling, QStringLiteral("ALLCALL"));
    if (!queueNextCallAttempt()) {
        resumeAvailable();
        return false;
    }
    return true;
}

bool Ale2gController::queueNextCallAttempt()
{
    while (!m_callQueue.isEmpty()) {
        const int idx = m_callQueue.takeFirst();
        TxJob job;
        job.kind = m_callIsAllCall ? JobKind::AllCall : JobKind::Call;
        job.words = m_callIsAllCall
                        ? Frames::buildAllCall(m_linkSelf, true, scanWordsForCall(), m_callAmd)
                        : Frames::buildCall(m_callAddress, m_linkSelf, true, scanWordsForCall(), m_callAmd);
        job.remote = m_callAddress;
        job.channelIndex = idx;
        job.lbtMs = m_lbtMs;
        job.notBeforeMs = now();
        const QString where = idx >= 0 ? m_channels.at(idx).name : QStringLiteral("current frequency");
        job.description = m_callIsAllCall
                              ? QStringLiteral("AllCall from %1 on %2").arg(m_linkSelf, where)
                              : QStringLiteral("Call %1 from %2 on %3").arg(m_callAddress, m_linkSelf, where);
        if (queueJob(job)) {
            return true;
        }
        if (!m_txEnabled) {
            return false;
        }
    }
    return false;
}

bool Ale2gController::sendAmd(const QString& text)
{
    if (!m_txEnabled) {
        log(QStringLiteral("Transmit disabled: AMD not sent."));
        return false;
    }
    if (m_state != State::Linked || m_remote.isEmpty() || m_txPhase != TxPhase::None) {
        log(QStringLiteral("AMD requires an established link (use Call with AMD otherwise)."));
        return false;
    }
    if (normalizeAmdText(text).isEmpty()) {
        return false;
    }
    TxJob job;
    job.kind = JobKind::Amd;
    job.words = Frames::buildResponse(m_remote, m_linkSelf, true, text);
    job.remote = m_remote;
    job.notBeforeMs = now();
    job.description = QStringLiteral("AMD to %1: %2").arg(m_remote, normalizeAmdText(text));
    return queueJob(job);
}

void Ale2gController::terminateLink()
{
    if (m_state != State::Linked && m_state != State::Calling && m_state != State::Responding) {
        return;
    }
    const QString remote = m_remote;
    if (m_state == State::Linked && m_txEnabled && m_txPhase == TxPhase::None
        && !remote.isEmpty() && remote != QStringLiteral("ALLCALL")) {
        TxJob job;
        job.kind = JobKind::Terminate;
        job.words = Frames::buildResponse(remote, m_linkSelf, false);
        job.remote = remote;
        job.notBeforeMs = now();
        job.description = QStringLiteral("Terminate link with %1 (TWAS)").arg(remote);
        if (queueJob(job)) {
            return;
        }
    }
    if (m_txPhase != TxPhase::None) {
        emergencyUnkey(QStringLiteral("Link terminated; transmission aborted."));
    }
    emit linkTerminated(remote, QStringLiteral("terminated locally"));
    log(QStringLiteral("Link with %1 terminated locally.").arg(remote));
    resumeAvailable();
}

void Ale2gController::abortTransmit(const QString& reason)
{
    if (m_txPhase == TxPhase::None && !m_keyRequested) {
        return;
    }
    emergencyUnkey(reason.isEmpty() ? QStringLiteral("Transmission aborted.") : reason);
    if (m_state != State::Linked) {
        resumeAvailable();
    }
}

void Ale2gController::setRadioTransmitting(bool transmitting)
{
    m_radioTransmitting = transmitting;
}

// ─── Transmit machinery ────────────────────────────────────────────────────

bool Ale2gController::queueJob(TxJob job)
{
    if (!m_txEnabled) {
        log(QStringLiteral("Transmit disabled: %1 not sent.").arg(job.description));
        return false;
    }
    if (m_txPhase != TxPhase::None) {
        return false;
    }
    if (job.words.isEmpty()) {
        fail(QStringLiteral("Cannot build ALE frame for: %1").arg(job.description));
        return false;
    }
    const qint64 durationMs = static_cast<qint64>(job.words.size()) * kTrwMs;
    if (durationMs + m_pttDelayMs + m_txTailMs > m_maxTransmitMs) {
        fail(QStringLiteral("Frame too long (%1 s) for TX watchdog: %2")
                 .arg(durationMs / 1000.0, 0, 'f', 1)
                 .arg(job.description));
        return false;
    }
    m_job = std::move(job);
    m_txPhase = TxPhase::Pending;
    log(QStringLiteral("Queued: %1 (%2 words, %3 s)")
            .arg(m_job.description)
            .arg(m_job.words.size())
            .arg(durationMs / 1000.0, 0, 'f', 1));
    return true;
}

void Ale2gController::emergencyUnkey(const QString& reason)
{
    const bool wasKeyed = m_keyRequested;
    m_keyRequested = false;
    m_txPhase = TxPhase::None;
    m_txSamples.clear();
    m_txPos = 0;
    if (wasKeyed) {
        m_lastUnkeyMs = now();
        emit requestTransmit(false);
        emit transmitActiveChanged(false);
    }
    m_rxBlankUntilMs = now() + kRxBlankAfterTxMs;
    m_demod.reset();
    m_assembler.reset();
    fail(reason);
}

void Ale2gController::startKeying(qint64 t)
{
    if (!m_txEnabled) {
        emergencyUnkey(QStringLiteral("Transmit disabled: not keying."));
        resumeAvailable();
        return;
    }
    if (radioBusyWithForeignTx(t)) {
        abandonJobRadioBusy();
        return;
    }
    Modulator::Options options;
    options.amplitude = m_txAmplitude;
    m_txSamples = Modulator::modulate(m_job.words, options);
    m_txPos = 0;
    m_demod.reset();
    m_assembler.reset();
    m_lastRxWordMs = 0;

    m_keyRequested = true;
    m_keyRequestMs = t;
    log(QStringLiteral("TX: %1").arg(m_job.description));
    emit transmitActiveChanged(true);
    emit requestTransmit(true);
    if (!m_keyRequested) {
        return;   // aborted from a slot connected to requestTransmit
    }
    if (m_requireTxConfirm) {
        m_txPhase = TxPhase::WaitKey;
        m_phaseDeadlineMs = t + m_keyTimeoutMs;
    } else {
        m_txPhase = TxPhase::PreAudio;
        m_phaseDeadlineMs = t + m_pttDelayMs;
    }
}

void Ale2gController::jobBusy(qint64 t)
{
    Q_UNUSED(t);
    const JobKind kind = m_job.kind;
    log(QStringLiteral("Channel busy (ALE activity): %1 deferred.").arg(m_job.description));
    m_txPhase = TxPhase::None;
    if (kind == JobKind::Call || kind == JobKind::AllCall) {
        if (!queueNextCallAttempt()) {
            fail(QStringLiteral("Call to %1 not sent: channel(s) busy.").arg(m_callAddress));
            resumeAvailable();
        }
    }
    // Sound: the Sounding state moves on to the next channel.
}

bool Ale2gController::radioBusyWithForeignTx(qint64 t) const
{
    // The radio is transmitting although we have not keyed it (operator MOX,
    // another Multi-Flex client, DAX/CAT PTT). Keying now would put ALE audio
    // on top of that transmission and our unkey would cut it off.
    return m_radioTransmitting && !m_keyRequested && t - m_lastUnkeyMs > kOwnUnkeyGraceMs;
}

void Ale2gController::abandonJobRadioBusy()
{
    const TxJob job = m_job;
    m_txPhase = TxPhase::None;
    fail(QStringLiteral("Radio is already transmitting: %1 not sent.").arg(job.description));
    switch (job.kind) {
    case JobKind::Amd:
        break;   // link stays up; the user can resend
    case JobKind::Terminate:
        emit linkTerminated(job.remote, QStringLiteral("terminated locally"));
        resumeAvailable();
        break;
    case JobKind::Sound:
    case JobKind::Call:
    case JobKind::AllCall:
    case JobKind::Response:
    case JobKind::Ack:
        if (m_state != State::Linked) {
            resumeAvailable();
        }
        break;
    }
}

void Ale2gController::finishJob(qint64 t)
{
    m_keyRequested = false;
    m_lastUnkeyMs = t;
    emit requestTransmit(false);
    emit transmitActiveChanged(false);
    m_txPhase = TxPhase::None;
    m_txSamples.clear();
    m_txPos = 0;
    m_rxBlankUntilMs = t + kRxBlankAfterTxMs;
    m_demod.reset();
    m_assembler.reset();
    m_lastRxWordMs = 0;

    const TxJob job = m_job;
    switch (job.kind) {
    case JobKind::Sound:
        m_deadlineMs = t + (m_soundTis ? m_replyTimeoutMs : 0);
        break;
    case JobKind::Call:
        m_deadlineMs = t + m_replyTimeoutMs;
        log(QStringLiteral("Waiting for response from %1...").arg(job.remote));
        break;
    case JobKind::AllCall:
        log(QStringLiteral("AllCall sent."));
        resumeAvailable();
        m_dwellEndMs = t + std::max(m_dwellMs, m_replyTimeoutMs * 2);
        break;
    case JobKind::Response:
        m_deadlineMs = t + m_replyTimeoutMs;
        log(QStringLiteral("Response sent; waiting for acknowledgment from %1...").arg(job.remote));
        break;
    case JobKind::Ack:
        m_lastActivityMs = t;
        setState(State::Linked, job.remote);
        log(QStringLiteral("LINKED with %1.").arg(job.remote));
        emit linkEstablished(job.remote);
        break;
    case JobKind::Amd:
        m_lastActivityMs = t;
        break;
    case JobKind::Terminate:
        emit linkTerminated(job.remote, QStringLiteral("terminated locally"));
        log(QStringLiteral("Link with %1 terminated.").arg(job.remote));
        resumeAvailable();
        break;
    }
}

void Ale2gController::processTx(qint64 t)
{
    switch (m_txPhase) {
    case TxPhase::None:
        return;
    case TxPhase::Pending:
        if (t < m_job.notBeforeMs) {
            return;
        }
        if (radioBusyWithForeignTx(t)) {
            abandonJobRadioBusy();   // before retuning a slice that is on the air
            return;
        }
        if (m_job.channelIndex >= 0 && m_job.channelIndex != m_channelIndex) {
            tuneToChannel(m_job.channelIndex);
            m_phaseDeadlineMs = t + m_tuneSettleMs;
        } else {
            m_phaseDeadlineMs = t;
        }
        m_txPhase = TxPhase::Tuning;
        return;
    case TxPhase::Tuning:
        if (t < m_phaseDeadlineMs) {
            return;
        }
        if (m_job.lbtMs > 0) {
            m_txPhase = TxPhase::Listening;
            m_phaseDeadlineMs = t + m_job.lbtMs;
            return;
        }
        startKeying(t);
        return;
    case TxPhase::Listening:
        if (rxActive()) {
            jobBusy(t);
            return;
        }
        if (t >= m_phaseDeadlineMs) {
            startKeying(t);
        }
        return;
    case TxPhase::WaitKey:
        if (m_radioTransmitting) {
            m_txPhase = TxPhase::PreAudio;
            m_phaseDeadlineMs = t + m_pttDelayMs;
            return;
        }
        if (t >= m_phaseDeadlineMs) {
            emergencyUnkey(QStringLiteral("Radio did not confirm transmit within %1 ms; unkeyed.")
                               .arg(m_keyTimeoutMs));
            resumeAvailable();
        }
        return;
    case TxPhase::PreAudio:
        if (m_requireTxConfirm && !m_radioTransmitting) {
            emergencyUnkey(QStringLiteral("Radio left transmit before ALE audio; aborted."));
            resumeAvailable();
            return;
        }
        if (t < m_phaseDeadlineMs) {
            return;
        }
        m_txStartMs = t;
        m_txPos = 0;
        m_txPhase = TxPhase::Sending;
        [[fallthrough]];
    case TxPhase::Sending: {
        if (m_requireTxConfirm && !m_radioTransmitting) {
            emergencyUnkey(QStringLiteral("Radio left transmit during ALE frame; aborted."));
            resumeAvailable();
            return;
        }
        const qint64 elapsed = t - m_txStartMs;
        const size_t total = m_txSamples.size();
        const qint64 dueSamples = (elapsed + kTxLeadMs) * kSampleRate / 1000;
        const size_t due = static_cast<size_t>(std::clamp<qint64>(dueSamples, 0, static_cast<qint64>(total)));
        if (due > m_txPos) {
            const QByteArray chunk = Modulator::toStereoFloat32(m_txSamples.data() + m_txPos,
                                                                static_cast<int>(due - m_txPos));
            m_txPos = due;
            emit txAudio(chunk);
            if (m_txPhase != TxPhase::Sending) {
                return;   // aborted re-entrantly
            }
        }
        const qint64 totalMs = static_cast<qint64>(total) * 1000 / kSampleRate;
        if (m_txPos >= total && elapsed >= totalMs + m_txTailMs) {
            finishJob(t);
        }
        return;
    }
    }
}

// ─── Scanning / sounding ───────────────────────────────────────────────────

void Ale2gController::startScheduledSounding(qint64 t)
{
    m_nextSoundMs = t + static_cast<qint64>(m_soundIntervalMin) * 60000;
    m_soundQueue.clear();
    for (int i = 0; i < m_channels.size(); ++i) {
        if (m_channels.at(i).sound) {
            m_soundQueue << i;
        }
    }
    if (m_soundQueue.isEmpty()) {
        log(QStringLiteral("Scheduled sounding skipped: no channels are flagged for sounding."));
        return;
    }
    if (m_selfAddresses.isEmpty()) {
        log(QStringLiteral("Scheduled sounding skipped: no self address."));
        m_soundQueue.clear();
        return;
    }
    m_deadlineMs = 0;
    setState(State::Sounding);
    if (!queueNextSound()) {
        resumeAvailable();
    }
}

bool Ale2gController::queueNextSound()
{
    while (!m_soundQueue.isEmpty()) {
        const int idx = m_soundQueue.takeFirst();
        TxJob job;
        job.kind = JobKind::Sound;
        job.words = Frames::buildSound(m_selfAddresses.value(0), m_soundTis, soundWords());
        job.channelIndex = idx;
        job.lbtMs = m_lbtMs;
        job.notBeforeMs = now();
        job.description = QStringLiteral("%1 sound %2 on %3")
                              .arg(m_soundTis ? QStringLiteral("TIS") : QStringLiteral("TWAS"),
                                   m_selfAddresses.value(0),
                                   idx >= 0 && idx < m_channels.size() ? m_channels.at(idx).name
                                                                       : QStringLiteral("current"));
        if (queueJob(job)) {
            return true;
        }
        if (!m_txEnabled) {
            m_soundQueue.clear();
            return false;
        }
    }
    return false;
}

void Ale2gController::scanTick(qint64 t)
{
    if (m_soundIntervalMin > 0 && m_txEnabled && t >= m_nextSoundMs && !rxActive()) {
        startScheduledSounding(t);
        if (m_state == State::Sounding) {
            return;
        }
    }
    if (m_channels.size() <= 1) {
        return;
    }

    bool hold = rxActive();
    if (hold && m_assembler.inFrame()) {
        // Stop scan only for calls that might be for us (A.5.5.3.2); keep
        // reading sounds and frames without a calling cycle.
        const QStringList& firsts = m_assembler.partialToFirstWords();
        if (!firsts.isEmpty()) {
            bool forUs = false;
            for (const QString& first : firsts) {
                if (first.contains(QLatin1Char('?'))
                    || (m_acceptAllCalls && isAllCallAddress(first))
                    || (m_acceptAnyCalls && isAnyCallAddress(first))) {
                    forUs = true;
                    break;
                }
                for (const QString& self : m_selfAddresses) {
                    if (wordChars(addressFirstWord(Preamble::To, self)) == first) {
                        forUs = true;
                        break;
                    }
                }
                if (forUs) {
                    break;
                }
            }
            hold = forUs;
        }
    }
    if (hold) {
        if (m_holdUntilMs == 0) {
            m_holdUntilMs = t + kMaxChannelHoldMs;
        }
        if (t < m_holdUntilMs) {
            return;
        }
    }
    if (t < m_dwellEndMs) {
        return;
    }
    m_holdUntilMs = 0;
    tuneToChannel((m_channelIndex + 1) % static_cast<int>(m_channels.size()));
    m_dwellEndMs = t + m_dwellMs;
}

void Ale2gController::tick()
{
    const qint64 t = now();

    if (m_keyRequested && t - m_keyRequestMs > static_cast<qint64>(m_maxTransmitMs) + m_keyTimeoutMs) {
        emergencyUnkey(QStringLiteral("TX watchdog expired; unkeyed."));
        resumeAvailable();
        return;
    }
    if (m_txPhase != TxPhase::None) {
        processTx(t);
        return;
    }

    switch (m_state) {
    case State::Idle:
        break;
    case State::Scanning:
        scanTick(t);
        break;
    case State::Sounding:
        if (t < m_deadlineMs || rxActive()) {
            break;
        }
        if (!queueNextSound()) {
            resumeAvailable();
        }
        break;
    case State::Calling:
    case State::Responding:
        if (t < m_deadlineMs) {
            break;
        }
        if (rxActive() && t < m_deadlineMs + kMaxReplyReadMs) {
            break;   // a frame is being read; wait for its conclusion
        }
        if (m_state == State::Calling) {
            log(QStringLiteral("No response from %1.").arg(m_callAddress));
            if (!queueNextCallAttempt()) {
                fail(QStringLiteral("Call to %1 failed: no response.").arg(m_callAddress));
                resumeAvailable();
            }
        } else {
            fail(QStringLiteral("No acknowledgment from %1; handshake abandoned.").arg(m_remote));
            resumeAvailable();
        }
        break;
    case State::Linked:
        if (m_linkTimeoutMs > 0 && t - m_lastActivityMs > m_linkTimeoutMs) {
            const QString remote = m_remote;
            log(QStringLiteral("Link with %1 timed out (no activity).").arg(remote));
            emit linkTerminated(remote, QStringLiteral("activity timeout"));
            resumeAvailable();
        }
        break;
    }
}

// ─── Receive ───────────────────────────────────────────────────────────────

void Ale2gController::feedRxAudio(const QByteArray& pcm24kStereoFloat32)
{
    const int frames = pcm24kStereoFloat32.size() / static_cast<int>(2 * sizeof(float));
    if (frames <= 0) {
        return;
    }
    const auto* src = reinterpret_cast<const float*>(pcm24kStereoFloat32.constData());
    std::vector<float> mono(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        mono[static_cast<size_t>(i)] = 0.5f * (src[2 * i] + src[2 * i + 1]);
    }
    feedRxMono(mono.data(), frames);
}

void Ale2gController::feedRxMono(const float* samples, int count)
{
    if (m_keyRequested || now() < m_rxBlankUntilMs || count <= 0) {
        return;
    }
    std::vector<Demodulator::Event> events;
    m_demod.process(samples, count, &events);
    handleDemodEvents(events);
}

void Ale2gController::handleDemodEvents(const std::vector<Demodulator::Event>& events)
{
    for (const Demodulator::Event& e : events) {
        if (e.type == Demodulator::Event::Type::Word) {
            m_lastRxWordMs = now();
            emit wordReceived(wordToString(e.word.word), e.word.nonUnanimous, e.word.snrDb);
            if (auto f = m_assembler.addWord(e.word)) {
                handleFrame(*f);
            }
        } else {
            if (auto f = m_assembler.flush()) {
                handleFrame(*f);
            }
        }
    }
}

void Ale2gController::updateHeard(const Frame& frame, const QString& kind)
{
    const QString& address = frame.fromAddress;
    Ale2gHeardStation* entry = nullptr;
    for (Ale2gHeardStation& h : m_heard) {
        if (h.address == address && h.channelIndex == m_channelIndex) {
            entry = &h;
            break;
        }
    }
    if (!entry) {
        Ale2gHeardStation h;
        h.address = address;
        h.channelIndex = m_channelIndex;
        m_heard.push_back(h);
        entry = &m_heard.last();
    }
    entry->frequencyHz = (m_channelIndex >= 0 && m_channelIndex < m_channels.size())
                             ? m_channels.at(m_channelIndex).frequencyHz
                             : 0;
    entry->lastHeardUtc = QDateTime::currentDateTimeUtc();
    entry->snrDb = frame.meanSnrDb();
    entry->berCode = frame.berCode();
    entry->count += 1;
    entry->lastKind = kind;
    emit heardStationsChanged();
}

void Ale2gController::handleFrame(const Frame& frame)
{
    const QString self = matchSelf(frame.toAddresses);

    bool allMatch = false;
    if (frame.allCall && m_acceptAllCalls) {
        bool selective = false;
        for (const QString& a : frame.toAddresses) {
            if (!isAllCallAddress(a)) {
                continue;
            }
            selective = true;
            for (const QString& s : m_selfAddresses) {
                allMatch = allMatch || addressMatchesSelf(a, s);
            }
        }
        allMatch = allMatch || !selective;
    }
    bool anyMatch = false;
    if (frame.anyCall && m_acceptAnyCalls) {
        bool selective = false;
        for (const QString& a : frame.toAddresses) {
            if (!isAnyCallAddress(a)) {
                continue;
            }
            selective = true;
            for (const QString& s : m_selfAddresses) {
                anyMatch = anyMatch || addressMatchesSelf(a, s);
            }
        }
        anyMatch = anyMatch || !selective;
    }

    QString kind;
    if (frame.isSound()) {
        kind = frame.tis ? QStringLiteral("TIS sound") : QStringLiteral("TWAS sound");
    } else if (frame.allCall) {
        kind = QStringLiteral("AllCall");
    } else if (frame.anyCall) {
        kind = QStringLiteral("AnyCall");
    } else if (!self.isEmpty()) {
        kind = QStringLiteral("call to %1").arg(self);
    } else {
        kind = QStringLiteral("call to %1").arg(frame.toAddresses.join(QLatin1Char(',')));
    }

    const QString where = (m_channelIndex >= 0 && m_channelIndex < m_channels.size())
                              ? m_channels.at(m_channelIndex).name
                              : QStringLiteral("current");
    emit frameReceived(QStringLiteral("[%1] %2").arg(where, frame.summary()));

    if (frame.complete && !frame.fromAddress.isEmpty()) {
        updateHeard(frame, kind);
    }
    for (const QString& amd : frame.amdMessages) {
        emit amdReceived(frame.fromAddress.isEmpty() ? QStringLiteral("?") : frame.fromAddress, amd);
    }

    if (!frame.complete || frame.fromAddress.isEmpty()
        || m_selfAddresses.contains(frame.fromAddress)) {
        return;
    }
    const QString from = frame.fromAddress;
    const qint64 t = now();

    switch (m_state) {
    case State::Idle:
        if (!self.isEmpty()) {
            log(QStringLiteral("Call from %1 to %2 received while ALE is stopped (not answered).")
                    .arg(from, self));
        }
        return;

    case State::Scanning:
    case State::Sounding:
        if (m_txPhase != TxPhase::None) {
            return;
        }
        if (!self.isEmpty()) {
            if (frame.twas) {
                log(QStringLiteral("%1 called %2 with TWAS (no response requested).").arg(from, self));
                return;
            }
            if (!m_respondToCalls) {
                log(QStringLiteral("Call from %1 to %2 (auto-response disabled).").arg(from, self));
                return;
            }
            if (!m_txEnabled) {
                log(QStringLiteral("Call from %1 to %2 received: transmit disabled, not responding.")
                        .arg(from, self));
                return;
            }
            TxJob job;
            job.kind = JobKind::Response;
            job.words = Frames::buildResponse(from, self, true);
            job.remote = from;
            job.notBeforeMs = t + kTrwMs;   // Tlww (A.5.5.3.2)
            job.description = QStringLiteral("Response to %1 from %2").arg(from, self);
            m_soundQueue.clear();
            m_linkSelf = self;
            setState(State::Responding, from);
            m_deadlineMs = t + 24L * 3600L * 1000L;   // re-armed when the response is sent
            if (!queueJob(job)) {
                resumeAvailable();
            }
            return;
        }
        if (allMatch) {
            if (frame.tis) {
                m_linkSelf = m_selfAddresses.value(0);
                m_lastActivityMs = t;
                setState(State::Linked, from);
                log(QStringLiteral("AllCall from %1: linked (listening, no response sent).").arg(from));
                emit linkEstablished(from);
            } else {
                log(QStringLiteral("AllCall from %1 (TWAS).").arg(from));
            }
            return;
        }
        if (anyMatch && frame.tis && m_respondToCalls && m_txEnabled && !m_selfAddresses.isEmpty()) {
            // A.5.5.4.5: random slot 1..16, 20 Tw slots.
            const int slot = QRandomGenerator::global()->bounded(1, kAnyCallSlots + 1);
            const qint64 slotMs = static_cast<qint64>(slot) * kAnyCallSlotTw * kTrwMs / 3;
            TxJob job;
            job.kind = JobKind::Response;
            job.words = Frames::buildResponse(from, m_selfAddresses.first(), true);
            job.remote = from;
            job.notBeforeMs = t + kTrwMs + slotMs;
            job.description = QStringLiteral("AnyCall response to %1 (slot %2)").arg(from).arg(slot);
            m_linkSelf = m_selfAddresses.first();
            setState(State::Responding, from);
            m_deadlineMs = t + 24L * 3600L * 1000L;
            if (!queueJob(job)) {
                resumeAvailable();
            }
        }
        return;

    case State::Calling:
        if (m_callIsAllCall || self.isEmpty() || from != m_callAddress) {
            return;
        }
        if (frame.twas) {
            fail(QStringLiteral("%1 rejected the call (TWAS).").arg(from));
            emit linkTerminated(from, QStringLiteral("rejected"));
            resumeAvailable();
            return;
        }
        if (m_txPhase != TxPhase::None) {
            return;
        }
        {
            m_linkSelf = self;
            TxJob job;
            job.kind = JobKind::Ack;
            job.words = Frames::buildResponse(from, self, true);
            job.remote = from;
            job.notBeforeMs = t + kTrwMs;   // Tlww
            job.description = QStringLiteral("Acknowledgment to %1").arg(from);
            m_deadlineMs = t + 24L * 3600L * 1000L;
            if (!queueJob(job)) {
                fail(QStringLiteral("Could not send acknowledgment to %1.").arg(from));
                resumeAvailable();
            }
        }
        return;

    case State::Responding:
        if (self.isEmpty() || from != m_remote) {
            return;
        }
        if (frame.twas) {
            log(QStringLiteral("%1 ended the handshake (TWAS).").arg(from));
            emit linkTerminated(from, QStringLiteral("rejected"));
            resumeAvailable();
            return;
        }
        m_lastActivityMs = t;
        setState(State::Linked, from);
        log(QStringLiteral("LINKED with %1.").arg(from));
        emit linkEstablished(from);
        return;

    case State::Linked:
        if (from != m_remote) {
            if (!self.isEmpty()) {
                log(QStringLiteral("Call from %1 while linked with %2 (not answered).").arg(from, m_remote));
            }
            return;
        }
        m_lastActivityMs = t;
        if (frame.twas && (!self.isEmpty() || frame.toAddresses.contains(QStringLiteral("?@?")))) {
            log(QStringLiteral("Link terminated by %1.").arg(from));
            emit linkTerminated(from, QStringLiteral("terminated by remote"));
            resumeAvailable();
        }
        return;
    }
}

} // namespace AetherSDR
