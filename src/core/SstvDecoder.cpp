#include "SstvDecoder.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

float wrapPi(float angle)
{
    while (angle <= -kPi)
        angle += kTwoPi;
    while (angle > kPi)
        angle -= kTwoPi;
    return angle;
}

float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

std::array<SstvDecoder::HilbertTap, SstvDecoder::kHilbertTaps / 2> SstvDecoder::makeHilbertTaps()
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

SstvDecoder::SstvDecoder(QObject* parent)
    : QObject(parent)
    , m_hilbertTaps(makeHilbertTaps())
{
    emit statusChanged(QStringLiteral("SSTV decoder ready."));
}

void SstvDecoder::clear()
{
    m_history.fill(0.0f);
    m_historyPos = 0;
    m_samplesSeen = 0;
    m_havePrevPhase = false;
    m_prevPhase = 0.0f;
    m_freqAccum = 1900.0f;
    m_decimationCounter = 0;
    m_tones.clear();
    m_prefixSums.assign(1, 0.0);
    m_toneStartIndex = 0;
    m_totalToneSamples = 0;
    m_headerSearchIndex = 0;
    m_searchIndex = 0;
    m_expectedSyncIndex = -1;
    m_syncLocked = false;
    m_lastSyncQuality = 0.0f;
    m_activeMode = m_autoMode ? nullptr : m_manualMode;
    m_image = QImage();
    m_nextLine = 0;
    emit syncStateChanged(false, 0.0f);
    emit imageUpdated(QImage());
    emit lineProgressChanged(0, 0);
    emit detectedModeChanged(m_activeMode ? QString::fromLatin1(m_activeMode->name)
                                          : QStringLiteral("Unknown"));
    emit statusChanged(QStringLiteral("SSTV buffer cleared."));
}

QImage SstvDecoder::currentImage() const
{
    return m_image;
}

void SstvDecoder::setAutoMode(bool autoMode)
{
    if (m_autoMode == autoMode)
        return;
    m_autoMode = autoMode;
    m_activeMode = m_autoMode ? nullptr : m_manualMode;
    m_expectedSyncIndex = -1;
    m_syncLocked = false;
    emit syncStateChanged(false, 0.0f);
    emit detectedModeChanged(m_activeMode ? QString::fromLatin1(m_activeMode->name)
                                          : QStringLiteral("Unknown"));
}

void SstvDecoder::setManualMode(const QString& modeName)
{
    m_manualMode = findSstvModeByName(modeName);
    if (!m_autoMode) {
        m_activeMode = m_manualMode;
        m_expectedSyncIndex = -1;
        m_syncLocked = false;
        emit syncStateChanged(false, 0.0f);
        emit detectedModeChanged(m_activeMode ? QString::fromLatin1(m_activeMode->name)
                                              : QStringLiteral("Unknown"));
    }
}

void SstvDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    const int floatCount = pcm24kStereo.size() / static_cast<int>(sizeof(float));
    if (floatCount < 2)
        return;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int frames = floatCount / 2;
    constexpr int center = kHilbertTaps / 2;

    for (int i = 0; i < frames; ++i) {
        const float mono = 0.5f * (src[i * 2] + src[i * 2 + 1]);
        m_history[m_historyPos] = mono;
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

        float delta = wrapPi(phase - m_prevPhase);
        m_prevPhase = phase;

        float toneHz = (delta * static_cast<float>(kInputRate)) / kTwoPi;
        if (toneHz < 0.0f)
            toneHz += static_cast<float>(kInputRate);
        if (toneHz < 700.0f || toneHz > 2600.0f)
            continue;

        m_freqAccum += toneHz;
        ++m_decimationCounter;
        if (m_decimationCounter >= kToneDecimation) {
            pushTone(m_freqAccum / static_cast<float>(kToneDecimation));
            m_freqAccum = 0.0f;
            m_decimationCounter = 0;
        }
    }

    processToneBuffer();
}

void SstvDecoder::pushTone(float toneHz)
{
    m_tones.push_back(toneHz);
    m_prefixSums.push_back(m_prefixSums.back() + toneHz);
    ++m_totalToneSamples;
}

