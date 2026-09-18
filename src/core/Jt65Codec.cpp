#include "Jt65Codec.h"

#include "Resampler.h"

#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef HAVE_FFTW3
#include <fftw3.h>
#endif

extern "C" {
void aethersdr_jt65_encode(const char message[23], int tones[126], char normalized[23], int* itype, int* rc);
void aethersdr_jt65_decode(const int symbols[63], const int erased[63], char message[23], int* nerr, int* rc);
}

namespace AetherSDR {

namespace {

constexpr int kSourceSampleRate = 12000;
constexpr int kAnalysisSampleRate = 11025;
constexpr int kSymbolSamples = 4096;
constexpr int kSymbolCount = 126;
constexpr int kDataSymbolCount = 63;
constexpr int kMessageToneOffset = 2;
constexpr int kMessageToneCount = 64;
constexpr double kToneSpacingHz = 11025.0 / 4096.0;
constexpr double kExpectedLeadSec = 1.0;
constexpr int kDefaultSlotSeconds = 60;
constexpr int kSearchStartSamples = 0;
constexpr int kSearchStopSamples = kAnalysisSampleRate * 3;
constexpr int kSearchStepSamples = 1024;
constexpr float kMinCandidateScoreDb = 2.2f;
constexpr int kMaxCandidatesPerOffset = 6;
constexpr int kMaxCandidatesToDecode = 48;

constexpr std::array<int, kSymbolCount> kSyncPattern{
    1,0,0,1,1,0,0,0,1,1,1,1,1,1,0,1,0,1,0,0,
    0,1,0,1,1,0,0,1,0,0,0,1,1,1,0,0,1,1,1,1,
    0,1,1,0,1,1,1,1,0,0,0,1,1,0,1,0,1,0,1,1,
    0,0,1,1,0,1,0,1,0,1,0,0,1,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,1,1,0,1,0,0,1,0,1,1,0,1,
    0,1,0,1,0,0,1,1,0,0,1,0,0,1,0,0,0,0,1,1,
    1,1,1,1,1,1
};

QString normalizeText(const QString& text)
{
    return text.trimmed().toUpper();
}

std::array<char, 23> makeFixedMessage(const QString& text)
{
    std::array<char, 23> buffer{};
    buffer.fill('\0');

    const QByteArray latin = normalizeText(text).left(22).toLatin1();
    std::memcpy(buffer.data(), latin.constData(), static_cast<std::size_t>(latin.size()));
    return buffer;
}

QString fromFixedMessage(const char* text, int maxLength)
{
    QByteArray bytes(text, maxLength);
    const int terminator = bytes.indexOf('\0');
    if (terminator >= 0)
        bytes.truncate(terminator);
    return QString::fromLatin1(bytes).trimmed();
}

struct CandidateScore {
    int startOffsetSamples{0};
    int baseBin{0};
    float scoreDb{0.0f};
};

QString decodeSymbols(const std::array<int, kDataSymbolCount>& symbols,
                      const std::array<int, kDataSymbolCount>& erased,
                      int* correctedErrors,
                      int* rc)
{
    std::array<char, 23> message{};
    message.fill('\0');

    int nerr = -1;
    int localRc = -1;
    aethersdr_jt65_decode(symbols.data(), erased.data(), message.data(), &nerr, &localRc);
    if (correctedErrors)
        *correctedErrors = nerr;
    if (rc)
        *rc = localRc;
    return fromFixedMessage(message.data(), 22);
}

double safePowerRatioDb(double numerator, double denominator)
{
    constexpr double kFloor = 1.0e-18;
    return 10.0 * std::log10((numerator + kFloor) / (denominator + kFloor));
}

} // namespace

bool Jt65Codec::encodeMessage(const QString& message,
                              std::vector<int>* tones,
                              QString* normalizedMessage,
                              QString* errorMessage)
{
    if (!tones) {
        if (errorMessage)
            *errorMessage = QStringLiteral("JT65 tone buffer is not available.");
        return false;
    }

    const QString text = normalizeText(message).left(22);
    if (text.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Enter a JT65 message to transmit.");
        return false;
    }

    const std::array<char, 23> input = makeFixedMessage(text);
    std::array<char, 23> normalized{};
    normalized.fill('\0');
    int itype = 0;
    int rc = -1;
    int jt65Tones[kSymbolCount]{};
    aethersdr_jt65_encode(input.data(), jt65Tones, normalized.data(), &itype, &rc);
    if (rc != 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("JT65 encoding failed.");
        return false;
    }

    Q_UNUSED(itype);

    tones->assign(std::begin(jt65Tones), std::end(jt65Tones));
    if (normalizedMessage) {
        *normalizedMessage = fromFixedMessage(normalized.data(), 22);
        if (normalizedMessage->isEmpty())
            *normalizedMessage = text;
    }
    return true;
}

QVector<Jt65Codec::DecodeResult> Jt65Codec::decodeSlot(const std::vector<float>& samples12k,
                                                       QString* statusMessage)
{
#ifndef HAVE_FFTW3
    Q_UNUSED(samples12k);
    if (statusMessage)
        *statusMessage = QStringLiteral("JT65 native decoding requires FFTW3 in this build.");
    return {};
#else
    if (samples12k.empty()) {
        if (statusMessage)
            *statusMessage = QStringLiteral("No JT65 audio captured yet.");
        return {};
    }

    Resampler resampler(static_cast<double>(kSourceSampleRate),
                        static_cast<double>(kAnalysisSampleRate),
                        std::max<int>(4096, static_cast<int>(samples12k.size())));
    const QByteArray resampledBytes = resampler.process(samples12k.data(),
                                                        static_cast<int>(samples12k.size()));
    const int sampleCount = resampledBytes.size() / static_cast<int>(sizeof(float));
    if (sampleCount < (kAnalysisSampleRate * kDefaultSlotSeconds)) {
        if (statusMessage)
            *statusMessage = QStringLiteral("Waiting for a complete JT65 slot.");
        return {};
    }

    const auto* resampled = reinterpret_cast<const float*>(resampledBytes.constData());
    const int maxSearchBin = static_cast<int>(
        std::floor((3000.0 + ((kMessageToneOffset + kMessageToneCount) * kToneSpacingHz)) / kToneSpacingHz));
    const int minBaseBin = std::max(0, static_cast<int>(std::floor(200.0 / kToneSpacingHz)));
    const int maxBaseBin = std::max(minBaseBin + 1,
                                    static_cast<int>(std::floor((3000.0 - ((kMessageToneOffset + kMessageToneCount - 1) * kToneSpacingHz))
                                                                 / kToneSpacingHz)));

    std::vector<double> fftIn(static_cast<std::size_t>(kSymbolSamples), 0.0);
    fftw_complex* fftOut = fftw_alloc_complex(static_cast<std::size_t>(kSymbolSamples / 2 + 1));
    if (!fftOut) {
        if (statusMessage)
            *statusMessage = QStringLiteral("JT65 FFT allocation failed.");
        return {};
    }

    fftw_plan plan = fftw_plan_dft_r2c_1d(kSymbolSamples, fftIn.data(), fftOut, FFTW_MEASURE);
    if (!plan) {
        fftw_free(fftOut);
        if (statusMessage)
            *statusMessage = QStringLiteral("JT65 FFT plan creation failed.");
        return {};
    }

    std::vector<CandidateScore> allCandidates;
    allCandidates.reserve(256);
    std::vector<float> powerGrid(static_cast<std::size_t>(kSymbolCount * (maxSearchBin + 1)), 0.0f);

    const int maxStartOffset = std::min(kSearchStopSamples,
                                        sampleCount - (kSymbolCount * kSymbolSamples));
    for (int startOffset = kSearchStartSamples; startOffset <= maxStartOffset; startOffset += kSearchStepSamples) {
        for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
            const int sampleOffset = startOffset + (symbol * kSymbolSamples);
            const float* symbolStart = resampled + sampleOffset;
            for (int i = 0; i < kSymbolSamples; ++i)
                fftIn[static_cast<std::size_t>(i)] = symbolStart[i];

            fftw_execute(plan);

            const std::size_t rowOffset = static_cast<std::size_t>(symbol * (maxSearchBin + 1));
            for (int bin = 0; bin <= maxSearchBin; ++bin) {
                const double re = fftOut[bin][0];
                const double im = fftOut[bin][1];
                powerGrid[rowOffset + static_cast<std::size_t>(bin)] = static_cast<float>((re * re) + (im * im));
            }
        }

        std::vector<CandidateScore> offsetCandidates;
        offsetCandidates.reserve(maxBaseBin - minBaseBin + 1);
        for (int baseBin = minBaseBin; baseBin <= maxBaseBin; ++baseBin) {
            double syncSum = 0.0;
            double dataLeakSum = 0.0;
            double adjacentSyncSum = 0.0;

            for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
                const std::size_t rowOffset = static_cast<std::size_t>(symbol * (maxSearchBin + 1));
                const double center = powerGrid[rowOffset + static_cast<std::size_t>(baseBin)];
                const double left = (baseBin > 0) ? powerGrid[rowOffset + static_cast<std::size_t>(baseBin - 1)] : center;
                const double right = powerGrid[rowOffset + static_cast<std::size_t>(baseBin + 1)];
                if (kSyncPattern[static_cast<std::size_t>(symbol)] != 0) {
                    syncSum += center;
                    adjacentSyncSum += (left + right) * 0.5;
                } else {
                    dataLeakSum += center;
                }
            }

            const double syncAvg = syncSum / static_cast<double>(kDataSymbolCount);
            const double noiseAvg = (adjacentSyncSum + dataLeakSum) / static_cast<double>(kSymbolCount);
            const float scoreDb = static_cast<float>(safePowerRatioDb(syncAvg, noiseAvg));
            if (scoreDb >= kMinCandidateScoreDb)
                offsetCandidates.push_back({startOffset, baseBin, scoreDb});
        }

