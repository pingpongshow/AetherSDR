#include "PanadapterModel.h"
#include "core/LogManager.h"
#include <QDebug>
#include <QHostAddress>

namespace AetherSDR {

PanadapterModel::PanadapterModel(const QString& panId, QObject* parent)
    : QObject(parent)
    , m_panId(panId)
{}

quint32 PanadapterModel::panStreamId() const
{
    bool ok = false;
    quint32 id = m_panId.toUInt(&ok, 0);  // base 0: auto-detect 0x prefix
    return ok ? id : 0;
}

quint32 PanadapterModel::wfStreamId() const
{
    bool ok = false;
    quint32 id = m_waterfallId.toUInt(&ok, 0);  // base 0: auto-detect 0x prefix
    return ok ? id : 0;
}

void PanadapterModel::setWaterfallId(const QString& id)
{
    if (m_waterfallId != id) {
        m_waterfallId = id;
        emit waterfallIdChanged(id);
    }
}

void PanadapterModel::setClientHandle(const QString& h)
{
    m_clientHandle = h;
}

void PanadapterModel::setRfGainInfo(int low, int high, int step)
{
    m_rfGainLow = low;
    m_rfGainHigh = high;
    m_rfGainStep = step;
    emit rfGainInfoChanged(low, high, step);
}

void PanadapterModel::applyPanStatus(const QMap<QString, QString>& kvs)
{
    bool infoChanged = false;
    bool levelChanged = false;

    if (kvs.contains("center")) {
        double c = kvs["center"].toDouble();
        if (c != m_centerMhz) { m_centerMhz = c; infoChanged = true; }
    }
    if (kvs.contains("bandwidth")) {
        double b = kvs["bandwidth"].toDouble();
        if (b != m_bandwidthMhz) { m_bandwidthMhz = b; infoChanged = true; }
    }
    if (kvs.contains("min_dbm")) {
        float v = kvs["min_dbm"].toFloat();
        if (v != m_minDbm) { m_minDbm = v; levelChanged = true; }
    }
    if (kvs.contains("max_dbm")) {
        float v = kvs["max_dbm"].toFloat();
        if (v != m_maxDbm) { m_maxDbm = v; levelChanged = true; }
    }
    if (kvs.contains("rfgain")) {
        int g = kvs["rfgain"].toInt();
        if (g != m_rfGain) {
            m_rfGain = g;
            emit rfGainChanged(m_rfGain);
        }
    }
    if (kvs.contains("pre")) {
        QString pre = kvs["pre"];
        if (pre != m_preamp) {
            // Preamp is internal state only — no UI listeners, no emit.
            m_preamp = pre;
        }
    }
    if (kvs.contains("wnb")) {
        bool w = kvs["wnb"].toInt() != 0;
        int lvl = kvs.value("wnb_level", QString::number(m_wnbLevel)).toInt();
        if (w != m_wnbActive || lvl != m_wnbLevel) {
            m_wnbActive = w;
            m_wnbLevel = lvl;
            emit wnbChanged(m_wnbActive, m_wnbLevel);
        }
    }
    if (kvs.contains("wide")) {
        bool wide = kvs["wide"].toInt() != 0;
        if (wide != m_wideActive) {
            m_wideActive = wide;
            emit wideChanged(m_wideActive);
        }
    }
    if (kvs.contains("ant_list")) {
        QStringList ants = kvs["ant_list"].split(',', Qt::SkipEmptyParts);
        if (ants != m_antList) {
            m_antList = ants;
            emit antListChanged(m_antList);
        }
    }
    if (kvs.contains("waterfall")) {
        setWaterfallId(kvs["waterfall"]);
    }
    if (kvs.contains("daxiq_channel")) {
        int ch = kvs["daxiq_channel"].toInt();
        if (ch != m_daxiqChannel) {
            m_daxiqChannel = ch;
            emit daxiqChannelChanged(ch);
        }
    }

    if (kvs.contains("loopa") || kvs.contains("loopb")) {
        bool a = kvs.contains("loopa") ? kvs["loopa"].toInt() != 0 : m_loopA;
        bool b = kvs.contains("loopb") ? kvs["loopb"].toInt() != 0 : m_loopB;
        if (a != m_loopA || b != m_loopB) {
            m_loopA = a;
            m_loopB = b;
            emit loopsChanged(m_loopA, m_loopB);
        }
    }
    // n1mm_* keys are not reported by fw 4.1.5; parse them in case a later
    // firmware starts echoing them.
    {
        bool n1mmChanged = false;
        if (kvs.contains("n1mm_spectrum_enable")) {
            bool on = kvs["n1mm_spectrum_enable"].toInt() != 0;
            if (on != m_n1mmEnabled) { m_n1mmEnabled = on; n1mmChanged = true; }
        }
        if (kvs.contains("n1mm_address")) {
            const QString addr = kvs["n1mm_address"];
            if (!addr.isEmpty() && addr != m_n1mmAddress) {
                m_n1mmAddress = addr;
                n1mmChanged = true;
            }
        }
        if (kvs.contains("n1mm_port")) {
            bool ok = false;
            uint port = kvs["n1mm_port"].toUInt(&ok);
            if (ok && port > 0 && port <= 65535 && port != m_n1mmPort) {
                m_n1mmPort = static_cast<quint16>(port);
                n1mmChanged = true;
            }
        }
        if (kvs.contains("n1mm_radio")) {
            int radio = kvs["n1mm_radio"].toInt();
            if ((radio == 1 || radio == 2) && radio != m_n1mmRadio) {
                m_n1mmRadio = radio;
                n1mmChanged = true;
            }
        }
        if (n1mmChanged) {
            emitN1mmChanged();
        }
    }

    if (infoChanged)
        emit this->infoChanged(m_centerMhz, m_bandwidthMhz);
    if (levelChanged)
        emit this->levelChanged(m_minDbm, m_maxDbm);
}

void PanadapterModel::sendPanSet(const QString& keyValue)
{
    emit commandReady(QStringLiteral("display pan set %1 %2").arg(m_panId, keyValue));
}

void PanadapterModel::emitN1mmChanged()
{
    emit n1mmSpectrumChanged(m_n1mmEnabled, m_n1mmAddress, m_n1mmPort, m_n1mmRadio);
}

void PanadapterModel::setLoopA(bool on)
{
    // No optimistic update: the radio echoes loopa on pan + slices.
    sendPanSet(QStringLiteral("loopa=%1").arg(on ? 1 : 0));
}

void PanadapterModel::setLoopB(bool on)
{
    sendPanSet(QStringLiteral("loopb=%1").arg(on ? 1 : 0));
}

void PanadapterModel::setN1mmSpectrumEnabled(bool on)
{
    // No equality guard on n1mm_* setters: fw 4.1.5 never reports these keys,
    // so the local value may not match what the radio actually holds.
    m_n1mmEnabled = on;
    sendPanSet(QStringLiteral("n1mm_spectrum_enable=%1").arg(on ? 1 : 0));
    emitN1mmChanged();
}

void PanadapterModel::setN1mmAddress(const QString& address)
{
    const QString trimmed = address.trimmed();
    QHostAddress host;
    if (!host.setAddress(trimmed) || host.protocol() != QAbstractSocket::IPv4Protocol) {
        qCWarning(lcProtocol) << "PanadapterModel: rejecting invalid n1mm_address" << address;
        return;
    }
    const QString normalized = host.toString();
    m_n1mmAddress = normalized;
    sendPanSet(QStringLiteral("n1mm_address=%1").arg(normalized));
    emitN1mmChanged();
}

void PanadapterModel::setN1mmPort(quint16 port)
{
    if (port == 0) {
        return;
    }
    m_n1mmPort = port;
    sendPanSet(QStringLiteral("n1mm_port=%1").arg(port));
    emitN1mmChanged();
}

void PanadapterModel::setN1mmRadio(int radio)
{
    if (radio != 1 && radio != 2) {
        return;
    }
    m_n1mmRadio = radio;
    sendPanSet(QStringLiteral("n1mm_radio=%1").arg(radio));
    emitN1mmChanged();
}

void PanadapterModel::applyWaterfallStatus(const QMap<QString, QString>& kvs)
{
    // Waterfall status shares center/bandwidth with pan — sync if present
    if (kvs.contains("center") || kvs.contains("bandwidth")) {
        bool changed = false;
        if (kvs.contains("center")) {
            double c = kvs["center"].toDouble();
            if (c != m_centerMhz) { m_centerMhz = c; changed = true; }
        }
        if (kvs.contains("bandwidth")) {
            double b = kvs["bandwidth"].toDouble();
            if (b != m_bandwidthMhz) { m_bandwidthMhz = b; changed = true; }
        }
        if (changed)
            emit infoChanged(m_centerMhz, m_bandwidthMhz);
    }
}

} // namespace AetherSDR
