#pragma once

#include <QByteArray>
#include <QString>

#include <deque>

namespace AetherSDR {

class RttyModulator {
public:
    double baud() const { return m_baud; }
    int markHz() const { return m_markHz; }
    int shiftHz() const { return m_shiftHz; }
    bool inverted() const { return m_inverted; }
    bool unshiftOnSpace() const { return m_unshiftOnSpace; }

    void clearQueue();
    bool hasPendingData() const;
    int pendingBits() const;
    QByteArray renderAudio(int frames);
    void resetStreamState();
    void queueText(const QString& text);
    void setBaud(double baud);
    void setInverted(bool on);
    void setMarkHz(int hz);
    void setShiftHz(int hz);
    void setUnshiftOnSpace(bool on);

private:
    void appendCode(quint8 code);
    void appendPrintableCharacter(QChar ch);

    double m_sampleRate{24000.0};
    double m_baud{45.45};
    double m_samplesPerBit{528.0528};
    int m_markHz{2125};
    int m_shiftHz{170};
    bool m_inverted{false};
    bool m_unshiftOnSpace{true};
    bool m_lettersMode{true};

    std::deque<int> m_bitQueue;

    double m_phase{0.0};
    double m_bitSamplePos{0.0};
    int m_currentBit{1};
    bool m_currentBitFromQueue{false};
};

} // namespace AetherSDR