int SstvDecoder::msToToneSamples(double ms) const
{
    return std::max(1, static_cast<int>(std::lround(ms * static_cast<double>(kToneRate) / 1000.0)));
}

qint64 SstvDecoder::toLocalIndex(qint64 absIndex) const
{
    return absIndex - m_toneStartIndex;
}

float SstvDecoder::averageTone(qint64 absStart, int count) const
{
    if (count <= 0)
        return 1900.0f;
    const qint64 localStart = toLocalIndex(absStart);
    const qint64 localEnd = localStart + count;
    if (localStart < 0 || localEnd >= static_cast<qint64>(m_prefixSums.size()))
        return 1900.0f;

    const double sum = m_prefixSums[static_cast<size_t>(localEnd)] - m_prefixSums[static_cast<size_t>(localStart)];
    return static_cast<float>(sum / static_cast<double>(count));
}

float SstvDecoder::sampleTone(double absCenter, double span) const
{
    const double half = std::max(0.5, span * 0.5);
    const qint64 begin = static_cast<qint64>(std::floor(absCenter - half));
    const qint64 end = static_cast<qint64>(std::ceil(absCenter + half));
    return averageTone(begin, static_cast<int>(std::max<qint64>(1, end - begin)));
}

float SstvDecoder::sampleLevel(const SstvModeDefinition&,
                               qint64 syncIndexAbs,
                               double beginMs,
                               double endMs,
                               int pixelIndex,
                               int totalPixels,
                               float offsetHz) const
{
    const double beginSamples = (beginMs * static_cast<double>(kToneRate)) / 1000.0;
    const double endSamples = (endMs * static_cast<double>(kToneRate)) / 1000.0;
    const double spanSamples = std::max(1.0, endSamples - beginSamples);
    const double samplesPerPixel = std::max(0.75, spanSamples / static_cast<double>(std::max(1, totalPixels)));
    const double center = static_cast<double>(syncIndexAbs)
        + beginSamples
        + ((static_cast<double>(pixelIndex) + 0.5) * spanSamples / static_cast<double>(std::max(1, totalPixels)));
    const float correctedTone = sampleTone(center, samplesPerPixel * 0.85) - offsetHz;
    return clamp01((correctedTone - 1500.0f) / 800.0f);
}

int SstvDecoder::decodeVisCode(qint64 visStartAbs) const
{
    const int bitSamples = msToToneSamples(30.0);
    const float start = averageTone(visStartAbs, bitSamples);
    const float stop = averageTone(visStartAbs + (9 * bitSamples), bitSamples);
    if (std::abs(start - 1200.0f) > 90.0f || std::abs(stop - 1200.0f) > 90.0f)
        return -1;

    int code = 0;
    int parity = 0;
    for (int bit = 0; bit < 7; ++bit) {
        const float tone = averageTone(visStartAbs + ((bit + 1) * bitSamples), bitSamples);
        if (std::abs(tone - 1100.0f) <= 110.0f) {
            code |= (1 << bit);
            parity ^= 1;
        } else if (std::abs(tone - 1300.0f) > 110.0f) {
            return -1;
        }
    }

    const float parityTone = averageTone(visStartAbs + (8 * bitSamples), bitSamples);
    const int parityBit = (std::abs(parityTone - 1100.0f) <= 110.0f) ? 1
                        : (std::abs(parityTone - 1300.0f) <= 110.0f) ? 0 : -1;
    if (parityBit < 0 || parityBit != parity)
        return -1;

    return code;
}

