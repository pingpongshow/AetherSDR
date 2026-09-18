// IQ WAV / RF64 writer, 'auxi' chunk, file naming, crash repair, IqRecorder
// and IQ → spectrum conversion tests.
//
// Build: produced by CMake as `iq_wav_test`.
// Run:   ./build/iq_wav_test      (exit 0 = all pass)

#include "core/IqRecorder.h"
#include "core/IqSpectrumConverter.h"
#include "core/SpectrumRecordingFormat.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) { ++g_failed; }
}

quint32 le32(const QByteArray& b, int off) { return qFromLittleEndian<quint32>(b.constData() + off); }
quint16 le16(const QByteArray& b, int off) { return qFromLittleEndian<quint16>(b.constData() + off); }

std::vector<float> tone(int frames, double freqHz, int rate, float amp = 0.5f)
{
    std::vector<float> v(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double ph = 2.0 * std::numbers::pi * freqHz * i / rate;
        v[size_t(i) * 2]     = amp * static_cast<float>(std::cos(ph));
        v[size_t(i) * 2 + 1] = amp * static_cast<float>(std::sin(ph));
    }
    return v;
}

QByteArray readAll(const QString& p)
{
    QFile f(p);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}

void testFloatWriter(const QString& dir)
{
    const QDateTime start(QDate(2026, 9, 17), QTime(14, 25, 1, 250), QTimeZone::UTC);
    const QString path = QDir(dir).filePath(IqWav::makeFileName(start, 14074000));
    report("naming: SDR#-style AetherSDR_<date>_<time>Z_<Hz>Hz_IQ.wav",
           QFileInfo(path).fileName() == QLatin1String("AetherSDR_20260917_142501Z_14074000Hz_IQ.wav"),
           QFileInfo(path).fileName().toStdString());

    IqWavWriter w;
    IqWavWriter::Params p;
    p.sampleRate = 48000;
    p.centerHz = 14074000;
    p.startUtc = start;
    const auto data = tone(4800, 1000.0, 48000);
    const bool ok = w.open(path, p) && w.writeFrames(data.data(), 4800) && w.close();
    report("float32 writer: open/write/close", ok);

    const QByteArray b = readAll(path);
    report("float32 header: RIFF/WAVE, JUNK(28) placeholder at 12",
           b.left(4) == "RIFF" && b.mid(8, 4) == "WAVE" && b.mid(12, 4) == "JUNK" && le32(b, 16) == 28);
    report("float32 header: RIFF size = file size - 8", le32(b, 4) == quint32(b.size() - 8));
    report("float32 fmt: tag 3, 2 ch, 48000, 32-bit, blockAlign 8, cbSize 0",
           b.mid(48, 4) == "fmt " && le32(b, 52) == 18 && le16(b, 56) == 3 && le16(b, 58) == 2
           && le32(b, 60) == 48000 && le32(b, 64) == 384000 && le16(b, 68) == 8 && le16(b, 70) == 32
           && le16(b, 72) == 0);
    report("auxi chunk: id + 164-byte body after fmt", b.mid(74, 4) == "auxi" && le32(b, 78) == 164);
    // auxi body at 82: SYSTEMTIME start (Thursday = 4), stop, CenterFreq, ADFrequency…
    report("auxi: StartTime SYSTEMTIME 2026-09-17 (Thu) 14:25:01.250",
           le16(b, 82) == 2026 && le16(b, 84) == 9 && le16(b, 86) == 4 && le16(b, 88) == 17
           && le16(b, 90) == 14 && le16(b, 92) == 25 && le16(b, 94) == 1 && le16(b, 96) == 250);
    report("auxi: StopTime = start + 100 ms", le16(b, 98 + 12) == 1 && le16(b, 98 + 14) == 350);
    report("auxi: CenterFreq / ADFrequency / Bandwidth",
           le32(b, 114) == 14074000 && le32(b, 118) == 48000 && le32(b, 126) == 48000);
    report("data chunk: size = frames × 8", b.mid(246, 4) == "data" && le32(b, 250) == 4800 * 8
           && b.size() == 254 + 4800 * 8);

    IqWavInfo info;
    const bool parsed = IqWav::parse(path, info);
    report("parse: own float file", parsed && !info.rf64 && info.formatTag == 3 && info.channels == 2
           && info.sampleRate == 48000 && info.frames == 4800 && info.hasAuxi
           && info.centerHz == 14074000 && info.startUtc == start && !info.headerSizeMismatch);

    QFile f(path);
    f.open(QIODevice::ReadOnly);
    std::vector<std::complex<float>> out;
    IqWav::readFrames(f, info, 100, 10, out);
    bool same = out.size() == 10;
    for (int i = 0; same && i < 10; ++i) {
        same = out[size_t(i)].real() == data[size_t(100 + i) * 2]
            && out[size_t(i)].imag() == data[size_t(100 + i) * 2 + 1];
    }
    report("readFrames: float samples bit-exact (I=left, Q=right)", same);
}

