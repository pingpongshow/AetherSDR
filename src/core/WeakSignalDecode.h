#pragma once

#include <QTime>

namespace AetherSDR {

struct WeakSignalDecode {
    QString instanceId;
    bool isNew{false};
    QTime utcTime;
    int snr{0};
    double deltaTimeSec{0.0};
    quint32 deltaFrequencyHz{0};
    quint64 frequencyHz{0};
    QString mode;
    QString message;
    bool lowConfidence{false};
    bool offAir{false};
    QString callsign;
    QString grid;
    int driftHz{0};
    int powerDbm{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::WeakSignalDecode)
