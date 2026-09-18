#include "DaxDevicePipeline.h"

#include <QMap>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Zero crossings of the sinc kernel on each side of the centre.  16 keeps
// the stop band well below the -60 dB a 16-bit digital-mode modem cares
// about while holding latency under 1 ms at 24 kHz.
constexpr int kZeroCrossings = 16;
// Pass band as a fraction of the lower Nyquist rate.  DAX audio from the
// radio is already band-limited to ~10 kHz at 24 kHz, so 0.9 loses nothing.
constexpr double kCutoffFraction = 0.9;

uint64_t roundUpPow2(uint64_t v)
{
    uint64_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

const QRegularExpression& dupPrefixRe()
{
    // Windows numbers duplicate endpoint names: "2- CABLE Input (...)".
    static const QRegularExpression re(QStringLiteral("^\\s*\\d+\\s*-\\s*"));
    return re;
}

QString stripDupPrefix(const QString& name)
{
    QString n = name.trimmed();
    n.remove(dupPrefixRe());
    return n;
}

} // namespace

// ── Sample format / channel conversion ──────────────────────────────────────

int daxBytesPerSample(DaxSampleFormat fmt)
{
    switch (fmt) {
    case DaxSampleFormat::UInt8: return 1;
    case DaxSampleFormat::Int16: return 2;
    case DaxSampleFormat::Int32: return 4;
    case DaxSampleFormat::Float: return 4;
    }
    return 4;
}

void daxConvertFromFloat(const float* src, void* dst, int samples, DaxSampleFormat fmt)
{
    if (samples <= 0) {
        return;
    }
    switch (fmt) {
    case DaxSampleFormat::UInt8: {
        auto* d = static_cast<uint8_t*>(dst);
        for (int i = 0; i < samples; ++i) {
            const float v = std::clamp(src[i] * 128.0f + 128.0f, 0.0f, 255.0f);
            d[i] = static_cast<uint8_t>(std::lrint(v));
        }
        break;
    }
    case DaxSampleFormat::Int16: {
        auto* d = static_cast<int16_t*>(dst);
        for (int i = 0; i < samples; ++i) {
            const float v = std::clamp(src[i] * 32768.0f, -32768.0f, 32767.0f);
            d[i] = static_cast<int16_t>(std::lrint(v));
        }
        break;
    }
    case DaxSampleFormat::Int32: {
        auto* d = static_cast<int32_t*>(dst);
        for (int i = 0; i < samples; ++i) {
            const double v = std::clamp(static_cast<double>(src[i]) * 2147483648.0,
                                        -2147483648.0, 2147483647.0);
            d[i] = static_cast<int32_t>(std::llrint(v));
        }
        break;
    }
    case DaxSampleFormat::Float:
        std::memcpy(dst, src, static_cast<size_t>(samples) * sizeof(float));
        break;
    }
}

void daxConvertToFloat(const void* src, float* dst, int samples, DaxSampleFormat fmt)
{
    if (samples <= 0) {
        return;
    }
    switch (fmt) {
    case DaxSampleFormat::UInt8: {
        const auto* s = static_cast<const uint8_t*>(src);
        for (int i = 0; i < samples; ++i) {
            dst[i] = (static_cast<float>(s[i]) - 128.0f) / 128.0f;
        }
        break;
    }
    case DaxSampleFormat::Int16: {
        const auto* s = static_cast<const int16_t*>(src);
        for (int i = 0; i < samples; ++i) {
            dst[i] = static_cast<float>(s[i]) / 32768.0f;
        }
        break;
    }
    case DaxSampleFormat::Int32: {
        const auto* s = static_cast<const int32_t*>(src);
        for (int i = 0; i < samples; ++i) {
            dst[i] = static_cast<float>(static_cast<double>(s[i]) / 2147483648.0);
        }
        break;
    }
    case DaxSampleFormat::Float:
        std::memcpy(dst, src, static_cast<size_t>(samples) * sizeof(float));
        break;
    }
}