        std::sort(offsetCandidates.begin(), offsetCandidates.end(),
                  [](const CandidateScore& lhs, const CandidateScore& rhs) {
                      return lhs.scoreDb > rhs.scoreDb;
                  });

        int kept = 0;
        for (std::size_t i = 0; i < offsetCandidates.size() && kept < kMaxCandidatesPerOffset; ++i) {
            const CandidateScore& candidate = offsetCandidates[i];
            bool localMaximum = true;
            for (const CandidateScore& other : offsetCandidates) {
                if (other.baseBin == candidate.baseBin)
                    continue;
                if (std::abs(other.baseBin - candidate.baseBin) <= 1 && other.scoreDb > candidate.scoreDb) {
                    localMaximum = false;
                    break;
                }
            }
            if (!localMaximum)
                continue;

            allCandidates.push_back(candidate);
            ++kept;
        }
    }

    fftw_destroy_plan(plan);
    fftw_free(fftOut);

    std::sort(allCandidates.begin(), allCandidates.end(),
              [](const CandidateScore& lhs, const CandidateScore& rhs) {
                  return lhs.scoreDb > rhs.scoreDb;
              });
    if (static_cast<int>(allCandidates.size()) > kMaxCandidatesToDecode)
        allCandidates.resize(static_cast<std::size_t>(kMaxCandidatesToDecode));

    QVector<DecodeResult> results;
    QSet<QString> seenKeys;
    for (const CandidateScore& candidate : allCandidates) {
        std::array<int, kDataSymbolCount> channelSymbols{};
        std::array<int, kDataSymbolCount> erased{};
        double signalSum = 0.0;
        double competitorSum = 0.0;
        int dataIndex = 0;

        for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
            if (kSyncPattern[static_cast<std::size_t>(symbol)] != 0)
                continue;

            const std::size_t rowOffset = static_cast<std::size_t>(symbol * (maxSearchBin + 1));
            float bestPower = -1.0f;
            float secondPower = -1.0f;
            int bestTone = 0;
            double meanPower = 0.0;
            for (int tone = 0; tone < kMessageToneCount; ++tone) {
                const int bin = candidate.baseBin + kMessageToneOffset + tone;
                const float power = powerGrid[rowOffset + static_cast<std::size_t>(bin)];
                meanPower += power;
                if (power > bestPower) {
                    secondPower = bestPower;
                    bestPower = power;
                    bestTone = tone;
                } else if (power > secondPower) {
                    secondPower = power;
                }
            }

            meanPower /= static_cast<double>(kMessageToneCount);
            channelSymbols[static_cast<std::size_t>(dataIndex)] = bestTone;
            erased[static_cast<std::size_t>(dataIndex)] =
                ((bestPower < (meanPower * 1.6)) || (bestPower < (std::max(1.0f, secondPower) * 1.18f))) ? 1 : 0;
            signalSum += bestPower;
            competitorSum += std::max<double>(meanPower, std::max(0.0f, secondPower));
            ++dataIndex;
        }

        int correctedErrors = -1;
        int rc = -1;
        const QString message = decodeSymbols(channelSymbols, erased, &correctedErrors, &rc);
        if (rc != 0 || message.isEmpty())
            continue;

        const int toneHz = std::max(0, static_cast<int>(std::lround(candidate.baseBin * kToneSpacingHz)));
        const double deltaTimeSec =
            (static_cast<double>(candidate.startOffsetSamples) / static_cast<double>(kAnalysisSampleRate)) - kExpectedLeadSec;
        const int snr = std::clamp(static_cast<int>(std::lround(
                                       safePowerRatioDb(signalSum / static_cast<double>(kDataSymbolCount),
                                                        competitorSum / static_cast<double>(kDataSymbolCount)) - 24.0)),
                                   -30, 20);
        const QString key = QStringLiteral("%1|%2|%3")
                                .arg(message,
                                     QString::number(toneHz),
                                     QString::number(deltaTimeSec, 'f', 1));
        if (seenKeys.contains(key))
            continue;

        seenKeys.insert(key);
        results.push_back({
            message,
            toneHz,
            snr,
            deltaTimeSec,
            (candidate.scoreDb < 3.5f) || (correctedErrors > 8)
        });
    }

    if (results.isEmpty() && statusMessage)
        *statusMessage = allCandidates.empty()
            ? QStringLiteral("No JT65 candidates found in the last slot.")
            : QStringLiteral("JT65 slot analyzed, but no valid messages decoded.");
    return results;
#endif
}

} // namespace AetherSDR
