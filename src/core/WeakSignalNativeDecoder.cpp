#include "WeakSignalNativeDecoder.h"

#include "Jt65Codec.h"
#include "WsprCodec.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <common/monitor.h>
#include <ft8/message.h>
}

namespace AetherSDR {

namespace {

constexpr int kNativeSampleRate = 12000;
constexpr qint64 kFtxDecodeLatencyMs = 700;
constexpr qint64 kWsprDecodeLatencyMs = 2500;
constexpr qint64 kFtxSlotMs = 15000;
constexpr qint64 kFt4SlotMs = 7500;
constexpr qint64 kJt65SlotMs = 60000;
constexpr qint64 kWsprSlotMs = 120000;
constexpr qint64 kWsprCaptureMs = 114000;
constexpr qint64 kMaxBufferMs = 135000;
constexpr qint64 kRecentDecodeWindowMs = 10 * 60 * 1000;

constexpr int kMinScore = 10;
constexpr int kMaxCandidates = 140;
constexpr int kLdpcIterations = 25;
constexpr int kFreqOsr = 2;
constexpr int kTimeOsr = 2;

thread_local WeakSignalNativeDecoder* g_hashOwner = nullptr;

bool lookupHashBridge(ftx_callsign_hash_type_t hashType, uint32_t hash, char* callsign)
{
    return g_hashOwner && g_hashOwner->lookupHash(hashType, hash, callsign);
}

void saveHashBridge(const char* callsign, uint32_t hash)
{
    if (g_hashOwner)
        g_hashOwner->saveHash(callsign, hash);
}

QString makeRecentDecodeKey(const QString& mode,
                            qint64 slotStartMs,
                            int frequencyHz,
                            const QString& message)
{
    return QStringLiteral("%1|%2|%3|%4")
        .arg(mode,
             QString::number(slotStartMs / 1000),
             QString::number(frequencyHz),
             message.trimmed().toUpper());
}

qint64 currentUtcMs()
{
    return QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
}

} // namespace

WeakSignalNativeDecoder::WeakSignalNativeDecoder(QObject* parent)
    : QObject(parent)
    , m_resampler(24000.0, 12000.0, 4096)
{
}

WeakSignalNativeDecoder::~WeakSignalNativeDecoder() = default;

void WeakSignalNativeDecoder::startListening()
{
    if (m_running)
        return;

    resetCaptureState();
    m_running = true;
    emit listening();
    emit statusMessage(QStringLiteral("Native weak-signal decoder running."));
}

void WeakSignalNativeDecoder::stopListening()
{
    if (!m_running)
        return;

    m_running = false;
    resetCaptureState();
    emit stopped();
    emit statusMessage(QStringLiteral("Native weak-signal decoder stopped."));
}

void WeakSignalNativeDecoder::setMode(Mode mode)
{
    if (m_mode == mode)
        return;

    m_mode = mode;
    resetCaptureState();
    emit cleared(0);

    if (m_running) {
        emit statusMessage(QStringLiteral("Decoder reset for %1.")
                               .arg(m_mode == Mode::FT4 ? QStringLiteral("FT4")
                                    : m_mode == Mode::JT65 ? QStringLiteral("JT65")
                                    : m_mode == Mode::WSPR ? QStringLiteral("WSPR")
                                                           : QStringLiteral("FT8")));
    }
}

void WeakSignalNativeDecoder::setDialFrequencyKhz(double khz)
{
    if (std::abs(m_dialFrequencyKhz - khz) < 0.05)
        return;

    m_dialFrequencyKhz = std::max(0.0, khz);
    resetCaptureState();
    emit cleared(0);

    if (m_running)
        emit statusMessage(QStringLiteral("Decoder reset for %1 kHz.")
                               .arg(QString::number(m_dialFrequencyKhz, 'f', 1)));
}

void WeakSignalNativeDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    if (!m_running)
        return;

    appendAudio(pcm24kStereo);
    maybeDecode();
}

