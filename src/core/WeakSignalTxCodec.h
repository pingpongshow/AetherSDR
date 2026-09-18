#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR {

class WeakSignalTxCodec {
public:
    enum class Mode {
        FT8 = 0,
        FT4 = 1,
        JT65 = 2,
        WSPR = 3
    };

    struct Request {
        Mode mode{Mode::FT8};
        QString message;
        QString callsign;
        QString grid;
        int powerDbm{30};
        double toneHz{1500.0};
        int sampleRate{24000};
    };

    struct Waveform {
        QByteArray pcm24kStereo;
        QString normalizedMessage;
        int durationMs{0};
        double toneHz{0.0};
    };

    static bool buildWaveform(const Request& request,
                              Waveform* waveform,
                              QString* errorMessage = nullptr);

    static int slotDurationMs(Mode mode);
};

} // namespace AetherSDR
