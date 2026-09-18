// Spectrum recording (.aesr) round-trip, crash recovery and rotation tests.
//
// Build: produced by CMake as `spectrum_recording_test`.
// Run:   ./build/spectrum_recording_test      (exit 0 = all pass)

#include "core/SpectrumPlayer.h"
#include "core/SpectrumRecorder.h"
#include "core/SpectrumRecordingFormat.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) { ++g_failed; }
}

struct Frame {
    bool           isRow{false};
    QVector<float> bins;
    double         lo{0}, hi{0};
    quint32        tc{0};
};

struct Capture {
    QVector<Frame> frames;
    QVector<SpecRec::PanState> pans;
    QVector<QVector<SpecRec::SliceMarker>> slices;
    void wire(SpectrumPlayer& p)
    {
        QObject::connect(&p, &SpectrumPlayer::spectrumFrame, [this](const QVector<float>& b) {
            frames.append({false, b, 0, 0, 0});
        });
        QObject::connect(&p, &SpectrumPlayer::waterfallRow,
                         [this](const QVector<float>& b, double lo, double hi, quint32 tc) {
            frames.append({true, b, lo, hi, tc});
        });
        QObject::connect(&p, &SpectrumPlayer::panStateChanged,
                         [this](double c, double bw, float mn, float mx) {
            pans.append({c, bw, mn, mx});
        });
        QObject::connect(&p, &SpectrumPlayer::sliceMarkersChanged,
                         [this](const QVector<SpecRec::SliceMarker>& m) { slices.append(m); });
    }
};

QVector<float> randomFft(int n, QRandomGenerator& rng)
{
    QVector<float> v(n);
    for (float& x : v) { x = -130.0f + static_cast<float>(rng.generateDouble() * 90.0); }
    return v;
}

// Waterfall tiles are int16/128 on the wire → exactly representable in Q7.
QVector<float> randomRow(int n, QRandomGenerator& rng)
{
    QVector<float> v(n);
    for (float& x : v) { x = static_cast<float>(static_cast<qint16>(rng.bounded(9000, 16000))) / 128.0f; }
    return v;
}

// Records `nFrames` alternating FFT/waterfall frames with a deterministic clock
// (50 ms/frame) and returns what was submitted.
QVector<Frame> recordSession(SpectrumRecorder& rec, const SpectrumRecorder::Options& o,
                             int nFrames, int nBins, quint32 seed, bool withPanChange = true)
{
    QRandomGenerator rng(seed);
    QVector<Frame> sent;
    qint64 t = 1'700'000'000'000'000LL;
    rec.setClockOverrideUs(t);
    SpecRec::PanState ps{14.225, 0.2, -130.0f, -40.0f};
    QString err;
    if (!rec.start(o, ps, &err)) {
        std::printf("start failed: %s\n", qPrintable(err));
        return sent;
    }
    QVector<SpecRec::SliceMarker> sm{{0, 14.225, -2800, -100, true, false, QStringLiteral("LSB")}};
    rec.submitSliceMarkers(sm);
    for (int i = 0; i < nFrames; ++i) {
        t += 50'000;
        rec.setClockOverrideUs(t);
        if (withPanChange && i == nFrames / 2) {
            rec.submitPanState({14.100, 0.1, -120.0f, -50.0f});
        }
        if (i % 2 == 0) {
            Frame f{false, randomFft(nBins, rng), 0, 0, 0};
            rec.submitSpectrum(0x40000000u, f.bins);
            sent.append(f);
        } else {
            Frame f{true, randomRow(nBins, rng), 14.1, 14.3, static_cast<quint32>(i)};
            rec.submitWaterfallRow(0x42000000u, f.bins, f.lo, f.hi, f.tc);
            sent.append(f);
        }
    }
    rec.stop();
    return sent;
}

bool framesEqual(const QVector<Frame>& a, const QVector<Frame>& b, float tol, std::string& why)
{
    if (a.size() != b.size()) {
        why = "count " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (a[i].isRow != b[i].isRow || a[i].bins.size() != b[i].bins.size()
            || a[i].lo != b[i].lo || a[i].hi != b[i].hi || a[i].tc != b[i].tc) {
            why = "frame " + std::to_string(i) + " metadata";
            return false;
        }
        for (int k = 0; k < a[i].bins.size(); ++k) {
            const float d = std::fabs(a[i].bins[k] - b[i].bins[k]);
            if ((tol == 0.0f && a[i].bins[k] != b[i].bins[k]) || d > tol) {
                why = "frame " + std::to_string(i) + " bin " + std::to_string(k)
                    + " diff " + std::to_string(d);
                return false;
            }
        }
    }
    return true;
}