void testInt16Writer(const QString& dir)
{
    const QString path = QDir(dir).filePath(QStringLiteral("int16_IQ.wav"));
    IqWavWriter w;
    IqWavWriter::Params p;
    p.sampleRate = 24000;
    p.centerHz = 7100000;
    p.format = IqSampleFormat::Int16;
    const auto data = tone(2400, -3000.0, 24000, 0.9f);
    w.open(path, p);
    w.writeFrames(data.data(), 2400);
    w.close();
    IqWavInfo info;
    IqWav::parse(path, info);
    report("int16: PCM tag 1, 16-bit, blockAlign 4, fmt size 16",
           info.formatTag == 1 && info.bitsPerSample == 16 && info.blockAlign == 4 && info.frames == 2400);
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    std::vector<std::complex<float>> out;
    IqWav::readFrames(f, info, 0, 2400, out);
    float maxErr = 0.0f;
    for (size_t i = 0; i < out.size(); ++i) {
        maxErr = std::max(maxErr, std::fabs(out[i].real() - data[i * 2]));
        maxErr = std::max(maxErr, std::fabs(out[i].imag() - data[i * 2 + 1]));
    }
    report("int16: round-trip within 1 LSB", out.size() == 2400 && maxErr < 2.0f / 32768.0f,
           std::to_string(maxErr));
}

void testRf64(const QString& dir)
{
    const QString path = QDir(dir).filePath(QStringLiteral("big_IQ.wav"));
    IqWavWriter w;
    w.setRf64ThresholdForTest(1000);   // promote as if we had passed 4 GiB
    IqWavWriter::Params p;
    p.sampleRate = 96000;
    p.centerHz = 50313000;
    const auto data = tone(1000, 5000.0, 96000);
    w.open(path, p);
    w.writeFrames(data.data(), 1000);
    w.close();
    const QByteArray b = readAll(path);
    report("RF64: 'RF64' id, RIFF size 0xFFFFFFFF, JUNK→ds64",
           b.left(4) == "RF64" && le32(b, 4) == 0xFFFFFFFFu && b.mid(12, 4) == "ds64" && le32(b, 16) == 28);
    report("RF64: ds64 riffSize/dataSize/sampleCount 64-bit",
           qFromLittleEndian<quint64>(b.constData() + 20) == quint64(b.size() - 8)
           && qFromLittleEndian<quint64>(b.constData() + 28) == 8000
           && qFromLittleEndian<quint64>(b.constData() + 36) == 1000);
    report("RF64: data chunk size field 0xFFFFFFFF", le32(b, 250) == 0xFFFFFFFFu);
    IqWavInfo info;
    report("RF64: parser resolves sizes via ds64",
           IqWav::parse(path, info) && info.rf64 && info.frames == 1000 && info.centerHz == 50313000);
}