void daxRemapChannels(const float* src, int srcChannels,
                      float* dst, int dstChannels, int frames)
{
    if (frames <= 0 || srcChannels <= 0 || dstChannels <= 0) {
        return;
    }
    if (srcChannels == dstChannels) {
        std::memcpy(dst, src, static_cast<size_t>(frames) * srcChannels * sizeof(float));
        return;
    }
    for (int f = 0; f < frames; ++f) {
        const float* in = src + static_cast<size_t>(f) * srcChannels;
        float* out = dst + static_cast<size_t>(f) * dstChannels;
        if (srcChannels == 1) {
            for (int c = 0; c < dstChannels; ++c) {
                out[c] = in[0];
            }
        } else if (dstChannels == 1) {
            out[0] = (in[0] + in[1]) * 0.5f;
        } else {
            // src ≥ 2, dst ≥ 2, different counts: carry L/R, silence the rest.
            out[0] = in[0];
            out[1] = in[1];
            for (int c = 2; c < dstChannels; ++c) {
                out[c] = 0.0f;
            }
        }
    }
}

// ── DaxSpscRing ─────────────────────────────────────────────────────────────

DaxSpscRing::DaxSpscRing(int channels, int capacityFrames)
    : m_channels(std::max(1, channels))
    , m_mask(roundUpPow2(static_cast<uint64_t>(std::max(2, capacityFrames))) - 1)
    , m_buf(static_cast<size_t>(m_mask + 1) * static_cast<size_t>(m_channels), 0.0f)
{}

int DaxSpscRing::availableFrames() const
{
    const uint64_t w = m_write.load(std::memory_order_acquire);
    const uint64_t r = m_read.load(std::memory_order_acquire);
    return static_cast<int>(w - r);
}

int DaxSpscRing::freeFrames() const
{
    return capacityFrames() - availableFrames();
}

int DaxSpscRing::write(const float* interleaved, int frames)
{
    if (frames <= 0) {
        return 0;
    }
    const uint64_t w = m_write.load(std::memory_order_relaxed);
    const uint64_t r = m_read.load(std::memory_order_acquire);
    const int space = capacityFrames() - static_cast<int>(w - r);
    const int n = std::min(frames, space);
    if (n <= 0) {
        return 0;
    }
    const uint64_t cap = m_mask + 1;
    const uint64_t start = w & m_mask;
    const uint64_t first = std::min<uint64_t>(static_cast<uint64_t>(n), cap - start);
    std::memcpy(&m_buf[start * m_channels], interleaved,
                first * m_channels * sizeof(float));
    if (first < static_cast<uint64_t>(n)) {
        std::memcpy(&m_buf[0], interleaved + first * m_channels,
                    (n - first) * m_channels * sizeof(float));
    }
    m_write.store(w + n, std::memory_order_release);
    return n;
}

int DaxSpscRing::read(float* interleaved, int frames)
{
    if (frames <= 0) {
        return 0;
    }
    const uint64_t r = m_read.load(std::memory_order_relaxed);
    const uint64_t w = m_write.load(std::memory_order_acquire);
    const int n = std::min(frames, static_cast<int>(w - r));
    if (n <= 0) {
        return 0;
    }
    const uint64_t cap = m_mask + 1;
    const uint64_t start = r & m_mask;
    const uint64_t first = std::min<uint64_t>(static_cast<uint64_t>(n), cap - start);
    std::memcpy(interleaved, &m_buf[start * m_channels],
                first * m_channels * sizeof(float));
    if (first < static_cast<uint64_t>(n)) {
        std::memcpy(interleaved + first * m_channels, &m_buf[0],
                    (n - first) * m_channels * sizeof(float));
    }
    m_read.store(r + n, std::memory_order_release);
    return n;
}

int DaxSpscRing::skip(int frames)
{
    if (frames <= 0) {
        return 0;
    }
    const uint64_t r = m_read.load(std::memory_order_relaxed);
    const uint64_t w = m_write.load(std::memory_order_acquire);
    const int n = std::min(frames, static_cast<int>(w - r));
    if (n <= 0) {
        return 0;
    }
    m_read.store(r + n, std::memory_order_release);
    return n;
}

// ── DaxPlayoutBuffer ────────────────────────────────────────────────────────

