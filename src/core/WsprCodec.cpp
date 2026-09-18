#include "WsprCodec.h"

#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#ifdef HAVE_FFTW3
#include <fftw3.h>
#endif

extern "C" {
void aethersdr_wspr_decode(const int softSymbols[162], char message[23], int* metric, int* ncycles, int* rc);
}

namespace AetherSDR {

namespace {

constexpr int kSampleRate = 12000;
constexpr int kSymbolSamples = 8192;
constexpr int kFftSamples = 16384;
constexpr int kSymbolCount = 162;
constexpr int kToneCount = 4;
constexpr double kToneSpacingHz = 12000.0 / 8192.0;
constexpr double kBinSpacingHz = 12000.0 / 16384.0;
constexpr int kToneStrideBins = kFftSamples / kSymbolSamples;
constexpr int kSearchOffsetStep = 1024;
constexpr int kMinToneHz = 200;
constexpr int kMaxToneHz = 3000;
constexpr float kMinCandidateScoreDb = 1.6f;
constexpr int kMaxCandidatesPerOffset = 6;
constexpr int kMaxCandidatesTotal = 64;

constexpr std::array<int, kSymbolCount> kSyncPattern{
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,
    0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,
    0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,
    1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,
    0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,
    0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,
    0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,
    0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
    0,0
};

struct CandidateScore {
    int startOffsetSamples{0};
    int baseBin{0};
    float scoreDb{0.0f};
};

QString fromFixedMessage(const char* text, int maxLength)
{
    QByteArray bytes(text, maxLength);
    const int terminator = bytes.indexOf('\0');
    if (terminator >= 0)
        bytes.truncate(terminator);
    return QString::fromLatin1(bytes).trimmed();
}

double safePowerRatioDb(double numerator, double denominator)
{
    constexpr double kFloor = 1.0e-18;
    return 10.0 * std::log10((numerator + kFloor) / (denominator + kFloor));
}

} // namespace

QVector<WsprCodec::DecodeResult> WsprCodec::decodeSlot(const std::vector<float>& samples12k,
                                                       QString* statusMessage)
{
#ifndef HAVE_FFTW3
    Q_UNUSED(samples12k);
    if (statusMessage)
        *statusMessage = QStringLiteral("WSPR native decoding requires FFTW3 in this build.");
    return {};
#else
    const int minimumSamples = kSymbolCount * kSymbolSamples;
    if (static_cast<int>(samples12k.size()) < minimumSamples) {
        if (statusMessage)
            *statusMessage = QStringLiteral("Waiting for a complete WSPR slot.");
        return {};
    }

    const int maxStartOffset = std::max(0, static_cast<int>(samples12k.size()) - minimumSamples);
    const int minBaseBin = std::max(0, static_cast<int>(std::floor(kMinToneHz / kBinSpacingHz)));
    const int maxTone0Hz = std::max(kMinToneHz, kMaxToneHz - static_cast<int>(std::ceil(3.0 * kToneSpacingHz)));
    const int maxBaseBin = std::max(minBaseBin + 1, static_cast<int>(std::floor(maxTone0Hz / kBinSpacingHz)));
    const int maxSearchBin = maxBaseBin + (3 * kToneStrideBins);

    std::vector<double> fftIn(static_cast<std::size_t>(kFftSamples), 0.0);
    fftw_complex* fftOut = fftw_alloc_complex(static_cast<std::size_t>(kFftSamples / 2 + 1));
    if (!fftOut) {
        if (statusMessage)
            *statusMessage = QStringLiteral("WSPR FFT allocation failed.");
        return {};
    }

    fftw_plan plan = fftw_plan_dft_r2c_1d(kFftSamples, fftIn.data(), fftOut, FFTW_MEASURE);
    if (!plan) {
        fftw_free(fftOut);
        if (statusMessage)
            *statusMessage = QStringLiteral("WSPR FFT plan creation failed.");
        return {};
    }

    std::vector<float> powerGrid(static_cast<std::size_t>(kSymbolCount * (maxSearchBin + 1)), 0.0f);
    std::vector<CandidateScore> allCandidates;
    allCandidates.reserve(512);

    for (int startOffset = 0; startOffset <= maxStartOffset; startOffset += kSearchOffsetStep) {
        for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
            const int sampleOffset = startOffset + (symbol * kSymbolSamples);
            std::fill(fftIn.begin(), fftIn.end(), 0.0);
            for (int i = 0; i < kSymbolSamples; ++i)
                fftIn[static_cast<std::size_t>(i)] = samples12k[static_cast<std::size_t>(sampleOffset + i)];

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
            double validSum = 0.0;
            double invalidSum = 0.0;
            for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
                const std::size_t rowOffset = static_cast<std::size_t>(symbol * (maxSearchBin + 1));
                const int syncParity = kSyncPattern[static_cast<std::size_t>(symbol)];
                const int valid0 = baseBin + (syncParity * kToneStrideBins);
                const int valid1 = valid0 + (2 * kToneStrideBins);
                const int invalid0 = baseBin + ((1 - syncParity) * kToneStrideBins);
                const int invalid1 = invalid0 + (2 * kToneStrideBins);
                validSum += powerGrid[rowOffset + static_cast<std::size_t>(valid0)]
                         +  powerGrid[rowOffset + static_cast<std::size_t>(valid1)];
                invalidSum += powerGrid[rowOffset + static_cast<std::size_t>(invalid0)]
                           +  powerGrid[rowOffset + static_cast<std::size_t>(invalid1)];
            }

            const float scoreDb = static_cast<float>(safePowerRatioDb(validSum, invalidSum));
            if (scoreDb >= kMinCandidateScoreDb)
                offsetCandidates.push_back({startOffset, baseBin, scoreDb});
        }

