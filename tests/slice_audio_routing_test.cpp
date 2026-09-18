// Per-slice audio routing logic (SliceAudioRouting): route persistence
// format, routing plan rules (DAX separation, bridge conflicts), radio
// volume/pan mirror and the lock-free SliceMixer.  No audio devices, no
// radio.  Exit code 0 on all pass.

#include "core/SliceAudioRouting.h"

#include <QMap>
#include <QString>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;
constexpr double kPi = 3.141592653589793;

void check(bool ok, const char* what, const QString& detail = QString())
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s %s\n", ok ? " OK " : "FAIL", what, qPrintable(detail));
}

double toneAmp(const std::vector<float>& x, int stride, int offset, int n0, int n,
               double freq, double rate)
{
    double re = 0.0;
    double im = 0.0;
    for (int i = 0; i < n; ++i) {
        const double v = x[static_cast<size_t>((n0 + i) * stride + offset)];
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        re += w * v * std::cos(2.0 * kPi * freq * i / rate);
        im += w * v * std::sin(2.0 * kPi * freq * i / rate);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (0.5 * n);
}

void testRouteSettings()
{
    std::printf("--- route persistence ---\n");
    SliceAudioRoute def;
    check(def.isDefault(), "default route is main-only");
    SliceAudioRoute r;
    r.toMain = false;
    r.outputDevice = QStringLiteral("USB Headset \"Left\"");
    r.daxChannel = 3;
    const QString s = r.toSetting();
    check(SliceAudioRoute::fromSetting(s) == r, "round trip", s);
    check(SliceAudioRoute::fromSetting(QStringLiteral("garbage")).isDefault(), "garbage -> default");
    check(SliceAudioRoute::fromSetting(QStringLiteral(R"({"main":true,"dax":9})")).daxChannel == 0,
          "out-of-range DAX channel dropped");
    check(SliceAudioRoute::fromSetting(QStringLiteral(R"({"main":false})")).toMain,
          "route to nowhere falls back to main");
    check(SliceAudioRoute::settingsKey(QChar('b')) == QStringLiteral("SliceAudioRoute_B"),
          "settings key per slice letter", SliceAudioRoute::settingsKey(QChar('b')));
}

void testPlan()
{
    std::printf("--- routing plan ---\n");
    QVector<SliceAudioInfo> slices{{0, QChar('A'), 1}, {1, QChar('B'), 0}, {2, QChar('C'), 2},
                                   {3, QChar('D'), 2}};
    QMap<QChar, SliceAudioRoute> routes;
    check(planSliceAudioRouting(routes, slices, true).isEmpty(), "no routes -> empty plan");

    routes[QChar('A')] = SliceAudioRoute{true, QStringLiteral("Headphones"), 0};
    routes[QChar('B')] = SliceAudioRoute{true, QStringLiteral("Headphones"), 0};
    routes[QChar('C')] = SliceAudioRoute{false, QStringLiteral("Speaker 2"), 0};
    SliceAudioPlan p = planSliceAudioRouting(routes, slices, true);
    check(p.entries.size() == 1 && p.entries[0].sliceId == 0 && p.entries[0].sourceDaxChannel == 1,
          "slice A routed from its DAX channel");
    check(p.slicesNeedingDax == QVector<int>{1}, "slice B needs a DAX channel");
    check(p.warnings.size() == 2, "B (no DAX) and C (shared DAX 2) warned", p.warnings.join(" / "));
    check(p.outputDevices() == QStringList{QStringLiteral("Headphones")}, "device list");
    check(p.entryForDaxChannel(1) && !p.entryForDaxChannel(2), "lookup by DAX channel");

    // DAX targets.
    slices = {{0, QChar('A'), 1}, {1, QChar('B'), 2}, {2, QChar('C'), 3}};
    routes.clear();
    routes[QChar('A')] = SliceAudioRoute{true, QString(), 1};   // same as its own: no-op
    routes[QChar('B')] = SliceAudioRoute{true, QString(), 4};   // free virtual channel
    routes[QChar('C')] = SliceAudioRoute{true, QString(), 4};   // duplicate target
    p = planSliceAudioRouting(routes, slices, true);
    check(p.entries.size() == 1 && p.entries[0].sliceId == 1 && p.entries[0].bridgeChannel == 4,
          "B -> DAX 4 fed; A's own channel is a no-op");
    check(p.warnings.size() == 1, "duplicate DAX 4 target refused", p.warnings.join(" / "));

    routes.clear();
    routes[QChar('A')] = SliceAudioRoute{true, QString(), 2};   // DAX 2 carries slice B
    p = planSliceAudioRouting(routes, slices, true);
    check(p.isEmpty() && p.warnings.size() == 1,
          "target already fed by the radio (bridge running) refused (#1679 double feed)");
    p = planSliceAudioRouting(routes, slices, false);
    check(p.entries.size() == 1 && p.entries[0].bridgeChannel == 2,
          "same target allowed when the bridge does not feed radio DAX");

    routes.clear();
    routes[QChar('A')] = SliceAudioRoute{false, QStringLiteral("Headphones"), 0};
    p = planSliceAudioRouting(routes, slices, true);
    check(p.entries.size() == 1 && p.entries[0].removeFromMain, "not-on-main flags a radio mute");
}

void testGains()
{
    std::printf("--- radio volume/pan mirror ---\n");
    StereoGain g = sliceOutputGains(100.0f, 50, false);
    check(g.left == 1.0f && g.right == 1.0f, "centre, full level");
    g = sliceOutputGains(50.0f, 0, false);
    check(g.left == 0.5f && g.right == 0.0f, "hard left, half level");
    g = sliceOutputGains(80.0f, 75, false);
    check(std::abs(g.left - 0.4f) < 1e-6f && std::abs(g.right - 0.8f) < 1e-6f, "pan 75");
    g = sliceOutputGains(100.0f, 50, true);
    check(g.left == 0.0f && g.right == 0.0f, "muted");
}

void testMixer()
{
    std::printf("--- SliceMixer ---\n");
    constexpr int kDevRate = 48000;
    SliceMixer mixer({0, 2}, kDevRate, 60);
    check(mixer.hasSlice(0) && mixer.hasSlice(2) && !mixer.hasSlice(1), "slots for routed slices only");
    std::vector<float> blk(480 * 2, 0.0f);
    check(mixer.push(1, blk.data(), 480) == -1, "unrouted slice rejected");
    mixer.setGain(0, {1.0f, 0.0f});   // slice A hard left
    mixer.setGain(2, {0.0f, 0.5f});   // slice C right, half level

    std::vector<float> out;
    std::vector<float> pullBuf(960 * 2);
    long n = 0;
    bool silentWhilePriming = true;
    for (int b = 0; b < 200; ++b) {   // 4 s: 20 ms blocks, pull the same duration
        std::vector<float> a(480 * 2);
        std::vector<float> c(480 * 2);
        for (int i = 0; i < 480; ++i, ++n) {
            const auto va = static_cast<float>(0.5 * std::sin(2.0 * kPi * 600.0 * n / 24000.0));
            const auto vc = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1100.0 * n / 24000.0));
            a[static_cast<size_t>(2 * i)] = va;
            a[static_cast<size_t>(2 * i + 1)] = va;
            c[static_cast<size_t>(2 * i)] = vc;
            c[static_cast<size_t>(2 * i + 1)] = vc;
        }
        mixer.push(0, a.data(), 480);
        mixer.push(2, c.data(), 480);
        mixer.pull(pullBuf.data(), 960, 2);
        if (b == 0) {
            for (float v : pullBuf) {
                silentWhilePriming = silentWhilePriming && v == 0.0f;
            }
        }
        out.insert(out.end(), pullBuf.begin(), pullBuf.end());
    }
    check(silentWhilePriming, "silence while priming (first 20 ms < 60 ms target)");
    const int frames = static_cast<int>(out.size() / 2);
    const int tail = kDevRate;   // last second
    const double lA = toneAmp(out, 2, 0, frames - tail, tail, 600.0, kDevRate);
    const double lC = toneAmp(out, 2, 0, frames - tail, tail, 1100.0, kDevRate);
    const double rA = toneAmp(out, 2, 1, frames - tail, tail, 600.0, kDevRate);
    const double rC = toneAmp(out, 2, 1, frames - tail, tail, 1100.0, kDevRate);
    check(std::abs(lA - 0.5) < 0.03 && lC < 0.005, "left = slice A only (24k -> 48k)",
          QStringLiteral("A=%1 C=%2").arg(lA).arg(lC));
    check(std::abs(rC - 0.25) < 0.02 && rA < 0.005, "right = slice C at half level",
          QStringLiteral("A=%1 C=%2").arg(rA).arg(rC));
    check(mixer.underruns() == 0, "steady state: no underruns",
          QString::number(static_cast<qulonglong>(mixer.underruns())));

    // Starve: pull without pushing → underrun, then silence.
    mixer.pull(pullBuf.data(), 960, 2);
    mixer.pull(pullBuf.data(), 960, 2);
    mixer.pull(pullBuf.data(), 960, 2);
    check(mixer.underruns() > 0, "starved -> underrun counted");

    // Flood: push 2 s without pulling → next pull skips back to target.
    std::vector<float> a(480 * 2, 0.1f);
    for (int b = 0; b < 100; ++b) {
        mixer.push(0, a.data(), 480);
    }
    const int before = mixer.bufferedFrames(0);
    mixer.pull(pullBuf.data(), 960, 2);
    const int after = mixer.bufferedFrames(0);
    check(before > mixer.targetFrames() * 3 && after <= mixer.targetFrames(),
          "overflow skips back to target",
          QStringLiteral("%1 -> %2 (target %3)").arg(before).arg(after).arg(mixer.targetFrames()));

    // Mono device: (L+R)/2 gain applied.
    SliceMixer mono({5}, 24000, 20);
    mono.setGain(5, {1.0f, 0.0f});
    std::vector<float> one(480 * 2, 0.8f);
    for (int b = 0; b < 10; ++b) {
        mono.push(5, one.data(), 480);
    }
    std::vector<float> m(480);
    mono.pull(m.data(), 480, 1);
    check(std::abs(m[400] - 0.4f) < 0.01f, "mono device: pan-weighted downmix",
          QString::number(m[400]));
}

} // namespace

int main()
{
    testRouteSettings();
    testPlan();
    testGains();
    testMixer();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
