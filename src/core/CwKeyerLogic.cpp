#include "CwKeyerLogic.h"

#include <algorithm>

namespace AetherSDR {

// ── Settings string mapping ─────────────────────────────────────────────────

QString cwKeyerModeToString(CwKeyerMode mode)
{
    switch (mode) {
    case CwKeyerMode::Radio:     return QStringLiteral("Radio");
    case CwKeyerMode::IambicA:   return QStringLiteral("IambicA");
    case CwKeyerMode::IambicB:   return QStringLiteral("IambicB");
    case CwKeyerMode::Ultimatic: return QStringLiteral("Ultimatic");
    case CwKeyerMode::Bug:       return QStringLiteral("Bug");
    case CwKeyerMode::Straight:  return QStringLiteral("Straight");
    }
    return QStringLiteral("Radio");
}

CwKeyerMode cwKeyerModeFromString(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("ultimatic")) { return CwKeyerMode::Ultimatic; }
    if (v == QLatin1String("bug"))       { return CwKeyerMode::Bug; }
    if (v == QLatin1String("straight"))  { return CwKeyerMode::Straight; }
    if (v == QLatin1String("iambica"))   { return CwKeyerMode::IambicA; }
    if (v == QLatin1String("iambicb"))   { return CwKeyerMode::IambicB; }
    return CwKeyerMode::Radio;
}

CwKeyerMode settingsDispatchMode(CwKeyerMode stored)
{
    // Iambic A/B are handled by the radio keyer (its iambic_mode is
    // radio-authoritative); keep the existing sendCwPaddle() path.
    if (stored == CwKeyerMode::IambicA || stored == CwKeyerMode::IambicB) {
        return CwKeyerMode::Radio;
    }
    return stored;
}

// ── CwKeyerLogic ────────────────────────────────────────────────────────────

void CwKeyerLogic::setWpm(int wpm)
{
    m_wpm = std::clamp(wpm, 5, 100);
}

void CwKeyerLogic::setWeight(int weight)
{
    m_weight = std::clamp(weight, 25, 75);
}

std::vector<CwKeyerLogic::KeyEvent> CwKeyerLogic::takeEvents()
{
    std::vector<KeyEvent> out;
    out.swap(m_events);
    return out;
}

std::int64_t CwKeyerLogic::nextEventUs() const
{
    if (m_phase == Phase::Idle) {
        return kNoEvent;
    }
    return m_phaseEndUs;
}

void CwKeyerLogic::updateOutput(std::int64_t atUs)
{
    const bool out = m_autoKey || m_manualKey;
    if (out != m_output) {
        m_output = out;
        m_events.push_back({atUs, out});
    }
}

void CwKeyerLogic::reset(std::int64_t nowUs)
{
    m_phase = Phase::Idle;
    m_current = Element::None;
    m_dit = m_dah = false;
    m_lastPressed = Element::None;
    m_ditEdgeLatch = m_dahEdgeLatch = false;
    m_ditHeldLatch = m_dahHeldLatch = false;
    m_autoKey = false;
    m_manualKey = false;
    updateOutput(nowUs);
}

void CwKeyerLogic::startElement(Element e, std::int64_t atUs)
{
    m_current = e;
    m_phase = Phase::Mark;
    m_phaseEndUs = atUs + (e == Element::Dah ? dahOnUs() : ditOnUs());
    m_autoKey = true;

    // Memory window opens at the start of each element.  Paddles already
    // held count toward Mode B squeeze memory but are not "new" presses.
    m_ditEdgeLatch = m_dahEdgeLatch = false;
    m_ditHeldLatch = m_dit;
    m_dahHeldLatch = m_dah;
    updateOutput(atUs);
}

CwKeyerLogic::Element CwKeyerLogic::chooseFromIdle() const
{
    if (m_dit && m_dah) {
        if (m_mode == CwKeyerMode::Ultimatic) {
            return m_lastPressed == Element::None ? Element::Dit : m_lastPressed;
        }
        return Element::Dit;  // simultaneous press: iambic starts with a dit
    }
    if (m_dit) { return Element::Dit; }
    if (m_dah) { return Element::Dah; }
    return Element::None;
}

