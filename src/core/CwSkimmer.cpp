#include "CwSkimmer.h"

#include <QHash>
#include <QMetaObject>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace AetherSDR {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr int    kEnvelopeRate = 500;       // Hz — Morse timing resolution 2 ms
constexpr double kEnvelopeMs = 1000.0 / kEnvelopeRate;
constexpr int    kStage1Rate = 8000;        // Hz — after integrate-and-dump
constexpr double kChannelLpfHz = 75.0;      // one-sided → ~150 Hz channel
constexpr double kHistorySec = 0.5;         // IQ pre-roll used to prime new channels
constexpr int    kCandidateHits = 3;        // frames a peak must persist
constexpr double kHeldDecaySec = 3.0;       // held spectrum falls 10 dB in this time

const QHash<QString, QChar>& morseTable()
{
    static const QHash<QString, QChar> table = {
        {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},   {".", 'E'},
        {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},  {"..", 'I'},    {".---", 'J'},
        {"-.-", 'K'},   {".-..", 'L'},  {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},
        {".--.", 'P'},  {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
        {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},  {"-.--", 'Y'},
        {"--..", 'Z'},
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
        {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
        {"-..-.", '/'}, {"..--..", '?'}, {".-.-.-", '.'}, {"--..--", ','},
        {"-...-", '='}, {".-.-.", '+'}, {"-....-", '-'},
    };
    return table;
}

// RBJ low-pass biquad, transposed direct form II, complex data / real coeffs.
struct Biquad {
    double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
    std::complex<double> z1{}, z2{};

    void design(double fc, double fs, double q)
    {
        const double w0 = kTwoPi * fc / fs;
        const double cw = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = (1.0 - cw) / 2.0 / a0;
        b1 = (1.0 - cw) / a0;
        b2 = b0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    std::complex<double> step(std::complex<double> x)
    {
        const std::complex<double> y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ── Adaptive-timing Morse decoder on a 500 Hz envelope ─────────────────────
class MorseTimingDecoder {
public:
    // Appends characters decoded by this sample to `out`: letters, ' ' at a
    // word gap, '\n' at end-of-transmission (long silence).
    void push(float rawX, QString& out)
    {
        // Speed-adaptive post-detection smoothing (≈ 0.3 dit moving average):
        // a matched-ish filter that stops noise dropouts splitting dahs.
        const int len = m_ditMs > 0.0
            ? std::clamp(static_cast<int>(0.3 * m_ditMs / kEnvelopeMs), 1, kMaxSmooth) : 3;
        m_smooth[m_smoothPos] = rawX;
        m_smoothPos = (m_smoothPos + 1) % kMaxSmooth;
        float x = 0.0f;
        for (int i = 1; i <= len; ++i) {
            x += m_smooth[(m_smoothPos - i + kMaxSmooth) % kMaxSmooth];
        }
        x /= static_cast<float>(len);

        m_ring[m_ringPos] = x;
        m_ringPos = (m_ringPos + 1) % kRing;
        m_ringCount = std::min(m_ringCount + 1, kRing);

        if (++m_sinceLevels >= kLevelEvery && m_ringCount >= kRing / 4) {
            m_sinceLevels = 0;
            const bool wasOpen = m_squelchOpen;
            updateLevels();
            if (m_squelchOpen && !wasOpen && m_closedCount > 0) {
                // Squelch just opened: re-run the stretch that was stepped
                // while closed (all "key-up") with the new thresholds, so the
                // first characters of a transmission are not lost.
                const int n = std::min(m_closedCount, m_ringCount - 1);
                m_on = false;
                m_runMs = std::max(0.0, m_runMs - n * kEnvelopeMs);
                m_pendingMs = 0.0;
                m_closedCount = 0;
                const int start = (m_ringPos - 1 - n + 2 * kRing) % kRing;
                for (int i = 0; i < n; ++i) {
                    step(m_ring[(start + i) % kRing], out);
                }
            }
        }
        step(x, out);
        m_closedCount = m_squelchOpen ? 0 : std::min(m_closedCount + 1, kRing);
    }

    double ditMs() const { return m_ditMs; }
    float wpm() const { return m_ditMs > 0.0 ? static_cast<float>(1200.0 / m_ditMs) : 0.0f; }

private:
    static constexpr int   kRing = 1000;        // 2 s of envelope at 500 Hz
    static constexpr int   kMaxSmooth = 24;     // ≤ 48 ms smoothing window
    static constexpr int   kLevelEvery = 25;    // re-estimate levels every 50 ms
    static constexpr float kSquelchRatio = 5.0f;

    // Robust levels from percentiles of the last 2 s: 10th ≈ noise (CW
    // key-down duty rarely exceeds 90 % over 2 s), 85th ≈ key-down level.
    // For pure noise the 85th/10th ratio of a Rayleigh envelope is ≈ 4.2
    // before smoothing (less after), so 5× only opens on a real carrier.
    void updateLevels()
    {
        m_sorted.assign(m_ring.begin(), m_ring.begin() + m_ringCount);
        const auto lo = m_sorted.begin() + (m_ringCount * 10) / 100;
        std::nth_element(m_sorted.begin(), lo, m_sorted.end());
        m_noise = *lo;
        const auto hi = m_sorted.begin() + (m_ringCount * 85) / 100;
        std::nth_element(lo, hi, m_sorted.end());
        m_sig = *hi;
        m_squelchOpen = m_noise > 0.0f && m_sig > kSquelchRatio * m_noise;
    }

    void step(float x, QString& out)
    {
        const float range = m_sig - m_noise;
        bool raw = false;
        if (m_squelchOpen) {
            raw = m_on ? (x > m_noise + 0.35f * range) : (x > m_noise + 0.5f * range);
        }

        if (raw != m_on) {
            m_pendingMs += kEnvelopeMs;
            if (m_pendingMs >= debounceMs()) {
                // Confirmed transition, back-dated to the raw edge.
                if (m_on) {
                    onMarkEnd(m_runMs);
                } else {
                    onSpaceEnd(m_runMs);
                }
                m_on = raw;
                m_runMs = m_pendingMs;
                m_pendingMs = 0.0;
            }
        } else {
            m_runMs += m_pendingMs + kEnvelopeMs;
            m_pendingMs = 0.0;
        }

        if (!m_on) {
            checkSpace(m_runMs, out);
        }
    }

    double debounceMs() const
    {
        if (m_ditMs <= 0.0) {
            return 6.0;
        }
        return std::clamp(0.25 * m_ditMs, 4.0, 20.0);
    }

    void onMarkEnd(double m)
    {
        if (m < 10.0) {
            return;  // faster than 120 WPM: noise
        }
        m_marks.push_back(m);
        m_charPending = true;
        m_wordGapSent = false;
        m_eotSent = false;

        m_history.push_back(m);
        if (m_history.size() > 24) {
            m_history.pop_front();
        }
        if (m_ditMs <= 0.0 || (m_history.size() < 4 && m < 0.6 * m_ditMs)) {
            m_ditMs = m;  // bootstrap: first mark, or the first mark was a dah
        }
        reestimateDit();
    }

    // Two-cluster estimate over recent marks: split the sorted durations at
    // the largest ratio gap; short cluster = dits, long cluster = dahs (÷3).
    // Glitches (< ¼ of the median) are ignored, so a single noise spike can
    // no longer drag the speed estimate to the clamp and wedge the decoder.
    void reestimateDit()
    {
        if (m_history.size() < 4) {
            return;
        }
        std::vector<double> v(m_history.begin(), m_history.end());
        std::sort(v.begin(), v.end());
        const double med = v[v.size() / 2];
        v.erase(std::remove_if(v.begin(), v.end(), [med](double x) { return x < 0.25 * med; }),
                v.end());
        if (v.size() < 3) {
            return;
        }
        std::size_t split = 0;
        double bestRatio = 1.0;
        for (std::size_t i = 1; i < v.size(); ++i) {
            const double r = v[i] / v[i - 1];
            if (r > bestRatio) {
                bestRatio = r;
                split = i;
            }
        }
        double est = 0.0;
        if (bestRatio >= 1.8) {
            double sum = 0.0;
            for (std::size_t i = 0; i < v.size(); ++i) {
                sum += (i < split) ? v[i] : v[i] / 3.0;
            }
            est = sum / static_cast<double>(v.size());
        } else {
            // One cluster: all dits or all dahs.  Pick the reading closer
            // to the current estimate.
            double mean = 0.0;
            for (double x : v) {
                mean += x;
            }
            mean /= static_cast<double>(v.size());
            est = (std::abs(std::log(mean / m_ditMs)) <= std::abs(std::log(mean / 3.0 / m_ditMs)))
                ? mean : mean / 3.0;
        }
        m_ditMs = std::clamp(0.5 * m_ditMs + 0.5 * est, 20.0, 240.0);  // 60 … 5 WPM
    }

    void onSpaceEnd(double s)
    {
        // Intra-character spaces are one unit: use them to refine the estimate.
        if (m_ditMs > 0.0 && s > 0.5 * m_ditMs && s < 1.8 * m_ditMs) {
            m_ditMs += 0.1 * (s - m_ditMs);
        }
    }

    void checkSpace(double s, QString& out)
    {
        if (m_ditMs <= 0.0) {
            return;
        }
        if (m_charPending && s > 2.0 * m_ditMs) {
            emitChar(out);
        }
        if (!m_wordGapSent && !m_charPending && s > 5.0 * m_ditMs) {
            out += ' ';
            m_wordGapSent = true;
        }
        if (!m_eotSent && s > std::max(20.0 * m_ditMs, 1500.0)) {
            out += '\n';
            m_eotSent = true;
        }
    }

    void emitChar(QString& out)
    {
        QString pattern;
        for (double m : m_marks) {
            pattern += (m < 2.0 * m_ditMs) ? '.' : '-';
        }
        m_marks.clear();
        m_charPending = false;
        const auto it = morseTable().constFind(pattern);
        out += (it != morseTable().constEnd()) ? *it : QChar('*');
    }

    std::vector<float> m_ring = std::vector<float>(kRing, 0.0f);
    std::vector<float> m_smooth = std::vector<float>(kMaxSmooth, 0.0f);
    int    m_smoothPos{0};
    std::vector<float> m_sorted;
    int    m_ringPos{0};
    int    m_ringCount{0};
    int    m_sinceLevels{0};
    int    m_closedCount{0};        // samples stepped while squelch closed
    bool   m_squelchOpen{false};
    float  m_sig{0.0f};
    float  m_noise{0.0f};
    bool   m_on{false};
    double m_runMs{0.0};
    double m_pendingMs{0.0};
    double m_ditMs{0.0};
    std::vector<double> m_marks;     // marks of the character being received
    std::deque<double>  m_history;   // recent marks for speed estimation
    bool   m_charPending{false};
    bool   m_wordGapSent{true};
    bool   m_eotSent{true};
};

// FFT (iterative radix-2, in place).  Kept local so the skimmer adds no
// dependency (FFTW is optional in this project).
void fftInPlace(std::vector<std::complex<float>>& a,
                const std::vector<std::complex<float>>& tw,
                const std::vector<int>& bitrev)
{
    const int n = static_cast<int>(a.size());
    for (int i = 0; i < n; ++i) {
        if (i < bitrev[i]) {
            std::swap(a[i], a[bitrev[i]]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; ++j) {
                const std::complex<float> t = a[i + j + half] * tw[j * step];
                a[i + j + half] = a[i + j] - t;
                a[i + j] += t;
            }
        }
    }
}

} // namespace

// ── Channel: NCO → I&D ↓ → Butterworth → envelope → Morse ──────────────────

class CwSkimmerEngine::Channel {
public:
    Channel(int id, double offsetHz, int sampleRate)
        : m_id(id), m_fs(sampleRate)
    {
        m_decim = std::max(1, static_cast<int>(std::lround(double(sampleRate) / kStage1Rate)));
        const double stageRate = double(sampleRate) / m_decim;
        // 4th-order Butterworth = two biquads with Q 0.5412 / 1.3066.
        m_lpf[0].design(kChannelLpfHz, stageRate, 0.54119610);
        m_lpf[1].design(kChannelLpfHz, stageRate, 1.30656296);
        m_envDecim = std::max(1, static_cast<int>(std::lround(stageRate / kEnvelopeRate)));
        retune(offsetHz);
    }

    int id() const { return m_id; }
    double offsetHz() const { return m_offsetHz; }

    void retune(double offsetHz)
    {
        m_offsetHz = offsetHz;
        const double w = -kTwoPi * offsetHz / m_fs;
        m_rot = {std::cos(w), std::sin(w)};
    }

    void process(const std::complex<float>* x, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i) {
            m_acc += std::complex<double>(x[i]) * m_osc;
            m_osc *= m_rot;
            if (++m_renorm >= 1024) {
                m_osc /= std::abs(m_osc);
                m_renorm = 0;
            }
            if (++m_accCount < m_decim) {
                continue;
            }
            const std::complex<double> y = m_lpf[1].step(m_lpf[0].step(m_acc / double(m_decim)));
            m_acc = {};
            m_accCount = 0;

            m_env += std::abs(y);
            if (++m_envCount >= m_envDecim) {
                m_decoder.push(static_cast<float>(m_env / m_envDecim), m_pendingText);
                m_env = 0.0;
                m_envCount = 0;
            }
        }
    }

    QString takeText()
    {
        QString t;
        t.swap(m_pendingText);
        return t;
    }

    float wpm() const { return m_decoder.wpm(); }

    // Tracking / bookkeeping (owned by the engine)
    float  snrDb{0.0f};
    double lastActiveSec{0.0};
    QString text;
    QString curWord;
    std::deque<QString> words;
    int    evaluated{0};              // words[0..evaluated) already judged
    QHash<QString, double> spottedAt; // callsign → engine time of last spot
    QString lastCall;

private:
    int    m_id;
    double m_fs;
    double m_offsetHz{0.0};
    std::complex<double> m_osc{1.0, 0.0};
    std::complex<double> m_rot{1.0, 0.0};
    int    m_renorm{0};
    int    m_decim{1};
    int    m_accCount{0};
    std::complex<double> m_acc{};
    Biquad m_lpf[2];
    int    m_envDecim{16};
    int    m_envCount{0};
    double m_env{0.0};
    MorseTimingDecoder m_decoder;
    QString m_pendingText;
};

// ── Engine ─────────────────────────────────────────────────────────────────

CwSkimmerEngine::CwSkimmerEngine(int sampleRate, CwSkimmerConfig cfg)
    : m_sampleRate(std::max(8000, sampleRate)), m_cfg(cfg)
{
    // ≈12 Hz bins at every DAX IQ rate: 24k→2048, 48k→4096, 96k→8192, 192k→16384.
    int n = 256;
    while (static_cast<double>(m_sampleRate) / n > 12.0) {
        n <<= 1;
    }
    m_fftSize = n;
    m_hop = n / 2;

    m_window.resize(n);
    for (int i = 0; i < n; ++i) {
        m_window[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / n));
    }
    m_twiddle.resize(n / 2);
    for (int i = 0; i < n / 2; ++i) {
        const double a = -kTwoPi * i / n;
        m_twiddle[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    m_bitrev.resize(n);
    int bits = 0;
    while ((1 << bits) < n) {
        ++bits;
    }
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) {
            r |= ((i >> b) & 1) << (bits - 1 - b);
        }
        m_bitrev[i] = r;
    }
    m_fftIn.assign(n, {});
    m_fftBuf.assign(n, {});
    m_avgPower.assign(n, 0.0f);
    m_heldPower.assign(n, 0.0f);
    m_history.assign(static_cast<std::size_t>(m_sampleRate * kHistorySec), {});
}

CwSkimmerEngine::~CwSkimmerEngine() = default;

void CwSkimmerEngine::process(const std::complex<float>* iq, std::size_t count)
{
    std::size_t pos = 0;
    while (pos < count) {
        const std::size_t room = static_cast<std::size_t>(m_fftSize - m_fftFill);
        const std::size_t chunk = std::min(room, count - pos);
        const std::complex<float>* x = iq + pos;

        std::copy(x, x + chunk, m_fftIn.begin() + m_fftFill);
        m_fftFill += static_cast<int>(chunk);

        for (std::size_t i = 0; i < chunk; ++i) {
            m_history[m_historyPos] = x[i];
            if (++m_historyPos >= m_history.size()) {
                m_historyPos = 0;
            }
        }
        m_historyFilled = std::min(m_history.size(), m_historyFilled + chunk);
        m_samples += chunk;

        for (const std::unique_ptr<Channel>& ch : m_channels) {
            ch->process(x, chunk);
        }

        if (m_fftFill >= m_fftSize) {
            runDetector();
            std::copy(m_fftIn.begin() + m_hop, m_fftIn.end(), m_fftIn.begin());
            m_fftFill = m_fftSize - m_hop;
        }
        pos += chunk;

        drainChannels();
    }
}

void CwSkimmerEngine::drainChannels()
{
    for (const std::unique_ptr<Channel>& chPtr : m_channels) {
        Channel& ch = *chPtr;
        const QString t = ch.takeText();
        if (t.isEmpty()) {
            continue;
        }
        QString visible;
        for (QChar c : t) {
            if (c == '\n') {
                if (!ch.curWord.isEmpty()) {
                    handleWord(ch, ch.curWord);
                    ch.curWord.clear();
                }
                flushWords(ch);
                continue;
            }
            if (c == ' ') {
                if (!ch.curWord.isEmpty()) {
                    handleWord(ch, ch.curWord);
                    ch.curWord.clear();
                }
                if (!ch.text.endsWith(' ')) {
                    ch.text += ' ';
                    visible += ' ';
                }
                continue;
            }
            ch.curWord += c;
            ch.text += c;
            visible += c;
        }
        if (ch.text.size() > m_cfg.textTailChars) {
            ch.text.remove(0, ch.text.size() - m_cfg.textTailChars);
        }
        if (!visible.isEmpty() && m_onText) {
            m_onText(ch.id(), ch.offsetHz(), visible);
        }
    }
}

void CwSkimmerEngine::runDetector()
{
    const int n = m_fftSize;
    for (int i = 0; i < n; ++i) {
        m_fftBuf[i] = m_fftIn[i] * m_window[i];
    }
    fftInPlace(m_fftBuf, m_twiddle, m_bitrev);

    const double hopSec = static_cast<double>(m_hop) / m_sampleRate;
    const float decay = static_cast<float>(std::pow(10.0, -hopSec / kHeldDecaySec));
    constexpr float kAlpha = 0.3f;

    for (int k = 0; k < n; ++k) {
        // fftshift: index 0 = -fs/2, n/2 = DC
        const int src = (k + n / 2) % n;
        const float p = std::norm(m_fftBuf[src]);
        m_avgPower[k] += kAlpha * (p - m_avgPower[k]);
        m_heldPower[k] = std::max(m_avgPower[k], m_heldPower[k] * decay);
    }
    ++m_frame;

    // Median noise floor over the usable band (signals occupy few bins).
    const int lo = static_cast<int>(n * 0.05);
    const int hi = static_cast<int>(n * 0.95);
    std::vector<float> tmp(m_avgPower.begin() + lo, m_avgPower.begin() + hi);
    std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
    const float med = std::max(tmp[tmp.size() / 2], 1e-20f);
    m_noiseFloor = (m_noiseFloor <= 0.0f) ? med : m_noiseFloor + 0.2f * (med - m_noiseFloor);

    if (m_frame < kCandidateHits) {
        return;  // let the averages settle
    }

    const double binHz = static_cast<double>(m_sampleRate) / n;
    const float onRatio = static_cast<float>(std::pow(10.0, m_cfg.onSnrDb / 10.0));
    std::vector<double> peakHz;
    std::vector<float>  peakSnr;
    for (int k = std::max(lo, 2); k < std::min(hi, n - 2); ++k) {
        const float p = m_heldPower[k];
        if (p < onRatio * m_noiseFloor) {
            continue;
        }
        if (p < m_heldPower[k - 1] || p < m_heldPower[k + 1]
            || p < m_heldPower[k - 2] || p <= m_heldPower[k + 2]) {
            continue;
        }
        // Parabolic interpolation on log power for sub-bin frequency.
        const double l = std::log(std::max(m_heldPower[k - 1], 1e-30f));
        const double c = std::log(std::max(p, 1e-30f));
        const double r = std::log(std::max(m_heldPower[k + 1], 1e-30f));
        const double den = l - 2.0 * c + r;
        const double delta = (std::abs(den) > 1e-12) ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
        const double f = (k + delta - n / 2) * binHz;
        if (std::abs(f) < m_cfg.dcGuardHz) {
            continue;
        }
        peakHz.push_back(f);
        peakSnr.push_back(static_cast<float>(10.0 * std::log10(p / m_noiseFloor)));
    }
    updateTracks(peakHz, peakSnr);
}

float CwSkimmerEngine::snrAtOffset(double offsetHz) const
{
    const int n = m_fftSize;
    const double binHz = static_cast<double>(m_sampleRate) / n;
    const int k = static_cast<int>(std::lround(offsetHz / binHz)) + n / 2;
    float p = 0.0f;
    for (int j = std::max(0, k - 1); j <= std::min(n - 1, k + 1); ++j) {
        p = std::max(p, m_heldPower[j]);
    }
    if (m_noiseFloor <= 0.0f || p <= 0.0f) {
        return -100.0f;
    }
    return static_cast<float>(10.0 * std::log10(p / m_noiseFloor));
}

void CwSkimmerEngine::updateTracks(const std::vector<double>& peakHz,
                                   const std::vector<float>& peakSnr)
{
    const double now = timeSec();
    const double halfSep = m_cfg.minSeparationHz / 2.0;

    // 1. Refresh existing channels; retire idle ones.
    for (auto it = m_channels.begin(); it != m_channels.end();) {
        Channel& ch = **it;
        ch.snrDb = snrAtOffset(ch.offsetHz());
        if (ch.snrDb >= m_cfg.offSnrDb) {
            ch.lastActiveSec = now;
        }
        if (now - ch.lastActiveSec > m_cfg.idleTimeoutSec) {
            const int id = ch.id();
            it = m_channels.erase(it);
            if (m_onRemoved) {
                m_onRemoved(id);
            }
        } else {
            ++it;
        }
    }

    // 2. Match peaks to channels / candidates.
    for (std::size_t p = 0; p < peakHz.size(); ++p) {
        const double f = peakHz[p];
        Channel* owner = nullptr;
        for (const std::unique_ptr<Channel>& ch : m_channels) {
            if (std::abs(ch->offsetHz() - f) <= halfSep) {
                owner = ch.get();
                break;
            }
        }
        if (owner) {
            // Slow frequency refinement; the ±75 Hz channel tolerates drift.
            const double d = f - owner->offsetHz();
            if (std::abs(d) > 2.0) {
                owner->retune(owner->offsetHz() + 0.2 * d);
            }
            continue;
        }

        Candidate* cand = nullptr;
        for (Candidate& c : m_candidates) {
            if (std::abs(c.offsetHz - f) <= halfSep) {
                cand = &c;
                break;
            }
        }
        if (!cand) {
            m_candidates.push_back({f, 0, m_frame, peakSnr[p]});
            cand = &m_candidates.back();
        }
        cand->hits += 1;
        cand->lastFrame = m_frame;
        cand->offsetHz = f;
        cand->snrDb = peakSnr[p];
    }

    // 3. Promote persistent candidates to channels (bounded by maxChannels).
    for (auto it = m_candidates.begin(); it != m_candidates.end();) {
        if (m_frame - it->lastFrame > 4) {
            it = m_candidates.erase(it);
            continue;
        }
        if (it->hits < kCandidateHits) {
            ++it;
            continue;
        }
        if (static_cast<int>(m_channels.size()) >= m_cfg.maxChannels) {
            // Replace the weakest channel only for a clearly stronger carrier.
            auto weakest = std::min_element(m_channels.begin(), m_channels.end(),
                [](const std::unique_ptr<Channel>& a, const std::unique_ptr<Channel>& b) {
                    return a->snrDb < b->snrDb;
                });
            if (weakest == m_channels.end() || (*weakest)->snrDb + 6.0f > it->snrDb) {
                ++it;
                continue;
            }
            const int id = (*weakest)->id();
            m_channels.erase(weakest);
            if (m_onRemoved) {
                m_onRemoved(id);
            }
        }

        auto ch = std::make_unique<Channel>(m_nextChannelId++, it->offsetHz, m_sampleRate);
        ch->snrDb = it->snrDb;
        ch->lastActiveSec = now;
        // Prime with the IQ pre-roll so the first characters are not lost
        // while the detector was confirming the carrier.
        // Only the part of the ring actually written (engine may be < 0.5 s old).
        const std::size_t h = m_history.size();
        const std::size_t start = (m_historyPos + h - m_historyFilled) % h;
        if (start + m_historyFilled <= h) {
            ch->process(m_history.data() + start, m_historyFilled);
        } else {
            ch->process(m_history.data() + start, h - start);
            ch->process(m_history.data(), m_historyFilled - (h - start));
        }
        m_channels.push_back(std::move(ch));
        it = m_candidates.erase(it);
    }
}

bool CwSkimmerEngine::looksLikeCallsign(const QString& word)
{
    static const QRegularExpression re(QStringLiteral(
        "^(?:[A-Z0-9]{1,3}/)?"                       // optional prefix override
        "(?:[A-Z]{1,2}|[0-9][A-Z]{1,2}|[A-Z][0-9])"  // ITU prefix
        "[0-9]{1,2}"                                 // call-area digit(s)
        "[A-Z]{1,4}"                                 // suffix
        "(?:/[A-Z0-9]{1,4})?$"));                    // optional /P, /QRP, /7 …
    if (word.size() < 3 || word.size() > 12) {
        return false;
    }
    return re.match(word).hasMatch();
}

void CwSkimmerEngine::handleWord(Channel& ch, const QString& word)
{
    ch.words.push_back(word);
    // Judge the previous word now that we know what followed it.
    if (ch.words.size() >= 2 && ch.evaluated <= static_cast<int>(ch.words.size()) - 2) {
        evaluateWord(ch, static_cast<int>(ch.words.size()) - 2, word);
        ch.evaluated = static_cast<int>(ch.words.size()) - 1;
    }
    while (ch.words.size() > 24) {
        ch.words.pop_front();
        ch.evaluated = std::max(0, ch.evaluated - 1);
    }
}

void CwSkimmerEngine::flushWords(Channel& ch)
{
    if (!ch.words.empty() && ch.evaluated <= static_cast<int>(ch.words.size()) - 1) {
        evaluateWord(ch, static_cast<int>(ch.words.size()) - 1, QString());
        ch.evaluated = static_cast<int>(ch.words.size());
    }
}

void CwSkimmerEngine::evaluateWord(Channel& ch, int index, const QString& next)
{
    const QString& w = ch.words[static_cast<std::size_t>(index)];
    if (!looksLikeCallsign(w)) {
        return;
    }
    // "<called> DE <caller>": the word before DE is the station being called.
    if (next == QLatin1String("DE")) {
        return;
    }
    const QString prev = index > 0 ? ch.words[static_cast<std::size_t>(index - 1)] : QString();
    const bool keyed = prev == QLatin1String("DE") || prev == QLatin1String("CQ")
                    || prev == QLatin1String("TEST") || prev == QLatin1String("QRZ");
    int count = 0;
    bool cq = false;
    for (int i = 0; i < static_cast<int>(ch.words.size()); ++i) {
        const QString& x = ch.words[static_cast<std::size_t>(i)];
        if (x == w) {
            ++count;
        }
        if (i <= index && i >= index - 8 && x == QLatin1String("CQ")) {
            cq = true;
        }
    }
    // Precision over recall: a call must be copied at least twice on this
    // channel, and either follow CQ/DE/TEST/QRZ or be copied three times.
    if (count < 2 || (!keyed && count < 3)) {
        return;
    }

    QString dxcc;
    if (m_cty && m_cty->isLoaded()) {
        dxcc = m_cty->resolvePrimaryPrefix(w);
        if (dxcc.isEmpty()) {
            return;  // not a real prefix — almost certainly a decode error
        }
    }

    const double now = timeSec();
    const auto it = ch.spottedAt.constFind(w);
    if (it != ch.spottedAt.constEnd() && now - *it < m_cfg.respotIntervalSec) {
        return;
    }
    ch.spottedAt.insert(w, now);
    ch.lastCall = w;

    if (m_onSpot) {
        CwSkimmerSpot s;
        s.offsetHz = ch.offsetHz();
        s.freqHz = m_centerHz + ch.offsetHz();
        s.callsign = w;
        s.wpm = static_cast<int>(std::lround(ch.wpm()));
        s.snrDb = ch.snrDb;
        s.isCq = cq;
        s.dxccPrefix = dxcc;
        s.channelId = ch.id();
        s.timeSec = now;
        m_onSpot(s);
    }
}

std::vector<CwSkimmerChannelInfo> CwSkimmerEngine::channels() const
{
    std::vector<CwSkimmerChannelInfo> out;
    out.reserve(m_channels.size());
    for (const std::unique_ptr<Channel>& ch : m_channels) {
        CwSkimmerChannelInfo info;
        info.id = ch->id();
        info.offsetHz = ch->offsetHz();
        info.freqHz = m_centerHz + ch->offsetHz();
        info.snrDb = ch->snrDb;
        info.wpm = ch->wpm();
        info.text = ch->text;
        info.lastCall = ch->lastCall;
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const CwSkimmerChannelInfo& a, const CwSkimmerChannelInfo& b) {
        return a.offsetHz < b.offsetHz;
    });
    return out;
}

// ── Worker (skimmer thread) ────────────────────────────────────────────────

CwSkimmerWorker::CwSkimmerWorker(QObject* parent)
    : QObject(parent)
{}

CwSkimmerWorker::~CwSkimmerWorker() = default;

void CwSkimmerWorker::configure(int maxChannels)
{
    m_cfg.maxChannels = std::clamp(maxChannels, 1, 64);
    m_engine.reset();  // rebuilt with the new config on the next packet
}

void CwSkimmerWorker::loadCty(const QString& path)
{
    m_cty = CtyDatParser{};
    if (path.startsWith(QLatin1Char(':'))) {
        m_cty.loadFromResource(path);
    } else {
        m_cty.loadFromFile(path);
    }
}

void CwSkimmerWorker::setCenterFrequencyHz(double hz)
{
    m_centerHz = hz;
    if (m_engine) {
        m_engine->setCenterFrequencyHz(hz);
    }
}

void CwSkimmerWorker::setSwapIq(bool swap)
{
    m_swapIq = swap;
}

void CwSkimmerWorker::reset()
{
    m_engine.reset();
    emit channelsUpdated({});
}

void CwSkimmerWorker::ensureEngine(int sampleRate)
{
    if (m_engine && m_engine->sampleRate() == sampleRate) {
        return;
    }
    if (m_engine) {
        emit channelsUpdated({});
    }
    m_engine = std::make_unique<CwSkimmerEngine>(sampleRate, m_cfg);
    m_engine->setCenterFrequencyHz(m_centerHz);
    m_engine->setCtyParser(&m_cty);
    m_engine->onSpot([this](const CwSkimmerSpot& s) { emit spotDetected(s); });
    m_engine->onChannelRemoved([this](int id) { emit channelRemoved(id); });
    m_lastPublishSec = 0.0;
}

void CwSkimmerWorker::processPacket(const QByteArray& rawPayload, int sampleRate)
{
    m_pending.fetch_sub(1, std::memory_order_relaxed);
    if (sampleRate <= 0) {
        return;
    }
    ensureEngine(sampleRate);

    const int numFloats = static_cast<int>(rawPayload.size() / 4);
    const int numSamples = numFloats / 2;
    m_scratch.resize(static_cast<std::size_t>(numSamples));
    const auto* src = reinterpret_cast<const uchar*>(rawPayload.constData());
    for (int i = 0; i < numSamples; ++i) {
        // DAX IQ payload is big-endian float32 I/Q pairs (see DaxIqWorker).
        const quint32 iBits = qFromBigEndian<quint32>(src + 8 * i);
        const quint32 qBits = qFromBigEndian<quint32>(src + 8 * i + 4);
        float fi = 0.0f;
        float fq = 0.0f;
        std::memcpy(&fi, &iBits, sizeof(float));
        std::memcpy(&fq, &qBits, sizeof(float));
        m_scratch[static_cast<std::size_t>(i)] = m_swapIq ? std::complex<float>(fq, fi)
                                                          : std::complex<float>(fi, fq);
    }
    m_engine->process(m_scratch.data(), m_scratch.size());

    // Publish channel table at ≤ 4 Hz of IQ time.
    const double t = m_engine->timeSec();
    if (t - m_lastPublishSec >= 0.25) {
        m_lastPublishSec = t;
        const std::vector<CwSkimmerChannelInfo> ch = m_engine->channels();
        emit channelsUpdated(QVector<CwSkimmerChannelInfo>(ch.begin(), ch.end()));
    }
}

// ── Facade (GUI thread) ────────────────────────────────────────────────────

CwSkimmer::CwSkimmer(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<AetherSDR::CwSkimmerSpot>();
    qRegisterMetaType<AetherSDR::CwSkimmerChannelInfo>();
    qRegisterMetaType<QVector<AetherSDR::CwSkimmerChannelInfo>>();

    m_thread.setObjectName(QStringLiteral("CwSkimmer"));
    m_worker = new CwSkimmerWorker;  // owned by m_thread: deleteLater on finish
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &CwSkimmerWorker::spotDetected, this, &CwSkimmer::spotDetected);
    connect(m_worker, &CwSkimmerWorker::channelsUpdated, this, &CwSkimmer::channelsUpdated);
    connect(m_worker, &CwSkimmerWorker::channelRemoved, this, &CwSkimmer::channelRemoved);

    m_thread.start(QThread::LowPriority);
    setCtyDatPath(QStringLiteral(":/cty.dat"));
}

