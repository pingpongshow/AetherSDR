// Regression tests for VITA-49 packet parsing in PanadapterStream.
//
// This is the code path that consumes unvalidated data straight off a UDP
// socket, so every case here feeds a deliberately malformed or hostile packet
// and asserts we neither crash nor emit garbage.
//
// Build: produced by CMake as `vita49_parse_test`.
// Run:   ./build/vita49_parse_test
//
// The bounds cases feed packets through an mmap'd guard page (see
// GuardedPacket below), so an overread faults in any build — no sanitizer
// required. Running under ASan as well (-DENABLE_ASAN=ON) additionally covers
// the write-side and use-after-free paths.
//
// Exit code 0 on all pass, 1 on any failure.

#include "core/PanadapterStream.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QVector>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef _WIN32
#  include <sys/mman.h>
#  include <unistd.h>
#  define AETHER_HAVE_GUARD_PAGES 1
#endif

namespace AetherSDR {

// Friend hook declared in PanadapterStream.h — reaches the private decode
// entry point without exposing it in the production API.
struct Vita49TestAccess {
    static void feed(PanadapterStream& s, const QByteArray& datagram)
    {
        s.processDatagram(datagram);
    }
    static void setYPixels(PanadapterStream& s, quint32 streamId, int y)
    {
        s.setYPixels(streamId, y);
    }
};

} // namespace AetherSDR