CwKeyerLogic::Element CwKeyerLogic::chooseNext() const
{
    const Element last = m_current;
    const Element opp = (last == Element::Dit) ? Element::Dah : Element::Dit;
    const bool lastHeld = (last == Element::Dit) ? m_dit : m_dah;
    const bool oppHeld  = (last == Element::Dit) ? m_dah : m_dit;
    const bool lastEdge = (last == Element::Dit) ? m_ditEdgeLatch : m_dahEdgeLatch;
    const bool oppEdge  = (last == Element::Dit) ? m_dahEdgeLatch : m_ditEdgeLatch;
    const bool oppHeldLatch = (last == Element::Dit) ? m_dahHeldLatch : m_ditHeldLatch;

    switch (m_mode) {
    case CwKeyerMode::Bug:
        return m_dit ? Element::Dit : Element::None;

    case CwKeyerMode::Straight:
        return Element::None;

    case CwKeyerMode::Ultimatic:
        // Both held: the most recently pressed paddle repeats.
        if (m_dit && m_dah) {
            return m_lastPressed == Element::None ? last : m_lastPressed;
        }
        // Tap memory: opposite paddle pressed (and maybe released) during
        // the element is sent once.
        if (oppEdge) { return opp; }
        if (m_dit) { return Element::Dit; }
        if (m_dah) { return Element::Dah; }
        if (lastEdge) { return last; }
        return Element::None;

    case CwKeyerMode::IambicA:
        // Alternate while squeezed.  Releasing a squeeze stops after the
        // current element (no squeeze memory); a *new* press of the opposite
        // paddle during the element is still remembered (dit/dah memory).
        if (oppHeld || oppEdge) { return opp; }
        if (lastHeld || lastEdge) { return last; }
        return Element::None;

    case CwKeyerMode::Radio:
    case CwKeyerMode::IambicB:
        // Mode B: opposite paddle held at *any* time during the element
        // (including a squeeze released mid-element) earns one more element.
        if (oppHeld || oppHeldLatch || oppEdge) { return opp; }
        if (lastHeld || lastEdge) { return last; }
        return Element::None;
    }
    return Element::None;
}

void CwKeyerLogic::advance(std::int64_t nowUs)
{
    while (m_phase != Phase::Idle && m_phaseEndUs <= nowUs) {
        const std::int64_t t = m_phaseEndUs;
        if (m_phase == Phase::Mark) {
            m_autoKey = false;
            updateOutput(t);
            m_phase = Phase::Space;
            m_phaseEndUs = t + gapUs();
        } else {
            const Element next = chooseNext();
            if (next != Element::None) {
                startElement(next, t);
            } else {
                m_phase = Phase::Idle;
                m_current = Element::None;
            }
        }
    }
}

void CwKeyerLogic::setPaddles(bool dit, bool dah, std::int64_t nowUs)
{
    advance(nowUs);
    onPaddleEdges(dit, dah, nowUs);
}

void CwKeyerLogic::onPaddleEdges(bool dit, bool dah, std::int64_t nowUs)
{
    const bool ditPressed = dit && !m_dit;
    const bool dahPressed = dah && !m_dah;
    m_dit = dit;
    m_dah = dah;
    if (dahPressed) { m_lastPressed = Element::Dah; }
    if (ditPressed) { m_lastPressed = Element::Dit; }

    if (m_mode == CwKeyerMode::Straight) {
        m_manualKey = dit || dah;
        updateOutput(nowUs);
        return;
    }

    if (m_mode == CwKeyerMode::Bug) {
        m_manualKey = dah;
        if (m_phase == Phase::Idle && dit) {
            startElement(Element::Dit, nowUs);
        }
        updateOutput(nowUs);
        return;
    }

    m_manualKey = false;

    if (m_phase != Phase::Idle) {
        if (ditPressed) { m_ditEdgeLatch = true; }
        if (dahPressed) { m_dahEdgeLatch = true; }
        if (dit) { m_ditHeldLatch = true; }
        if (dah) { m_dahHeldLatch = true; }
        updateOutput(nowUs);
        return;
    }

    const Element first = chooseFromIdle();
    if (first != Element::None) {
        startElement(first, nowUs);
    } else {
        updateOutput(nowUs);
    }
}

// ── CwPaddleKeyer ───────────────────────────────────────────────────────────

CwPaddleKeyer::CwPaddleKeyer(QObject* parent)
    : QObject(parent)
{
    m_clock.start();
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &CwPaddleKeyer::onTimer);
}

CwPaddleKeyer::~CwPaddleKeyer()
{
    m_timer.stop();
}

void CwPaddleKeyer::setMode(CwKeyerMode mode)
{
    if (mode == m_logic.mode()) {
        return;
    }
    // Never carry a keyed element across a mode change.
    abort();
    m_logic.setMode(mode);
}

void CwPaddleKeyer::setWpm(int wpm)
{
    m_logic.setWpm(wpm);
}

void CwPaddleKeyer::setWeight(int weight)
{
    m_logic.setWeight(weight);
}

void CwPaddleKeyer::setPaddles(bool dit, bool dah)
{
    m_logic.setPaddles(dit, dah, nowUs());
    flushAndReschedule();
}

void CwPaddleKeyer::abort()
{
    m_logic.reset(nowUs());
    flushAndReschedule();
}

void CwPaddleKeyer::onTimer()
{
    m_logic.advance(nowUs());
    flushAndReschedule();
}

void CwPaddleKeyer::flushAndReschedule()
{
    for (const CwKeyerLogic::KeyEvent& ev : m_logic.takeEvents()) {
        emit keyChanged(ev.down);
    }

    const std::int64_t next = m_logic.nextEventUs();
    if (next == CwKeyerLogic::kNoEvent) {
        m_timer.stop();
        return;
    }
    const std::int64_t waitUs = std::max<std::int64_t>(0, next - nowUs());
    // Round up so we never wake early and spin; worst case 1 ms late.
    m_timer.start(static_cast<int>((waitUs + 999) / 1000));
}

} // namespace AetherSDR