bool SstvDecoder::tryDetectVisHeader()
{
    const int leaderSamples = msToToneSamples(300.0);
    const int breakSamples = msToToneSamples(10.0);
    const int bitSamples = msToToneSamples(30.0);
    const qint64 patternSamples = (2LL * leaderSamples) + breakSamples + (10LL * bitSamples);
    const qint64 lastStart = m_totalToneSamples - patternSamples;
    if (lastStart <= m_headerSearchIndex)
        return false;

    for (qint64 startAbs = std::max(m_headerSearchIndex, m_toneStartIndex);
         startAbs <= lastStart;
         startAbs += bitSamples / 4) {
        const float leader1 = averageTone(startAbs, leaderSamples);
        if (std::abs(leader1 - 1900.0f) > 80.0f)
            continue;

        const float breakTone = averageTone(startAbs + leaderSamples, breakSamples);
        if (std::abs(breakTone - 1200.0f) > 90.0f)
            continue;

        const float leader2 = averageTone(startAbs + leaderSamples + breakSamples, leaderSamples);
        if (std::abs(leader2 - 1900.0f) > 80.0f)
            continue;

        const qint64 visStart = startAbs + (2 * leaderSamples) + breakSamples;
        const int visCode = decodeVisCode(visStart);
        const auto* mode = findSstvModeByVisCode(visCode);
        if (!mode)
            continue;

        startFrame(mode, QStringLiteral("VIS %1 detected.").arg(visCode));
        m_searchIndex = visStart + (10LL * bitSamples);
        m_headerSearchIndex = m_searchIndex;
        return true;
    }

    m_headerSearchIndex = std::max(m_headerSearchIndex, lastStart - leaderSamples);
    return false;
}

bool SstvDecoder::findSyncNear(qint64 expectedAbs, qint64* syncIndexAbs, float* qualityOut) const
{
    if (!m_activeMode)
        return false;

    const int syncSamples = msToToneSamples(m_activeMode->syncMs);
    const int searchRadius = std::max(syncSamples * 2, msToToneSamples(6.0));
    const qint64 minBefore = static_cast<qint64>(std::floor(std::min({m_activeMode->redBeginMs,
                                                                      m_activeMode->greenBeginMs,
                                                                      m_activeMode->blueBeginMs,
                                                                      m_activeMode->grayBeginMs})
                                                            * kToneRate / 1000.0)) - 4;
    const qint64 maxAfter = static_cast<qint64>(std::ceil(std::max({m_activeMode->redEndMs,
                                                                    m_activeMode->greenEndMs,
                                                                    m_activeMode->blueEndMs,
                                                                    m_activeMode->grayEndMs,
                                                                    m_activeMode->lineMs})
                                                          * kToneRate / 1000.0)) + 4;

    qint64 best = -1;
    float bestScore = 1.0e9f;
    for (qint64 cand = expectedAbs - searchRadius; cand <= expectedAbs + searchRadius; cand += 2) {
        if (cand + minBefore < m_toneStartIndex || cand + maxAfter >= m_totalToneSamples)
            continue;

        const float syncTone = averageTone(cand, syncSamples);
        const float postTone = averageTone(cand + syncSamples, std::max(4, syncSamples / 2));
        const float score = std::abs(syncTone - 1200.0f) + 0.25f * std::max(0.0f, 1450.0f - postTone);
        if (score < bestScore) {
            bestScore = score;
            best = cand;
        }
    }

    if (best < 0 || bestScore > 95.0f)
        return false;

    if (syncIndexAbs)
        *syncIndexAbs = best;
    if (qualityOut)
        *qualityOut = clamp01(1.0f - (bestScore / 95.0f));
    return true;
}