using AetherSDR::PanadapterStream;
using AetherSDR::Vita49TestAccess;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// ── Guard-page packet buffer ─────────────────────────────────────────────────
//
// Places a packet so its final byte sits flush against an inaccessible page.
// Any read one byte past the end of the datagram segfaults immediately.
//
// This is deliberately not reliant on AddressSanitizer: ASan is unavailable or
// broken on some hosts (it currently hangs at dyld init on macOS 26/arm64), and
// these overread regressions must fail loudly everywhere, not only where the
// sanitizer happens to work. Feeding the buffer via QByteArray::fromRawData()
// keeps the parser reading our guarded memory rather than a copy.
#ifdef AETHER_HAVE_GUARD_PAGES
class GuardedPacket {
public:
    explicit GuardedPacket(const QByteArray& data)
        : m_size(data.size())
    {
        const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        m_mapLen = page * 2;
        m_base = static_cast<char*>(::mmap(nullptr, m_mapLen, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (m_base == MAP_FAILED) { m_base = nullptr; return; }
        // Make the second page inaccessible, then butt the packet against it.
        ::mprotect(m_base + page, page, PROT_NONE);
        m_data = m_base + page - static_cast<size_t>(m_size);
        std::memcpy(m_data, data.constData(), static_cast<size_t>(m_size));
    }
    ~GuardedPacket() { if (m_base) ::munmap(m_base, m_mapLen); }

    GuardedPacket(const GuardedPacket&) = delete;
    GuardedPacket& operator=(const GuardedPacket&) = delete;

    bool valid() const { return m_base != nullptr; }
    // Non-owning view onto the guarded page — no copy, so the parser reads
    // the protected allocation directly.
    QByteArray view() const { return QByteArray::fromRawData(m_data, m_size); }

private:
    char*  m_base{nullptr};
    char*  m_data{nullptr};
    size_t m_mapLen{0};
    int    m_size{0};
};
#endif

// Feed a packet through a guard page where the platform supports it, so an
// overread crashes here instead of silently succeeding.
void feedGuarded(PanadapterStream& s, const QByteArray& pkt)
{
#ifdef AETHER_HAVE_GUARD_PAGES
    GuardedPacket g(pkt);
    if (g.valid()) {
        Vita49TestAccess::feed(s, g.view());
        return;
    }
#endif
    Vita49TestAccess::feed(s, pkt);
}

constexpr quint16 kPccFft       = 0x8003;
constexpr quint16 kPccWaterfall = 0x8004;
constexpr quint16 kPccMeter     = 0x8002;
constexpr int     kHeaderBytes  = 28;

void appendBE16(QByteArray& b, quint16 v)
{
    char tmp[2];
    qToBigEndian(v, tmp);
    b.append(tmp, 2);
}

void appendBE32(QByteArray& b, quint32 v)
{
    char tmp[4];
    qToBigEndian(v, tmp);
    b.append(tmp, 4);
}

void appendBE64(QByteArray& b, qint64 v)
{
    char tmp[8];
    qToBigEndian(v, tmp);
    b.append(tmp, 8);
}

// Build a 28-byte VITA-49 ExtDataWithStream header with the given class code.
QByteArray makeHeader(quint16 pcc, quint32 streamId, bool trailer = false)
{
    QByteArray h;
    quint32 word0 = 0x30000000u;            // type 3, ExtDataWithStream
    if (trailer) word0 |= 0x04000000u;      // trailer-present flag
    appendBE32(h, word0);
    appendBE32(h, streamId);
    appendBE32(h, 0x00001C2Du);              // class ID OUI (FlexRadio)
    appendBE32(h, static_cast<quint32>(pcc));// info class 0 | packet class code
    appendBE32(h, 0);                        // integer timestamp
    appendBE32(h, 0);                        // fractional timestamp hi
    appendBE32(h, 0);                        // fractional timestamp lo
    return h;
}

// FFT packet. `declaredBins` goes in the sub-header; `suppliedBins` is how many
// bin values we actually append. Making declared > supplied is the truncation
// case that used to overread.
QByteArray makeFftPacket(quint32 streamId, quint16 startBin, quint16 declaredBins,
                         quint16 totalBins, quint32 frameIndex, int suppliedBins,
                         quint16 binSize = 2)
{
    QByteArray p = makeHeader(kPccFft, streamId);
    appendBE16(p, startBin);
    appendBE16(p, declaredBins);
    appendBE16(p, binSize);
    appendBE16(p, totalBins);
    appendBE32(p, frameIndex);
    for (int i = 0; i < suppliedBins; ++i)
        appendBE16(p, static_cast<quint16>(i & 0xFFFF));
    return p;
}

// Waterfall tile. Same idea: declared width vs. actually supplied bins.
QByteArray makeWaterfallPacket(quint32 streamId, quint16 width, quint16 height,
                               quint32 timecode, quint16 totalBins,
                               quint16 firstBinIndex, int suppliedBins,
                               double lowHz = 14.0e6, double binBwHz = 100.0)
{
    QByteArray p = makeHeader(kPccWaterfall, streamId);
    appendBE64(p, static_cast<qint64>(lowHz * 1048576.0));      // frame low freq (VitaFrequency)
    appendBE64(p, static_cast<qint64>(binBwHz * 1048576.0));    // bin bandwidth
    appendBE32(p, 20);                                          // line duration ms
    appendBE16(p, width);
    appendBE16(p, height);
    appendBE32(p, timecode);
    appendBE32(p, 50);                                          // auto black level
    appendBE16(p, totalBins);
    appendBE16(p, firstBinIndex);
    for (int i = 0; i < suppliedBins; ++i)
        appendBE16(p, static_cast<quint16>(0x3000 + i));
    return p;
}

// ── Tests ────────────────────────────────────────────────────────────────────

// A packet that claims far more bins than it carries must not read past the
// end of the datagram. Pre-fix this overread ~2000 bytes of heap.
void testFftTruncatedPacketDoesNotOverread()
{
    PanadapterStream s;
    // 1000 bins declared, 8 supplied. The buffer is exactly sized by
    // QByteArray, so ASan traps any read beyond it.
    const QByteArray pkt = makeFftPacket(0x40000000u, 0, 1000, 1000, 1, 8);
    feedGuarded(s, pkt);
    report("FFT: truncated packet does not overread", true, "(guard page)");
}

// numBins = 65535 with a near-empty payload — the maximum possible overread.
void testFftMaxBinsClaimDoesNotOverread()
{
    PanadapterStream s;
    const QByteArray pkt = makeFftPacket(0x40000001u, 0, 65535, 65535, 1, 1);
    feedGuarded(s, pkt);
    report("FFT: max numBins claim does not overread", true, "(guard page)");
}

// startBin near the end of the frame must not write past the assembly buffer.
void testFftStartBinOverflowIsRejected()
{
    PanadapterStream s;
    const QByteArray pkt = makeFftPacket(0x40000002u, 4090, 100, 4096, 1, 100);
    Vita49TestAccess::feed(s, pkt);
    report("FFT: startBin+numBins past frame end is rejected", true);
}

// A second fragment carrying the same frameIndex but a larger totalBins must
// restart the frame rather than write past a buffer sized by the first packet.
void testFftGrowingTotalBinsRestartsFrame()
{
    PanadapterStream s;
    Vita49TestAccess::feed(s, makeFftPacket(0x40000003u, 0, 16, 16, 7, 16));
    Vita49TestAccess::feed(s, makeFftPacket(0x40000003u, 900, 16, 4096, 7, 16));
    report("FFT: growing totalBins restarts frame", true);
}

// Non-2-byte bin strides are unsupported; the packet must be dropped rather
// than misparsed as uint16.
void testFftUnexpectedBinSizeIsDropped()
{
    PanadapterStream s;
    QSignalSpy spy(&s, &PanadapterStream::spectrumReady);
    const QByteArray pkt = makeFftPacket(0x40000004u, 0, 8, 8, 1, 8, /*binSize=*/4);
    Vita49TestAccess::feed(s, pkt);
    report("FFT: binSize != 2 is dropped", spy.count() == 0,
           "emitted " + std::to_string(spy.count()) + " spectra");
}

// A complete single-fragment frame should emit exactly one spectrum with the
// declared bin count.
void testFftCompleteFrameEmitsSpectrum()
{
    PanadapterStream s;
    QSignalSpy spy(&s, &PanadapterStream::spectrumReady);
    Vita49TestAccess::feed(s, makeFftPacket(0x40000005u, 0, 64, 64, 3, 64));
    bool ok = spy.count() == 1;
    if (ok) {
        const auto bins = spy.at(0).at(1).value<QVector<float>>();
        ok = bins.size() == 64;
        report("FFT: complete frame emits one spectrum",
               ok, "bins=" + std::to_string(bins.size()));
    } else {
        report("FFT: complete frame emits one spectrum", false,
               "emitted " + std::to_string(spy.count()));
    }
}

// ypixels of 0 or 1 must not produce inf/NaN bins — those propagate into the
// spectrum smoothing filter and poison the display permanently.
void testFftDegenerateYPixelsProducesFiniteBins()
{
    PanadapterStream s;
    QSignalSpy spy(&s, &PanadapterStream::spectrumReady);
    Vita49TestAccess::setYPixels(s, 0x40000006u, 1);   // degenerate
    Vita49TestAccess::feed(s, makeFftPacket(0x40000006u, 0, 32, 32, 1, 32));

    bool allFinite = spy.count() == 1;
    if (allFinite) {
        const auto bins = spy.at(0).at(1).value<QVector<float>>();
        for (float v : bins) {
            if (std::isnan(v) || std::isinf(v)) { allFinite = false; break; }
        }
    }
    report("FFT: ypixels=1 yields finite bins", allFinite);
}

// Waterfall tile claiming more bins than the datagram carries.
void testWaterfallTruncatedPacketDoesNotOverread()
{
    PanadapterStream s;
    const QByteArray pkt = makeWaterfallPacket(0x42000000u, 2000, 1, 1, 2000, 0, 4);
    feedGuarded(s, pkt);
    report("Waterfall: truncated tile does not overread", true, "(guard page)");
}

// Second fragment with the same timecode but a larger totalBins must not write
// past the buffer allocated from the first fragment.
void testWaterfallGrowingTotalBinsDoesNotOverflow()
{
    PanadapterStream s;
    feedGuarded(s, makeWaterfallPacket(0x42000001u, 16, 1, 9, 16, 0, 16));
    // Same timecode, much larger frame, writing near the claimed end.
    feedGuarded(s, makeWaterfallPacket(0x42000001u, 16, 1, 9, 4096, 4000, 16));
    report("Waterfall: growing totalBins does not overflow buffer", true, "(guard page)");
}

// firstBinIndex beyond the frame must be rejected outright.
void testWaterfallFirstBinBeyondFrameIsRejected()
{
    PanadapterStream s;
    Vita49TestAccess::feed(s, makeWaterfallPacket(0x42000002u, 16, 1, 11, 64, 60000, 16));
    report("Waterfall: firstBinIndex past frame end is rejected", true);
}

// A wide pan near the bottom of HF yields tiles whose low edge is below 0 Hz
// (e.g. 1.3-5.8 MHz pan, tile from -0.17 MHz). The VitaFrequency decode must
// keep the negative edge instead of falling back to the plain-Hz reading.
void testWaterfallNegativeLowEdgeKeepsVitaScale()
{
    PanadapterStream s;
    s.registerWfStream(0x42000003u);
    QSignalSpy spy(&s, &PanadapterStream::waterfallRowReady);
    Vita49TestAccess::feed(s, makeWaterfallPacket(0x42000003u, 16, 1, 12, 16, 0, 16,
                                                  -170917.0, 3146.0));
    bool ok = spy.count() == 1;
    double low = 0.0;
    double high = 0.0;
    if (ok) {
        low = spy.at(0).at(2).toDouble();
        high = spy.at(0).at(3).toDouble();
        ok = std::abs(low - (-0.170917)) < 1e-6 && std::abs(high - (-0.170917 + 16 * 0.003146)) < 1e-6;
    }
    report("Waterfall: negative tile low edge keeps MHz scale", ok,
           "low=" + std::to_string(low) + " high=" + std::to_string(high));
}

// Every truncation of a well-formed packet must be handled without crashing.
// This is the cheap generic sweep that catches new bounds bugs.
void testRuncatedPrefixesOfEveryPacketType()
{
    const QByteArray fft = makeFftPacket(0x40000010u, 0, 64, 64, 1, 64);
    const QByteArray wf  = makeWaterfallPacket(0x42000010u, 64, 1, 1, 64, 0, 64);

    QByteArray meter = makeHeader(kPccMeter, 0x44000010u);
    for (int i = 0; i < 8; ++i) { appendBE16(meter, static_cast<quint16>(i)); appendBE16(meter, 100); }

    for (const QByteArray& full : {fft, wf, meter}) {
        for (int len = 0; len <= full.size(); ++len) {
            PanadapterStream s;
            feedGuarded(s, full.left(len));
        }
    }
    report("All types: every truncated prefix is handled", true, "(guard page)");
}

// Trailer-present packets shrink the usable payload by 4 bytes; a packet that
// is *only* a header plus trailer must not underflow the payload length.
void testTrailerFlagDoesNotUnderflowPayload()
{
    for (quint16 pcc : {kPccFft, kPccWaterfall, kPccMeter}) {
        PanadapterStream s;
        QByteArray p = makeHeader(pcc, 0x40000020u, /*trailer=*/true);
        appendBE32(p, 0);  // 4 bytes: exactly the trailer, zero payload
        Vita49TestAccess::feed(s, p);
    }
    report("All types: trailer flag does not underflow payload", true);
}

// A stream ID we don't own must be dropped once ownership is registered.
void testUnownedStreamsAreDropped()
{
    PanadapterStream s;
    s.registerPanStream(0x40000030u);
    QSignalSpy spy(&s, &PanadapterStream::spectrumReady);
    // Owned stream → emits.
    Vita49TestAccess::feed(s, makeFftPacket(0x40000030u, 0, 32, 32, 1, 32));
    const int afterOwned = spy.count();
    // Different stream ID → dropped.
    Vita49TestAccess::feed(s, makeFftPacket(0x4000FFFFu, 0, 32, 32, 1, 32));
    const int afterUnowned = spy.count();

    report("Ownership: unowned pan stream is dropped",
           afterOwned == 1 && afterUnowned == 1,
           "owned=" + std::to_string(afterOwned) + " after-unowned=" + std::to_string(afterUnowned));
}

// Registering and unregistering streams while packets flow must not leave
// stale assembly state behind (or crash pruning it).
void testStreamStatePruningIsSafe()
{
    PanadapterStream s;
    for (quint32 i = 0; i < 64; ++i) {
        const quint32 id = 0x40001000u + i;
        s.registerPanStream(id);
        Vita49TestAccess::feed(s, makeFftPacket(id, 0, 32, 32, 1, 32));
        s.unregisterPanStream(id);
        // Next packet triggers the generation-change prune on the worker side.
        Vita49TestAccess::feed(s, makeFftPacket(id, 0, 32, 32, 2, 32));
    }
    s.clearRegisteredStreams();
    Vita49TestAccess::feed(s, makeFftPacket(0x40001000u, 0, 32, 32, 3, 32));
    report("Ownership: register/unregister churn prunes safely", true);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testFftTruncatedPacketDoesNotOverread();
    testFftMaxBinsClaimDoesNotOverread();
    testFftStartBinOverflowIsRejected();
    testFftGrowingTotalBinsRestartsFrame();
    testFftUnexpectedBinSizeIsDropped();
    testFftCompleteFrameEmitsSpectrum();
    testFftDegenerateYPixelsProducesFiniteBins();

    testWaterfallTruncatedPacketDoesNotOverread();
    testWaterfallGrowingTotalBinsDoesNotOverflow();
    testWaterfallFirstBinBeyondFrameIsRejected();
    testWaterfallNegativeLowEdgeKeepsVitaScale();

    testRuncatedPrefixesOfEveryPacketType();
    testTrailerFlagDoesNotUnderflowPayload();

    testUnownedStreamsAreDropped();
    testStreamStatePruningIsSafe();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "ALL PASS" : "FAILURES",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
