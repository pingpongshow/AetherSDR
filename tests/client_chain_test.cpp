// Standalone test harness for the client DSP chain ordering helpers
// (src/core/ClientChainStages.h) and RX-chain stage processing.
// Build: produced by CMake as `client_chain_test` target.
// Run:   ./build/client_chain_test
// Exit code 0 on all pass, 1 on any failure.
//
// The RX chain in AudioEngine runs on 24 kHz stereo float32 (post-NR,
// pre-resample).  AudioEngine itself pulls in QtMultimedia, so the
// processing tests below mirror its applyClientRxChain() dispatch using
// the same header walker (ClientChain::forEachStage) and DSP classes.

#include "core/ClientChainStages.h"
#include "core/ClientComp.h"
#include "core/ClientGate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using AetherSDR::ClientChainStage;
using AetherSDR::ClientComp;
using AetherSDR::ClientGate;
namespace ClientChain = AetherSDR::ClientChain;

namespace {

constexpr double kSampleRate = 24000.0;   // RX chain native rate
constexpr int    kBlockSize  = 240;       // 10 ms at 24 kHz

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

std::vector<float> makeStereoTone(double freq, int frames, float amplitude)
{
    std::vector<float> buf(static_cast<size_t>(frames) * 2);
    const double twoPi = 6.283185307179586476;
    for (int i = 0; i < frames; ++i) {
        const float s = amplitude * static_cast<float>(
            std::sin(twoPi * freq * i / kSampleRate));
        buf[static_cast<size_t>(i) * 2]     = s;
        buf[static_cast<size_t>(i) * 2 + 1] = s;
    }
    return buf;
}

float rms(const float* data, int samples)
{
    double sumSq = 0.0;
    for (int i = 0; i < samples; ++i) {
        sumSq += static_cast<double>(data[i]) * data[i];
    }
    return samples > 0 ? static_cast<float>(std::sqrt(sumSq / samples)) : 0.0f;
}

// Minimal RX chain mirroring AudioEngine::applyClientRxChain() for the
// two stages under test.  Unlisted stage types are pass-through.
struct RxChain {
    ClientComp comp;
    ClientGate gate;

    RxChain()
    {
        comp.prepare(kSampleRate);
        gate.prepare(kSampleRate);
    }

    void process(uint64_t packed, float* stereo, int frames)
    {
        ClientChain::forEachStage(packed, [&](ClientChainStage s) {
            switch (s) {
                case ClientChainStage::Comp:
                    if (comp.isEnabled()) {
                        comp.process(stereo, frames, 2);
                    }
                    break;
                case ClientChainStage::Gate:
                    if (gate.isEnabled()) {
                        gate.process(stereo, frames, 2);
                    }
                    break;
                default:
                    break;
            }
        });
    }