bool SstvDecoder::findSyncWide(qint64 fromAbs, qint64* syncIndexAbs, float* qualityOut) const
{
    if (!m_activeMode)
        return false;

    const int syncSamples = msToToneSamples(m_activeMode->syncMs);
    const qint64 minBefore = static_cast<qint64>(std::floor(std::min({m_activeMode->redBeginMs,
                                                                      m_activeMode->greenBeginMs,
                                                                      m_activeMode->blueBeginMs,
                                                                      m_activeMode->grayBeginMs})
                                                            * kToneRate / 1000.0)) - 4;
    const qint64 maxAfter = static_cast<qint64>(std::ceil(std::max({m_activeMode->redEndMs,
                                                                    m_activeMode->greenEndMs,
                                                                    m_activeMode->blueEndMs,
                                                                    m_activeMode->grayEndMs,
                                                                    m_activeMode->lineMs})
                                                          * kToneRate / 1000.0)) + 4;
    const qint64 lastCandidate = m_totalToneSamples - maxAfter - syncSamples - 1;
    if (lastCandidate <= fromAbs)
        return false;

    qint64 best = -1;
    float bestScore = 1.0e9f;
    for (qint64 cand = std::max(fromAbs, m_toneStartIndex - minBefore); cand <= lastCandidate; cand += 4) {
        if (cand + minBefore < m_toneStartIndex)
            continue;

        const float syncTone = averageTone(cand, syncSamples);
        if (std::abs(syncTone - 1200.0f) > 95.0f)
            continue;

        const float postTone = averageTone(cand + syncSamples, std::max(4, syncSamples / 2));
        if (postTone < 1300.0f)
            continue;

        const float score = std::abs(syncTone - 1200.0f) + 0.25f * std::max(0.0f, 1450.0f - postTone);
        if (score < bestScore) {
            bestScore = score;
            best = cand;
            if (score < 18.0f)
                break;
        }
    }

    if (best < 0 || bestScore > 90.0f)
        return false;

    if (syncIndexAbs)
        *syncIndexAbs = best;
    if (qualityOut)
        *qualityOut = clamp01(1.0f - (bestScore / 90.0f));
    return true;
}

void SstvDecoder::startFrame(const SstvModeDefinition* mode, const QString& reason)
{
    m_activeMode = mode;
    m_expectedSyncIndex = -1;
    m_syncLocked = false;
    m_lastSyncQuality = 0.0f;
    m_nextLine = 0;
    m_image = QImage(mode->width, mode->height, QImage::Format_RGB32);
    m_image.fill(Qt::black);
    emit imageUpdated(m_image);
    emit lineProgressChanged(0, mode->height);
    emit detectedModeChanged(QString::fromLatin1(mode->name));
    emit statusChanged(QStringLiteral("%1 %2").arg(QString::fromLatin1(mode->name), reason));
}

bool SstvDecoder::decodeLineAtSync(qint64 syncIndexAbs, float* qualityOut)
{
    if (!m_activeMode)
        return false;

    if (m_image.isNull() || m_image.size() != QSize(m_activeMode->width, m_activeMode->height)) {
        m_image = QImage(m_activeMode->width, m_activeMode->height, QImage::Format_RGB32);
        m_image.fill(Qt::black);
        m_nextLine = 0;
    }

    if (m_nextLine >= m_activeMode->height) {
        m_image.fill(Qt::black);
        m_nextLine = 0;
    }

    const int syncSamples = msToToneSamples(m_activeMode->syncMs);
    const float syncTone = averageTone(syncIndexAbs, syncSamples);
    const float offsetHz = syncTone - 1200.0f;
    const float quality = clamp01(1.0f - (std::abs(offsetHz) / 120.0f));

    auto* line = reinterpret_cast<QRgb*>(m_image.scanLine(m_nextLine));
    for (int x = 0; x < m_activeMode->width; ++x) {
        if (m_activeMode->colorModel == SstvColorModel::Gray) {
            const float gray = sampleLevel(*m_activeMode, syncIndexAbs,
                                           m_activeMode->grayBeginMs, m_activeMode->grayEndMs,
                                           x, m_activeMode->width, offsetHz);
            const int level = static_cast<int>(std::lround(gray * 255.0f));
            line[x] = qRgb(level, level, level);
        } else {
            const float red = sampleLevel(*m_activeMode, syncIndexAbs,
                                          m_activeMode->redBeginMs, m_activeMode->redEndMs,
                                          x, m_activeMode->width, offsetHz);
            const float green = sampleLevel(*m_activeMode, syncIndexAbs,
                                            m_activeMode->greenBeginMs, m_activeMode->greenEndMs,
                                            x, m_activeMode->width, offsetHz);
            const float blue = sampleLevel(*m_activeMode, syncIndexAbs,
                                           m_activeMode->blueBeginMs, m_activeMode->blueEndMs,
                                           x, m_activeMode->width, offsetHz);
            line[x] = qRgb(static_cast<int>(std::lround(red * 255.0f)),
                           static_cast<int>(std::lround(green * 255.0f)),
                           static_cast<int>(std::lround(blue * 255.0f)));
        }
    }

    ++m_nextLine;
    emit imageUpdated(m_image);
    emit lineProgressChanged(m_nextLine, m_activeMode->height);
    emit statusChanged(QStringLiteral("Receiving %1 (%2/%3)")
                       .arg(QString::fromLatin1(m_activeMode->name))
                       .arg(m_nextLine)
                       .arg(m_activeMode->height));
    if (qualityOut)
        *qualityOut = quality;
    return true;
}