CwSkimmer::~CwSkimmer()
{
    m_running = false;
    m_thread.quit();
    m_thread.wait();
}

void CwSkimmer::start()
{
    if (m_running.exchange(true)) {
        return;
    }
    emit runningChanged(true);
}

void CwSkimmer::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    QMetaObject::invokeMethod(m_worker, &CwSkimmerWorker::reset, Qt::QueuedConnection);
    emit runningChanged(false);
}

void CwSkimmer::setDaxIqChannel(int channel)
{
    const int ch = std::clamp(channel, 1, 4);
    if (m_daxIqChannel.exchange(ch) != ch) {
        QMetaObject::invokeMethod(m_worker, &CwSkimmerWorker::reset, Qt::QueuedConnection);
    }
}

void CwSkimmer::setCenterFrequencyMhz(double mhz)
{
    const double hz = mhz * 1.0e6;
    QMetaObject::invokeMethod(m_worker, [w = m_worker, hz] { w->setCenterFrequencyHz(hz); },
                              Qt::QueuedConnection);
}

void CwSkimmer::setMaxChannels(int n)
{
    QMetaObject::invokeMethod(m_worker, [w = m_worker, n] { w->configure(n); },
                              Qt::QueuedConnection);
}

void CwSkimmer::setSwapIq(bool swap)
{
    QMetaObject::invokeMethod(m_worker, [w = m_worker, swap] { w->setSwapIq(swap); },
                              Qt::QueuedConnection);
}

void CwSkimmer::setCtyDatPath(const QString& path)
{
    QMetaObject::invokeMethod(m_worker, [w = m_worker, path] { w->loadCty(path); },
                              Qt::QueuedConnection);
}

void CwSkimmer::feedRawIqPacket(int channel, const QByteArray& rawPayload, int sampleRate)
{
    // May run on the network thread (DirectConnection) — atomics only.
    if (!m_running.load(std::memory_order_relaxed)
        || channel != m_daxIqChannel.load(std::memory_order_relaxed)) {
        return;
    }
    std::atomic<int>& pending = m_worker->pendingCounter();
    if (pending.load(std::memory_order_relaxed) >= kMaxBacklogPackets) {
        return;  // worker is behind: drop rather than grow latency/memory
    }
    pending.fetch_add(1, std::memory_order_relaxed);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, rawPayload, sampleRate] {
        w->processPacket(rawPayload, sampleRate);
    }, Qt::QueuedConnection);
}

} // namespace AetherSDR
