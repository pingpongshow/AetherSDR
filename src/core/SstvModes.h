#pragma once

#include <array>
#include <QString>

namespace AetherSDR {

enum class SstvColorModel {
    Rgb,
    Gray
};

struct SstvModeDefinition {
    const char* name;
    int visCode;
    int width;
    int height;
    SstvColorModel colorModel;
    double lineMs;
    double syncMs;
    double redBeginMs;
    double redEndMs;
    double greenBeginMs;
    double greenEndMs;
    double blueBeginMs;
    double blueEndMs;
    double grayBeginMs;
    double grayEndMs;
};

namespace detail {

constexpr SstvModeDefinition martinMode(const char* name, int visCode, int width, int height, double channelMs)
{
    constexpr double kSyncMs = 4.862;
    constexpr double kSeparatorMs = 0.572;
    const double greenBegin = kSeparatorMs;
    const double greenEnd = greenBegin + channelMs;
    const double blueBegin = greenEnd + kSeparatorMs;
    const double blueEnd = blueBegin + channelMs;
    const double redBegin = blueEnd + kSeparatorMs;
    const double redEnd = redBegin + channelMs;
    const double lineMs = redEnd + kSeparatorMs;

    return {
        name,
        visCode,
        width,
        height,
        SstvColorModel::Rgb,
        lineMs,
        kSyncMs,
        redBegin,
        redEnd,
        greenBegin,
        greenEnd,
        blueBegin,
        blueEnd,
        0.0,
        0.0
    };
}

constexpr SstvModeDefinition scottieMode(const char* name, int visCode, double channelMs)
{
    constexpr double kSyncMs = 9.0;
    constexpr double kSeparatorMs = 1.5;
    const double redBegin = kSeparatorMs;
    const double redEnd = redBegin + channelMs;
    const double blueEnd = -kSyncMs;
    const double blueBegin = blueEnd - channelMs;
    const double greenEnd = blueBegin - kSeparatorMs;
    const double greenBegin = greenEnd - channelMs;
    const double lineMs = kSyncMs + 3.0 * (channelMs + kSeparatorMs);

    return {
        name,
        visCode,
        320,
        256,
        SstvColorModel::Rgb,
        lineMs,
        kSyncMs,
        redBegin,
        redEnd,
        greenBegin,
        greenEnd,
        blueBegin,
        blueEnd,
        0.0,
        0.0
    };
}

constexpr SstvModeDefinition wraaseMode(const char* name,
                                        int visCode,
                                        double porchMs,
                                        double channelRedMs,
                                        double channelGreenMs,
                                        double channelBlueMs,
                                        double gap01Ms = 0.0,
                                        double gap12Ms = 0.0)
{
    constexpr double kSyncMs = 5.5225;
    const double redBegin = porchMs;
    const double redEnd = redBegin + channelRedMs;
    const double greenBegin = redEnd + gap01Ms;
    const double greenEnd = greenBegin + channelGreenMs;
    const double blueBegin = greenEnd + gap12Ms;
    const double blueEnd = blueBegin + channelBlueMs;
    const double lineMs = kSyncMs + porchMs + channelRedMs + gap01Ms + channelGreenMs + gap12Ms + channelBlueMs;

    return {
        name,
        visCode,
        320,
        256,
        SstvColorModel::Rgb,
        lineMs,
        kSyncMs,
        redBegin,
        redEnd,
        greenBegin,
        greenEnd,
        blueBegin,
        blueEnd,
        0.0,
        0.0
    };
}

constexpr SstvModeDefinition pasokonMode(const char* name,
                                         int visCode,
                                         double syncMs,
                                         double porchMs,
                                         double channelMs)
{
    const double redBegin = porchMs;
    const double redEnd = redBegin + channelMs;
    const double greenBegin = redEnd + porchMs;
    const double greenEnd = greenBegin + channelMs;
    const double blueBegin = greenEnd + porchMs;
    const double blueEnd = blueBegin + channelMs;
    const double lineMs = blueEnd + porchMs;

    return {
        name,
        visCode,
        640,
        496,
        SstvColorModel::Rgb,
        lineMs,
        syncMs,
        redBegin,
        redEnd,
        greenBegin,
        greenEnd,
        blueBegin,
        blueEnd,
        0.0,
        0.0
    };
}

constexpr SstvModeDefinition robotBwMode(const char* name,
                                         int visCode,
                                         int width,
                                         int height,
                                         double syncMs,
                                         double lineMs)
{
    const double grayBegin = syncMs;
    const double grayEnd = lineMs;

    return {
        name,
        visCode,
        width,
        height,
        SstvColorModel::Gray,
        lineMs,
        syncMs,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        grayBegin,
        grayEnd
    };
}

} // namespace detail

inline const std::array<SstvModeDefinition, 15>& allSstvModes()
{
    static const std::array<SstvModeDefinition, 15> kModes{{
        detail::martinMode("Martin M1", 44, 320, 256, 146.432),
        detail::martinMode("Martin M2", 40, 320, 256, 73.216),
        detail::martinMode("Martin M3", 36, 320, 128, 146.432),
        detail::martinMode("Martin M4", 32, 320, 128, 73.216),
        detail::scottieMode("Scottie S1", 60, 138.240),
        detail::scottieMode("Scottie S2", 56, 88.064),
        detail::scottieMode("Scottie DX", 76, 344.100),
        detail::robotBwMode("Robot 24 B/W", 10, 320, 240, 7.000, 93.000),
        detail::robotBwMode("Robot 12 B/W", 134, 160, 120, 7.000, 93.000),
        detail::robotBwMode("Robot 8 B/W", 2, 160, 120, 7.000, 59.900),
        detail::pasokonMode("Pasokon P3", 113, 5.208333, 1.041667, 133.333333),
        detail::pasokonMode("Pasokon P5", 114, 7.812500, 1.562500, 200.000000),
        detail::pasokonMode("Pasokon P7", 115, 10.416667, 2.083333, 266.666667),
        detail::wraaseMode("Wraase SC-2 120", 63, 1.000, 155.985, 155.985, 155.985, 0.500, 0.500),
        detail::wraaseMode("Wraase SC-2 180", 55, 0.500, 235.000, 235.000, 235.000),
    }};
    return kModes;
}

inline const SstvModeDefinition* findSstvModeByName(const QString& name)
{
    for (const auto& mode : allSstvModes()) {
        if (QString::fromLatin1(mode.name) == name)
            return &mode;
    }
    return nullptr;
}

inline const SstvModeDefinition* findSstvModeByVisCode(int visCode)
{
    for (const auto& mode : allSstvModes()) {
        if (mode.visCode == visCode)
            return &mode;
    }
    return nullptr;
}

} // namespace AetherSDR