void WeakSignalNativeDecoder::requestReplay()
{
    if (!m_running) {
        emit statusMessage(QStringLiteral("RX is off."));
        return;
    }

    maybeDecode();

    if (m_mode == Mode::JT65)
        decodeLatestCompletedJt65Slot(true);
    else if (m_mode == Mode::WSPR)
        decodeLatestCompletedWsprSlot(true);
    else
        decodeLatestCompletedFtxSlot(true);
}

void WeakSignalNativeDecoder::requestClear(quint8 window)
{
    Q_UNUSED(window);
    m_recentDecodeKeys.clear();
    emit cleared(0);
}

void WeakSignalNativeDecoder::resetCaptureState()
{
    m_audio12k.clear();
    m_bufferEndTimeMs = 0.0;
    m_lastFtxSlotStartMs = -1;
    m_lastJt65SlotStartMs = -1;
    m_lastWsprSlotStartMs = -1;
    m_lastJt65NoticeMs = 0;
}

void WeakSignalNativeDecoder::appendAudio(const QByteArray& pcm24kStereo)
{
    const int floatCount = pcm24kStereo.size() / static_cast<int>(sizeof(float));
    if (floatCount < 2)
        return;

    const auto* stereo = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int frames = floatCount / 2;
    const QByteArray mono12k = m_resampler.processStereoToMono(stereo, frames);
    const int sampleCount = mono12k.size() / static_cast<int>(sizeof(float));
    if (sampleCount <= 0)
        return;

    const auto* samples = reinterpret_cast<const float*>(mono12k.constData());
    m_audio12k.insert(m_audio12k.end(), samples, samples + sampleCount);
    m_bufferEndTimeMs = static_cast<double>(currentUtcMs());
    pruneAudioBuffer();
}

void WeakSignalNativeDecoder::pruneAudioBuffer()
{
    const std::size_t maxSamples = static_cast<std::size_t>((kMaxBufferMs * kNativeSampleRate) / 1000);
    if (m_audio12k.size() <= maxSamples)
        return;

    const std::size_t trimCount = m_audio12k.size() - maxSamples;
    m_audio12k.erase(m_audio12k.begin(), m_audio12k.begin() + static_cast<std::ptrdiff_t>(trimCount));
}

std::vector<float> WeakSignalNativeDecoder::audioWindowForTimes(qint64 startMs, qint64 endMs) const
{
    if (m_audio12k.empty() || endMs <= startMs || m_bufferEndTimeMs <= 0.0)
        return {};

    const double durationMs = (static_cast<double>(m_audio12k.size()) * 1000.0) / kNativeSampleRate;
    const double bufferStartMs = m_bufferEndTimeMs - durationMs;

    if (startMs < static_cast<qint64>(std::floor(bufferStartMs)) || endMs > static_cast<qint64>(std::ceil(m_bufferEndTimeMs)))
        return {};

    const qint64 startIndex = std::max<qint64>(0, std::llround((startMs - bufferStartMs) * kNativeSampleRate / 1000.0));
    const qint64 endIndex = std::min<qint64>(static_cast<qint64>(m_audio12k.size()),
                                             std::llround((endMs - bufferStartMs) * kNativeSampleRate / 1000.0));
    if (endIndex <= startIndex)
        return {};

    return std::vector<float>(m_audio12k.begin() + startIndex, m_audio12k.begin() + endIndex);
}

void WeakSignalNativeDecoder::maybeDecode()
{
    if (!m_running)
        return;

    pruneRecentDecodeKeys(currentUtcMs());

    switch (m_mode) {
    case Mode::FT4:
    case Mode::FT8:
        decodeLatestCompletedFtxSlot(false);
        break;
    case Mode::WSPR:
        decodeLatestCompletedWsprSlot(false);
        break;
    case Mode::JT65:
        decodeLatestCompletedJt65Slot(false);
        break;
    }
}