// A hand-built HDSDR/SDRuno-style file: PCM16, auxi with nextfilename, plus a
// trailing LIST chunk before data to exercise chunk walking.
void testForeignAuxi(const QString& dir)
{
    const QString path = QDir(dir).filePath(QStringLiteral("HDSDR_20240101_120000Z_7074kHz_RF.wav"));
    QByteArray body;
    auto p16 = [&](quint16 v) { char t[2]; qToLittleEndian(v, t); body.append(t, 2); };
    auto p32 = [&](quint32 v) { char t[4]; qToLittleEndian(v, t); body.append(t, 4); };
    body.append("WAVE");
    body.append("fmt "); p32(16); p16(1); p16(2); p32(192000); p32(192000 * 4); p16(4); p16(16);
    body.append("auxi"); p32(164);
    for (quint16 v : {quint16(2024), quint16(1), quint16(1), quint16(1), quint16(12), quint16(0), quint16(0), quint16(0)}) { p16(v); }
    for (quint16 v : {quint16(2024), quint16(1), quint16(1), quint16(1), quint16(12), quint16(5), quint16(0), quint16(0)}) { p16(v); }
    p32(7074000); p32(192000); p32(0); p32(192000); p32(0); p32(0); p32(0); p32(57539); p32(0);
    QByteArray next("HDSDR_20240101_120500Z_7074kHz_RF.wav");
    next.append(QByteArray(96 - next.size(), '\0'));
    body.append(next);
    body.append("LIST"); p32(3); body.append("abc"); body.append('\0');   // odd size + pad
    body.append("data"); p32(16);
    for (int i = 0; i < 8; ++i) { p16(quint16(i * 1000)); }
    QByteArray file("RIFF");
    char t[4]; qToLittleEndian<quint32>(quint32(body.size()), t); file.append(t, 4);
    file.append(body);
    { QFile f(path); f.open(QIODevice::WriteOnly); f.write(file); }

    IqWavInfo info;
    const bool ok = IqWav::parse(path, info);
    report("foreign auxi: parsed centre/rate/times/nextfilename",
           ok && info.hasAuxi && info.centerHz == 7074000 && info.sampleRate == 192000
           && info.startUtc == QDateTime(QDate(2024, 1, 1), QTime(12, 0), QTimeZone::UTC)
           && info.stopUtc == QDateTime(QDate(2024, 1, 1), QTime(12, 5), QTimeZone::UTC)
           && info.nextFileName == QLatin1String("HDSDR_20240101_120500Z_7074kHz_RF.wav")
           && info.frames == 4);
    report("filename: SDR# _<Hz>Hz_ parsed",
           IqWav::centerFromFileName(QStringLiteral("SDRSharp_20240520_181030Z_97300000Hz_IQ.wav")) == 97300000);
    report("filename: HDSDR/SDRuno _<kHz>kHz_ parsed",
           IqWav::centerFromFileName(QStringLiteral("HDSDR_20240101_120000Z_7074kHz_RF.wav")) == 7074000
           && IqWav::centerFromFileName(QStringLiteral("SDRuno_20240101_120000_800kHz.wav")) == 800000);
    report("filename: SDR++ baseband_<Hz>Hz_ parsed",
           IqWav::centerFromFileName(QStringLiteral("baseband_14074000Hz_12-00-00_01-01-2024.wav")) == 14074000);
}

void testCrashRepair(const QString& dir)
{
    const QString path = QDir(dir).filePath(QStringLiteral("crash_IQ.wav"));
    IqWavWriter w;
    IqWavWriter::Params p;
    p.sampleRate = 48000;
    const auto data = tone(1000, 100.0, 48000);
    w.open(path, p);
    w.writeFrames(data.data(), 1000);
    w.close();
    // Simulate a crash: 500 more frames reached disk but the header was not updated.
    {
        QFile f(path);
        f.open(QIODevice::Append);
        f.write(reinterpret_cast<const char*>(data.data()), 500 * 8);
    }
    IqWavInfo before;
    IqWav::parse(path, before);
    report("crash: stale header detected", before.headerSizeMismatch && before.frames == 1000);
    const bool fixed = IqWav::repairHeader(path);
    IqWavInfo after;
    IqWav::parse(path, after);
    report("crash: repairHeader recomputes sizes from file length",
           fixed && !after.headerSizeMismatch && after.frames == 1500);

    // Header never written past open(): data size 0 → parser trusts file length.
    const QString p2 = QDir(dir).filePath(QStringLiteral("crash0_IQ.wav"));
    {
        IqWavWriter w2;
        w2.open(p2, p);
        w2.writeFrames(data.data(), 1000);
        // copy the file before close() finalises it
        QFile::copy(p2, p2 + QStringLiteral(".snap"));
    }
    IqWavInfo z;
    // The snapshot may miss unflushed bytes; it only needs a parseable header.
    IqWav::parse(p2 + QStringLiteral(".snap"), z);
    report("crash: header with data size 0 falls back to file length",
           z.headerSizeMismatch && z.frames * 8 == QFileInfo(p2 + ".snap").size() - 254);
}

QByteArray bePayload(const std::vector<float>& iq, size_t from, size_t frames)
{
    QByteArray b(qsizetype(frames * 8), Qt::Uninitialized);
    for (size_t i = 0; i < frames * 2; ++i) {
        quint32 u;
        std::memcpy(&u, &iq[from * 2 + i], 4);
        qToBigEndian<quint32>(u, b.data() + i * 4);
    }
    return b;
}

