#include "SstvModulator.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr float kAmplitude = 0.82f;

QImage prepareImage(const QImage& image, const SstvModeDefinition& mode)
{
    if (image.isNull())
        return {};

    return image.scaled(mode.width, mode.height,
                        Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);
}

} // namespace

bool SstvModulator::startTransmission(const QImage& image, const SstvModeDefinition& mode)
{
    clear();

    const QImage prepared = prepareImage(image, mode);
    if (prepared.isNull())
        return false;

    const QString name = QString::fromLatin1(mode.name);
    const double estimatedMs = 910.0 + (mode.lineMs * static_cast<double>(mode.height))
        + (name.startsWith(QStringLiteral("Scottie ")) ? mode.syncMs : 0.0);
    const qsizetype estimatedSamples =
        std::max<qsizetype>(1, static_cast<qsizetype>(std::llround(estimatedMs * m_sampleRate / 1000.0)));
    m_audio.reserve(static_cast<size_t>(estimatedSamples));
    m_activeModeName = name;

    appendVisHeader(mode.visCode);

    if (name.startsWith(QStringLiteral("Martin "))) {
        appendMartinImage(prepared, mode);
    } else if (name.startsWith(QStringLiteral("Scottie "))) {
        appendScottieImage(prepared, mode);
    } else if (name.startsWith(QStringLiteral("Pasokon "))) {
        appendPasokonImage(prepared, mode);
    } else if (name.startsWith(QStringLiteral("Wraase "))) {
        appendWraaseImage(prepared, mode);
    } else if (name.startsWith(QStringLiteral("Robot "))) {
        appendRobotBwImage(prepared, mode);
    } else {
        clear();
        return false;
    }

    return !m_audio.empty();
}

void SstvModulator::clear()
{
    m_phase = 0.0;
    m_activeModeName.clear();
    m_audio.clear();
    m_readPos = 0;
}

bool SstvModulator::hasPendingData() const
{
    return m_readPos < static_cast<qsizetype>(m_audio.size());
}

qsizetype SstvModulator::pendingFrames() const
{
    return std::max<qsizetype>(0, static_cast<qsizetype>(m_audio.size()) - m_readPos);
}

QByteArray SstvModulator::renderAudio(int frames)
{
    frames = std::max(frames, 1);
    QByteArray pcm(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());

    for (int i = 0; i < frames; ++i) {
        const float sample = hasPendingData() ? m_audio[static_cast<size_t>(m_readPos++)] : 0.0f;
        dst[i * 2] = sample;
        dst[i * 2 + 1] = sample;
    }

    return pcm;
}

int SstvModulator::msToSamples(double ms) const
{
    return std::max(1, static_cast<int>(std::llround(ms * m_sampleRate / 1000.0)));
}

void SstvModulator::appendTone(double frequencyHz, double durationMs)
{
    appendToneSamples(frequencyHz, msToSamples(durationMs));
}

void SstvModulator::appendToneSamples(double frequencyHz, int samples)
{
    if (samples <= 0)
        return;

    m_audio.reserve(m_audio.size() + static_cast<size_t>(samples));
    const double phaseInc = kTwoPi * frequencyHz / m_sampleRate;
    for (int i = 0; i < samples; ++i) {
        m_phase += phaseInc;
        if (m_phase >= kTwoPi)
            m_phase = std::fmod(m_phase, kTwoPi);
        m_audio.push_back(kAmplitude * static_cast<float>(std::sin(m_phase)));
    }
}

void SstvModulator::appendVisHeader(int visCode)
{
    appendTone(1900.0, 300.0);
    appendTone(1200.0, 10.0);
    appendTone(1900.0, 300.0);
    appendTone(1200.0, 30.0);

    int parity = 0;
    for (int bit = 0; bit < 7; ++bit) {
        const int value = (visCode >> bit) & 0x01;
        parity ^= value;
        appendTone(value ? 1100.0 : 1300.0, 30.0);
    }

    appendTone(parity ? 1100.0 : 1300.0, 30.0);
    appendTone(1200.0, 30.0);
}

void SstvModulator::appendMartinImage(const QImage& image, const SstvModeDefinition& mode)
{
    const double porchMs = mode.greenBeginMs;
    const double greenMs = mode.greenEndMs - mode.greenBeginMs;
    const double gap01Ms = mode.blueBeginMs - mode.greenEndMs;
    const double blueMs = mode.blueEndMs - mode.blueBeginMs;
    const double gap12Ms = mode.redBeginMs - mode.blueEndMs;
    const double redMs = mode.redEndMs - mode.redBeginMs;
    const double endGapMs = mode.lineMs - mode.redEndMs;

    for (int y = 0; y < image.height(); ++y) {
        appendTone(1200.0, mode.syncMs);
        appendTone(1500.0, porchMs);
        appendChannelScan(image, y, Channel::Green, greenMs);
        appendTone(1500.0, gap01Ms);
        appendChannelScan(image, y, Channel::Blue, blueMs);
        appendTone(1500.0, gap12Ms);
        appendChannelScan(image, y, Channel::Red, redMs);
        appendTone(1500.0, endGapMs);
    }
}

