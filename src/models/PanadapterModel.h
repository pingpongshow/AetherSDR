#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Per-panadapter state model. Replaces the single-pan fields that were
// previously in RadioModel. Each PanadapterModel represents one FFT/waterfall
// display on the radio, identified by its hex pan ID (e.g. "0x40000000").
class PanadapterModel : public QObject {
    Q_OBJECT

public:
    explicit PanadapterModel(const QString& panId, QObject* parent = nullptr);

    // Identity
    QString panId() const { return m_panId; }
    quint32 panStreamId() const;   // numeric form of panId for VITA-49 matching
    QString waterfallId() const { return m_waterfallId; }
    quint32 wfStreamId() const;    // numeric form of waterfallId
    void setWaterfallId(const QString& id);
    QString clientHandle() const { return m_clientHandle; }
    void setClientHandle(const QString& h);

    // Display state
    double centerMhz() const { return m_centerMhz; }
    double bandwidthMhz() const { return m_bandwidthMhz; }
    float minDbm() const { return m_minDbm; }
    float maxDbm() const { return m_maxDbm; }
    QStringList antList() const { return m_antList; }
    int rfGain() const { return m_rfGain; }
    int rfGainLow() const { return m_rfGainLow; }
    int rfGainHigh() const { return m_rfGainHigh; }
    int rfGainStep() const { return m_rfGainStep; }
    void setRfGainInfo(int low, int high, int step);
    bool wnbActive() const { return m_wnbActive; }
    int wnbLevel() const { return m_wnbLevel; }
    bool wideActive() const { return m_wideActive; }
    QString preamp() const { return m_preamp; }
    void setPreamp(const QString& pre) {
        // Preamp is internal state only — no UI listeners. Do not emit
        // rfGainChanged here; doing so caused a ~33ms echo loop because the
        // gain display listener would re-assert the current rfgain back to
        // the radio on every status cycle (#1498).
        if (m_preamp != pre) { m_preamp = pre; }
    }
    int daxiqChannel() const { return m_daxiqChannel; }

    // RX loops (display pan loopa=/loopb=). Radio-authoritative: setters only
    // send the command; state updates when the status echo arrives. Verified
    // on FLEX-6700 fw 4.1.5 (protocol v1.4.0.0): "display pan set <pan> loopa=1"
    // echoes loopa on the pan, its waterfall AND every slice on that pan
    // (slice set <n> loopa= is an equivalent alias, FlexLib Slice.cs).
    bool loopA() const { return m_loopA; }
    bool loopB() const { return m_loopB; }
    void setLoopA(bool on);
    void setLoopB(bool on);

    // N1MM+ spectrum forwarding (FlexLib Panadapter.LoggerDisplay*). Stored
    // radio-side per pan — never persist client-side. FlexLib does not parse
    // n1mm_* status keys and fw 4.1.5 does not include them in pan status, so
    // setters update the model optimistically (breaks Multi-Flex sync).
    static constexpr quint16 kN1mmDefaultPort = 13064;  // N1MM+ spectrum listen port
    bool n1mmSpectrumEnabled() const { return m_n1mmEnabled; }
    QString n1mmAddress() const { return m_n1mmAddress; }
    quint16 n1mmPort() const { return m_n1mmPort; }
    int n1mmRadio() const { return m_n1mmRadio; }
    void setN1mmSpectrumEnabled(bool on);
    void setN1mmAddress(const QString& address);  // IPv4 dotted quad
    void setN1mmPort(quint16 port);
    void setN1mmRadio(int radio);                 // 1 or 2

    // Configuration flags
    bool isResized() const { return m_resized; }
    void setResized(bool r) { m_resized = r; }
    bool isWaterfallConfigured() const { return m_wfConfigured; }
    void setWaterfallConfigured(bool c) { m_wfConfigured = c; }

    // Apply status from protocol
    void applyPanStatus(const QMap<QString, QString>& kvs);
    void applyWaterfallStatus(const QMap<QString, QString>& kvs);

signals:
    void infoChanged(double centerMhz, double bandwidthMhz);
    void levelChanged(float minDbm, float maxDbm);
    void antListChanged(const QStringList& ants);
    void rfGainChanged(int gain);
    void rfGainInfoChanged(int low, int high, int step);
    void wnbChanged(bool active, int level);
    void wideChanged(bool active);
    void waterfallIdChanged(const QString& wfId);
    void daxiqChannelChanged(int channel);
    void loopsChanged(bool loopA, bool loopB);
    void n1mmSpectrumChanged(bool enabled, const QString& address,
                             quint16 port, int radio);
    // Protocol command for RadioModel::sendCommand (wired by MainWindow).
    void commandReady(const QString& cmd);

private:
    QString     m_panId;
    QString     m_waterfallId;
    QString     m_clientHandle;
    double      m_centerMhz{14.1};
    double      m_bandwidthMhz{0.2};
    float       m_minDbm{-130.0f};
    float       m_maxDbm{-40.0f};
    QStringList m_antList;
    int         m_rfGain{0};
    int         m_rfGainLow{-8};
    int         m_rfGainHigh{32};
    int         m_rfGainStep{8};
    bool        m_wnbActive{false};
    bool        m_wideActive{false};
    int         m_wnbLevel{50};
    QString     m_preamp;
    int         m_daxiqChannel{0};
    bool        m_loopA{false};
    bool        m_loopB{false};
    bool        m_n1mmEnabled{false};
    QString     m_n1mmAddress{QStringLiteral("127.0.0.1")};
    quint16     m_n1mmPort{kN1mmDefaultPort};
    int         m_n1mmRadio{1};

    void sendPanSet(const QString& keyValue);
    void emitN1mmChanged();
    bool        m_resized{false};
    bool        m_wfConfigured{false};
};

} // namespace AetherSDR