    // Run `buf` through the chain in kBlockSize blocks.
    void run(uint64_t packed, std::vector<float>& buf)
    {
        const int frames = static_cast<int>(buf.size() / 2);
        for (int off = 0; off < frames; off += kBlockSize) {
            const int n = std::min(kBlockSize, frames - off);
            process(packed, buf.data() + static_cast<size_t>(off) * 2, n);
        }
    }
};

// ── Ordering snapshot ───────────────────────────────────────────────────

void testPackRoundTrip()
{
    const QVector<ClientChainStage> order{
        ClientChainStage::Reverb, ClientChainStage::Eq,
        ClientChainStage::Comp,   ClientChainStage::Gate,
    };
    const uint64_t packed = ClientChain::pack(order);
    report("pack/unpack round-trips a custom RX order",
           ClientChain::unpack(packed) == order);
    report("slot 0 holds first stage byte",
           ClientChain::stageAt(packed, 0) == ClientChainStage::Reverb);
    report("slot after last stage is None terminator",
           ClientChain::stageAt(packed, 4) == ClientChainStage::None);
    report("empty list packs to zero", ClientChain::pack({}) == 0);
    report("zero unpacks to empty list", ClientChain::unpack(0).isEmpty());
}

void testPackTruncatesAtMaxStages()
{
    QVector<ClientChainStage> many;
    for (int i = 0; i < ClientChain::kMaxStages + 3; ++i) {
        many.append(ClientChainStage::Eq);
    }
    report("pack keeps at most kMaxStages entries",
           ClientChain::unpack(ClientChain::pack(many)).size()
               == ClientChain::kMaxStages);
}

void testForEachStageOrder()
{
    const QVector<ClientChainStage> order{
        ClientChainStage::Tube, ClientChainStage::DeEss, ClientChainStage::Enh,
    };
    QVector<ClientChainStage> visited;
    ClientChain::forEachStage(ClientChain::pack(order),
                              [&](ClientChainStage s) { visited.append(s); });
    report("forEachStage visits stages in packed order", visited == order);
}

void testPersistenceFormat()
{
    const QVector<ClientChainStage> order{
        ClientChainStage::Comp, ClientChainStage::Eq, ClientChainStage::Enh,
    };
    const QString text = ClientChain::serializeList(order);
    report("serializeList uses stage names",
           text == QStringLiteral("Comp,Eq,Enh"), text.toStdString());
    report("parseList round-trips serializeList",
           ClientChain::parseList(text) == order);
    report("parseList skips unknown names and whitespace",
           ClientChain::parseList(QStringLiteral(" Gate ,Bogus,,Reverb"))
               == QVector<ClientChainStage>{ClientChainStage::Gate,
                                            ClientChainStage::Reverb});
}

void testNormalize()
{
    const QVector<ClientChainStage> stored{
        ClientChainStage::Reverb, ClientChainStage::Comp, ClientChainStage::Reverb,
    };
    const QVector<ClientChainStage> n = ClientChain::normalize(stored);
    const QVector<ClientChainStage> canon = ClientChain::defaultOrder();
    bool allPresentOnce = n.size() == canon.size();
    for (ClientChainStage s : canon) {
        allPresentOnce = allPresentOnce && (n.count(s) == 1);
    }
    report("normalize keeps every canonical stage exactly once", allPresentOnce);
    report("normalize preserves user order for stored stages",
           n.size() >= 2 && n[0] == ClientChainStage::Reverb
               && n[1] == ClientChainStage::Comp);
    report("normalize of empty list is the default order",
           ClientChain::normalize({}) == canon);
    report("default order fits the packed snapshot",
           canon.size() <= ClientChain::kMaxStages);
}

// ── RX stage processing (24 kHz stereo) ─────────────────────────────────

void testRxBypassedChainIsBitExact()
{
    RxChain chain;   // both stages disabled by default
    const uint64_t packed = ClientChain::pack(ClientChain::defaultOrder());
    std::vector<float> buf = makeStereoTone(1000.0, 4800, 0.5f);
    const std::vector<float> ref = buf;
    chain.run(packed, buf);
    report("RX: fully bypassed chain leaves audio bit-exact", buf == ref);
}

void testRxCompReducesLoudTone()
{
    RxChain chain;
    chain.comp.setThresholdDb(-30.0f);
    chain.comp.setRatio(10.0f);
    chain.comp.setAttackMs(1.0f);
    chain.comp.setReleaseMs(50.0f);
    chain.comp.setKneeDb(0.0f);
    chain.comp.setMakeupDb(0.0f);
    chain.comp.setLimiterEnabled(false);
    chain.comp.setEnabled(true);

    const uint64_t packed = ClientChain::pack(ClientChain::defaultOrder());
    const int frames = 24000;   // 1 s
    std::vector<float> buf = makeStereoTone(1000.0, frames, 0.5f);
    const float inRms = rms(buf.data() + frames, frames);   // second half
    chain.run(packed, buf);
    const float outRms = rms(buf.data() + frames, frames);
    const float grDb = 20.0f * std::log10(std::max(outRms, 1e-9f) / inRms);
    char detail[64];
    std::snprintf(detail, sizeof(detail), "GR=%.1f dB", grDb);
    report("RX: enabled Comp stage compresses a -6 dBFS tone",
           grDb < -10.0f, detail);

    bool finite = true;
    for (float v : buf) {
        finite = finite && std::isfinite(v);
    }
    report("RX: Comp output is finite", finite);
}

void testRxStageOmittedFromOrderDoesNotRun()
{
    RxChain chain;
    chain.comp.setThresholdDb(-40.0f);
    chain.comp.setRatio(20.0f);
    chain.comp.setLimiterEnabled(false);
    chain.comp.setEnabled(true);

    // A packed order without Comp — the enabled comp must not process.
    const uint64_t packed = ClientChain::pack(
        {ClientChainStage::Gate, ClientChainStage::Eq});
    std::vector<float> buf = makeStereoTone(500.0, 4800, 0.5f);
    const std::vector<float> ref = buf;
    chain.run(packed, buf);
    report("RX: stage absent from snapshot is not executed", buf == ref);
}

void testRxGateAttenuatesQuietSignal()
{
    RxChain chain;
    chain.gate.setMode(ClientGate::Mode::Gate);
    chain.gate.setThresholdDb(-30.0f);
    chain.gate.setFloorDb(-40.0f);
    chain.gate.setAttackMs(0.5f);
    chain.gate.setHoldMs(0.0f);
    chain.gate.setReleaseMs(20.0f);
    chain.gate.setEnabled(true);

    const uint64_t packed = ClientChain::pack(
        {ClientChainStage::Gate, ClientChainStage::Comp});
    const int frames = 24000;
    // -50 dBFS hiss-level tone, well below threshold.
    std::vector<float> buf = makeStereoTone(700.0, frames, 0.00316f);
    const float inRms = rms(buf.data() + frames, frames);
    chain.run(packed, buf);
    const float outRms = rms(buf.data() + frames, frames);
    const float attDb = 20.0f * std::log10(std::max(outRms, 1e-12f) / inRms);
    char detail[64];
    std::snprintf(detail, sizeof(detail), "att=%.1f dB", attDb);
    report("RX: enabled Gate stage attenuates below-threshold audio",
           attDb < -10.0f, detail);
}

} // namespace