void WeakSignalNativeDecoder::decodeLatestCompletedFtxSlot(bool force)
{
    const qint64 slotMs = (m_mode == Mode::FT4) ? kFt4SlotMs : kFtxSlotMs;
    const qint64 now = currentUtcMs();
    const qint64 completedSlotEnd = ((now - kFtxDecodeLatencyMs) / slotMs) * slotMs;
    if (completedSlotEnd < slotMs)
        return;

    const qint64 slotStart = completedSlotEnd - slotMs;
    if (!force && slotStart == m_lastFtxSlotStartMs)
        return;

    const auto samples = audioWindowForTimes(slotStart, completedSlotEnd);
    const std::size_t minimumSamples = static_cast<std::size_t>((slotMs * kNativeSampleRate * 95) / 100000);
    if (samples.size() < minimumSamples) {
        if (force) {
            emit statusMessage(QStringLiteral("Waiting for a complete %1 slot.")
                                   .arg(m_mode == Mode::FT4 ? QStringLiteral("FT4")
                                                            : QStringLiteral("FT8")));
        }
        return;
    }

    m_lastFtxSlotStartMs = slotStart;
    emitFtxDecodes(samples, slotStart, m_mode);
}

void WeakSignalNativeDecoder::decodeLatestCompletedJt65Slot(bool force)
{
    const qint64 now = currentUtcMs();
    const qint64 completedSlotEnd = ((now - kWsprDecodeLatencyMs) / kJt65SlotMs) * kJt65SlotMs;
    if (completedSlotEnd < kJt65SlotMs)
        return;

    const qint64 slotStart = completedSlotEnd - kJt65SlotMs;
    if (!force && slotStart == m_lastJt65SlotStartMs)
        return;

    const auto samples = audioWindowForTimes(slotStart, completedSlotEnd);
    const std::size_t minimumSamples = static_cast<std::size_t>((kJt65SlotMs * kNativeSampleRate * 95) / 100000);
    if (samples.size() < minimumSamples) {
        if (force)
            emit statusMessage(QStringLiteral("Waiting for a complete JT65 slot."));
        return;
    }

    m_lastJt65SlotStartMs = slotStart;
    emitJt65Decodes(samples, slotStart);
}

void WeakSignalNativeDecoder::decodeLatestCompletedWsprSlot(bool force)
{
    const qint64 now = currentUtcMs();
    const qint64 completedSlotStart = ((now - (kWsprCaptureMs + kWsprDecodeLatencyMs)) / kWsprSlotMs) * kWsprSlotMs;
    if (completedSlotStart < 0)
        return;

    if (!force && completedSlotStart == m_lastWsprSlotStartMs)
        return;

    const auto samples = audioWindowForTimes(completedSlotStart, completedSlotStart + kWsprCaptureMs);
    const std::size_t minimumSamples = static_cast<std::size_t>((kWsprCaptureMs * kNativeSampleRate * 95) / 100000);
    if (samples.size() < minimumSamples) {
        if (force)
            emit statusMessage(QStringLiteral("Waiting for a complete 2-minute WSPR slot."));
        return;
    }

    m_lastWsprSlotStartMs = completedSlotStart;
    emitWsprDecodes(samples, completedSlotStart);
}

