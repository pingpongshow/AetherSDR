// Regression tests for the GUI-test transmit interlock (src/testing/TxSafety.h).
// Every command that can key a FlexRadio must be blocked; ordinary RX/status
// commands must pass so live-radio GUI testing still works.
//
// Build: produced by CMake as `tx_safety_test`. Exit code 0 on all pass.

#include "testing/TxSafety.h"

#include <cstdio>

namespace {
int g_failures = 0;
void expect(const char* cmd, bool blocked)
{
    const bool got = AetherSDR::TxSafety::isTransmitCommand(QString::fromLatin1(cmd));
    const bool ok = (got == blocked);
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %-50s %s\n", ok ? " OK " : "FAIL", cmd, blocked ? "blocked" : "allowed");
}
} // namespace

int main()
{
    // Must block
    expect("xmit 1", true);
    expect("XMIT 1", true);
    expect("transmit tune 1", true);
    expect("transmit set mox=1", true);
    expect("transmit set vox_enable=1", true);
    expect("atu start", true);
    expect("tgxl autotune handle=0x1234", true);
    expect("cw key 1", true);
    expect("cw ptt 1", true);
    expect("cwx send \"CQ\x7f" "TEST\" 3", true);
    expect("cwx macro send 2", true);
    expect("dvk playback id=1", true);

    // Must allow
    expect("xmit 0", false);
    expect("transmit tune 0", false);
    expect("transmit set rfpower=0", false);
    expect("transmit set vox_enable=0", false);
    expect("slice set 0 tx=1", false);
    expect("slice tune 1 3.431000 autopan=0", false);
    expect("sub tx all", false);
    expect("cw wpm 25", false);
    expect("cwx clear", false);
    expect("atu bypass", false);
    expect("display pan set 0x40000000 band=20", false);

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