DaxPlayoutBuffer::Config DaxPlayoutBuffer::defaultConfig(int sampleRate, int targetLatencyMs)
{
    Config c;
    const int rate = std::max(8000, sampleRate);
    c.targetFrames = std::max(64, rate * std::max(5, targetLatencyMs) / 1000);
    // Dead band on the *smoothed* fill (the 0.5 s EMA already removes
    // packet jitter): ±5 ms keeps the mean close enough to the target that
    // a slow radio clock is corrected before jitter dips can underrun.
    c.hysteresisFrames = c.targetFrames / 8;
    c.maxFrames = c.targetFrames * 3 + rate / 20;
    c.adjustIntervalFrames = rate / 20;       // ≤ 1 correction per 50 ms
    c.smoothingFrames = rate / 2;             // 0.5 s fill-level average
    return c;
}

DaxPlayoutBuffer::DaxPlayoutBuffer(int channels, const Config& cfg)
    : m_ring(std::clamp(channels, 1, kMaxChannels),
             // 2 s at the device rate is ample; the drift logic keeps it near target.
             std::max(cfg.maxFrames * 2, cfg.smoothingFrames * 4))
    , m_cfg(cfg)
{}

int DaxPlayoutBuffer::push(const float* interleaved, int frames)
{
    const int written = m_ring.write(interleaved, frames);
    if (written < frames) {
        m_overflowFrames.fetch_add(static_cast<uint64_t>(frames - written),
                                   std::memory_order_relaxed);
    }
    return written;
}

void DaxPlayoutBuffer::pull(float* out, int frames)
{
    if (frames <= 0) {
        return;
    }
    const int ch = m_ring.channels();
    auto silence = [&](int fromFrame) {
        std::fill(out + static_cast<size_t>(fromFrame) * ch,
                  out + static_cast<size_t>(frames) * ch, 0.0f);
    };

    // The device pulls in periods we do not control (WASAPI ~10 ms; Qt 6.9
    // CoreAudio pull mode reads 2048 frames ≈ 43 ms at a time).  The buffer
    // must hold a whole period *plus* the jitter margin before each pull.
    if (frames > m_maxPullFrames) {
        m_maxPullFrames = frames;
        m_effectiveTarget.store(m_cfg.targetFrames + m_maxPullFrames, std::memory_order_relaxed);
    }
    const int target = m_cfg.targetFrames + m_maxPullFrames;
    const int maxFill = m_cfg.maxFrames + m_maxPullFrames;

    if (m_flushRequested.exchange(false, std::memory_order_acq_rel)) {
        m_ring.skip(m_ring.availableFrames());
        m_priming.store(true, std::memory_order_relaxed);
    }

    int avail = m_ring.availableFrames();

    if (m_priming.load(std::memory_order_relaxed)) {
        if (avail < target) {
            silence(0);
            return;
        }
        // Start exactly at the target: a packet-sized overshoot here would
        // otherwise be trimmed by hundreds of single-frame drift drops.
        if (avail > target) {
            m_ring.skip(avail - target);
            avail = target;
        }
        m_priming.store(false, std::memory_order_relaxed);
        m_smoothedFill = avail;
        m_framesSinceAdjust = 0;
    }

    // Hard resync after a stall / burst: jump back to the target fill.
    if (avail > maxFill) {
        m_ring.skip(avail - target);
        avail = target;
        m_smoothedFill = avail;
        m_framesSinceAdjust = 0;
        m_resyncs.fetch_add(1, std::memory_order_relaxed);
    }

    const double alpha = std::min(1.0, static_cast<double>(frames)
                                       / std::max(1, m_cfg.smoothingFrames));
    m_smoothedFill += alpha * (avail - m_smoothedFill);
    m_framesSinceAdjust += frames;

    // +1 = drop one frame (we are behind the producer), -1 = insert one.
    int adjust = 0;
    if (m_framesSinceAdjust >= m_cfg.adjustIntervalFrames && frames >= 2) {
        if (m_smoothedFill > target + m_cfg.hysteresisFrames) {
            adjust = 1;
        } else if (m_smoothedFill < target - m_cfg.hysteresisFrames) {
            adjust = -1;
        }
    }

    const int need = frames + adjust;
    if (avail < need) {
        // Underrun: play what we have, pad with silence, re-prime.
        const int got = m_ring.read(out, std::min(avail, frames));
        silence(got);
        m_priming.store(true, std::memory_order_relaxed);
        m_underruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (adjust > 0) {
        m_ring.read(out, frames);
        m_ring.skip(1);
        m_drops.fetch_add(1, std::memory_order_relaxed);
        m_smoothedFill -= 1.0;
        m_framesSinceAdjust = 0;
    } else if (adjust < 0) {
        m_ring.read(out, frames - 1);
        std::memcpy(out + static_cast<size_t>(frames - 1) * ch,
                    out + static_cast<size_t>(frames - 2) * ch,
                    static_cast<size_t>(ch) * sizeof(float));
        m_inserts.fetch_add(1, std::memory_order_relaxed);
        m_smoothedFill += 1.0;
        m_framesSinceAdjust = 0;
    } else {
        m_ring.read(out, frames);
    }
}

// ── DaxStreamResampler ──────────────────────────────────────────────────────

DaxStreamResampler::DaxStreamResampler(int channels, int inRate, int outRate)
    : m_channels(std::clamp(channels, 1, DaxPlayoutBuffer::kMaxChannels))
    , m_inRate(std::max(1, inRate))
    , m_outRate(std::max(1, outRate))
{
    m_step = static_cast<double>(m_inRate) / m_outRate;
    const double c = std::min(1.0, static_cast<double>(m_outRate) / m_inRate) * kCutoffFraction;
    m_halfWidth = kZeroCrossings / c;
    m_halfTaps = static_cast<int>(std::ceil(m_halfWidth));

    const int n = static_cast<int>(std::ceil(m_halfWidth * m_tableRes)) + 2;
    m_table.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double d = static_cast<double>(i) / m_tableRes;
        if (d >= m_halfWidth) {
            m_table[static_cast<size_t>(i)] = 0.0f;
            continue;
        }
        const double x = c * d;
        const double sinc = (x == 0.0) ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double t = d / m_halfWidth;  // 0..1
        const double blackman = 0.42 + 0.5 * std::cos(kPi * t) + 0.08 * std::cos(2.0 * kPi * t);
        m_table[static_cast<size_t>(i)] = static_cast<float>(c * sinc * blackman);
    }
    reset();
}