void WeakSignalNativeDecoder::emitFtxDecodes(const std::vector<float>& samples, qint64 slotStartMs, Mode mode)
{
    const ftx_protocol_t protocol = (mode == Mode::FT4) ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
    const QString modeLabel = (mode == Mode::FT4) ? QStringLiteral("FT4") : QStringLiteral("FT8");

    monitor_config_t monitorConfig = {
        .f_min = 200.0f,
        .f_max = 3000.0f,
        .sample_rate = kNativeSampleRate,
        .time_osr = kTimeOsr,
        .freq_osr = kFreqOsr,
        .protocol = protocol,
    };

    monitor_t monitor{};
    monitor_init(&monitor, &monitorConfig);
    for (int framePos = 0; framePos + monitor.block_size <= static_cast<int>(samples.size()); framePos += monitor.block_size)
        monitor_process(&monitor, samples.data() + framePos);

    ftx_candidate_t candidates[kMaxCandidates];
    const int candidateCount = ftx_find_candidates(&monitor.wf, kMaxCandidates, candidates, kMinScore);

    QSet<QByteArray> emittedPayloads;
    ftx_callsign_hash_interface_t hashInterface = {
        .lookup_hash = lookupHashBridge,
        .save_hash = saveHashBridge
    };
    g_hashOwner = this;

    for (int i = 0; i < candidateCount; ++i) {
        const ftx_candidate_t& candidate = candidates[i];
        const float freqHz = (monitor.min_bin
                              + candidate.freq_offset
                              + (static_cast<float>(candidate.freq_sub) / monitor.wf.freq_osr))
                             / monitor.symbol_period;
        const float timeSec = (candidate.time_offset
                               + (static_cast<float>(candidate.time_sub) / monitor.wf.time_osr))
                              * monitor.symbol_period;

        ftx_message_t message{};
        ftx_decode_status_t status{};
        if (!ftx_decode_candidate(&monitor.wf, &candidate, kLdpcIterations, &message, &status))
            continue;

        QByteArray payloadKey(reinterpret_cast<const char*>(message.payload), sizeof(message.payload));
        payloadKey.append(static_cast<char>(message.hash >> 8));
        payloadKey.append(static_cast<char>(message.hash & 0xff));
        if (emittedPayloads.contains(payloadKey))
            continue;
        emittedPayloads.insert(payloadKey);

        char text[FTX_MAX_MESSAGE_LENGTH]{};
        ftx_message_offsets_t offsets{};
        if (ftx_message_decode(&message, &hashInterface, text, &offsets) != FTX_MESSAGE_RC_OK)
            continue;

        const QString messageText = QString::fromLatin1(text).trimmed();
        if (messageText.isEmpty())
            continue;

        const int frequencyOffsetHz = std::max(0, static_cast<int>(std::lround(freqHz)));
        const qint64 decodeTimeMs = slotStartMs + static_cast<qint64>(std::llround(timeSec * 1000.0));
        const QString recentKey = makeRecentDecodeKey(modeLabel, slotStartMs, frequencyOffsetHz, messageText);
        if (hasSeenDecodeKey(recentKey, decodeTimeMs))
            continue;

        WeakSignalDecode decode;
        decode.isNew = true;
        decode.utcTime = QDateTime::fromMSecsSinceEpoch(decodeTimeMs, QTimeZone::UTC).time();
        decode.snr = std::clamp(static_cast<int>(std::lround(candidate.score * 0.5f)), -30, 20);
        decode.deltaTimeSec = timeSec;
        decode.deltaFrequencyHz = static_cast<quint32>(frequencyOffsetHz);
        decode.frequencyHz = static_cast<quint64>(std::max(0.0, std::round(m_dialFrequencyKhz * 1000.0) + frequencyOffsetHz));
        decode.mode = modeLabel;
        decode.message = messageText;
        decode.lowConfidence = candidate.score < (kMinScore + 2);
        decode.callsign = extractCallsign(messageText);
        decode.grid = extractGrid(messageText);

        rememberDecodeKey(recentKey, decodeTimeMs);
        emit decodeReceived(decode);
    }

    g_hashOwner = nullptr;
    cleanupHashTable(10);
    monitor_free(&monitor);
}

