#pragma once

#include <QByteArray>
#include <QString>

#include <deque>

namespace AetherSDR {

class PskModulator {
public:
    void clearQueue();
    bool hasPendingData() const;
    int pendingSymbols() const;
    void queueText(const QString& text);
    QByteArray renderAudio(int frames);
    void reset();
    void setBaud(double baud);
    void setQpsk(bool on);
    void setTxOffsetHz(int hz);

private:
    double m_sampleRate{24000.0};
    double m_baud{31.25};
    bool m_qpsk{false};
    int m_txOffsetHz{2000};
    double m_samplesPerSymbol{768.0};

    std::deque<int> m_symbolQueue;
    double m_carrierPhase{0.0};
    double m_symbolPhase{0.0};
    double m_symbolProgress{0.0};
    double m_symbolStartPhase{0.0};
    double m_symbolDiff{0.0};
    bool m_symbolLoaded{false};
};

} // namespace AetherSDR
