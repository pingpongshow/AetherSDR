#include "HfFaxDecoder.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace AetherSDR {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kBlackToneHz = 1500.0f;
constexpr float kWhiteToneHz = 2300.0f;
constexpr int kSearchWindow = 72;
constexpr float kAcquireThreshold = 0.14f;
constexpr float kWeakSyncThreshold = 0.40f;
constexpr int kWeakSyncGraceLines = 12;

inline float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

} // namespace

HfFaxDecoder::HfFaxDecoder(QObject* parent)
    : QObject(parent)
    , m_hilbertTaps(makeHilbertTaps())
{}

HfFaxDecoder::~HfFaxDecoder()
{
    stop();
}

void HfFaxDecoder::start()
{
    if (m_running)
        return;

    {
        QMutexLocker lock(&m_bufMutex);
        m_inputBuf.clear();
    }

    m_running = true;
    m_paused = false;
    m_clearRequested = false;
    resetSyncState();

    auto* worker = QThread::create([this]() { decodeLoop(); });
    worker->setObjectName("HfFaxDecoder");
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    m_workerThread = worker;
    worker->start();

    emit statusChanged(QStringLiteral("HF-Fax ready. Waiting for sync."));
}

void HfFaxDecoder::stop()
{
    if (!m_running)
        return;

    m_running = false;
    if (m_workerThread) {
        m_workerThread->wait(2000);
        m_workerThread = nullptr;
    }

    {
        QMutexLocker lock(&m_bufMutex);
        m_inputBuf.clear();
    }
    resetSyncState();
}

void HfFaxDecoder::clear()
{
    {
        QMutexLocker lock(&m_bufMutex);
        m_inputBuf.clear();
    }
    m_clearRequested = true;
}

void HfFaxDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    if (!m_running || m_paused || pcm24kStereo.isEmpty())
        return;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int frames = pcm24kStereo.size() / (2 * static_cast<int>(sizeof(float)));
    if (frames <= 0)
        return;

    QByteArray mono(frames * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(mono.data());
    for (int i = 0; i < frames; ++i)
        dst[i] = 0.5f * (src[2 * i] + src[2 * i + 1]);

    QMutexLocker lock(&m_bufMutex);
    m_inputBuf.append(mono);
    if (m_inputBuf.size() > kInputQueueCapacity)
        m_inputBuf.remove(0, m_inputBuf.size() - kInputQueueCapacity);
}

int HfFaxDecoder::lineSamples() const
{
    const double correction = 1.0 + (static_cast<double>(m_clockAdjustPpm.load()) / 1.0e6);
    return std::clamp(static_cast<int>(std::lround((kOutputRate * 60.0 / kLineRateLpm) * correction)),
                      2800, 3200);
}

int HfFaxDecoder::syncSamples() const
{
    return (kOutputRate * kSyncPulseMs) / 1000;
}

void HfFaxDecoder::resetSyncState(bool keepTransmissionState)
{
    m_brightnessBuf.clear();
    m_locked = false;
    m_weakSyncLines = 0;
    m_startToneCount = 0;
    m_stopToneCount = 0;
    m_havePrevPhase = false;
    m_prevPhase = 0.0f;
    m_freqEma = 1900.0f;
    m_decimationCounter = 0;
    m_toneEmitCounter = 0;
    m_samplesSeen = 0;
    m_historyPos = 0;
    m_history.fill(0.0f);
    if (!keepTransmissionState)
        m_transmissionActive = false;
}

std::array<HfFaxDecoder::HilbertTap, HfFaxDecoder::kHilbertTaps / 2> HfFaxDecoder::makeHilbertTaps()
{
    std::array<HilbertTap, kHilbertTaps / 2> taps{};
    constexpr int center = kHilbertTaps / 2;

    int out = 0;
    for (int i = 0; i < kHilbertTaps; ++i) {
        const int m = i - center;
        if (m == 0 || (m % 2) == 0)
            continue;

        const float window = 0.54f - 0.46f
            * std::cos((kTwoPi * static_cast<float>(i)) / static_cast<float>(kHilbertTaps - 1));
        const float coeff = (2.0f / (kPi * static_cast<float>(m))) * window;
        taps[out++] = {i, coeff};
    }
    return taps;
}

void HfFaxDecoder::decodeLoop()
{
    constexpr int kChunkSamples = 2400;
    const int chunkBytes = kChunkSamples * static_cast<int>(sizeof(float));

    while (m_running) {
        if (m_clearRequested.exchange(false)) {
            {
                QMutexLocker lock(&m_bufMutex);
                m_inputBuf.clear();
            }
            resetSyncState();
            emit syncStateChanged(false, 0.0f);
            emit statusChanged(QStringLiteral("HF-Fax buffer cleared. Waiting for sync."));
        }

        QByteArray chunk;
        {
            QMutexLocker lock(&m_bufMutex);
            if (m_inputBuf.size() < chunkBytes) {
                lock.unlock();
                QThread::msleep(20);
                continue;
            }
            chunk = m_inputBuf.left(chunkBytes);
            m_inputBuf.remove(0, chunkBytes);
        }

        processMonoBlock(reinterpret_cast<const float*>(chunk.constData()), kChunkSamples);
    }
}