void WeakSignalNativeDecoder::emitWsprDecodes(const std::vector<float>& samples, qint64 slotStartMs)
{
    QString statusText;
    const QVector<WsprCodec::DecodeResult> decodes = WsprCodec::decodeSlot(samples, &statusText);
    if (decodes.isEmpty()) {
        if (!statusText.isEmpty())
            emit statusMessage(statusText);
        return;
    }

    static const QRegularExpression powerRx(R"(([-+]?\d+)\s*$)");
    for (const auto& result : decodes) {
        const int frequencyOffsetHz = std::max(0, result.toneHz);
        const qint64 decodeTimeMs = slotStartMs + static_cast<qint64>(std::llround(result.deltaTimeSec * 1000.0));
        const QString recentKey = makeRecentDecodeKey(QStringLiteral("WSPR"), slotStartMs, frequencyOffsetHz, result.message);
        if (hasSeenDecodeKey(recentKey, decodeTimeMs))
            continue;

        WeakSignalDecode decode;
        decode.isNew = true;
        decode.utcTime = QDateTime::fromMSecsSinceEpoch(decodeTimeMs, QTimeZone::UTC).time();
        decode.snr = result.snr;
        decode.deltaTimeSec = result.deltaTimeSec;
        decode.deltaFrequencyHz = static_cast<quint32>(frequencyOffsetHz);
        decode.frequencyHz = static_cast<quint64>(std::max(0.0, std::round(m_dialFrequencyKhz * 1000.0) + frequencyOffsetHz));
        decode.mode = QStringLiteral("WSPR");
        decode.message = result.message;
        decode.lowConfidence = result.lowConfidence;
        decode.callsign = extractCallsign(result.message);
        decode.grid = extractGrid(result.message);
        const auto powerMatch = powerRx.match(result.message);
        if (powerMatch.hasMatch())
            decode.powerDbm = powerMatch.captured(1).toInt();

        rememberDecodeKey(recentKey, decodeTimeMs);
        emit decodeReceived(decode);
    }
}

void WeakSignalNativeDecoder::emitJt65Decodes(const std::vector<float>& samples, qint64 slotStartMs)
{
    QString statusText;
    const QVector<Jt65Codec::DecodeResult> decodes = Jt65Codec::decodeSlot(samples, &statusText);
    if (decodes.isEmpty()) {
        const qint64 now = currentUtcMs();
        if (!statusText.isEmpty() && (m_lastJt65NoticeMs == 0 || (now - m_lastJt65NoticeMs) >= 15000 || !m_running)) {
            m_lastJt65NoticeMs = now;
            emit statusMessage(statusText);
        }
        return;
    }

    for (const auto& result : decodes) {
        const int frequencyOffsetHz = std::max(0, result.toneHz);
        const qint64 decodeTimeMs = slotStartMs + static_cast<qint64>(std::llround((1.0 + result.deltaTimeSec) * 1000.0));
        const QString recentKey = makeRecentDecodeKey(QStringLiteral("JT65"), slotStartMs, frequencyOffsetHz, result.message);
        if (hasSeenDecodeKey(recentKey, decodeTimeMs))
            continue;

        WeakSignalDecode decode;
        decode.isNew = true;
        decode.utcTime = QDateTime::fromMSecsSinceEpoch(decodeTimeMs, QTimeZone::UTC).time();
        decode.snr = result.snr;
        decode.deltaTimeSec = result.deltaTimeSec;
        decode.deltaFrequencyHz = static_cast<quint32>(frequencyOffsetHz);
        decode.frequencyHz = static_cast<quint64>(std::max(0.0, std::round(m_dialFrequencyKhz * 1000.0) + frequencyOffsetHz));
        decode.mode = QStringLiteral("JT65");
        decode.message = result.message;
        decode.lowConfidence = result.lowConfidence;
        decode.callsign = extractCallsign(result.message);
        decode.grid = extractGrid(result.message);

        rememberDecodeKey(recentKey, decodeTimeMs);
        emit decodeReceived(decode);
    }
}

QString WeakSignalNativeDecoder::extractCallsign(const QString& message) const
{
    static const QRegularExpression callRx(R"(\b([A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z])\b)");
    const QStringList parts = message.trimmed().toUpper().split(QLatin1Char(' '), Qt::SkipEmptyParts);

    if (parts.isEmpty())
        return {};

    if (parts.first() == QLatin1String("CQ")) {
        for (int i = 1; i < parts.size(); ++i) {
            const auto match = callRx.match(parts.at(i));
            if (match.hasMatch())
                return match.captured(1);
        }
        return {};
    }

    if (parts.size() >= 2) {
        const auto first = callRx.match(parts.at(0));
        const auto second = callRx.match(parts.at(1));
        if (first.hasMatch() && second.hasMatch())
            return second.captured(1);
        if (first.hasMatch())
            return first.captured(1);
    }

    const auto fallback = callRx.match(message.toUpper());
    return fallback.hasMatch() ? fallback.captured(1) : QString{};
}

