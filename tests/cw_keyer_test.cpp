// Virtual-clock tests for CwKeyerLogic (client-side paddle keyer):
// Iambic A / Iambic B / Ultimatic squeeze behaviour, dit/dah memory, Bug,
// Straight, WPM + weight timing.  Nothing here touches a radio.
// Exit code 0 on all pass.

#include "core/CwKeyerLogic.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {})
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %-62s %s\n", ok ? " OK " : "FAIL", what.c_str(), detail.c_str());
}

struct Step {
    int  tMs;
    bool dit;
    bool dah;
};

struct Mark {
    std::int64_t startUs;
    std::int64_t lenUs;
};

std::vector<Mark> run(CwKeyerMode mode, const std::vector<Step>& script,
                      int wpm = 20, int weight = 50, int endMs = 5000)
{
    CwKeyerLogic k;
    k.setMode(mode);
    k.setWpm(wpm);
    k.setWeight(weight);

    std::vector<CwKeyerLogic::KeyEvent> all;
    auto drain = [&] {
        for (const CwKeyerLogic::KeyEvent& e : k.takeEvents()) {
            all.push_back(e);
        }
    };
    for (const Step& s : script) {
        k.setPaddles(s.dit, s.dah, static_cast<std::int64_t>(s.tMs) * 1000);
        drain();
    }
    k.advance(static_cast<std::int64_t>(endMs) * 1000);
    drain();

    std::vector<Mark> marks;
    std::int64_t downAt = -1;
    for (const CwKeyerLogic::KeyEvent& e : all) {
        if (e.down) {
            downAt = e.timeUs;
        } else if (downAt >= 0) {
            marks.push_back({downAt, e.timeUs - downAt});
            downAt = -1;
        }
    }
    return marks;
}

// Mark lengths → ".-" string (threshold 2 units at the given WPM).
std::string pattern(const std::vector<Mark>& marks, int wpm = 20)
{
    const std::int64_t unit = 1'200'000 / wpm;
    std::string s;
    for (const Mark& m : marks) {
        s += (m.lenUs < 2 * unit) ? '.' : '-';
    }
    return s;
}

void expectPattern(const char* name, CwKeyerMode mode, const std::vector<Step>& script,
                   const std::string& want)
{
    const std::string got = pattern(run(mode, script));
    check(got == want, std::string(name) + " -> \"" + want + "\"", "got \"" + got + "\"");
}

} // namespace

