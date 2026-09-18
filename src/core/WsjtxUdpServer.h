#pragma once

// WSJT-X / JTDX UDP message server.
//
// Plays the role of the WSJT-X "message_aggregator" reference server
// (WSJT-X UDPExamples/MessageServer.cpp): listens on UDP (default 2237,
// optionally joined to a multicast group such as 224.0.0.1), tracks every
// WSJT-X instance by (sender address, id), negotiates the schema, and turns
// Decode / Status / QSOLogged / LoggedADIF traffic into Qt signals.
//
// Outgoing traffic is limited to *messages to WSJT-X* (Reply, HaltTx,
// FreeText, Location, HighlightCallsign, Replay, Clear).  AetherSDR never
// keys the radio from here — any transmission is WSJT-X's own decision.
//
// Relay: because GridTracker / JTAlert / Log4OM usually own port 2237, every
// datagram received can be forwarded verbatim to a list of host:port relay
// targets.  Their replies (e.g. GridTracker "Reply" or "HighlightCallsign")
// arrive on a dedicated relay socket and are routed back to the WSJT-X
// instance whose id is in the message header, so those apps keep working
// behind AetherSDR.
//
// Threading: lives on whatever thread owns it (main thread is fine — the
// traffic is a few datagrams per T/R period).  All outputs are signals.
//
// Settings (AppSettings, read by applySettings()):
//   WsjtxUdpEnabled          "True"/"False"   (default False)
//   WsjtxUdpPort             int              (default 2237)
//   WsjtxUdpMulticastGroup   "224.0.0.1" / "" (default "", unicast)
//   WsjtxUdpRelayTargets     "127.0.0.1:2238,192.168.1.20:2237" (default "")
//   WsjtxSpotsEnabled        "True"/"False"   (default True)
//   WsjtxSpotsCqOnly         "True"/"False"   (default False)
//   WsjtxSpotLifetimeCycles  int              (default 2)
//   WsjtxSpotColor           #AARRGGBB        (default #FF7FDBFF)
//   WsjtxCqSpotColor         #AARRGGBB        (default #FF2ECC40)

#include "WsjtxMessage.h"

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>

class QTimer;
class QUdpSocket;

namespace AetherSDR {

// A WSJT-X (or JTDX) instance seen on the network.
struct WsjtxInstance {
    QString id;
    QHostAddress address;
    quint16 port{0};
    quint32 schema{Wsjtx::kPreNegotiationSchema}; // negotiated
    QString version;
    QString revision;
    bool hasStatus{false};
    Wsjtx::Status status;
    qint64 lastSeenMs{0};
};

// One decode, enriched with absolute frequency and parsed text.
struct WsjtxDecodeEvent {
    QString instanceId;
    Wsjtx::Decode decode;          // raw fields — needed verbatim for Reply
    Wsjtx::ParsedText parsed;
    QString modeName;              // "FT8", "FT4", ...
    quint64 dialFrequencyHz{0};    // 0 if no Status seen yet
    quint64 rfFrequencyHz{0};      // dial + audio offset
    qint64 receivedMs{0};
    int spotId{-1};                // SpotModel index if spotted, else -1
};

struct WsjtxRelayTarget {
    QHostAddress address;
    quint16 port{0};
    bool operator==(const WsjtxRelayTarget&) const = default;
};

class WsjtxUdpServer : public QObject {
    Q_OBJECT

public:
    // Client-side SpotModel index range owned by this server.  Chosen well
    // clear of radio spot indices, TCI (10000+) and Memory (negative).
    static constexpr int kSpotIdBase = 30000000;
    static constexpr int kSpotIdSpan = 1000000;
    static constexpr int kMaxRecentDecodes = 500;
    static constexpr int kMaxHighlights = 100;   // spec: keep ≤ ~100 active
    static constexpr int kPendingQsoLoggedMs = 750;
    static inline const QString kSpotSource = QStringLiteral("WSJT-X");

    explicit WsjtxUdpServer(QObject* parent = nullptr);
    ~WsjtxUdpServer() override;

    // Bind and (optionally) join a multicast group.  Rebinds if already
    // listening with different parameters.  Returns false on bind failure.
    bool start(quint16 port = Wsjtx::kDefaultPort, const QString& multicastGroup = {});
    void stop();
    bool isListening() const;
    quint16 port() const { return m_port; }
    QString multicastGroup() const { return m_group.isNull() ? QString() : m_group.toString(); }

    // Reads all Wsjtx* keys from AppSettings and starts/stops accordingly.
    void applySettings();

    void setRelayTargets(const QList<WsjtxRelayTarget>& targets);
    QList<WsjtxRelayTarget> relayTargets() const { return m_relayTargets; }
    static QList<WsjtxRelayTarget> parseRelayTargets(const QString& csv);

    void setSpotsEnabled(bool on);
    void setCqOnly(bool on) { m_cqOnly = on; }
    void setSpotLifetimeCycles(int cycles) { m_lifetimeCycles = qMax(1, cycles); }
    void setSpotColors(const QString& normalArgb, const QString& cqArgb);