        std::sort(offsetCandidates.begin(), offsetCandidates.end(),
                  [](const CandidateScore& lhs, const CandidateScore& rhs) {
                      return lhs.scoreDb > rhs.scoreDb;
                  });

        int kept = 0;
        for (const CandidateScore& candidate : offsetCandidates) {
            bool localMaximum = true;
            for (const CandidateScore& other : offsetCandidates) {
                if (&other == &candidate || other.startOffsetSamples != candidate.startOffsetSamples)
                    continue;
                if (std::abs(other.baseBin - candidate.baseBin) <= kToneStrideBins && other.scoreDb > candidate.scoreDb) {
                    localMaximum = false;
                    break;
                }
            }
            if (!localMaximum)
                continue;
            allCandidates.push_back(candidate);
            ++kept;
            if (kept >= kMaxCandidatesPerOffset)
                break;
        }
    }

    fftw_destroy_plan(plan);
    fftw_free(fftOut);

    std::sort(allCandidates.begin(), allCandidates.end(),
              [](const CandidateScore& lhs, const CandidateScore& rhs) {
                  return lhs.scoreDb > rhs.scoreDb;
              });
    if (static_cast<int>(allCandidates.size()) > kMaxCandidatesTotal)
        allCandidates.resize(static_cast<std::size_t>(kMaxCandidatesTotal));

    QVector<DecodeResult> results;
    QSet<QString> seenKeys;
    for (const CandidateScore& candidate : allCandidates) {
        int softSymbols[kSymbolCount]{};
        double selectedPowerSum = 0.0;
        double competitorPowerSum = 0.0;

        for (int symbol = 0; symbol < kSymbolCount; ++symbol) {
            const std::size_t rowOffset = static_cast<std::size_t>(symbol * (maxSearchBin + 1));
            const int syncParity = kSyncPattern[static_cast<std::size_t>(symbol)];
            const int tone0 = candidate.baseBin + (syncParity * kToneStrideBins);
            const int tone1 = tone0 + (2 * kToneStrideBins);
            const int other0 = candidate.baseBin + ((1 - syncParity) * kToneStrideBins);
            const int other1 = other0 + (2 * kToneStrideBins);
            const double p0 = powerGrid[rowOffset + static_cast<std::size_t>(tone0)];
            const double p1 = powerGrid[rowOffset + static_cast<std::size_t>(tone1)];
            const double invalidAvg =
                (powerGrid[rowOffset + static_cast<std::size_t>(other0)]
               + powerGrid[rowOffset + static_cast<std::size_t>(other1)]) * 0.5;
            const double denom = std::max(1.0e-12, p0 + p1);
            softSymbols[symbol] = std::clamp(static_cast<int>(std::lround(255.0 * p1 / denom)), 0, 255);
            selectedPowerSum += std::max(p0, p1);
            competitorPowerSum += std::max(std::min(p0, p1), invalidAvg);
        }

        std::array<char, 23> message{};
        message.fill('\0');
        int metric = 0;
        int ncycles = 0;
        int rc = -1;
        aethersdr_wspr_decode(softSymbols, message.data(), &metric, &ncycles, &rc);
        if (rc != 0)
            continue;

        const QString messageText = fromFixedMessage(message.data(), 22);
        if (messageText.isEmpty())
            continue;

        const int toneHz = std::max(0, static_cast<int>(std::lround(
            (candidate.baseBin + (1.5 * kToneStrideBins)) * kBinSpacingHz)));
        const double deltaTimeSec = static_cast<double>(candidate.startOffsetSamples) / static_cast<double>(kSampleRate);
        const int snr = std::clamp(
            static_cast<int>(std::lround(
                safePowerRatioDb(selectedPowerSum / static_cast<double>(kSymbolCount),
                                 competitorPowerSum / static_cast<double>(kSymbolCount)) - 33.0)),
            -40, 10);

        const QString key = QStringLiteral("%1|%2|%3")
                                .arg(messageText,
                                     QString::number(toneHz),
                                     QString::number(deltaTimeSec, 'f', 1));
        if (seenKeys.contains(key))
            continue;
        seenKeys.insert(key);

        results.push_back({
            messageText,
            toneHz,
            snr,
            deltaTimeSec,
            (candidate.scoreDb < 2.4f) || (metric < 0) || (ncycles >= 10000)
        });
    }

    if (results.isEmpty() && statusMessage)
        *statusMessage = allCandidates.empty()
            ? QStringLiteral("No WSPR candidates found in the last slot.")
            : QStringLiteral("WSPR slot analyzed, but no valid messages decoded.");

    return results;
#endif
}

} // namespace AetherSDR