void HfFaxDecoder::processMonoBlock(const float* mono, int sampleCount)
{
    constexpr int center = kHilbertTaps / 2;

    for (int n = 0; n < sampleCount; ++n) {
        m_history[m_historyPos] = mono[n];
        m_historyPos = (m_historyPos + 1) % kHilbertTaps;
        ++m_samplesSeen;

        if (m_samplesSeen < kHilbertTaps)
            continue;

        const int current = (m_historyPos - 1 + kHilbertTaps) % kHilbertTaps;
        const int delayed = (current - center + kHilbertTaps) % kHilbertTaps;
        const float iPart = m_history[delayed];

        float qPart = 0.0f;
        for (const auto& tap : m_hilbertTaps) {
            const int idx = (current - tap.delay + kHilbertTaps * 4) % kHilbertTaps;
            qPart += tap.coeff * m_history[idx];
        }

        const float power = iPart * iPart + qPart * qPart;
        if (power < 1.0e-8f)
            continue;

        const float phase = std::atan2(qPart, iPart);
        if (!m_havePrevPhase) {
            m_prevPhase = phase;
            m_havePrevPhase = true;
            continue;
        }

        float delta = phase - m_prevPhase;
        if (delta > kPi) delta -= kTwoPi;
        if (delta < -kPi) delta += kTwoPi;
        m_prevPhase = phase;

        float toneHz = (delta * static_cast<float>(kInputRate)) / kTwoPi;
        if (toneHz < 0.0f)
            toneHz += static_cast<float>(kInputRate);
        if (toneHz < 50.0f || toneHz > 4000.0f)
            continue;

        m_freqEma = (0.86f * m_freqEma) + (0.14f * toneHz);
        ++m_decimationCounter;
        if (m_decimationCounter >= kDecimation) {
            m_decimationCounter = 0;
            handleDecimatedTone(m_freqEma);
        }
    }
}

void HfFaxDecoder::handleDecimatedTone(float toneHz)
{
    if (++m_toneEmitCounter >= 24) {
        m_toneEmitCounter = 0;
        emit toneChanged(toneHz);
    }

    if (toneHz >= 220.0f && toneHz <= 380.0f)
        ++m_startToneCount;
    else
        m_startToneCount = 0;

    if (toneHz >= 360.0f && toneHz <= 540.0f)
        ++m_stopToneCount;
    else
        m_stopToneCount = 0;

    if (m_autoStart && m_startToneCount == (kOutputRate * 2)) {
        resetSyncState(true);
        m_transmissionActive = true;
        emit transmissionStarted();
        emit statusChanged(QStringLiteral("HF-Fax start tone detected."));
    }

    if (m_transmissionActive && m_stopToneCount == (kOutputRate * 2)) {
        m_transmissionActive = false;
        m_locked = false;
        m_brightnessBuf.clear();
        emit syncStateChanged(false, 0.0f);
        emit statusChanged(QStringLiteral("HF-Fax stop tone detected."));
        emit transmissionEnded();
        return;
    }

    const float brightness = clamp01((toneHz - kBlackToneHz) / (kWhiteToneHz - kBlackToneHz));
    m_brightnessBuf.push_back(brightness);

    const int maxBuffer = lineSamples() * 6;
    if (static_cast<int>(m_brightnessBuf.size()) > maxBuffer) {
        m_brightnessBuf.erase(m_brightnessBuf.begin(),
                              m_brightnessBuf.begin() + (m_brightnessBuf.size() - maxBuffer));
    }

    if (!m_locked) {
        maybeAcquireSync();
        return;
    }

    while (consumeLockedLine()) {}
}