QString WeakSignalNativeDecoder::extractGrid(const QString& message) const
{
    static const QRegularExpression gridRx(R"(\b([A-R]{2}[0-9]{2}(?:[A-X]{2})?)\b)");
    const auto match = gridRx.match(message.trimmed().toUpper());
    return match.hasMatch() ? match.captured(1) : QString{};
}

bool WeakSignalNativeDecoder::lookupHash(int hashTypeValue, quint32 hash, char* callsign)
{
    const auto hashType = static_cast<ftx_callsign_hash_type_t>(hashTypeValue);
    const quint8 shift = (hashType == FTX_CALLSIGN_HASH_10_BITS) ? 12
                         : (hashType == FTX_CALLSIGN_HASH_12_BITS) ? 10
                                                                    : 0;
    const quint16 hash10 = static_cast<quint16>((hash >> (12 - shift)) & 0x3ffu);
    int index = (hash10 * 23) % static_cast<int>(m_hashTable.size());
    while (m_hashTable[static_cast<std::size_t>(index)].callsign[0] != '\0') {
        const HashEntry& entry = m_hashTable[static_cast<std::size_t>(index)];
        if (((entry.hash & 0x3fffffu) >> shift) == hash) {
            std::strcpy(callsign, entry.callsign);
            return true;
        }
        index = (index + 1) % static_cast<int>(m_hashTable.size());
    }

    callsign[0] = '\0';
    return false;
}

void WeakSignalNativeDecoder::saveHash(const char* callsign, quint32 hash)
{
    const quint16 hash10 = static_cast<quint16>((hash >> 12) & 0x3ffu);
    int index = (hash10 * 23) % static_cast<int>(m_hashTable.size());
    while (m_hashTable[static_cast<std::size_t>(index)].callsign[0] != '\0') {
        HashEntry& entry = m_hashTable[static_cast<std::size_t>(index)];
        if (((entry.hash & 0x3fffffu) == hash) && (std::strcmp(entry.callsign, callsign) == 0)) {
            entry.hash &= 0x3fffffu;
            return;
        }
        index = (index + 1) % static_cast<int>(m_hashTable.size());
    }

    HashEntry& entry = m_hashTable[static_cast<std::size_t>(index)];
    std::strncpy(entry.callsign, callsign, sizeof(entry.callsign) - 1);
    entry.callsign[sizeof(entry.callsign) - 1] = '\0';
    entry.hash = hash;
}

void WeakSignalNativeDecoder::cleanupHashTable(quint8 maxAge)
{
    for (HashEntry& entry : m_hashTable) {
        if (entry.callsign[0] == '\0')
            continue;

        const quint8 age = static_cast<quint8>(entry.hash >> 24);
        if (age > maxAge) {
            entry.callsign[0] = '\0';
            entry.hash = 0;
            continue;
        }

        entry.hash = ((static_cast<quint32>(age + 1u) << 24) | (entry.hash & 0x3fffffu));
    }
}

bool WeakSignalNativeDecoder::hasSeenDecodeKey(const QString& key, qint64 timestampMs) const
{
    const auto it = m_recentDecodeKeys.constFind(key);
    return it != m_recentDecodeKeys.constEnd() && (timestampMs - it.value()) < kRecentDecodeWindowMs;
}

void WeakSignalNativeDecoder::rememberDecodeKey(const QString& key, qint64 timestampMs)
{
    m_recentDecodeKeys.insert(key, timestampMs);
}

void WeakSignalNativeDecoder::pruneRecentDecodeKeys(qint64 nowMs)
{
    for (auto it = m_recentDecodeKeys.begin(); it != m_recentDecodeKeys.end();) {
        if ((nowMs - it.value()) > kRecentDecodeWindowMs)
            it = m_recentDecodeKeys.erase(it);
        else
            ++it;
    }
}

} // namespace AetherSDR
