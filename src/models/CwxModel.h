#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace AetherSDR {

class CwxModel : public QObject {
    Q_OBJECT
public:
    explicit CwxModel(QObject* parent = nullptr);

    // State
    int   speed()    const { return m_speed; }
    int   delay()    const { return m_delay; }
    bool  qskOn()    const { return m_qsk; }
    bool  isLive()   const { return m_live; }
    int   sentIndex() const { return m_sentIndex; }
    QString macro(int idx) const;  // 0-based (0=F1, 11=F12)

    // Actions — send/sendChar/sendMacro return the client block number
    // attached to the command so callers can match messageQueued().
    int  send(const QString& text);      // Send mode: full string
    int  sendChar(const QString& ch);    // Live mode: single char
    int  sendMacro(int idx);             // 1-based (1=F1, 12=F12)
    void saveMacro(int idx, const QString& text); // 0-based
    void erase(int numChars);
    void clearBuffer();
    void setSpeed(int wpm);
    void setDelay(int ms);
    void setQsk(bool on);
    void setLive(bool on);

    // Status parsing (from radio)
    void applyStatus(const QMap<QString, QString>& kvs);

    // Reply parsing for "cwx send" / "cwx macro send" (body is
    // "<char_index>,<block>") and "cwx clear" (body is the removed index
    // range "<start>,<stop>"). Called by RadioModel's reply callback.
    void applySendReply(int code, const QString& body);
    void applyClearReply(int code, const QString& body);

signals:
    void commandReady(const QString& cmd);
    void speedChanged(int wpm);
    void delayChanged(int ms);
    void qskChanged(bool on);
    void charSent(int index);           // radio buffer index just keyed
    void erased(int start, int stop);   // radio buffer indices [start, stop] removed
    // Radio accepted a send; radioIndex is the buffer index of the first
    // character of that block (FlexLib CWX.MessageQueued).
    void messageQueued(int block, int radioIndex);
    void macroChanged(int idx, const QString& text);  // 0-based
    void liveChanged(bool on);

private:
    int     m_speed{20};
    int     m_delay{5};
    bool    m_qsk{false};
    bool    m_live{false};
    int     m_sentIndex{-1};
    int     m_nextBlock{1};
    QString m_macros[12];
};

} // namespace AetherSDR
