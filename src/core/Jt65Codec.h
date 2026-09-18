#pragma once

#include <QString>
#include <QVector>

#include <vector>

namespace AetherSDR {

class Jt65Codec {
public:
    struct DecodeResult {
        QString message;
        int toneHz{0};
        int snr{0};
        double deltaTimeSec{0.0};
        bool lowConfidence{false};
    };

    static bool encodeMessage(const QString& message,
                              std::vector<int>* tones,
                              QString* normalizedMessage,
                              QString* errorMessage = nullptr);

    static QVector<DecodeResult> decodeSlot(const std::vector<float>& samples12k,
                                            QString* statusMessage = nullptr);
};

} // namespace AetherSDR
