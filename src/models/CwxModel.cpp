#include "CwxModel.h"
#include <QDebug>
#include <QMap>

namespace AetherSDR {

CwxModel::CwxModel(QObject* parent)
    : QObject(parent)
{}

QString CwxModel::macro(int idx) const
{
    if (idx < 0 || idx >= 12) return {};
    return m_macros[idx];
}

int CwxModel::send(const QString& text)
{
    if (text.isEmpty()) {
        return -1;
    }
    // Encode spaces as DEL (0x7f) per FlexLib protocol
    QString encoded = text;
    encoded.replace(' ', QChar(0x7f));
    const int block = m_nextBlock++;
    emit commandReady(QString("cwx send \"%1\" %2").arg(encoded).arg(block));
    return block;
}

int CwxModel::sendChar(const QString& ch)
{
    if (ch.isEmpty()) {
        return -1;
    }
    QString encoded = ch;
    encoded.replace(' ', QChar(0x7f));
    const int block = m_nextBlock++;
    emit commandReady(QString("cwx send \"%1\" %2").arg(encoded).arg(block));
    return block;
}

int CwxModel::sendMacro(int idx)
{
    if (idx < 1 || idx > 12) {
        return -1;
    }
    // "cwx macro send <#> [<block>]" — optional block echoed in the reply
    // like "cwx send" (SmartSDR API v1.4 / FlexLib CWX.SendMacro).
    const int block = m_nextBlock++;
    emit commandReady(QString("cwx macro send %1 %2").arg(idx).arg(block));
    return block;
}

void CwxModel::applySendReply(int code, const QString& body)
{
    if (code != 0) {
        return;
    }
    // Reply body: "<character_index>" or "<character_index>,<block>" where
    // character_index is the radio buffer index of the first queued char
    // (SmartSDR API v1.4.0.0 CWX docs; FlexLib CWX.MessageQueued).
    const QStringList parts = body.trimmed().split(',');
    bool ok = false;
    const int index = parts.value(0).toInt(&ok);
    if (!ok) {
        return;
    }
    int block = -1;
    if (parts.size() >= 2) {
        bool okBlock = false;
        const int b = parts[1].toInt(&okBlock);
        if (okBlock) {
            block = b;
        }
    }
    emit messageQueued(block, index);
}

void CwxModel::applyClearReply(int code, const QString& body)
{
    if (code != 0) {
        return;
    }
    // "cwx clear" reply reports the removed character index range.
    const QStringList parts = body.trimmed().split(',');
    if (parts.size() != 2) {
        return;
    }
    bool ok1 = false;
    bool ok2 = false;
    const int start = parts[0].toInt(&ok1);
    const int stop  = parts[1].toInt(&ok2);
    if (ok1 && ok2 && start >= 0 && stop >= start) {
        emit erased(start, stop);
    }
}

void CwxModel::saveMacro(int idx, const QString& text)
{
    if (idx < 0 || idx >= 12) return;
    m_macros[idx] = text;
    QString encoded = text;
    encoded.replace(' ', QChar(0x7f));
    emit commandReady(QString("cwx macro save %1 \"%2\"").arg(idx + 1).arg(encoded));
}

void CwxModel::erase(int numChars)
{
    emit commandReady(QString("cwx erase %1").arg(numChars));
}

void CwxModel::clearBuffer()
{
    emit commandReady("cwx clear");
}

void CwxModel::setSpeed(int wpm)
{
    wpm = qBound(5, wpm, 100);
    if (wpm != m_speed) {
        m_speed = wpm;
        emit commandReady(QString("cwx wpm %1").arg(m_speed));
        emit speedChanged(m_speed);
    }
}

void CwxModel::setDelay(int ms)
{
    ms = qBound(0, ms, 2000);
    if (ms != m_delay) {
        m_delay = ms;
        emit commandReady(QString("cwx delay %1").arg(m_delay));
        emit delayChanged(m_delay);
    }
}

void CwxModel::setQsk(bool on)
{
    if (on != m_qsk) {
        m_qsk = on;
        emit commandReady(QString("cwx qsk_enabled %1").arg(m_qsk ? 1 : 0));
        emit qskChanged(m_qsk);
    }
}

void CwxModel::setLive(bool on)
{
    if (on != m_live) {
        m_live = on;
        emit liveChanged(m_live);
    }
}

void CwxModel::applyStatus(const QMap<QString, QString>& kvs)
{
    // Erase status: documented as "erase_start=<start> erase_stop=<stop>"
    // (SmartSDR API v1.4 CWX docs); older builds used "erase=<start>,<stop>",
    // which is handled in the loop below.
    if (kvs.contains("erase_start") && kvs.contains("erase_stop")) {
        bool ok1 = false;
        bool ok2 = false;
        const int start = kvs.value("erase_start").toInt(&ok1);
        const int stop  = kvs.value("erase_stop").toInt(&ok2);
        if (ok1 && ok2) {
            emit erased(start, stop);
        }
    }

    for (auto it = kvs.cbegin(); it != kvs.cend(); ++it) {
        const QString& key = it.key();
        const QString& val = it.value();

        if (key == "sent") {
            bool ok;
            int idx = val.toInt(&ok);
            if (ok) {
                m_sentIndex = idx;
                emit charSent(idx);
            }
        } else if (key == "wpm") {
            bool ok;
            int v = val.toInt(&ok);
            if (ok && v != m_speed) {
                m_speed = v;
                emit speedChanged(m_speed);
            }
        } else if (key == "break_in_delay") {
            bool ok;
            int v = val.toInt(&ok);
            if (ok && v != m_delay) {
                m_delay = v;
                emit delayChanged(m_delay);
            }
        } else if (key == "qsk_enabled") {
            bool on = (val == "1");
            if (on != m_qsk) {
                m_qsk = on;
                emit qskChanged(m_qsk);
            }
        } else if (key == "erase") {
            QStringList parts = val.split(',');
            if (parts.size() == 2) {
                bool ok1, ok2;
                int start = parts[0].toInt(&ok1);
                int stop  = parts[1].toInt(&ok2);
                if (ok1 && ok2) emit erased(start, stop);
            }
        } else if (key.startsWith("macro") && key.length() > 5) {
            bool ok;
            int idx = key.mid(5).toInt(&ok);
            if (ok && idx >= 1 && idx <= 12) {
                // Decode: strip quotes, \u007f → space, * → =
                QString decoded = val;
                if (decoded.startsWith('"') && decoded.endsWith('"'))
                    decoded = decoded.mid(1, decoded.length() - 2);
                decoded.replace(QChar(0x7f), ' ');
                decoded.replace('*', '=');
                m_macros[idx - 1] = decoded;
                emit macroChanged(idx - 1, decoded);
            }
        }
    }
}

} // namespace AetherSDR