bool HfFaxDecoder::maybeAcquireSync()
{
    const int ls = lineSamples();
    const int sync = syncSamples();
    if (static_cast<int>(m_brightnessBuf.size()) < ls * 3)
        return false;

    std::vector<float> prefix(m_brightnessBuf.size() + 1, 0.0f);
    for (int i = 0; i < static_cast<int>(m_brightnessBuf.size()); ++i)
        prefix[i + 1] = prefix[i] + m_brightnessBuf[static_cast<size_t>(i)];

    auto meanRange = [&prefix](int start, int count) -> float {
        const int a = std::max(0, start);
        const int b = std::max(a + 1, start + count);
        return (prefix[static_cast<size_t>(b)] - prefix[static_cast<size_t>(a)])
             / static_cast<float>(b - a);
    };

    int bestOffset = 0;
    float bestScore = -1.0f;
    for (int offset = 0; offset <= ls; offset += 2) {
        if (offset + (ls * 2) >= static_cast<int>(m_brightnessBuf.size()))
            break;

        const float pulseMean = 0.5f * (meanRange(offset, sync)
                                      + meanRange(offset + ls, sync));
        const float lineMean = 0.5f * (meanRange(offset, ls)
                                     + meanRange(offset + ls, ls));
        const float score = pulseMean - lineMean;
        if (score > bestScore) {
            bestScore = score;
            bestOffset = offset;
        }
    }

    if (bestScore < kAcquireThreshold)
        return false;

    const int trim = std::max(0, bestOffset - kSearchWindow);
    if (trim > 0)
        m_brightnessBuf.erase(m_brightnessBuf.begin(), m_brightnessBuf.begin() + trim);

    m_locked = true;
    m_weakSyncLines = 0;
    m_transmissionActive = true;
    emit syncStateChanged(true, std::clamp(bestScore, 0.0f, 1.0f));
    emit statusChanged(QStringLiteral("HF-Fax sync acquired."));
    return true;
}

bool HfFaxDecoder::consumeLockedLine()
{
    const int ls = lineSamples();
    const int sync = syncSamples();
    if (static_cast<int>(m_brightnessBuf.size()) < ls + (kSearchWindow * 2))
        return false;

    std::vector<float> prefix(m_brightnessBuf.size() + 1, 0.0f);
    for (int i = 0; i < static_cast<int>(m_brightnessBuf.size()); ++i)
        prefix[i + 1] = prefix[i] + m_brightnessBuf[static_cast<size_t>(i)];

    auto pulseMean = [&prefix, sync](int start) -> float {
        const int a = std::max(0, start);
        const int b = std::min(static_cast<int>(prefix.size()) - 1, start + sync);
        if (b <= a)
            return 0.0f;
        return (prefix[static_cast<size_t>(b)] - prefix[static_cast<size_t>(a)])
             / static_cast<float>(b - a);
    };

    int bestStart = 0;
    float bestPulse = -1.0f;
    for (int start = 0; start <= kSearchWindow * 2; ++start) {
        if (start + ls >= static_cast<int>(m_brightnessBuf.size()))
            break;

        const float score = pulseMean(start);
        if (score > bestPulse) {
            bestPulse = score;
            bestStart = start;
        }
    }

    if (bestPulse < kWeakSyncThreshold)
        ++m_weakSyncLines;
    else
        m_weakSyncLines = 0;

    if (m_weakSyncLines > kWeakSyncGraceLines) {
        resetSyncState(true);
        emit syncStateChanged(false, 0.0f);
        emit statusChanged(QStringLiteral("HF-Fax sync lost. Reacquiring."));
        return false;
    }

    if (bestStart + ls > static_cast<int>(m_brightnessBuf.size()))
        return false;

    std::vector<float> line(static_cast<size_t>(ls));
    std::copy_n(m_brightnessBuf.begin() + bestStart, ls, line.begin());
    emit rawLineDecoded(buildGrayLine(line, sync));
    emit syncStateChanged(true, std::clamp(bestPulse, 0.0f, 1.0f));

    const int removeCount = std::max(1, bestStart + ls - kSearchWindow);
    m_brightnessBuf.erase(m_brightnessBuf.begin(),
                          m_brightnessBuf.begin() + std::min(removeCount,
                                                              static_cast<int>(m_brightnessBuf.size())));
    return static_cast<int>(m_brightnessBuf.size()) >= ls + (kSearchWindow * 2);
}

QByteArray HfFaxDecoder::buildGrayLine(const std::vector<float>& line, int syncSampleCount) const
{
    QByteArray gray(kImageWidth, Qt::Uninitialized);
    auto* dst = reinterpret_cast<uchar*>(gray.data());

    if (static_cast<int>(line.size()) <= syncSampleCount) {
        std::memset(dst, 127, static_cast<size_t>(kImageWidth));
        return gray;
    }

    const int imageSamples = static_cast<int>(line.size()) - syncSampleCount;
    for (int x = 0; x < kImageWidth; ++x) {
        const int start = syncSampleCount + ((x * imageSamples) / kImageWidth);
        const int end = syncSampleCount + (((x + 1) * imageSamples) / kImageWidth);
        float accum = 0.0f;
        int count = 0;
        for (int i = start; i < std::max(start + 1, end); ++i) {
            accum += line[static_cast<size_t>(i)];
            ++count;
        }
        const float avg = count > 0 ? (accum / static_cast<float>(count)) : 0.5f;
        dst[x] = static_cast<uchar>(std::lround(clamp01(avg) * 255.0f));
    }

    return gray;
}

} // namespace AetherSDR