void DaxStreamResampler::reset()
{
    m_hist.assign(static_cast<size_t>(m_halfTaps) * m_channels, 0.0f);
    m_pos = m_halfTaps;
}

float DaxStreamResampler::kernel(double distance) const
{
    const double idx = distance * m_tableRes;
    const auto i = static_cast<size_t>(idx);
    if (i + 1 >= m_table.size()) {
        return 0.0f;
    }
    const float frac = static_cast<float>(idx - static_cast<double>(i));
    return m_table[i] + (m_table[i + 1] - m_table[i]) * frac;
}

int DaxStreamResampler::process(const float* in, int frames, std::vector<float>& out)
{
    if (frames <= 0) {
        return 0;
    }
    if (isPassthrough()) {
        out.insert(out.end(), in, in + static_cast<size_t>(frames) * m_channels);
        return frames;
    }

    m_hist.insert(m_hist.end(), in, in + static_cast<size_t>(frames) * m_channels);
    const int histFrames = static_cast<int>(m_hist.size() / m_channels);

    int produced = 0;
    std::array<double, DaxPlayoutBuffer::kMaxChannels> acc{};
    for (;;) {
        const int base = static_cast<int>(std::floor(m_pos));
        if (base + m_halfTaps >= histFrames) {
            break;
        }
        acc.fill(0.0);
        double wsum = 0.0;
        const int lo = std::max(0, base - m_halfTaps + 1);
        const int hi = base + m_halfTaps;
        for (int i = lo; i <= hi; ++i) {
            const float w = kernel(std::abs(static_cast<double>(i) - m_pos));
            if (w == 0.0f) {
                continue;
            }
            wsum += w;
            const float* frame = &m_hist[static_cast<size_t>(i) * m_channels];
            for (int c = 0; c < m_channels; ++c) {
                acc[static_cast<size_t>(c)] += static_cast<double>(w) * frame[c];
            }
        }
        const double norm = (wsum != 0.0) ? 1.0 / wsum : 0.0;
        for (int c = 0; c < m_channels; ++c) {
            out.push_back(static_cast<float>(acc[static_cast<size_t>(c)] * norm));
        }
        m_pos += m_step;
        ++produced;
    }

    // Keep only the history the next output still needs.
    const int keepFrom = std::clamp(static_cast<int>(std::floor(m_pos)) - m_halfTaps + 1,
                                    0, histFrames);
    if (keepFrom > 0) {
        m_hist.erase(m_hist.begin(),
                     m_hist.begin() + static_cast<std::ptrdiff_t>(keepFrom) * m_channels);
        m_pos -= keepFrom;
    }
    return produced;
}

