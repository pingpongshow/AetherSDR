#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <vector>

namespace AetherSDR {

// ─────────────────────────────────────────────────────────────────────────────
// Client-side paddle keyer.
//
// FLEX firmware (v1.4.0.0 … 4.1.x) implements Iambic A / B (plus B-Strict and
// Bug via `cw mode 2|3`) in the radio, fed by `cw key <dit> <dah>` over the
// netcw stream (RadioModel::sendCwPaddle).  Ultimatic is NOT a radio mode, so
// when the user selects it the paddles must be timed here and the radio is
// driven with straight-key edges (`cw key 1` / `cw key 0`, RadioModel::sendCwKey).
//
// CwKeyerLogic is a pure, clock-agnostic state machine: every call carries the
// current time in microseconds, and it reports key edges stamped with the exact
// time they *should* have happened.  That makes it deterministic under a
// virtual clock (tests/cw_keyer_test.cpp).  CwPaddleKeyer below drives it from
// a monotonic QElapsedTimer + precise QTimer on the GUI thread.
// ─────────────────────────────────────────────────────────────────────────────

enum class CwKeyerMode {
    Radio,      // pass paddles through to the radio's own keyer (legacy path)
    IambicA,    // client-side Iambic A (no squeeze memory; tap memory kept)
    IambicB,    // client-side Iambic B (squeeze memory → one extra element)
    Ultimatic,  // last-pressed paddle wins while both are held
    Bug,        // dit paddle = automatic dits, dah paddle = manual (straight)
    Straight,   // either paddle contact keys directly (cootie / sideswiper)
};

// AppSettings key "CwKeyerMode" ↔ enum.  Stored values:
//   "Radio" (default) | "Ultimatic" | "Bug" | "Straight" | "IambicA" | "IambicB"
// "IambicA"/"IambicB" are accepted, but settingsDispatchMode() maps them to
// Radio: the radio's iambic_mode is radio-authoritative, so client-side
// iambic is only used by tests / future work.
QString     cwKeyerModeToString(CwKeyerMode mode);
CwKeyerMode cwKeyerModeFromString(const QString& s);
CwKeyerMode settingsDispatchMode(CwKeyerMode stored);
inline bool cwKeyerModeIsClientSide(CwKeyerMode m) { return m != CwKeyerMode::Radio; }

class CwKeyerLogic {
public:
    enum class Element { None, Dit, Dah };

    struct KeyEvent {
        std::int64_t timeUs{0};
        bool         down{false};
    };

    static constexpr std::int64_t kNoEvent = INT64_MAX;

    CwKeyerLogic() = default;

    void setMode(CwKeyerMode mode) { m_mode = mode; }
    CwKeyerMode mode() const { return m_mode; }

    // 5–100 WPM (PARIS timing: dit = 1200 ms / wpm).
    void setWpm(int wpm);
    int  wpm() const { return m_wpm; }

    // 25–75, 50 = standard 1:1 mark/space.  Weight lengthens every mark and
    // shortens the following space by the same amount (element period fixed).
    void setWeight(int weight);
    int  weight() const { return m_weight; }

    // Paddle state change at time nowUs.  Processes any due timeline events
    // first, then applies the new paddle state.
    void setPaddles(bool dit, bool dah, std::int64_t nowUs);

    // Process all timeline events due at or before nowUs.
    void advance(std::int64_t nowUs);

    // Next time advance() has work to do, or kNoEvent when idle.
    std::int64_t nextEventUs() const;

    bool keyDown() const { return m_output; }
    bool isIdle() const { return m_phase == Phase::Idle && !m_manualKey; }

    // Key edges produced since the last call, in time order.
    std::vector<KeyEvent> takeEvents();

    // Abort: force key-up at nowUs and forget all paddle state / memory.
    void reset(std::int64_t nowUs);

    // Timing helpers (µs), exposed for tests.
    std::int64_t unitUs() const { return 1'200'000 / m_wpm; }
    std::int64_t ditOnUs() const  { return unitUs() + weightExtraUs(); }
    std::int64_t dahOnUs() const  { return 3 * unitUs() + weightExtraUs(); }
    std::int64_t gapUs() const    { return unitUs() - weightExtraUs(); }

private:
    enum class Phase { Idle, Mark, Space };

    std::int64_t weightExtraUs() const { return unitUs() * (m_weight - 50) / 50; }

    void onPaddleEdges(bool dit, bool dah, std::int64_t nowUs);
    void startElement(Element e, std::int64_t atUs);
    Element chooseNext() const;
    Element chooseFromIdle() const;
    void updateOutput(std::int64_t atUs);

    CwKeyerMode m_mode{CwKeyerMode::IambicB};
    int m_wpm{20};
    int m_weight{50};

    bool m_dit{false};
    bool m_dah{false};
    Element m_lastPressed{Element::None};

    // Memory latches, cleared at the start of each element.
    //   edge latch: paddle was newly pressed during current element/space
    //   held latch: paddle was held at any time during element/space (Mode B)
    bool m_ditEdgeLatch{false};
    bool m_dahEdgeLatch{false};
    bool m_ditHeldLatch{false};
    bool m_dahHeldLatch{false};

    Phase        m_phase{Phase::Idle};
    Element      m_current{Element::None};
    std::int64_t m_phaseEndUs{0};

    bool m_autoKey{false};    // timed element mark
    bool m_manualKey{false};  // Bug dah / Straight contacts
    bool m_output{false};

    std::vector<KeyEvent> m_events;
};

// ─────────────────────────────────────────────────────────────────────────────
// CwPaddleKeyer — real-time driver for CwKeyerLogic (GUI thread).
//
// Feed raw paddle state with setPaddles(); it emits keyChanged(bool) at the
// timed element edges.  Wire keyChanged → RadioModel::sendCwKey.  Uses
// Qt::PreciseTimer; expect ~1 ms scheduling jitter on an idle desktop, which
// is < 3 % of a dit at 40 WPM.
// ─────────────────────────────────────────────────────────────────────────────
class CwPaddleKeyer : public QObject {
    Q_OBJECT

public:
    explicit CwPaddleKeyer(QObject* parent = nullptr);
    ~CwPaddleKeyer() override;

    CwKeyerMode mode() const { return m_logic.mode(); }

public slots:
    void setMode(AetherSDR::CwKeyerMode mode);
    void setWpm(int wpm);
    void setWeight(int weight);
    void setPaddles(bool dit, bool dah);
    // Immediate key-up + clear memory (e.g. on disconnect or mode change).
    void abort();

signals:
    void keyChanged(bool down);

private:
    std::int64_t nowUs() const { return m_clock.nsecsElapsed() / 1000; }
    void onTimer();
    void flushAndReschedule();

    CwKeyerLogic  m_logic;
    QElapsedTimer m_clock;
    QTimer        m_timer;
};

} // namespace AetherSDR