// FTZ/DAZ guard: inside the scope a subnormal product flushes to zero on
// x86 (no-op elsewhere), and the caller's FP state is restored after.
void testScopedNoDenormals()
{
    volatile float tiny = 1.0e-30f;
    volatile float scale = 1.0e-10f;
    const bool before = std::fpclassify(tiny * scale) == FP_SUBNORMAL;
    bool inside = false;
    {
        [[maybe_unused]] const AetherSDR::ClientChain::ScopedNoDenormals guard;
        inside = std::fpclassify(tiny * scale) == FP_SUBNORMAL;
    }
    const bool after = std::fpclassify(tiny * scale) == FP_SUBNORMAL;
#if defined(__SSE__) || defined(_M_X64)
    report("ScopedNoDenormals flushes subnormals to zero", before && !inside && after);
#else
    report("ScopedNoDenormals is a harmless no-op on this arch", before == inside && before == after);
#endif
}

int main()
{
    testPackRoundTrip();
    testPackTruncatesAtMaxStages();
    testForEachStageOrder();
    testPersistenceFormat();
    testNormalize();
    testRxBypassedChainIsBitExact();
    testRxCompReducesLoudTone();
    testRxStageOmittedFromOrderDoesNotRun();
    testRxGateAttenuatesQuietSignal();
    testScopedNoDenormals();

    if (g_failed > 0) {
        std::printf("\n%d test(s) FAILED\n", g_failed);
        return EXIT_FAILURE;
    }
    std::printf("\nAll tests passed\n");
    return EXIT_SUCCESS;
}