    QList<WsjtxInstance> instances() const { return m_instances.values(); }
    bool hasInstance(const QString& id) const { return m_instances.contains(id); }
    WsjtxInstance instance(const QString& id) const { return m_instances.value(id); }
    const QVector<WsjtxDecodeEvent>& recentDecodes() const { return m_recent; }

public slots:
    // ── Messages to WSJT-X (never keys the radio directly) ──
    // Reply == "double-click this decode" in WSJT-X.  WSJT-X only acts if
    // the fields exactly match a prior CQ/QRZ decode still in its window.
    bool sendReply(const WsjtxDecodeEvent& decode, quint8 modifiers = 0);
    // Reply to the most recent decode that produced SpotModel index spotId.
    bool replyToSpot(int spotId, quint8 modifiers = 0);
    bool sendHaltTx(const QString& instanceId, bool autoTxOnly = true);
    bool sendFreeText(const QString& instanceId, const QString& text, bool send = false);
    bool sendLocation(const QString& instanceId, const QString& grid);
    // instanceId empty → all instances.  Invalid QColor clears.
    bool sendHighlightCallsign(const QString& instanceId, const QString& callsign,
                               const QColor& background, const QColor& foreground,
                               bool highlightLast = false);
    void clearAllHighlights();
    bool sendReplay(const QString& instanceId);
    bool sendClear(const QString& instanceId, quint8 window = 0);

    // Remove every spot this server has placed (e.g. on disable).
    void clearSpots();

signals:
    void listeningChanged(bool listening);
    void errorOccurred(const QString& message);

    void instanceAdded(const QString& id);
    void instanceRemoved(const QString& id);
    void instanceStatusChanged(const QString& id, const Wsjtx::Status& status);

    void decodeReceived(const WsjtxDecodeEvent& decode);
    void wsprDecodeReceived(const QString& instanceId, const Wsjtx::WsprDecode& decode);
    void decodesCleared(const QString& instanceId);

    // SpotModel feed — MainWindow applies these to RadioModel::spotModel():
    //   spotUpsert → applySpotStatus(spotId, kvs); spotRemove → removeSpot()
    void spotUpsert(int spotId, const QMap<QString, QString>& kvs);
    void spotRemove(int spotId);

    // One ADIF record (…<EOR>) per logged QSO — from LoggedADIF when WSJT-X
    // sends it, otherwise synthesised from QSOLogged.
    void qsoLogged(const QString& adifRecord);
    void qsoLoggedDetails(const QString& instanceId, const Wsjtx::QsoLogged& qso);

private slots:
    void onReadyRead();
    void onRelayReadyRead();
    void onHousekeeping();

private:
    struct SpotEntry {
        int spotId{-1};
        QString instanceId;
        QString callsign;
        qint64 expiresMs{0};
        int decodeIndexHint{-1};
        WsjtxDecodeEvent lastDecode;
    };
    struct PendingQso {
        QString adif;
        qint64 deadlineMs{0};
    };

    void handleDatagram(const QByteArray& datagram, const QHostAddress& sender, quint16 senderPort);
    WsjtxInstance& touchInstance(const Wsjtx::Message& msg, const QHostAddress& sender,
                                 quint16 senderPort, bool* isNew);
    void handleDecode(const QString& id, const Wsjtx::Decode& d);
    void handleClear(const QString& id);
    void removeInstance(const QString& id);
    void relay(const QByteArray& datagram);
    bool sendTo(const QString& instanceId, const Wsjtx::Body& body);
    void updateSpot(WsjtxDecodeEvent& ev);
    void removeSpotsForInstance(const QString& id);
    int allocateSpotId();
    double periodSecondsFor(const QString& instanceId, const QString& mode) const;
    void flushPendingQsos(bool force);

    // Parented to this (Qt ownership) so moveToThread() carries them along.
    QUdpSocket* m_socket{nullptr};
    QUdpSocket* m_relaySocket{nullptr};
    QTimer* m_housekeeping{nullptr};
    quint16 m_port{0};
    QHostAddress m_group;

    QMap<QString, WsjtxInstance> m_instances;          // by id
    QVector<WsjtxDecodeEvent> m_recent;
    QHash<QString, SpotEntry> m_spotsByKey;             // "id\ncall" → spot
    QHash<int, QString> m_spotKeyById;
    QHash<QString, PendingQso> m_pendingQso;            // by instance id
    QHash<QString, QPair<QColor, QColor>> m_highlights; // callsign → (bg, fg)
    QList<WsjtxRelayTarget> m_relayTargets;

    bool m_spotsEnabled{true};
    bool m_cqOnly{false};
    int m_lifetimeCycles{2};
    QString m_spotColor{QStringLiteral("#FF7FDBFF")};
    QString m_cqSpotColor{QStringLiteral("#FF2ECC40")};
    int m_nextSpotOffset{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::WsjtxDecodeEvent)
Q_DECLARE_METATYPE(AetherSDR::Wsjtx::Status)
Q_DECLARE_METATYPE(AetherSDR::Wsjtx::WsprDecode)
Q_DECLARE_METATYPE(AetherSDR::Wsjtx::QsoLogged)