void SstvModulator::appendScottieImage(const QImage& image, const SstvModeDefinition& mode)
{
    const double sepMs = mode.redBeginMs;
    const double greenMs = mode.greenEndMs - mode.greenBeginMs;
    const double blueMs = mode.blueEndMs - mode.blueBeginMs;
    const double redMs = mode.redEndMs - mode.redBeginMs;

    for (int y = 0; y < image.height(); ++y) {
        if (y == 0)
            appendTone(1200.0, mode.syncMs);
        appendTone(1500.0, sepMs);
        appendChannelScan(image, y, Channel::Green, greenMs);
        appendTone(1500.0, sepMs);
        appendChannelScan(image, y, Channel::Blue, blueMs);
        appendTone(1200.0, mode.syncMs);
        appendTone(1500.0, sepMs);
        appendChannelScan(image, y, Channel::Red, redMs);
    }
}

void SstvModulator::appendPasokonImage(const QImage& image, const SstvModeDefinition& mode)
{
    const double porchMs = mode.redBeginMs;
    const double redMs = mode.redEndMs - mode.redBeginMs;
    const double gap01Ms = mode.greenBeginMs - mode.redEndMs;
    const double greenMs = mode.greenEndMs - mode.greenBeginMs;
    const double gap12Ms = mode.blueBeginMs - mode.greenEndMs;
    const double blueMs = mode.blueEndMs - mode.blueBeginMs;
    const double endGapMs = mode.lineMs - mode.blueEndMs;

    for (int y = 0; y < image.height(); ++y) {
        appendTone(1200.0, mode.syncMs);
        appendTone(1500.0, porchMs);
        appendChannelScan(image, y, Channel::Red, redMs);
        appendTone(1500.0, gap01Ms);
        appendChannelScan(image, y, Channel::Green, greenMs);
        appendTone(1500.0, gap12Ms);
        appendChannelScan(image, y, Channel::Blue, blueMs);
        appendTone(1500.0, endGapMs);
    }
}

void SstvModulator::appendWraaseImage(const QImage& image, const SstvModeDefinition& mode)
{
    const double porchMs = mode.redBeginMs;
    const double redMs = mode.redEndMs - mode.redBeginMs;
    const double gap01Ms = mode.greenBeginMs - mode.redEndMs;
    const double greenMs = mode.greenEndMs - mode.greenBeginMs;
    const double gap12Ms = mode.blueBeginMs - mode.greenEndMs;
    const double blueMs = mode.blueEndMs - mode.blueBeginMs;

    for (int y = 0; y < image.height(); ++y) {
        appendTone(1200.0, mode.syncMs);
        appendTone(1500.0, porchMs);
        appendChannelScan(image, y, Channel::Red, redMs);
        if (gap01Ms > 0.0)
            appendTone(1500.0, gap01Ms);
        appendChannelScan(image, y, Channel::Green, greenMs);
        if (gap12Ms > 0.0)
            appendTone(1500.0, gap12Ms);
        appendChannelScan(image, y, Channel::Blue, blueMs);
    }
}

void SstvModulator::appendRobotBwImage(const QImage& image, const SstvModeDefinition& mode)
{
    const double grayMs = mode.grayEndMs - mode.grayBeginMs;
    for (int y = 0; y < image.height(); ++y) {
        appendTone(1200.0, mode.syncMs);
        appendChannelScan(image, y, Channel::Gray, grayMs);
    }
}

void SstvModulator::appendChannelScan(const QImage& image,
                                      int y,
                                      Channel channel,
                                      double durationMs)
{
    const int samples = msToSamples(durationMs);
    if (samples <= 0)
        return;

    m_audio.reserve(m_audio.size() + static_cast<size_t>(samples));
    const int width = image.width();
    const int safeY = std::clamp(y, 0, image.height() - 1);

    for (int i = 0; i < samples; ++i) {
        const int x = std::clamp(
            static_cast<int>((static_cast<long long>(i) * width) / std::max(1, samples)),
            0, width - 1);
        const int level = channelLevel(image, x, safeY, channel);
        const double frequencyHz = 1500.0 + (800.0 * static_cast<double>(level) / 255.0);
        m_phase += kTwoPi * frequencyHz / m_sampleRate;
        if (m_phase >= kTwoPi)
            m_phase = std::fmod(m_phase, kTwoPi);
        m_audio.push_back(kAmplitude * static_cast<float>(std::sin(m_phase)));
    }
}

int SstvModulator::channelLevel(const QImage& image, int x, int y, Channel channel) const
{
    const QRgb pixel = image.pixel(x, y);
    switch (channel) {
    case Channel::Red:
        return qRed(pixel);
    case Channel::Green:
        return qGreen(pixel);
    case Channel::Blue:
        return qBlue(pixel);
    case Channel::Gray:
        return qGray(pixel);
    }
    return 0;
}

} // namespace AetherSDR