void testRecorder(const QString& dirBase)
{
    const QString dir = QDir(dirBase).filePath(QStringLiteral("rec"));
    IqRecorder rec;
    QStringList opened;
    QObject::connect(&rec, &IqRecorder::fileOpened, [&](const QString& p) { opened << p; });
    IqRecorder::Options o;
    o.directory = dir;
    o.channel = 2;
    o.centerHz = 14074000;
    o.minFreeBytes = 0;
    QString err;
    report("recorder: start", rec.start(o, &err), err.toStdString());
    const auto data = tone(48000, 1500.0, 48000);
    for (size_t off = 0; off < 24000; off += 120) {
        rec.submitIq(2, bePayload(data, off, 120), 48000);
        rec.submitIq(1, bePayload(data, off, 120), 48000);   // other channel: ignored
    }
    rec.setCenterFrequency(14080000);   // → rotate
    QThread::msleep(1100);               // different second → distinct file name
    for (size_t off = 24000; off < 48000; off += 120) {
        rec.submitIq(2, bePayload(data, off, 120), 48000);
    }
    rec.stop();
    QCoreApplication::processEvents();
    report("recorder: centre change rotates to a second file", opened.size() == 2,
           "files=" + std::to_string(opened.size()));
    IqWavInfo a, b;
    const bool pa = IqWav::parse(opened.value(0), a);
    const bool pb = IqWav::parse(opened.value(1), b);
    report("recorder: files stamped with their own centre frequency",
           pa && pb && a.centerHz == 14074000 && b.centerHz == 14080000);
    report("recorder: all frames of the selected channel written",
           a.frames + b.frames == 48000 && rec.droppedPackets() == 0,
           std::to_string(a.frames + b.frames));
    QFile f(opened.value(1));
    f.open(QIODevice::ReadOnly);
    std::vector<std::complex<float>> out;
    IqWav::readFrames(f, b, 0, 4, out);
    report("recorder: big-endian DAX payload converted exactly",
           out.size() == 4 && out[0].real() == data[24000 * 2] && out[3].imag() == data[24003 * 2 + 1]);
}

void testConverter(const QString& dir)
{
    const QString wav = QDir(dir).filePath(QStringLiteral("AetherSDR_20260101_000000Z_7000000Hz_IQ.wav"));
    IqWavWriter w;
    IqWavWriter::Params p;
    p.sampleRate = 48000;
    p.centerHz = 7000000;
    p.startUtc = QDateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
    auto data = tone(48000 * 2, 6000.0, 48000, 0.25f);
    w.open(wav, p);
    w.writeFrames(data.data(), 48000 * 2);
    w.close();

    const QString out = QDir(dir).filePath(QStringLiteral("iqview.aesr"));
    IqSpectrumConverter::Options o;
    o.fftSize = 2048;
    o.framesPerSecond = 10;
    double lastProgress = 0.0;
    QString err;
    const bool ok = IqSpectrumConverter::convert(wav, out, o, [&](double pr) { lastProgress = pr; return true; }, &err);
    report("converter: IQ WAV → .aesr", ok && lastProgress == 1.0, err.toStdString());

    SpecRec::Reader rd;
    rd.open(out);
    int frames = 0, peakBin = -1;
    float peakDb = -1e9f;
    SpecRec::PanState ps;
    rd.forEach(rd.startUs(), rd.endUs(), [&](const SpecRec::Record& r) {
        if (r.type == SpecRec::RecordType::PanState) { ps = r.pan; }
        if (r.type == SpecRec::RecordType::FftFrame) {
            ++frames;
            if (frames == 5) {
                for (int i = 0; i < r.bins.size(); ++i) {
                    if (r.bins[i] > peakDb) { peakDb = r.bins[i]; peakBin = i; }
                }
            }
        }
        return true;
    });
    const int expectBin = 1024 + int(std::lround(6000.0 / 48000.0 * 2048));
    report("converter: ~10 fps for 2 s", frames >= 19 && frames <= 20, std::to_string(frames));
    report("converter: +6 kHz tone lands at centre + 6 kHz bin (fftshift)",
           std::abs(peakBin - expectBin) <= 1, std::to_string(peakBin) + " vs " + std::to_string(expectBin));
    report("converter: tone level ≈ -12 dBFS (amplitude 0.25)", std::fabs(peakDb + 12.04f) < 1.0f,
           std::to_string(peakDb));
    report("converter: pan keyframe centre/bandwidth from auxi",
           std::fabs(ps.centerMhz - 7.0) < 1e-9 && std::fabs(ps.bandwidthMhz - 0.048) < 1e-9
           && ps.maxDbm > peakDb && ps.minDbm < peakDb - 40);
    report("converter: timestamps start at auxi StartTime",
           rd.startUs() == p.startUtc.toMSecsSinceEpoch() * 1000);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) { return 2; }
    testFloatWriter(tmp.path());
    testInt16Writer(tmp.path());
    testRf64(tmp.path());
    testForeignAuxi(tmp.path());
    testCrashRepair(tmp.path());
    testRecorder(tmp.path());
    testConverter(tmp.path());
    std::printf("\n%s (%d failure%s)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