int main()
{
    // 20 WPM → unit 60 ms: dit 0-60, gap 60-120, dah 120-300, gap 300-360…

    // ── Timing ──────────────────────────────────────────────────────────
    {
        std::vector<Mark> m = run(CwKeyerMode::IambicB, {{0, true, false}, {10, false, false}});
        check(m.size() == 1 && m[0].lenUs == 60'000, "20 WPM single dit = 60 ms",
              m.empty() ? "none" : std::to_string(m[0].lenUs));
    }
    {
        std::vector<Mark> m = run(CwKeyerMode::IambicB, {{0, true, false}, {300, false, false}});
        bool ok = m.size() == 3 && m[1].startUs == 120'000 && m[2].startUs == 240'000;
        check(ok, "held dit repeats every 2 units (3 dits in 300 ms)",
              std::to_string(m.size()) + " marks");
    }
    {
        std::vector<Mark> m = run(CwKeyerMode::IambicB, {{0, false, true}, {10, false, false}});
        check(m.size() == 1 && m[0].lenUs == 180'000, "20 WPM dah = 180 ms");
    }
    {
        // Weight 75: mark +0.5 unit, space -0.5 unit, period unchanged.
        std::vector<Mark> m = run(CwKeyerMode::IambicB, {{0, true, false}, {200, false, false}},
                                  20, 75);
        bool ok = m.size() == 2 && m[0].lenUs == 90'000 && m[1].startUs == 120'000;
        check(ok, "weight 75: dit 90 ms, period still 120 ms");
    }
    {
        std::vector<Mark> m = run(CwKeyerMode::IambicB, {{0, true, false}, {10, false, false}}, 40);
        check(m.size() == 1 && m[0].lenUs == 30'000, "40 WPM dit = 30 ms");
    }

    // ── Squeeze released mid-dah: A stops, B adds one, Ultimatic stops ──
    // dit@0, dah@10 (squeeze), both released @200 (inside the 120-300 dah).
    const std::vector<Step> squeezeRelease = {
        {0, true, false}, {10, true, true}, {200, false, false}};
    expectPattern("IambicA squeeze-release", CwKeyerMode::IambicA, squeezeRelease, ".-");
    expectPattern("IambicB squeeze-release", CwKeyerMode::IambicB, squeezeRelease, ".-.");
    expectPattern("Ultimatic squeeze-release", CwKeyerMode::Ultimatic, squeezeRelease, ".-");

    // ── Long squeeze: dit held 0-700, dah held 10-500 ────────────────────
    // Iambic alternates; Ultimatic repeats the last-pressed (dah) and then
    // falls back to dits once dah is released.
    const std::vector<Step> longSqueeze = {
        {0, true, false}, {10, true, true}, {500, true, false}, {700, false, false}};
    expectPattern("IambicA long squeeze", CwKeyerMode::IambicA, longSqueeze, ".-.-");
    expectPattern("IambicB long squeeze", CwKeyerMode::IambicB, longSqueeze, ".-.-.");
    expectPattern("Ultimatic long squeeze", CwKeyerMode::Ultimatic, longSqueeze, ".--.");

    // ── Ultimatic canonical: hold dit, add dah → dahs until dah released,
    //    then dits resume.
    {
        const std::vector<Step> s = {
            {0, true, false}, {100, true, true}, {1000, true, false}, {1300, false, false}};
        const std::string got = pattern(run(CwKeyerMode::Ultimatic, s));
        // 0 dit; 120 dah(→300); 360 dah; 600 dah; 840 dah(→1020); 1080 dit; 1200 dit
        check(got == ".----..", "Ultimatic: dit, dahs while squeezed, dits after", got);
        const std::string ia = pattern(run(CwKeyerMode::IambicA, s));
        check(ia.find("--") == std::string::npos && ia.substr(0, 4) == ".-.-",
              "IambicA same input alternates (never two dahs in a row)", ia);
    }

    // ── Reverse Ultimatic: hold dah, add dit → dits repeat ──────────────
    {
        const std::vector<Step> s = {
            {0, false, true}, {50, true, true}, {590, false, true}, {800, false, false}};
        // 0 dah(→180); 240 dit; 360 dit; 480 dit; 600 dah(dit released @590
        // before the 600 decision → dah) → 780; 840 decision: none → stop
        const std::string got = pattern(run(CwKeyerMode::Ultimatic, s));
        check(got == "-...-", "Ultimatic reverse: dah, dits while squeezed, dah", got);
    }

    // ── Dit memory: tap dit (50-70 ms) during a dah held 0-150 ms ────────
    const std::vector<Step> ditTap = {
        {0, false, true}, {50, true, true}, {70, false, true}, {150, false, false}};
    expectPattern("IambicA dit memory (tap during dah)", CwKeyerMode::IambicA, ditTap, "-.");
    expectPattern("IambicB dit memory (tap during dah)", CwKeyerMode::IambicB, ditTap, "-.");
    expectPattern("Ultimatic dit memory (tap during dah)", CwKeyerMode::Ultimatic, ditTap, "-.");

    // ── Bug: auto dits on dit paddle, manual dah contact ────────────────
    {
        std::vector<Mark> m = run(CwKeyerMode::Bug,
            {{0, true, false}, {250, false, false}, {400, false, true}, {700, false, false}});
        bool ok = m.size() == 4 && m[3].startUs == 400'000 && m[3].lenUs == 300'000;
        check(ok, "Bug: 3 auto dits + one 300 ms manual dah", pattern(m));
    }

    // ── Straight / cootie ───────────────────────────────────────────────
    {
        std::vector<Mark> m = run(CwKeyerMode::Straight,
            {{0, true, false}, {100, false, false}, {150, false, true}, {170, false, false}});
        bool ok = m.size() == 2 && m[0].lenUs == 100'000 && m[1].lenUs == 20'000;
        check(ok, "Straight: key follows either contact exactly");
    }

    // ── Reset forces key-up ─────────────────────────────────────────────
    {
        CwKeyerLogic k;
        k.setMode(CwKeyerMode::Ultimatic);
        k.setPaddles(false, true, 0);
        k.advance(50'000);
        bool downMid = k.keyDown();
        k.reset(60'000);
        std::vector<CwKeyerLogic::KeyEvent> ev = k.takeEvents();
        bool ok = downMid && !k.keyDown() && !ev.empty() && !ev.back().down
                  && ev.back().timeUs == 60'000 && k.nextEventUs() == CwKeyerLogic::kNoEvent;
        check(ok, "reset() mid-element emits key-up and goes idle");
    }

    // ── Settings strings ────────────────────────────────────────────────
    {
        bool ok = true;
        for (CwKeyerMode m : {CwKeyerMode::Radio, CwKeyerMode::IambicA, CwKeyerMode::IambicB,
                              CwKeyerMode::Ultimatic, CwKeyerMode::Bug, CwKeyerMode::Straight}) {
            ok = ok && cwKeyerModeFromString(cwKeyerModeToString(m)) == m;
        }
        ok = ok && cwKeyerModeFromString("garbage") == CwKeyerMode::Radio;
        ok = ok && settingsDispatchMode(CwKeyerMode::IambicA) == CwKeyerMode::Radio;
        ok = ok && settingsDispatchMode(CwKeyerMode::IambicB) == CwKeyerMode::Radio;
        ok = ok && settingsDispatchMode(CwKeyerMode::Ultimatic) == CwKeyerMode::Ultimatic;
        check(ok, "CwKeyerMode settings round-trip + Iambic→Radio dispatch");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
