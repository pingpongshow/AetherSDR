#include "WwvTimecodeDecoder.h"

#include <QByteArray>
#include <QDate>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

constexpr int kChuTickSamples = 240;              // 10 ms @ 24 kHz
constexpr int kChuPreambleSamples = 2960;         // 123.333... ms @ 24 kHz
constexpr int kChuBitSamples = 80;                // 300 baud @ 24 kHz
constexpr int kChuBitsPerByte = 11;               // 1 start + 8 data + 2 stop
constexpr int kChuBurstByteCount = 10;
constexpr int kChuDataBits = kChuBitsPerByte * kChuBurstByteCount;
constexpr int kChuDataStartSamples = kChuTickSamples + kChuPreambleSamples;
constexpr int kChuDataSamples = kChuDataBits * kChuBitSamples;

template <typename T>
T clampValue(T value, T low, T high)
{
    return std::max(low, std::min(high, value));
}

quint8 swapNibbles(quint8 value)
{
    return static_cast<quint8>(((value & 0x0F) << 4) | ((value & 0xF0) >> 4));
}

int parseBcdByte(quint8 value)
{
    const int hi = (value >> 4) & 0x0F;
    const int lo = value & 0x0F;
    if (hi > 9 || lo > 9)
        return -1;
    return (hi * 10) + lo;
}

QString formatHexBytes10(const std::array<quint8, 10>& bytes)
{
    QStringList parts;
    parts.reserve(static_cast<int>(bytes.size()));
    for (const quint8 value : bytes)
        parts.append(QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper());
    return parts.join(QLatin1Char(' '));
}

} // namespace

QString WwvTimecodeDecoder::profileName(Profile profile)
{
    switch (profile) {
    case Profile::Chu:
        return QStringLiteral("CHU");
    case Profile::Bpm:
        return QStringLiteral("BPM");
    case Profile::Wwv:
    default:
        return QStringLiteral("WWV / WWVH");
    }
}

WwvTimecodeDecoder::WwvTimecodeDecoder(QObject* parent)
    : QObject(parent)
{
    clearState();
}

void WwvTimecodeDecoder::reset()
{
    clearState();
    emit currentTimeUpdated(waitingDisplayText());
    emit statusChanged(readyStatusText());
}

void WwvTimecodeDecoder::setProfile(Profile profile)
{
    if (m_profile == profile)
        return;

    m_profile = profile;
    reset();
}

void WwvTimecodeDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    const int floatCount = pcm24kStereo.size() / static_cast<int>(sizeof(float));
    if (floatCount < 2)
        return;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int frames = floatCount / 2;
    m_sampleBuffer.reserve(m_sampleBuffer.size() + static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const float mono = (src[i * 2] + src[i * 2 + 1]) * 0.5f;
        m_sampleBuffer.push_back(mono);
    }

    m_totalSamples += frames;
    processAnalysisWindows();
    processLockedSeconds();
    trimBuffers();
}

float WwvTimecodeDecoder::goertzelPower(const float* samples, int count, double freqHz) const
{
    if (count <= 0)
        return 0.0f;

    freqHz = clampValue(freqHz, 10.0, (kSampleRate * 0.5) - 10.0);
    const double omega = kTwoPi * freqHz / static_cast<double>(kSampleRate);
    const double coeff = 2.0 * std::cos(omega);

    double q0 = 0.0;
    double q1 = 0.0;
    double q2 = 0.0;
    for (int i = 0; i < count; ++i) {
        q0 = coeff * q1 - q2 + samples[i];
        q2 = q1;
        q1 = q0;
    }

    const double power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
    return static_cast<float>(std::max(0.0, power) / static_cast<double>(count * count));
}

float WwvTimecodeDecoder::powerToDb(float power)
{
    const float rms = std::sqrt(std::max(power, 1e-12f));
    return 20.0f * std::log10(rms);
}

QString WwvTimecodeDecoder::formatHexBytes(const std::array<quint8, 5>& bytes)
{
    QStringList parts;
    parts.reserve(static_cast<int>(bytes.size()));
    for (const quint8 value : bytes)
        parts.append(QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper());
    return parts.join(QLatin1Char(' '));
}

QString WwvTimecodeDecoder::formatUtcLabel(const QDateTime& utc)
{
    return QStringLiteral("UTC: %1")
        .arg(utc.toString(QStringLiteral("yyyy-DDD HH:mm:ss 'UTC'")));
}

