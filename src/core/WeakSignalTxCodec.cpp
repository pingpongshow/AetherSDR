#include "WeakSignalTxCodec.h"

#include "Jt65Codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include <ft8/constants.h>
#include <ft8/encode.h>
#include <ft8/message.h>

void aethersdr_wspr_encode(const char message[23], int tones[162], char normalized[23], int* rc);
}

namespace AetherSDR {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kFt8SymbolBt = 2.0;
constexpr double kFt4SymbolBt = 1.0;
constexpr double kGfskConstK = 5.336446;
constexpr double kJt65ToneSpacingHz = 11025.0 / 4096.0;
constexpr double kJt65SymbolPeriodSec = 4096.0 / 11025.0;
constexpr double kJt65LeadSilenceSec = 1.0;
constexpr double kWsprToneSpacingHz = 12000.0 / 8192.0;
constexpr double kWsprSymbolPeriodSec = 8192.0 / 12000.0;
constexpr float kAmplitude = 0.45f;

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

void applyRaisedCosineRamp(std::vector<float>& mono, int sampleRate)
{
    if (mono.empty() || sampleRate <= 0)
        return;

    const int rampSamples = std::min<int>(mono.size() / 2, std::max(16, sampleRate / 50));
    for (int i = 0; i < rampSamples; ++i) {
        const float env = static_cast<float>((1.0 - std::cos(kPi * static_cast<double>(i) / rampSamples)) * 0.5);
        mono[static_cast<std::size_t>(i)] *= env;
        mono[mono.size() - 1 - static_cast<std::size_t>(i)] *= env;
    }
}

QByteArray monoToStereoPcm(const std::vector<float>& mono)
{
    QByteArray pcm(static_cast<int>(mono.size() * 2 * sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(pcm.data());
    for (std::size_t i = 0; i < mono.size(); ++i) {
        const float sample = mono[i];
        dst[i * 2] = sample;
        dst[i * 2 + 1] = sample;
    }
    return pcm;
}

void gfskPulse(int samplesPerSymbol, double symbolBt, std::vector<float>* pulse)
{
    pulse->assign(static_cast<std::size_t>(samplesPerSymbol * 3), 0.0f);
    for (int i = 0; i < samplesPerSymbol * 3; ++i) {
        const double t = static_cast<double>(i) / samplesPerSymbol - 1.5;
        const double arg1 = kGfskConstK * symbolBt * (t + 0.5);
        const double arg2 = kGfskConstK * symbolBt * (t - 0.5);
        (*pulse)[static_cast<std::size_t>(i)] = static_cast<float>((std::erf(arg1) - std::erf(arg2)) * 0.5);
    }
}

std::vector<float> synthGfsk(const uint8_t* tones,
                             int toneCount,
                             double baseHz,
                             double symbolBt,
                             double symbolPeriodSec,
                             int sampleRate,
                             int totalSamples)
{
    std::vector<float> mono(static_cast<std::size_t>(totalSamples), 0.0f);
    const int samplesPerSymbol = static_cast<int>(std::lround(sampleRate * symbolPeriodSec));
    const int signalSamples = samplesPerSymbol * toneCount;
    if (samplesPerSymbol <= 0 || signalSamples <= 0 || totalSamples <= 0)
        return mono;

    const int silenceSamples = std::max(0, (totalSamples - signalSamples) / 2);
    const double dphiPeak = kTwoPi / samplesPerSymbol;
    std::vector<double> dphi(static_cast<std::size_t>(signalSamples + 2 * samplesPerSymbol),
                             kTwoPi * baseHz / sampleRate);
    std::vector<float> pulse;
    gfskPulse(samplesPerSymbol, symbolBt, &pulse);

    for (int symbol = 0; symbol < toneCount; ++symbol) {
        const int offset = symbol * samplesPerSymbol;
        for (int j = 0; j < samplesPerSymbol * 3; ++j)
            dphi[static_cast<std::size_t>(offset + j)] += dphiPeak * tones[symbol] * pulse[static_cast<std::size_t>(j)];
    }

    for (int j = 0; j < samplesPerSymbol * 2; ++j) {
        dphi[static_cast<std::size_t>(j)] += dphiPeak * pulse[static_cast<std::size_t>(j + samplesPerSymbol)] * tones[0];
        dphi[static_cast<std::size_t>(j + signalSamples)] += dphiPeak * pulse[static_cast<std::size_t>(j)] * tones[toneCount - 1];
    }

    double phase = 0.0;
    for (int i = 0; i < signalSamples; ++i) {
        mono[static_cast<std::size_t>(silenceSamples + i)] = static_cast<float>(std::sin(phase) * kAmplitude);
        phase = std::fmod(phase + dphi[static_cast<std::size_t>(i + samplesPerSymbol)], kTwoPi);
    }

    applyRaisedCosineRamp(mono, sampleRate);
    return mono;
}

std::vector<float> synthPiecewiseFsk(const std::vector<int>& tones,
                                     double centerHz,
                                     double spacingHz,
                                     double toneOffset,
                                     double symbolPeriodSec,
                                     double leadSilenceSec,
                                     int sampleRate,
                                     int totalSamples)
{
    std::vector<float> mono(static_cast<std::size_t>(std::max(totalSamples, 0)), 0.0f);
    if (tones.empty() || sampleRate <= 0 || totalSamples <= 0)
        return mono;

    double phase = 0.0;
    for (std::size_t i = 0; i < tones.size(); ++i) {
        const int startSample = std::clamp<int>(
            static_cast<int>(std::lround((leadSilenceSec + i * symbolPeriodSec) * sampleRate)),
            0, totalSamples);
        const int endSample = std::clamp<int>(
            static_cast<int>(std::lround((leadSilenceSec + (i + 1) * symbolPeriodSec) * sampleRate)),
            0, totalSamples);
        if (endSample <= startSample)
            continue;

        const double frequencyHz = centerHz + (tones[i] + toneOffset) * spacingHz;
        const double phaseInc = kTwoPi * frequencyHz / sampleRate;
        for (int sample = startSample; sample < endSample; ++sample) {
            mono[static_cast<std::size_t>(sample)] = static_cast<float>(std::sin(phase) * kAmplitude);
            phase = std::fmod(phase + phaseInc, kTwoPi);
        }
    }

    applyRaisedCosineRamp(mono, sampleRate);
    return mono;
}

int normalizeWsprPower(int powerDbm)
{
    static constexpr std::array<int, 28> kValidPowers{
        -30, -27, -23, -20, -17, -13, -10, -7, -3,
        0, 3, 7, 10, 13, 17, 20, 23, 27, 30, 33,
        37, 40, 43, 47, 50, 53, 57, 60
    };

    int normalized = kValidPowers.front();
    for (const int candidate : kValidPowers) {
        if (powerDbm >= candidate)
            normalized = candidate;
    }
    return normalized;
}

bool buildFt8Waveform(const WeakSignalTxCodec::Request& request,
                      WeakSignalTxCodec::Waveform* waveform,
                      QString* errorMessage)
{
    const QString text = normalizeText(request.message);
    if (text.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Enter a message to transmit.");
        return false;
    }

    ftx_message_t message{};
    ftx_message_init(&message);
    const QByteArray latin = text.toLatin1();
    const ftx_message_rc_t rc = ftx_message_encode(&message, nullptr, latin.constData());
    if (rc != FTX_MESSAGE_RC_OK) {
        if (errorMessage)
            *errorMessage = QStringLiteral("FT8/FT4 could not encode that message.");
        return false;
    }

    const bool isFt4 = request.mode == WeakSignalTxCodec::Mode::FT4;
    const int toneCount = isFt4 ? FT4_NN : FT8_NN;
    std::vector<uint8_t> tones(static_cast<std::size_t>(toneCount), 0);
    if (isFt4)
        ft4_encode(message.payload, tones.data());
    else
        ft8_encode(message.payload, tones.data());

    const int totalSamples = static_cast<int>(std::lround(request.sampleRate * (isFt4 ? FT4_SLOT_TIME : FT8_SLOT_TIME)));
    const std::vector<float> mono = synthGfsk(tones.data(), toneCount,
                                              std::clamp(request.toneHz, 0.0, 4000.0),
                                              isFt4 ? kFt4SymbolBt : kFt8SymbolBt,
                                              isFt4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD,
                                              request.sampleRate,
                                              totalSamples);
    waveform->pcm24kStereo = monoToStereoPcm(mono);
    waveform->normalizedMessage = text;
    waveform->durationMs = WeakSignalTxCodec::slotDurationMs(request.mode);
    waveform->toneHz = request.toneHz;
    return true;
}

bool buildJt65Waveform(const WeakSignalTxCodec::Request& request,
                       WeakSignalTxCodec::Waveform* waveform,
                       QString* errorMessage)
{
    const QString text = normalizeText(request.message).left(22);
    std::vector<int> tones;
    QString normalizedMessage;
    if (!Jt65Codec::encodeMessage(text, &tones, &normalizedMessage, errorMessage))
        return false;
    if (tones.size() != 126) {
        if (errorMessage)
            *errorMessage = QStringLiteral("JT65 tone generation returned an invalid symbol count.");
        return false;
    }
    const int totalSamples = request.sampleRate * 60;
    const std::vector<float> mono = synthPiecewiseFsk(tones,
                                                      std::clamp(request.toneHz, 0.0, 4000.0),
                                                      kJt65ToneSpacingHz,
                                                      0.0,
                                                      kJt65SymbolPeriodSec,
                                                      kJt65LeadSilenceSec,
                                                      request.sampleRate,
                                                      totalSamples);

    waveform->pcm24kStereo = monoToStereoPcm(mono);
    waveform->normalizedMessage = normalizedMessage.isEmpty() ? text : normalizedMessage;
    waveform->durationMs = 60000;
    waveform->toneHz = request.toneHz;
    return true;
}

bool buildWsprWaveform(const WeakSignalTxCodec::Request& request,
                       WeakSignalTxCodec::Waveform* waveform,
                       QString* errorMessage)
{
    const QString callsign = normalizeText(request.callsign).left(12);
    const QString grid = normalizeText(request.grid).left(4);
    if (callsign.isEmpty() || grid.size() < 4) {
        if (errorMessage)
            *errorMessage = QStringLiteral("WSPR needs a station callsign and a 4-character grid.");
        return false;
    }

    const int normalizedPower = normalizeWsprPower(request.powerDbm);
    const QString message = QStringLiteral("%1 %2 %3")
                                .arg(callsign, grid, QString::number(normalizedPower));
    const std::array<char, 23> input = makeFixedMessage(message);
    std::array<char, 23> normalized{};
    normalized.fill('\0');
    int rc = -1;
    int wsprTones[162]{};
    aethersdr_wspr_encode(input.data(), wsprTones, normalized.data(), &rc);
    if (rc != 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("WSPR encoding failed.");
        return false;
    }

    std::vector<int> tones(std::begin(wsprTones), std::end(wsprTones));
    const int totalSamples = request.sampleRate * 120;
    const std::vector<float> mono = synthPiecewiseFsk(tones,
                                                      std::clamp(request.toneHz, 0.0, 4000.0),
                                                      kWsprToneSpacingHz,
                                                      -1.5,
                                                      kWsprSymbolPeriodSec,
                                                      0.0,
                                                      request.sampleRate,
                                                      totalSamples);

    waveform->pcm24kStereo = monoToStereoPcm(mono);
    waveform->normalizedMessage = fromFixedMessage(normalized.data(), 22);
    if (waveform->normalizedMessage.isEmpty())
        waveform->normalizedMessage = message;
    waveform->durationMs = 120000;
    waveform->toneHz = request.toneHz;
    return true;
}

} // namespace

bool WeakSignalTxCodec::buildWaveform(const Request& request,
                                      Waveform* waveform,
                                      QString* errorMessage)
{
    if (!waveform)
        return false;

    waveform->pcm24kStereo.clear();
    waveform->normalizedMessage.clear();
    waveform->durationMs = 0;
    waveform->toneHz = std::clamp(request.toneHz, 0.0, 4000.0);

    switch (request.mode) {
    case Mode::FT4:
    case Mode::FT8:
        return buildFt8Waveform(request, waveform, errorMessage);
    case Mode::JT65:
        return buildJt65Waveform(request, waveform, errorMessage);
    case Mode::WSPR:
        return buildWsprWaveform(request, waveform, errorMessage);
    }

    if (errorMessage)
        *errorMessage = QStringLiteral("Unsupported weak-signal mode.");
    return false;
}

int WeakSignalTxCodec::slotDurationMs(Mode mode)
{
    switch (mode) {
    case Mode::FT4:
        return 7500;
    case Mode::JT65:
        return 60000;
    case Mode::WSPR:
        return 120000;
    case Mode::FT8:
    default:
        return 15000;
    }
}

} // namespace AetherSDR
