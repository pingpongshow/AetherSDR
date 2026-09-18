#include "IqSpectrumConverter.h"
#include "IqRecorder.h"
#include "SpectrumRecordingFormat.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace AetherSDR {

namespace {

using cf = std::complex<float>;

// In-place iterative radix-2 FFT (forward). n must be a power of two.
void fftRadix2(std::vector<cf>& a, const std::vector<cf>& twiddle)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) { j ^= bit; }
        j ^= bit;
        if (i < j) { std::swap(a[i], a[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < len / 2; ++k) {
                const cf u = a[i + k];
                const cf v = a[i + k + len / 2] * twiddle[k * step];
                a[i + k]           = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

class FrameComputer {
public:
    explicit FrameComputer(int n) : m_n(n), m_buf(n), m_win(n), m_tw(n / 2), m_acc(n)
    {
        constexpr double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double x = 2.0 * std::numbers::pi * i / (n - 1);
            m_win[i] = static_cast<float>(a0 - a1 * std::cos(x) + a2 * std::cos(2 * x) - a3 * std::cos(3 * x));
            sum += m_win[i];
        }
        m_norm = static_cast<float>(1.0 / (sum * sum));   // full-scale tone → 0 dBFS
        for (int k = 0; k < n / 2; ++k) {
            const double ang = -2.0 * std::numbers::pi * k / n;
            m_tw[k] = cf(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        }
    }

    // samples.size() >= n; averages floor(size/n) (≤ maxAvg) FFTs.
    QVector<float> compute(const std::vector<cf>& samples, int maxAvg, float dbOffset)
    {
        std::fill(m_acc.begin(), m_acc.end(), 0.0f);
        const int avail = static_cast<int>(samples.size()) / m_n;
        const int navg = std::clamp(avail, 1, std::max(1, maxAvg));
        const int stride = (avail > navg) ? (avail / navg) : 1;
        int used = 0;
        for (int a = 0; a < navg; ++a) {
            const size_t off = static_cast<size_t>(a) * stride * m_n;
            if (off + m_n > samples.size()) { break; }
            for (int i = 0; i < m_n; ++i) { m_buf[i] = samples[off + i] * m_win[i]; }
            fftRadix2(m_buf, m_tw);
            for (int i = 0; i < m_n; ++i) { m_acc[i] += std::norm(m_buf[i]); }
            ++used;
        }
        QVector<float> out(m_n);
        const float scale = m_norm / std::max(1, used);
        const int half = m_n / 2;
        for (int i = 0; i < m_n; ++i) {
            const int src = (i + half) % m_n;          // fftshift: −fs/2 … +fs/2
            out[i] = 10.0f * std::log10(m_acc[src] * scale + 1e-20f) + dbOffset;
        }
        return out;
    }

private:
    int m_n;
    std::vector<cf> m_buf;
    std::vector<float> m_win;
    std::vector<cf> m_tw;
    std::vector<float> m_acc;
    float m_norm{1.0f};
};

bool isPow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

} // namespace

bool IqSpectrumConverter::convert(const QString& wavPath, const QString& outPath,
                                  const Options& opts,
                                  const std::function<bool(double)>& progress, QString* err)
{
    if (!isPow2(opts.fftSize) || opts.fftSize < 256 || opts.fftSize > 65536) {
        if (err) { *err = QStringLiteral("FFT size must be a power of two (256-65536)"); }
        return false;
    }
    IqWavInfo info;
    if (!IqWav::parse(wavPath, info, err)) { return false; }
    if (info.channels != 2 || info.sampleRate <= 0 || info.frames < opts.fftSize) {
        if (err) { *err = QStringLiteral("not a 2-channel IQ file, or too short"); }
        return false;
    }
    QFile in(wavPath);
    if (!in.open(QIODevice::ReadOnly)) {
        if (err) { *err = in.errorString(); }
        return false;
    }

    const double fps = std::clamp(opts.framesPerSecond, 1.0, 100.0);
    const qint64 hop = std::max<qint64>(opts.fftSize, static_cast<qint64>(info.sampleRate / fps));
    const qint64 nFrames = std::max<qint64>(1, (info.frames - opts.fftSize) / hop + 1);
    const double frameSecs = static_cast<double>(hop) / info.sampleRate;
    FrameComputer fc(opts.fftSize);
    std::vector<cf> samples;

    // Pre-scan up to 16 frames spread across the file for a sensible dB range.
    std::vector<float> floorSamples, peakSamples;
    for (int k = 0; k < std::min<qint64>(16, nFrames); ++k) {
        const qint64 fi = nFrames * k / std::min<qint64>(16, nFrames);
        if (!IqWav::readFrames(in, info, fi * hop, static_cast<int>(hop), samples)) { continue; }
        if (static_cast<int>(samples.size()) < opts.fftSize) { continue; }
        QVector<float> b = fc.compute(samples, opts.maxAverages, static_cast<float>(opts.dbOffset));
        std::vector<float> v(b.begin(), b.end());
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        floorSamples.push_back(v[v.size() / 2]);
        peakSamples.push_back(*std::max_element(b.begin(), b.end()));
    }
    float noise = -100.0f, peak = -40.0f;
    if (!floorSamples.empty()) {
        std::sort(floorSamples.begin(), floorSamples.end());
        noise = floorSamples[floorSamples.size() / 2];
        peak = *std::max_element(peakSamples.begin(), peakSamples.end());
    }
    SpecRec::PanState pan;
    pan.centerMhz    = info.centerHz / 1e6;
    pan.bandwidthMhz = info.sampleRate / 1e6;
    pan.minDbm       = std::floor(noise - 10.0f);
    pan.maxDbm       = std::ceil(std::max(peak + 10.0f, pan.minDbm + 40.0f));

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) { *err = out.errorString(); }
        return false;
    }
    SpecRec::FileHeader h;
    h.createdUtcMs = QDateTime::currentMSecsSinceEpoch();
    h.meta.insert(QStringLiteral("app"), QStringLiteral("AetherSDR"));
    h.meta.insert(QStringLiteral("source"), QStringLiteral("iq"));
    h.meta.insert(QStringLiteral("iqFile"), QFileInfo(wavPath).fileName());
    h.meta.insert(QStringLiteral("sampleRate"), info.sampleRate);
    h.meta.insert(QStringLiteral("centerHz"), static_cast<double>(info.centerHz));
    h.meta.insert(QStringLiteral("fftSize"), opts.fftSize);
    h.meta.insert(QStringLiteral("units"), QStringLiteral("dBFS"));
    if (out.write(SpecRec::encodeFileHeader(h)) < 0) {
        if (err) { *err = out.errorString(); }
        return false;
    }

    QDateTime start = info.startUtc.isValid() ? info.startUtc : QFileInfo(wavPath).lastModified();
    const qint64 startUs = start.toMSecsSinceEpoch() * 1000;
    const int framesPerChunk = std::max(1, static_cast<int>(std::lround(fps)));

    QByteArray raw;
    quint32 count = 0;
    qint64 t0 = 0, t1 = 0;
    auto flush = [&]() -> bool {
        if (count == 0) { return true; }
        const QByteArray chunk = SpecRec::buildChunk(raw, t0, t1, count);
        raw.clear();
        count = 0;
        return out.write(chunk) == chunk.size();
    };

    for (qint64 fi = 0; fi < nFrames; ++fi) {
        const qint64 tUs = startUs + static_cast<qint64>(fi * frameSecs * 1e6);
        if (count == 0) {
            SpecRec::Record k;
            k.type = SpecRec::RecordType::PanState;
            k.tUs  = tUs;
            k.pan  = pan;
            SpecRec::appendRecord(raw, k);
            ++count;
            t0 = tUs;
        }
        if (!IqWav::readFrames(in, info, fi * hop, static_cast<int>(hop), samples)
            || static_cast<int>(samples.size()) < opts.fftSize) {
            break;
        }
        SpecRec::Record r;
        r.type     = SpecRec::RecordType::FftFrame;
        r.encoding = SpecRec::BinEncoding::Int16CentiDb;
        r.tUs      = tUs;
        r.bins     = fc.compute(samples, opts.maxAverages, static_cast<float>(opts.dbOffset));
        SpecRec::appendRecord(raw, r);
        ++count;
        t1 = tUs;
        if (fi % framesPerChunk == framesPerChunk - 1) {
            if (!flush()) {
                if (err) { *err = out.errorString(); }
                return false;
            }
            if (progress && !progress(static_cast<double>(fi + 1) / nFrames)) {
                if (err) { *err = QStringLiteral("cancelled"); }
                out.close();
                QFile::remove(outPath);
                return false;
            }
        }
    }
    if (!flush()) {
        if (err) { *err = out.errorString(); }
        return false;
    }
    if (progress) { progress(1.0); }
    return true;
}

} // namespace AetherSDR