QString WwvTimecodeDecoder::pulseClassLabel(PulseClass pulse)
{
    switch (pulse) {
    case PulseClass::Zero:
        return QStringLiteral("0");
    case PulseClass::One:
        return QStringLiteral("1");
    case PulseClass::Marker:
        return QStringLiteral("Marker");
    case PulseClass::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

QString WwvTimecodeDecoder::pulsePatternText(const QVector<PulseClass>& pulses)
{
    QString out;
    out.reserve(pulses.size());
    for (PulseClass pulse : pulses) {
        switch (pulse) {
        case PulseClass::Zero:
            out += QLatin1Char('0');
            break;
        case PulseClass::One:
            out += QLatin1Char('1');
            break;
        case PulseClass::Marker:
            out += QLatin1Char('P');
            break;
        case PulseClass::Unknown:
        default:
            out += QLatin1Char('-');
            break;
        }
    }
    return out;
}

void WwvTimecodeDecoder::clearState()
{
    m_sampleBuffer.clear();
    m_bufferStartSample = 0;
    m_totalSamples = 0;
    m_nextAnalysisStart = 0;
    m_recentAnalysis.clear();
    m_tickNoiseFloor = 1e-8f;
    m_lastPeakStart = -1;
    m_locked = false;
    m_nextSecondStart = -1;
    m_lockScore = 0;
    m_lastStation.clear();
    m_profileSecondOfMinute = -1;
    m_profileMinuteAligned = false;
    m_profileMinuteStartUtc = {};
    m_profileMinuteStartUtcValid = false;
    m_currentMinute = {};
    m_haveCurrentMinute = false;
    m_nextMinuteUtc = {};
    m_nextMinuteUtcValid = false;
    m_chuMinute = {};
    m_chuHourSilenceActive = false;
}

int WwvTimecodeDecoder::decodeWeighted(const QVector<PulseClass>& pulses,
                                       const std::initializer_list<std::pair<int, int>>& mapping) const
{
    int value = 0;
    for (const auto& [index, weight] : mapping) {
        if (index < 0 || index >= pulses.size())
            return -1;
        if (pulses.at(index) == PulseClass::Unknown || pulses.at(index) == PulseClass::Marker)
            return -1;
        if (pulses.at(index) == PulseClass::One)
            value += weight;
    }
    return value;
}

QString WwvTimecodeDecoder::dstSummary(bool current, bool transitionToday) const
{
    if (transitionToday)
        return current ? QStringLiteral("DST ends today") : QStringLiteral("DST starts today");
    return current ? QStringLiteral("DST in effect") : QStringLiteral("Standard time");
}

QString WwvTimecodeDecoder::waitingDisplayText() const
{
    switch (m_profile) {
    case Profile::Chu:
        return QStringLiteral("UTC: Awaiting CHU pulse and Bell 103 code lock.");
    case Profile::Bpm:
        return QStringLiteral("Signal: Awaiting BPM minute pulse.");
    case Profile::Wwv:
    default:
        return QStringLiteral("UTC: Awaiting WWV / WWVH lock.");
    }
}

QString WwvTimecodeDecoder::readyStatusText() const
{
    switch (m_profile) {
    case Profile::Chu:
        return QStringLiteral("CHU time decoder ready.");
    case Profile::Bpm:
        return QStringLiteral("BPM timing decoder ready.");
    case Profile::Wwv:
    default:
        return QStringLiteral("WWV / WWVH timecode decoder ready.");
    }
}

QString WwvTimecodeDecoder::lockStatusText() const
{
    switch (m_profile) {
    case Profile::Chu:
        return QStringLiteral("Locked to one-second CHU pulses.");
    case Profile::Bpm:
        return QStringLiteral("Locked to one-second BPM pulses.");
    case Profile::Wwv:
    default:
        return QStringLiteral("Locked to one-second WWV / WWVH ticks.");
    }
}

void WwvTimecodeDecoder::processAnalysisWindows()
{
    while (m_nextAnalysisStart + kAnalysisWindowSamples <= m_totalSamples) {
        const qint64 localStart = m_nextAnalysisStart - m_bufferStartSample;
        if (localStart < 0 || localStart + kAnalysisWindowSamples > static_cast<qint64>(m_sampleBuffer.size()))
            break;

        const auto* window = m_sampleBuffer.data() + static_cast<size_t>(localStart);
        AnalysisPoint point;
        point.startSample = m_nextAnalysisStart;
        point.p1000 = goertzelPower(window, kAnalysisWindowSamples, 1000.0);
        point.p1200 = goertzelPower(window, kAnalysisWindowSamples, 1200.0);
        point.p1500 = goertzelPower(window, kAnalysisWindowSamples, 1500.0);

        const float tickScore = (m_profile == Profile::Wwv)
                                    ? std::max({point.p1000, point.p1200, point.p1500})
                                    : point.p1000;
        const float floorMix = (m_profile == Profile::Wwv) ? 0.005f : 0.01f;
        m_tickNoiseFloor = (m_tickNoiseFloor * (1.0f - floorMix)) + (tickScore * floorMix);

        m_recentAnalysis.append(point);
        if (m_recentAnalysis.size() >= 3) {
            const AnalysisPoint& prev = m_recentAnalysis.at(m_recentAnalysis.size() - 3);
            const AnalysisPoint& mid = m_recentAnalysis.at(m_recentAnalysis.size() - 2);
            const AnalysisPoint& next = m_recentAnalysis.at(m_recentAnalysis.size() - 1);

            const float prevScore = (m_profile == Profile::Wwv)
                                        ? std::max({prev.p1000, prev.p1200, prev.p1500})
                                        : prev.p1000;
            const float midScore = (m_profile == Profile::Wwv)
                                       ? std::max({mid.p1000, mid.p1200, mid.p1500})
                                       : mid.p1000;
            const float nextScore = (m_profile == Profile::Wwv)
                                        ? std::max({next.p1000, next.p1200, next.p1500})
                                        : next.p1000;
            const float threshold = std::max(8.0f * m_tickNoiseFloor,
                                             (m_profile == Profile::Wwv) ? 2.5e-6f : 1.0e-6f);

            if (midScore >= prevScore && midScore > nextScore && midScore > threshold)
                registerPeak(mid.startSample);
        }

        if (m_recentAnalysis.size() > 8)
            m_recentAnalysis.remove(0, m_recentAnalysis.size() - 8);

        m_nextAnalysisStart += kAnalysisHopSamples;
    }
}

void WwvTimecodeDecoder::registerPeak(qint64 startSample)
{
    if (m_lastPeakStart >= 0) {
        const qint64 delta = startSample - m_lastPeakStart;
        if (!m_locked && std::llabs(delta - kSecondSamples) <= 1400) {
            m_locked = true;
            m_nextSecondStart = startSample;
            m_lockScore = 6;
            emit statusChanged(lockStatusText());
        }
    }

    m_lastPeakStart = startSample;
}

int WwvTimecodeDecoder::estimateToneDurationMs(const float* second, double freqHz, int blockMs) const
{
    const int blockSamples = (kSampleRate * blockMs) / 1000;
    const int maxBlocks = std::min((1000 / blockMs), kSecondSamples / std::max(blockSamples, 1));
    if (blockSamples <= 0 || maxBlocks <= 0)
        return 0;

    std::vector<float> powers(static_cast<size_t>(maxBlocks));
    float maxPower = 0.0f;
    float tailAverage = 0.0f;
    int tailCount = 0;

    for (int i = 0; i < maxBlocks; ++i) {
        const float power = goertzelPower(second + (i * blockSamples), blockSamples, freqHz);
        powers[static_cast<size_t>(i)] = power;
        maxPower = std::max(maxPower, power);
        if (i >= maxBlocks / 2) {
            tailAverage += power;
            ++tailCount;
        }
    }

    if (maxPower <= 0.0f)
        return 0;

    const float baseline = tailCount > 0 ? (tailAverage / static_cast<float>(tailCount)) : 0.0f;
    const float threshold = std::max(maxPower * 0.28f, baseline * 3.0f);

    int activeBlocks = 0;
    for (int i = 0; i < maxBlocks; ++i) {
        if (powers[static_cast<size_t>(i)] >= threshold)
            ++activeBlocks;
        else if (activeBlocks > 0)
            break;
    }

    return activeBlocks * blockMs;
}

WwvTimecodeDecoder::PulseClass WwvTimecodeDecoder::classifyTimecodePulse(const float* second) const
{
    const int durationMs = estimateToneDurationMs(second, 100.0, 50);
    if (durationMs >= 650)
        return PulseClass::Marker;
    if (durationMs >= 320)
        return PulseClass::One;
    if (durationMs >= 90)
        return PulseClass::Zero;
    return PulseClass::Unknown;
}

void WwvTimecodeDecoder::processLockedSeconds()
{
    if (!m_locked || m_nextSecondStart < 0)
        return;

    while (m_nextSecondStart + kSecondSamples <= m_totalSamples) {
        const qint64 localStart = m_nextSecondStart - m_bufferStartSample;
        if (localStart < 0 || localStart + kSecondSamples > static_cast<qint64>(m_sampleBuffer.size()))
            break;

        const auto* second = m_sampleBuffer.data() + static_cast<size_t>(localStart);
        processSecond(m_nextSecondStart, second);
        m_nextSecondStart += kSecondSamples;
    }
}

void WwvTimecodeDecoder::processSecond(qint64 secondStart, const float* second)
{
    switch (m_profile) {
    case Profile::Chu:
        Q_UNUSED(secondStart);
        processSecondChu(second);
        break;
    case Profile::Bpm:
        Q_UNUSED(secondStart);
        processSecondBpm(second);
        break;
    case Profile::Wwv:
    default:
        processSecondWwv(secondStart, second);
        break;
    }
}

void WwvTimecodeDecoder::processSecondWwv(qint64 secondStart, const float* second)
{
    const int tick1000Ms = estimateToneDurationMs(second, 1000.0, 20);
    const int tick1200Ms = estimateToneDurationMs(second, 1200.0, 20);
    const int tick1500Ms = estimateToneDurationMs(second, 1500.0, 20);

    const float tick1000Power = goertzelPower(second, 1200, 1000.0);
    const float tick1200Power = goertzelPower(second, 1200, 1200.0);
    const float tick1500Power = goertzelPower(second, 1200, 1500.0);
    const float codePower = goertzelPower(second, 2400, 100.0);

    SecondSummary summary;
    summary.tickLevelDb = powerToDb(std::max({tick1000Power, tick1200Power, tick1500Power}));
    summary.codeLevelDb = powerToDb(codePower);
    summary.hourMarker = tick1500Ms >= 600;
    summary.minuteMarker = summary.hourMarker || tick1000Ms >= 600 || tick1200Ms >= 600;
    summary.pulse = classifyTimecodePulse(second);
    summary.pulseLabel = pulseClassLabel(summary.pulse);

    if (tick1200Ms > tick1000Ms)
        summary.station = QStringLiteral("WWVH");
    else if (tick1000Ms > 0 || tick1500Ms > 0)
        summary.station = QStringLiteral("WWV");
    else
        summary.station = m_lastStation.isEmpty() ? QStringLiteral("Unknown") : m_lastStation;

    if (!summary.station.isEmpty() && summary.station != QLatin1String("Unknown"))
        m_lastStation = summary.station;

    if (summary.minuteMarker) {
        if (m_haveCurrentMinute && m_currentMinute.pulses.size() >= 60)
            finalizeCurrentMinute();

        m_currentMinute = {};
        m_currentMinute.startSample = secondStart;
        m_currentMinute.station = summary.station;
        if (m_nextMinuteUtcValid) {
            m_currentMinute.startUtc = m_nextMinuteUtc;
            m_currentMinute.startUtcValid = true;
        }
        m_haveCurrentMinute = true;
    }

    if (m_haveCurrentMinute && m_currentMinute.pulses.size() < 60)
        m_currentMinute.pulses.append(summary.pulse);

    int secondOfMinute = -1;
    if (m_haveCurrentMinute && !m_currentMinute.pulses.isEmpty()) {
        secondOfMinute = m_currentMinute.pulses.size() - 1;
        if (m_currentMinute.startUtcValid) {
            const QDateTime liveUtc = m_currentMinute.startUtc.addSecs(secondOfMinute);
            emit currentTimeUpdated(formatUtcLabel(liveUtc));
        }
    }

    emit metricsUpdated(summary.station,
                        summary.tickLevelDb,
                        summary.codeLevelDb,
                        m_locked,
                        secondOfMinute,
                        summary.minuteMarker,
                        summary.hourMarker,
                        summary.pulseLabel);

    if (summary.minuteMarker && !m_haveCurrentMinute)
        emit statusChanged(QStringLiteral("Minute marker detected."));
}

void WwvTimecodeDecoder::processSecondChu(const float* second)
{
    const int tick1000Ms = estimateToneDurationMs(second, 1000.0, 10);
    const float tickPower = goertzelPower(second, 2400, 1000.0);
    const float codeMarkPower =
        goertzelPower(second + kChuDataStartSamples, kChuDataSamples, 2225.0);
    const float codeSpacePower =
        goertzelPower(second + kChuDataStartSamples, kChuDataSamples, 2025.0);

    SecondSummary summary;
    summary.station = QStringLiteral("CHU");
    summary.tickLevelDb = powerToDb(tickPower);
    summary.codeLevelDb = powerToDb(std::max(codeMarkPower, codeSpacePower));
    summary.hourMarker = tick1000Ms >= 850;
    summary.minuteMarker = !summary.hourMarker && tick1000Ms >= 450;

    if (summary.hourMarker || summary.minuteMarker) {
        if (m_profileMinuteStartUtcValid)
            m_profileMinuteStartUtc = m_profileMinuteStartUtc.addSecs(60);
        m_profileMinuteAligned = true;
        m_profileSecondOfMinute = 0;
        if (!m_profileMinuteStartUtc.isValid())
            m_profileMinuteStartUtcValid = false;
        m_chuMinute = {};
        m_chuHourSilenceActive = summary.hourMarker;
    } else if (m_profileMinuteAligned && m_profileSecondOfMinute >= 0) {
        m_profileSecondOfMinute = (m_profileSecondOfMinute + 1) % 60;
        if (m_chuHourSilenceActive && m_profileSecondOfMinute >= 10)
            m_chuHourSilenceActive = false;
    }

    if (summary.hourMarker) {
        summary.pulseLabel = QStringLiteral("1s hour");
    } else if (summary.minuteMarker) {
        summary.pulseLabel = QStringLiteral("0.5s minute");
    } else if (m_profileSecondOfMinute == 29 && tick1000Ms < 30) {
        summary.pulseLabel = QStringLiteral("29 omitted");
    } else if (m_chuHourSilenceActive && m_profileSecondOfMinute >= 1 && m_profileSecondOfMinute <= 9 && tick1000Ms < 30) {
        summary.pulseLabel = QStringLiteral("Hour silence");
    } else if (tick1000Ms >= 250) {
        summary.pulseLabel = QStringLiteral("0.3s tick");
    } else if (tick1000Ms >= 5) {
        summary.pulseLabel = QStringLiteral("10ms tick");
    } else {
        summary.pulseLabel = QStringLiteral("Silent");
    }

    if (m_profileSecondOfMinute >= 31 && m_profileSecondOfMinute <= 39) {
        ChuBurst burst;
        if (decodeChuBurst(second, &burst)) {
            handleChuBurst(burst);
            summary.pulseLabel = burst.formatA ? QStringLiteral("A burst") : QStringLiteral("B burst");
        }
    }

    if (m_profileMinuteStartUtcValid && m_profileSecondOfMinute >= 0) {
        const QDateTime liveUtc = m_profileMinuteStartUtc.addSecs(m_profileSecondOfMinute);
        emit currentTimeUpdated(formatUtcLabel(liveUtc));
    }

    emit metricsUpdated(summary.station,
                        summary.tickLevelDb,
                        summary.codeLevelDb,
                        m_locked,
                        m_profileSecondOfMinute,
                        summary.minuteMarker,
                        summary.hourMarker,
                        summary.pulseLabel);
}

void WwvTimecodeDecoder::processSecondBpm(const float* second)
{
    const int tick1000Ms = estimateToneDurationMs(second, 1000.0, 10);
    const float tickPower = goertzelPower(second, 1200, 1000.0);

    SecondSummary summary;
    summary.station = QStringLiteral("BPM");
    summary.tickLevelDb = powerToDb(tickPower);
    summary.codeLevelDb = -120.0f;
    summary.minuteMarker = tick1000Ms >= 240;
    summary.hourMarker = false;

    if (summary.minuteMarker) {
        m_profileMinuteAligned = true;
        m_profileSecondOfMinute = 0;
        emit statusChanged(QStringLiteral("Detected BPM minute pulse."));
    } else if (m_profileMinuteAligned && m_profileSecondOfMinute >= 0) {
        m_profileSecondOfMinute = (m_profileSecondOfMinute + 1) % 60;
    }

    if (tick1000Ms >= 240)
        summary.pulseLabel = QStringLiteral("300ms minute");
    else if (tick1000Ms >= 80)
        summary.pulseLabel = QStringLiteral("100ms UT1");
    else if (tick1000Ms >= 5)
        summary.pulseLabel = QStringLiteral("10ms UTC");
    else
        summary.pulseLabel = QStringLiteral("Carrier / ID");

    if (m_profileMinuteAligned && m_profileSecondOfMinute >= 0) {
        emit currentTimeUpdated(
            QStringLiteral("BPM: aligned to minute pulse, second %1")
                .arg(m_profileSecondOfMinute, 2, 10, QLatin1Char('0')));
    }

    emit metricsUpdated(summary.station,
                        summary.tickLevelDb,
                        summary.codeLevelDb,
                        m_locked,
                        m_profileSecondOfMinute,
                        summary.minuteMarker,
                        summary.hourMarker,
                        summary.pulseLabel);
}

bool WwvTimecodeDecoder::decodeChuBurst(const float* second, ChuBurst* burst) const
{
    if (!burst)
        return false;

    burst->valid = false;

    if (kChuDataStartSamples + kChuDataSamples > kSecondSamples)
        return false;

    std::array<bool, kChuDataBits> bits{};
    float confidenceSum = 0.0f;

    for (int bitIndex = 0; bitIndex < kChuDataBits; ++bitIndex) {
        const int sampleOffset = kChuDataStartSamples + (bitIndex * kChuBitSamples);
        const float markPower = goertzelPower(second + sampleOffset, kChuBitSamples, 2225.0);
        const float spacePower = goertzelPower(second + sampleOffset, kChuBitSamples, 2025.0);
        const float sum = markPower + spacePower + 1.0e-12f;
        confidenceSum += std::abs(markPower - spacePower) / sum;
        bits[static_cast<size_t>(bitIndex)] = markPower >= spacePower;
    }

    if ((confidenceSum / static_cast<float>(kChuDataBits)) < 0.12f)
        return false;

    std::array<quint8, kChuBurstByteCount> rawBytes{};
    for (int byteIndex = 0; byteIndex < kChuBurstByteCount; ++byteIndex) {
        const int base = byteIndex * kChuBitsPerByte;
        const bool startBit = bits[static_cast<size_t>(base)];
        const bool stop1 = bits[static_cast<size_t>(base + 9)];
        const bool stop2 = bits[static_cast<size_t>(base + 10)];
        if (startBit || !stop1 || !stop2)
            return false;

        quint8 value = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if (bits[static_cast<size_t>(base + 1 + bit)])
                value = static_cast<quint8>(value | (1u << bit));
        }
        rawBytes[static_cast<size_t>(byteIndex)] = value;
    }

    bool formatA = true;
    bool formatB = true;
    for (int i = 0; i < 5; ++i) {
        const quint8 data = rawBytes[static_cast<size_t>(i)];
        const quint8 redundancy = rawBytes[static_cast<size_t>(i + 5)];
        formatA = formatA && (redundancy == data);
        formatB = formatB && (redundancy == static_cast<quint8>(~data));
    }
    if (!formatA && !formatB)
        return false;

    burst->formatA = formatA;
    burst->rawHex = formatHexBytes10(rawBytes);
    for (int i = 0; i < 5; ++i)
        burst->payload[static_cast<size_t>(i)] = swapNibbles(rawBytes[static_cast<size_t>(i)]);

    if (burst->formatA) {
        if (((burst->payload[0] >> 4) & 0x0F) != 6)
            return false;

        const int doyHundreds = burst->payload[0] & 0x0F;
        const int doyLow = parseBcdByte(burst->payload[1]);
        const int hour = parseBcdByte(burst->payload[2]);
        const int minute = parseBcdByte(burst->payload[3]);
        const int secondField = parseBcdByte(burst->payload[4]);

        if (doyLow < 0 || hour < 0 || minute < 0 || secondField < 0)
            return false;

        burst->dayOfYear = (doyHundreds * 100) + doyLow;
        burst->hour = hour;
        burst->minute = minute;
        burst->second = secondField;
        if (burst->dayOfYear < 1 || burst->dayOfYear > 366 || burst->hour >= 24 || burst->minute >= 60 || burst->second >= 60)
            return false;
    } else {
        const quint8 xz = burst->payload[0];
        const int yearHigh = parseBcdByte(burst->payload[1]);
        const int yearLow = parseBcdByte(burst->payload[2]);
        const int taiMinusUtc = parseBcdByte(burst->payload[3]);
        if (yearHigh < 0 || yearLow < 0 || taiMinusUtc < 0)
            return false;

        const int x = (xz >> 4) & 0x0F;
        burst->dutTenths = xz & 0x0F;
        burst->dutPositive = (x & 0x01) == 0;
        burst->leapSecond = (x & 0x02) != 0;
        burst->year = (yearHigh * 100) + yearLow;
        burst->taiMinusUtc = taiMinusUtc;
        burst->dstCode = burst->payload[4];
    }

    burst->valid = true;
    return true;
}

void WwvTimecodeDecoder::handleChuBurst(const ChuBurst& burst)
{
    if (!burst.valid)
        return;

    if (burst.formatA) {
        m_chuMinute.haveFormatA = true;
        m_chuMinute.formatA = burst.payload;
        m_chuMinute.rawAHex = burst.rawHex;
        m_chuMinute.dayOfYear = burst.dayOfYear;
        m_chuMinute.hour = burst.hour;
        m_chuMinute.minute = burst.minute;
        m_chuMinute.second = burst.second;
    } else {
        m_chuMinute.haveFormatB = true;
        m_chuMinute.formatB = burst.payload;
        m_chuMinute.rawBHex = burst.rawHex;
        m_chuMinute.year = burst.year;
        m_chuMinute.dutTenths = burst.dutTenths;
        m_chuMinute.dutPositive = burst.dutPositive;
        m_chuMinute.leapSecond = burst.leapSecond;
        m_chuMinute.taiMinusUtc = burst.taiMinusUtc;
        m_chuMinute.dstCode = burst.dstCode;
    }

    if (!m_chuMinute.haveFormatA || !m_chuMinute.haveFormatB)
        return;

    const QDate date = QDate(m_chuMinute.year, 1, 1).addDays(m_chuMinute.dayOfYear - 1);
    if (!date.isValid())
        return;

    const QTime time(m_chuMinute.hour, m_chuMinute.minute, m_chuMinute.second);
    if (!time.isValid())
        return;

    const QDateTime burstUtc(date, time, QTimeZone::UTC);
    m_profileMinuteStartUtc = burstUtc.addSecs(-m_chuMinute.second);
    m_profileMinuteStartUtcValid = true;
    m_profileMinuteAligned = true;
    m_profileSecondOfMinute = m_chuMinute.second;

    const QString utcText = formatUtcLabel(burstUtc);
    const QString frameText = QStringLiteral("%1  |  CHU  |  DOY %2  |  Year %3")
                                  .arg(burstUtc.toString(QStringLiteral("yyyy-DDD HH:mm:ss 'UTC'")))
                                  .arg(m_chuMinute.dayOfYear)
                                  .arg(m_chuMinute.year);
    const QString flagsText = QStringLiteral("DUT1 %1%2.%3  |  TAI-UTC %4  |  Leap second: %5  |  DST code 0x%6")
                                  .arg(m_chuMinute.dutPositive ? QStringLiteral("+") : QStringLiteral("-"))
                                  .arg(m_chuMinute.dutTenths / 10)
                                  .arg(m_chuMinute.dutTenths % 10)
                                  .arg(m_chuMinute.taiMinusUtc)
                                  .arg(m_chuMinute.leapSecond ? QStringLiteral("Warning") : QStringLiteral("No"))
                                  .arg(m_chuMinute.dstCode, 2, 16, QLatin1Char('0'))
                                  .toUpper();
    const QString bitsText = QStringLiteral("A %1  |  B %2")
                                 .arg(m_chuMinute.rawAHex, m_chuMinute.rawBHex);

    emit currentTimeUpdated(utcText);
    emit frameDecoded(utcText, frameText, flagsText, bitsText);
    emit statusChanged(QStringLiteral("Decoded CHU time code for %1.")
                           .arg(burstUtc.toString(QStringLiteral("yyyy-DDD HH:mm:ss 'UTC'"))));
}

bool WwvTimecodeDecoder::tryDecodeMinute(const MinuteFrame& frame, DecodedMinute* decoded) const
{
    if (frame.pulses.size() < 60 || !decoded)
        return false;

    const auto& p = frame.pulses;
    for (int marker : {9, 19, 29, 39, 49, 59}) {
        if (p.at(marker) != PulseClass::Marker)
            return false;
    }

    const int minute = decodeWeighted(p, {{10, 1}, {11, 2}, {12, 4}, {13, 8}, {15, 10}, {16, 20}, {17, 40}});
    const int hour = decodeWeighted(p, {{20, 1}, {21, 2}, {22, 4}, {23, 8}, {25, 10}, {26, 20}});
    const int dayOfYear = decodeWeighted(p, {{30, 1}, {31, 2}, {32, 4}, {33, 8}, {35, 10},
                                             {36, 20}, {37, 40}, {38, 80}, {40, 100}, {41, 200}});
    const int year = decodeWeighted(p, {{4, 1}, {5, 2}, {6, 4}, {7, 8}, {51, 10}, {52, 20}, {53, 40}, {54, 80}});
    const int ut1Tenths = decodeWeighted(p, {{56, 1}, {57, 2}, {58, 4}});

    if (minute < 0 || hour < 0 || dayOfYear < 1 || year < 0 || ut1Tenths < 0)
        return false;
    if (minute >= 60 || hour >= 24 || dayOfYear > 366 || year > 99 || ut1Tenths > 7)
        return false;

    if (p.at(2) == PulseClass::Unknown || p.at(2) == PulseClass::Marker
        || p.at(3) == PulseClass::Unknown || p.at(3) == PulseClass::Marker
        || p.at(50) == PulseClass::Unknown || p.at(50) == PulseClass::Marker
        || p.at(55) == PulseClass::Unknown || p.at(55) == PulseClass::Marker) {
        return false;
    }

    const int fullYear = 2000 + year;
    const QDate startDate = QDate(fullYear, 1, 1).addDays(dayOfYear - 1);
    if (!startDate.isValid())
        return false;

    decoded->startUtc = QDateTime(startDate, QTime(hour, minute, 0), QTimeZone::UTC);
    decoded->year = fullYear;
    decoded->dayOfYear = dayOfYear;
    decoded->hour = hour;
    decoded->minute = minute;
    decoded->ut1Tenths = ut1Tenths;
    decoded->ut1Positive = (p.at(50) == PulseClass::One);
    decoded->leapSecond = (p.at(3) == PulseClass::One);
    const bool dstBitA = (p.at(2) == PulseClass::One);
    const bool dstBitB = (p.at(55) == PulseClass::One);
    decoded->dstCurrent = dstBitA && dstBitB;
    decoded->dstTransitionToday = dstBitA != dstBitB;
    decoded->station = frame.station;
    decoded->bitsText = pulsePatternText(frame.pulses);
    return true;
}

void WwvTimecodeDecoder::finalizeCurrentMinute()
{
    if (!m_haveCurrentMinute)
        return;

    DecodedMinute decoded;
    if (tryDecodeMinute(m_currentMinute, &decoded)) {
        const QString utcText = formatUtcLabel(decoded.startUtc);
        const QString frameText = QStringLiteral("%1  |  %2  |  %3  |  %4")
                                      .arg(decoded.startUtc.toString(QStringLiteral("yyyy-DDD HH:mm 'UTC'")))
                                      .arg(decoded.station.isEmpty() ? QStringLiteral("WWV / WWVH") : decoded.station)
                                      .arg(QStringLiteral("DOY %1").arg(decoded.dayOfYear))
                                      .arg(QStringLiteral("Year %1").arg(decoded.year));
        const QString flagsText = QStringLiteral("DUT1 %1%2.%3  |  %4  |  Leap second: %5")
                                      .arg(decoded.ut1Positive ? QStringLiteral("+") : QStringLiteral("-"))
                                      .arg(decoded.ut1Tenths / 10)
                                      .arg(decoded.ut1Tenths % 10)
                                      .arg(dstSummary(decoded.dstCurrent, decoded.dstTransitionToday))
                                      .arg(decoded.leapSecond ? QStringLiteral("Warning") : QStringLiteral("No"));

        emit frameDecoded(utcText, frameText, flagsText, decoded.bitsText);

        m_nextMinuteUtc = decoded.startUtc.addSecs(60);
        m_nextMinuteUtcValid = true;
        emit statusChanged(QStringLiteral("Decoded WWV / WWVH timecode for %1.")
                               .arg(decoded.startUtc.toString(QStringLiteral("yyyy-DDD HH:mm 'UTC'"))));
    } else {
        emit statusChanged(QStringLiteral("Minute frame captured, but the WWV / WWVH timecode did not validate."));
    }
}

void WwvTimecodeDecoder::trimBuffers()
{
    qint64 keepFrom = 0;
    if (m_nextSecondStart >= 0)
        keepFrom = std::max<qint64>(0, m_nextSecondStart - (kSecondSamples * 2));
    else if (m_nextAnalysisStart > 0)
        keepFrom = std::max<qint64>(0, m_nextAnalysisStart - (kAnalysisWindowSamples * 4));

    if (keepFrom <= m_bufferStartSample)
        return;

    const qint64 drop = keepFrom - m_bufferStartSample;
    if (drop <= 0 || drop > static_cast<qint64>(m_sampleBuffer.size()))
        return;

    m_sampleBuffer.erase(m_sampleBuffer.begin(),
                         m_sampleBuffer.begin() + static_cast<std::ptrdiff_t>(drop));
    m_bufferStartSample = keepFrom;
}

} // namespace AetherSDR
