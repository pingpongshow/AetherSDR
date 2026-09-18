#pragma once

#include "RotatorClient.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QLoggingCategory>

#include <deque>
#include <optional>

Q_DECLARE_LOGGING_CATEGORY(lcRotator)

namespace AetherSDR {

// Internal transport interface used by RotatorClient.  Not part of the
// public API — include RotatorClient.h instead.
class RotatorBackend : public QObject {
    Q_OBJECT

public:
    explicit RotatorBackend(const RotatorConfig& cfg, QObject* parent = nullptr)
        : QObject(parent), m_cfg(cfg) {}
    ~RotatorBackend() override = default;

    virtual void open() = 0;
    virtual void close() = 0;
    virtual void poll() = 0;
    virtual void setPosition(double az, double el) = 0;
    virtual void stop() = 0;
    virtual void park() = 0;
    // True when the far end resolves overlap itself and wants a plain
    // 0–360 bearing (PstRotator, N1MM).  rotctld wants a mechanical az.
    virtual bool wantsCompassBearing() const = 0;

signals:
    void connected();
    void disconnected(const QString& reason);
    void positionReported(double az, double el);
    void commandFailed(const QString& command, int rprt, const QString& message);

protected:
    RotatorConfig m_cfg;
};

// ── Hamlib rotctld (TCP) ───────────────────────────────────────────────────
//
// Every command is sent with the '+' Extended Response Protocol prefix
// (rotctld(1): "+" → "Extended Response Protocol"), so every reply —
// success or failure — ends with a single "RPRT <code>" line:
//
//     > +p                  < get_pos:
//                           < Azimuth: 180.000000
//                           < Elevation: 0.000000
//                           < RPRT 0
//     > +P 180.0 0.0        < set_pos: 180.0 0.0
//                           < RPRT 0
//
// rotctld handles one connection's commands strictly in order, so commands
// are pipelined and matched FIFO against RPRT lines.  A timeout desyncs the
// stream, so it forces a reconnect.
class RotctldBackend : public RotatorBackend {
    Q_OBJECT

public:
    explicit RotctldBackend(const RotatorConfig& cfg, QObject* parent = nullptr);

    void open() override;
    void close() override;
    void poll() override;
    void setPosition(double az, double el) override;
    void stop() override;
    void park() override;
    bool wantsCompassBearing() const override { return false; }

    static constexpr int kMaxOutstanding = 16;
    static constexpr int kMaxRxBuffer = 64 * 1024;

private:
    struct Pending {
        QString    name;     // "get_pos", "set_pos 180.0 0.0", ...
        bool       isPoll{false};
    };

    void connectNow();
    void send(const QString& name, const QByteArray& wire, bool isPoll);
    void handleLine(const QString& line);
    void finishFront(int rprt);
    void linkLost(const QString& reason);
    void failAllPending(const QString& reason);

    QTcpSocket m_sock;
    QTimer     m_connectTimer;
    QTimer     m_cmdTimer;
    QTimer     m_reconnectTimer;
    QByteArray m_rx;
    std::deque<Pending> m_pending;
    QStringList m_respLines;
    int  m_backoffMs{0};
    bool m_wantOpen{false};
    bool m_attemptActive{false};
    bool m_linkUp{false};
};

// ── PstRotator / ARSWIN (UDP) ──────────────────────────────────────────────
class PstRotatorBackend : public RotatorBackend {
    Q_OBJECT

public:
    explicit PstRotatorBackend(const RotatorConfig& cfg, QObject* parent = nullptr);

    void open() override;
    void close() override;
    void poll() override;
    void setPosition(double az, double el) override;
    void stop() override;
    void park() override;
    bool wantsCompassBearing() const override { return true; }

    // "AZ:123" / "AZ:123.4\r" → 123.x ; nullopt otherwise.  Exposed for tests.
    static std::optional<double> parseAzReply(const QByteArray& datagram);

private:
    void sendDatagram(const QString& name, const QByteArray& payload);
    void onReadyRead();
    void resolveHost();

    QUdpSocket   m_sock;
    QHostAddress m_addr;
    QElapsedTimer m_lastReply;
    QTimer       m_rebindTimer;
    bool m_alive{false};
    bool m_open{false};
    int  m_lookupId{-1};
};

// ── N1MM Logger+ rotor protocol (UDP) ──────────────────────────────────────
class N1mmRotorBackend : public RotatorBackend {
    Q_OBJECT

public:
    explicit N1mmRotorBackend(const RotatorConfig& cfg, QObject* parent = nullptr);

    void open() override;
    void close() override;
    void poll() override {}   // protocol has no query; headings are broadcast
    void setPosition(double az, double el) override;
    void stop() override;
    void park() override;
    bool wantsCompassBearing() const override { return true; }

    static constexpr quint16 kDefaultStatusPort = 13010;

    // "<name> @ <tenths>" → degrees, if name matches (case-insensitive,
    // empty `name` matches anything).  Exposed for tests.
    static std::optional<double> parseHeadingBroadcast(const QByteArray& datagram,
                                                       const QString& name);

private:
    void sendDatagram(const QString& name, const QByteArray& payload);
    void onReadyRead();
    void resolveHost();

    QUdpSocket   m_tx;
    QUdpSocket   m_rx;
    QHostAddress m_addr;
    int  m_lookupId{-1};
};

} // namespace AetherSDR