// ── Virtual cable detection ─────────────────────────────────────────────────

DaxCableFamily daxClassifyDevice(const QString& rawName)
{
    const QString n = stripDupPrefix(rawName).toLower();
    if (n.isEmpty()) {
        return DaxCableFamily::NotVirtual;
    }
    // Our own macOS HAL devices are fed by the native backend, never mapped.
    if (n.contains(QLatin1String("aethersdr dax")) || n.contains(QLatin1String("aethersdr tx"))) {
        return DaxCableFamily::NotVirtual;
    }
    static const QRegularExpression letteredRe(
        QStringLiteral("(\\bcable-[a-z]\\b|vb-audio cable [a-z]\\b)"));
    static const QRegularExpression vbPlainRe(
        QStringLiteral("(^cable (input|output)\\b|vb-audio virtual cable|^vb-cable\\b)"));
    static const QRegularExpression vacRe(
        QStringLiteral("(virtual audio cable|^line \\d+$)"));
    static const QRegularExpression vaioRe(QStringLiteral("\\bvaio\\d*\\b"));

    if (n.contains(QLatin1String("voicemeeter")) || n.contains(vaioRe)) {
        return DaxCableFamily::Voicemeeter;
    }
    if (n.contains(QLatin1String("hi-fi cable"))) {
        return DaxCableFamily::VbHiFiCable;
    }
    if (n.contains(letteredRe)) {
        return DaxCableFamily::VbCableLettered;
    }
    if (n.contains(vbPlainRe)) {
        return DaxCableFamily::VbCable;
    }
    if (n.contains(vacRe)) {
        return DaxCableFamily::VirtualAudioCable;
    }
    if (n.contains(QLatin1String("blackhole"))) {
        return DaxCableFamily::BlackHole;
    }
    if (n.contains(QLatin1String("loopback")) || n.contains(QLatin1String("soundflower"))) {
        return DaxCableFamily::Loopback;
    }
    return DaxCableFamily::NotVirtual;
}

int daxCableRank(const QString& rawName)
{
    const QString n = stripDupPrefix(rawName).toLower();
    switch (daxClassifyDevice(rawName)) {
    case DaxCableFamily::VbCable:
        return 0;
    case DaxCableFamily::VbCableLettered: {
        static const QRegularExpression re(QStringLiteral("cable[- ]([a-z])\\b"));
        const auto m = re.match(n);
        const int letter = m.hasMatch() ? (m.captured(1).at(0).unicode() - 'a') : 0;
        return 10 + std::clamp(letter, 0, 15);
    }
    case DaxCableFamily::VbHiFiCable:
        return 30;
    case DaxCableFamily::VirtualAudioCable: {
        static const QRegularExpression re(QStringLiteral("line (\\d+)"));
        const auto m = re.match(n);
        const int line = m.hasMatch() ? m.captured(1).toInt() : 0;
        return 40 + std::clamp(line, 0, 15);
    }
    case DaxCableFamily::BlackHole:
        return n.contains(QLatin1String("2ch")) ? 60 : 61;
    case DaxCableFamily::Loopback:
        return 70;
    case DaxCableFamily::Voicemeeter:
        if (n.contains(QLatin1String("aux"))) {
            return 81;
        }
        return n.contains(QRegularExpression(QStringLiteral("vaio\\d"))) ? 82 : 80;
    case DaxCableFamily::NotVirtual:
        break;
    }
    return 1000;
}