void SstvDecoder::processToneBuffer()
{
    if (m_autoMode && !m_activeMode)
        tryDetectVisHeader();

    if (!m_autoMode && m_manualMode && !m_activeMode)
        startFrame(m_manualMode, QStringLiteral("manual mode armed."));

    if (!m_activeMode) {
        // Idle VIS search: previously returned without trimming, so the tone
        // and prefix-sum buffers grew by ~144 KB/s for as long as the dialog
        // was open without a picture being received.
        trimBuffers();
        return;
    }

    while (true) {
        qint64 syncIndex = -1;
        float quality = 0.0f;
        bool found = false;

        if (m_expectedSyncIndex >= 0)
            found = findSyncNear(m_expectedSyncIndex, &syncIndex, &quality);

        if (!found) {
            m_expectedSyncIndex = -1;
            found = findSyncWide(std::max(m_searchIndex, m_toneStartIndex), &syncIndex, &quality);
        }

        if (!found) {
            // Candidates up to the last complete one were rejected and will
            // not change as tones are appended: resume the wide search after
            // them. Without this the search start (and the trim point) never
            // moved once a picture ended, so every call rescanned the whole,
            // ever-growing buffer.
            const qint64 maxAfter = msToToneSamples(std::max({m_activeMode->redEndMs,
                                                              m_activeMode->greenEndMs,
                                                              m_activeMode->blueEndMs,
                                                              m_activeMode->grayEndMs,
                                                              m_activeMode->lineMs})) + 4;
            const qint64 lastCandidate = m_totalToneSamples - maxAfter
                                         - msToToneSamples(m_activeMode->syncMs) - 1;
            if (m_expectedSyncIndex < 0 && lastCandidate > m_searchIndex)
                m_searchIndex = lastCandidate;
            break;
        }

        if (!decodeLineAtSync(syncIndex, &quality))
            break;

        m_syncLocked = true;
        m_lastSyncQuality = quality;
        emit syncStateChanged(true, quality);

        m_searchIndex = syncIndex + msToToneSamples(m_activeMode->lineMs / 2.0);
        m_expectedSyncIndex = syncIndex + msToToneSamples(m_activeMode->lineMs);
    }

    trimBuffers();
}

void SstvDecoder::trimBuffers()
{
    qint64 keepFrom = m_activeMode ? m_searchIndex - msToToneSamples(1200.0)
                                   : m_headerSearchIndex;
    if (m_expectedSyncIndex > 0)
        keepFrom = std::min(keepFrom, m_expectedSyncIndex - msToToneSamples(1200.0));
    keepFrom = std::max(keepFrom, static_cast<qint64>(0));
    if (keepFrom <= m_toneStartIndex)
        return;

    const qint64 drop = keepFrom - m_toneStartIndex;
    if (drop <= 0 || drop >= static_cast<qint64>(m_tones.size()))
        return;

    m_tones.erase(m_tones.begin(), m_tones.begin() + static_cast<std::ptrdiff_t>(drop));
    std::vector<double> prefix;
    prefix.reserve(m_tones.size() + 1);
    prefix.push_back(0.0);
    for (float tone : m_tones)
        prefix.push_back(prefix.back() + tone);
    m_prefixSums.swap(prefix);
    m_toneStartIndex = keepFrom;
}

} // namespace AetherSDR