SpectrumRecorder::Options baseOptions(const QString& dir, const QString& name)
{
    SpectrumRecorder::Options o;
    o.directory = dir;
    o.baseName = name;
    o.minFreeBytes = 0;
    o.flushIntervalMs = 1000;
    return o;
}

// ── Tests ────────────────────────────────────────────────────────────────────

void testLosslessRoundTrip(const QString& dir)
{
    SpectrumRecorder rec;
    auto o = baseOptions(dir, QStringLiteral("lossless"));
    o.losslessFft = true;
    const QVector<Frame> sent = recordSession(rec, o, 400, 1024, 1);

    SpectrumPlayer p;
    Capture cap;
    cap.wire(p);
    QString err;
    const bool opened = p.open(rec.firstFilePath(), &err);
    report("round-trip: player opens recording", opened, err.toStdString());
    p.advanceTo(p.durationMs() + 1000);
    std::string why;
    report("round-trip (lossless): every FFT frame + wf row identical",
           framesEqual(sent, cap.frames, 0.0f, why), why);
    report("round-trip: pan state keyframe + mid-session change replayed",
           cap.pans.size() == 2 && cap.pans[0].centerMhz == 14.225 && cap.pans[1].centerMhz == 14.100,
           "pans=" + std::to_string(cap.pans.size()));
    report("round-trip: slice markers replayed",
           cap.slices.size() == 1 && cap.slices[0].size() == 1 && cap.slices[0][0].mode == "LSB");
    report("round-trip: duration ≈ 20 s", std::abs(p.durationMs() - 20000) <= 100,
           std::to_string(p.durationMs()) + " ms");
    report("round-trip: nothing dropped", rec.droppedFrames() == 0);

    // Seek into the middle: frames after the seek point must match the tail.
    Capture cap2;
    cap2.wire(p);
    p.seek(10'000);
    p.advanceTo(p.durationMs() + 1000);
    const int firstAfter = 10'000 / 50;  // frame i has t = (i+1)*50 ms
    QVector<Frame> tail = sent.mid(firstAfter);
    // seek() emits the last FFT frame at/before the target (display primer).
    if (!cap2.frames.isEmpty() && !cap2.frames.first().isRow && !tail.isEmpty()
        && cap2.frames.size() == tail.size() + 1) {
        cap2.frames.removeFirst();
    }
    report("seek: frames after seek point identical to recorded tail",
           framesEqual(tail, cap2.frames, 0.0f, why), why);
    report("seek: keyframe pan state re-emitted", !cap2.pans.isEmpty()
           && cap2.pans.last().centerMhz == 14.100);
}

void testDefaultQuantisation(const QString& dir)
{
    SpectrumRecorder rec;
    const QVector<Frame> sent = recordSession(rec, baseOptions(dir, QStringLiteral("quant")), 200, 2048, 2);
    SpectrumPlayer p;
    Capture cap;
    cap.wire(p);
    p.open(rec.firstFilePath());
    p.advanceTo(p.durationMs() + 1000);
    std::string why;
    report("default encoding: FFT within ±0.005 dB, wf rows exact",
           framesEqual(sent, cap.frames, 0.0051f, why), why);
    bool rowsExact = true;
    for (int i = 0; i < sent.size() && i < cap.frames.size(); ++i) {
        if (sent[i].isRow && sent[i].bins != cap.frames[i].bins) { rowsExact = false; }
    }
    report("default encoding: waterfall rows bit-exact (Q7)", rowsExact);
    const qint64 raw = 200LL * 2048 * 4;
    const qint64 onDisk = QFileInfo(rec.firstFilePath()).size();
    report("compression: file smaller than raw float data", onDisk < raw,
           std::to_string(onDisk) + " vs raw " + std::to_string(raw));
}

void testCrashRecovery(const QString& dir)
{
    SpectrumRecorder rec;
    auto o = baseOptions(dir, QStringLiteral("crash"));
    o.losslessFft = true;
    const QVector<Frame> sent = recordSession(rec, o, 200, 512, 3, false);
    const QString path = rec.firstFilePath();

    SpecRec::ScanResult clean;
    SpecRec::scanFile(path, clean);
    report("crash: clean file scans without truncation", !clean.truncated && clean.chunks.size() >= 5,
           "chunks=" + std::to_string(clean.chunks.size()));

    // Simulate a crash mid-write: chop the final chunk in half.
    const auto& last = clean.chunks.constLast();
    const qint64 cut = last.offset + SpecRec::kChunkHeaderBytes + last.compLen / 2;
    { QFile f(path); f.resize(cut); }

    SpecRec::ScanResult torn;
    SpecRec::scanFile(path, torn);
    report("crash: torn tail detected, earlier chunks kept",
           torn.truncated && torn.chunks.size() == clean.chunks.size() - 1);

    SpectrumPlayer p;
    Capture cap;
    cap.wire(p);
    const bool opened = p.open(path);
    p.advanceTo(p.durationMs() + 1000);
    // Everything in the surviving chunks must still be identical.
    std::string why;
    const bool prefixOk = opened && cap.frames.size() > 0 && cap.frames.size() < sent.size()
        && framesEqual(sent.mid(0, cap.frames.size()), cap.frames, 0.0f, why);
    report("crash: player plays the valid prefix identically", prefixOk,
           "frames=" + std::to_string(cap.frames.size()) + " " + why);

    qint64 dropped = 0;
    const bool rec1 = SpecRec::recoverFile(path, &dropped);
    SpecRec::ScanResult fixed;
    SpecRec::scanFile(path, fixed);
    report("crash: recoverFile truncates to last valid chunk",
           rec1 && dropped > 0 && !fixed.truncated && fixed.fileSize == torn.validEnd);

    // Corrupt a byte inside a middle chunk's payload: that chunk is skipped,
    // later chunks still play (CRC isolates damage).
    const auto& mid = fixed.chunks[fixed.chunks.size() / 2];
    {
        QFile f(path);
        f.open(QIODevice::ReadWrite);
        f.seek(mid.offset + SpecRec::kChunkHeaderBytes + 10);
        char c = 0;
        f.getChar(&c);
        f.seek(mid.offset + SpecRec::kChunkHeaderBytes + 10);
        f.putChar(static_cast<char>(c ^ 0x5A));
    }
    SpectrumPlayer p2;
    Capture cap2;
    cap2.wire(p2);
    p2.open(path);
    p2.advanceTo(p2.durationMs() + 1000);
    report("crash: corrupted middle chunk skipped, playback continues",
           cap2.frames.size() > 0 && cap2.frames.size() < cap.frames.size()
           && cap2.frames.last().bins == cap.frames.last().bins);

    // Header-only file (crash right after start) is valid but empty.
    const QString empty = QDir(dir).filePath(QStringLiteral("empty.aesr"));
    {
        QFile f(empty);
        f.open(QIODevice::WriteOnly);
        f.write(SpecRec::encodeFileHeader({}));
        f.write("CHN", 3);   // torn first chunk header
    }
    SpecRec::ScanResult e;
    const bool eok = SpecRec::scanFile(empty, e);
    report("crash: header-only file scans with zero chunks", eok && e.chunks.isEmpty() && e.truncated);

    const QString junk = QDir(dir).filePath(QStringLiteral("junk.aesr"));
    { QFile f(junk); f.open(QIODevice::WriteOnly); f.write("not a recording at all"); }
    SpecRec::ScanResult j;
    report("format: non-.aesr file rejected", !SpecRec::scanFile(junk, j));
}

void testRotation(const QString& dir)
{
    SpectrumRecorder rec;
    auto o = baseOptions(dir, QStringLiteral("rotate"));
    o.losslessFft = true;
    o.maxSegmentBytes = 64 * 1024;       // tiny segments to force rotation
    o.flushIntervalMs = 200;
    const QVector<Frame> sent = recordSession(rec, o, 600, 1024, 4);
    const QStringList segs = SpecRec::segmentPaths(rec.firstFilePath());
    report("rotation: multiple _partNNN segments written", segs.size() >= 3,
           "segments=" + std::to_string(segs.size()));
    bool sizesOk = true;
    for (int i = 0; i + 1 < segs.size(); ++i) {
        // A segment closes after the chunk that crosses the limit.
        if (QFileInfo(segs[i]).size() < o.maxSegmentBytes) { sizesOk = false; }
    }
    report("rotation: each closed segment reached the size limit", sizesOk);

    SpecRec::ScanResult s2;
    SpecRec::scanFile(segs.value(1), s2);
    report("rotation: later segment has its own header + keyframe",
           s2.header.meta.value("segment").toInt() == 2 && !s2.chunks.isEmpty());

    SpectrumPlayer p;
    Capture cap;
    cap.wire(p);
    p.open(rec.firstFilePath());
    p.advanceTo(p.durationMs() + 1000);
    std::string why;
    report("rotation: playback spans all segments identically",
           framesEqual(sent, cap.frames, 0.0f, why), why);
}

void testLimitsAndBudget(const QString& dir)
{
    // Size limit stops the recording.
    {
        SpectrumRecorder rec;
        auto o = baseOptions(dir, QStringLiteral("limit"));
        o.losslessFft = true;
        o.maxTotalBytes = 100 * 1024;
        o.flushIntervalMs = 100;
        QString reason;
        QObject::connect(&rec, &SpectrumRecorder::stopped,
                         [&](const QString& r, const QString&) { reason = r; });
        recordSession(rec, o, 800, 1024, 5);
        QCoreApplication::processEvents();
        const qint64 sz = QFileInfo(rec.firstFilePath()).size();
        report("limits: maxTotalBytes stops recording with reason size-limit",
               reason == QLatin1String("size-limit") && sz < 2 * o.maxTotalBytes,
               reason.toStdString() + " size=" + std::to_string(sz));
    }
    // Duration limit.
    {
        SpectrumRecorder rec;
        auto o = baseOptions(dir, QStringLiteral("dur"));
        o.maxDurationMs = 5000;
        o.flushIntervalMs = 500;
        QString reason;
        QObject::connect(&rec, &SpectrumRecorder::stopped,
                         [&](const QString& r, const QString&) { reason = r; });
        recordSession(rec, o, 400, 256, 6);
        QCoreApplication::processEvents();
        SpecRec::Reader rd;
        rd.open(rec.firstFilePath());
        report("limits: maxDurationMs stops recording",
               reason == QLatin1String("duration-limit") && rd.durationMs() <= 5600,
               std::to_string(rd.durationMs()) + " ms");
    }
    // Bounded memory: with a tiny pending budget frames are dropped, never
    // queued without bound, and every frame is either written or counted.
    {
        SpectrumRecorder rec;
        auto o = baseOptions(dir, QStringLiteral("budget"));
        o.maxPendingBytes = 64 * 1024;
        o.losslessFft = true;
        const QVector<Frame> sent = recordSession(rec, o, 3000, 4096, 7, false);
        SpectrumPlayer p;
        Capture cap;
        cap.wire(p);
        p.open(rec.firstFilePath());
        p.advanceTo(p.durationMs() + 1000);
        const qint64 total = cap.frames.size() + static_cast<qint64>(rec.droppedFrames());
        report("budget: written + dropped == submitted", total == sent.size(),
               "written=" + std::to_string(cap.frames.size())
               + " dropped=" + std::to_string(rec.droppedFrames()));
    }
}

void testSpeedCoalescing(const QString& dir)
{
    SpectrumRecorder rec;
    auto o = baseOptions(dir, QStringLiteral("speed"));
    const QVector<Frame> sent = recordSession(rec, o, 200, 256, 8, false);
    SpectrumPlayer p;
    Capture cap;
    cap.wire(p);
    p.open(rec.firstFilePath());
    p.setSpeed(8.0);
    for (qint64 ms = 0; ms <= p.durationMs() + 100; ms += 120) { p.advanceTo(ms); }
    int rows = 0, ffts = 0;
    for (const Frame& f : cap.frames) { (f.isRow ? rows : ffts)++; }
    report("8x: every waterfall row delivered, FFT frames coalesced",
           rows == 100 && ffts < 100 && ffts > 0,
           "rows=" + std::to_string(rows) + " ffts=" + std::to_string(ffts));
    report("8x: playback label", p.playbackLabel().isEmpty() == false);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) { return 2; }
    const QString dir = tmp.path();

    testLosslessRoundTrip(dir);
    testDefaultQuantisation(dir);
    testCrashRecovery(dir);
    testRotation(dir);
    testLimitsAndBudget(dir);
    testSpeedCoalescing(dir);

    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