QString daxCableKey(const QString& rawName)
{
    static const QRegularExpression dirRe(
        QStringLiteral("\\b(input|output|playback|capture|recording|record)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    QString n = stripDupPrefix(rawName);
    n.remove(dirRe);
    n.replace(wsRe, QStringLiteral(" "));
    return n.trimmed().toLower();
}

bool daxDevicesShareCable(const QString& outputName, const QString& inputName)
{
    if (outputName.isEmpty() || inputName.isEmpty()) {
        return false;
    }
    if (daxClassifyDevice(outputName) == DaxCableFamily::NotVirtual
        && daxClassifyDevice(inputName) == DaxCableFamily::NotVirtual) {
        return false;
    }
    return daxCableKey(outputName) == daxCableKey(inputName);
}

DaxDeviceSuggestion suggestDaxDeviceMapping(const QStringList& outputDevices,
                                            const QStringList& inputDevices)
{
    struct Cable {
        QString key;
        QString playback;  // endpoint AetherSDR / the app writes to
        QString capture;   // endpoint the other side reads from
        int rank{1000};
        bool voicemeeter{false};
    };
    QMap<QString, Cable> byKey;

    for (const QString& out : outputDevices) {
        if (daxClassifyDevice(out) == DaxCableFamily::NotVirtual) {
            continue;
        }
        Cable& c = byKey[daxCableKey(out)];
        if (c.playback.isEmpty()) {
            c.key = daxCableKey(out);
            c.playback = out;
            c.rank = daxCableRank(out);
            c.voicemeeter = (daxClassifyDevice(out) == DaxCableFamily::Voicemeeter);
        }
    }
    for (const QString& in : inputDevices) {
        if (daxClassifyDevice(in) == DaxCableFamily::NotVirtual) {
            continue;
        }
        const QString key = daxCableKey(in);
        auto it = byKey.find(key);
        if (it != byKey.end() && it->capture.isEmpty()) {
            it->capture = in;
        }
    }

    std::vector<Cable> cables;
    for (const Cable& c : std::as_const(byKey)) {
        if (!c.playback.isEmpty() && !c.capture.isEmpty()) {
            cables.push_back(c);
        }
    }
    std::sort(cables.begin(), cables.end(), [](const Cable& a, const Cable& b) {
        if (a.rank != b.rank) {
            return a.rank < b.rank;
        }
        return a.key < b.key;
    });

    DaxDeviceSuggestion s;
    if (cables.empty()) {
        s.notes << QStringLiteral(
            "No virtual audio cables found. Install VB-Audio Virtual Cable "
            "(free; the A+B pack gives separate RX and TX cables), VAC, or on "
            "macOS BlackHole, then press Auto-detect again.");
        return s;
    }

    auto assignRx = [&](int ch, const Cable& c) {
        s.rx[static_cast<size_t>(ch)] = c.playback;
        s.rxAppDevice[static_cast<size_t>(ch)] = c.capture;
    };

    size_t next = 0;
    assignRx(0, cables[next++]);
    if (next < cables.size()) {
        const Cable& t = cables[next++];
        s.tx = t.capture;
        s.txAppDevice = t.playback;
    } else {
        s.notes << QStringLiteral(
            "Only one virtual cable was found, so it is used for DAX RX 1 and "
            "DAX TX is left unmapped. DAX TX needs its own cable (e.g. VB-Audio "
            "Cable A+B): sharing one cable for RX and TX would transmit the "
            "received audio.");
    }
    for (int ch = 1; ch < 4 && next < cables.size(); ++ch) {
        assignRx(ch, cables[next++]);
    }

    const bool usesVoicemeeter = std::any_of(cables.begin(),
        cables.begin() + static_cast<std::ptrdiff_t>(next),
        [](const Cable& c) { return c.voicemeeter; });
    if (usesVoicemeeter) {
        s.notes << QStringLiteral(
            "Voicemeeter VAIO/AUX is a mixer bus: make sure that bus is not also "
            "routed to your speakers or to a hardware output that feeds the radio.");
    }
    return s;
}

} // namespace AetherSDR
