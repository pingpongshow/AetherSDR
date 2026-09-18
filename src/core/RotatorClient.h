#pragma once

#include "GeoUtils.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

namespace AetherSDR {

class CtyDatParser;
class RotatorBackend;

// ---------------------------------------------------------------------------
// RotatorClient — antenna rotator control (client-side only; the radio has
// no rotator concept, so every setting here is client-authoritative).
//
// Backends:
//   * Rotctld    Hamlib rotctld TCP network daemon (default localhost:4533).
//                Uses the Extended Response Protocol ('+' prefix) so each
//                reply is framed by a terminating "RPRT n" line.
//                Ref: Hamlib rotctld(1) man page, "PROTOCOL" and
//                "Extended Response Protocol" sections.
//   * PstRotator PstRotator / ARSWIN UDP control:
//                "<PST><AZIMUTH>nnn</AZIMUTH></PST>" to UDP port 12000,
//                "<PST>AZ?</PST>" query answered as "AZ:nnn" on port+1.
//                Ref: PstRotator user manual, "UDP Control" (pstrotator.com).
//   * N1mm       N1MM Logger+ rotor UDP protocol: "<N1MMRotor>…<goazi>" to
//                port 12040; heading broadcasts "<name> @ <deg×10>" on
//                UDP 13010.  Ref: N1MM Logger+ docs, "Rotor Control".
//
// All I/O is non-blocking and lives on the thread that owns this object
// (the main thread in AetherSDR — no extra thread needed; traffic is a few
// bytes per poll).
// ---------------------------------------------------------------------------

struct RotatorConfig {
    enum class Backend { Rotctld, PstRotator, N1mm };

    Backend backend{Backend::Rotctld};
    QString host{QStringLiteral("127.0.0.1")};
    quint16 port{4533};              // rotctld 4533, PstRotator 12000, N1MM 12040
    quint16 listenPort{0};           // UDP status port; 0 = backend default (Pst: port+1, N1MM: 13010)
    QString n1mmRotorName{QStringLiteral("Rotor")};
    int     pollIntervalMs{1000};
    int     commandTimeoutMs{3000};
    int     connectTimeoutMs{5000};
    int     reconnectMinMs{1000};
    int     reconnectMaxMs{30000};
    double  minAz{0.0};              // rotator mechanical range, e.g. 0..450 for overlap,
    double  maxAz{360.0};            // or -180..180 for a south-centred rotator
    double  reachedToleranceDeg{2.0};

    static QString backendToString(Backend b);
    static Backend backendFromString(const QString& s);
    static quint16 defaultPort(Backend b);
};

class RotatorClient : public QObject {
    Q_OBJECT

public:
    enum class State { Disabled, Connecting, Connected, Reconnecting };
    Q_ENUM(State)

    explicit RotatorClient(QObject* parent = nullptr);
    ~RotatorClient() override;

    // Config -------------------------------------------------------------
    void setConfig(const RotatorConfig& cfg);   // restarts the backend if running
    const RotatorConfig& config() const { return m_cfg; }

    // Station location used for grid/callsign bearings (StationGridSquare).
    void setStationGrid(const QString& grid);
    QString stationGrid() const { return m_stationGrid; }
    std::optional<LatLon> stationLocation() const;

    // cty.dat used for callsign → DXCC centroid.  If never set, the client
    // lazily loads its own copy from ":/cty.dat" on first callsign lookup.
    void setCtyParser(const CtyDatParser* cty) { m_cty = cty; }
    const CtyDatParser* ctyParser() const;

    // Lifecycle ----------------------------------------------------------
    void start();                   // connect (and keep reconnecting)
    void stop();                    // disconnect, no reconnect
    bool isRunning() const { return m_running; }
    State state() const { return m_state; }
    bool isConnected() const { return m_state == State::Connected; }

    // Position -----------------------------------------------------------
    bool   hasPosition() const { return m_hasPosition; }
    double rawAzimuth() const { return m_rawAz; }                      // as reported (may be >360 or <0)
    double azimuth() const { return GeoUtils::normalizeBearing(m_rawAz); }
    double elevation() const { return m_el; }
    bool   hasTarget() const { return m_hasTarget; }
    double targetAzimuth() const { return m_targetBearing; }          // compass bearing [0,360)

    // Commands -----------------------------------------------------------
    // turnTo() takes a compass bearing and maps it into the rotator's
    // mechanical range, choosing the overlap equivalent closest to the
    // current position.  Returns false if not running or out of range.
    bool turnTo(double bearingDeg, double elevationDeg = 0.0);
    bool turnToGrid(const QString& grid, bool longPath = false);
    bool turnToCallsign(const QString& callsign, bool longPath = false,
                        const QString& gridHint = {});
    void stopRotation();
    void park();
    void requestPosition();         // extra poll outside the timer

    // Bearing resolution without moving (for tooltips/menus).
    std::optional<GreatCirclePath> pathToGrid(const QString& grid) const;
    std::optional<GreatCirclePath> pathToCallsign(const QString& callsign,
                                                  const QString& gridHint = {}) const;

    // Map a compass bearing into [minAz, maxAz].  With an overlap range the
    // candidate nearest `current` is picked.  nullopt if unreachable.
    static std::optional<double> mapToRotatorRange(double bearingDeg, double minAz, double maxAz,
                                                   std::optional<double> current);

    // Hamlib rig_errcode_e text for an RPRT code (e.g. -5 → "Communication timed out").
    static QString rprtErrorString(int code);

signals:
    void stateChanged(AetherSDR::RotatorClient::State state);
    void positionChanged(double azimuthDeg, double elevationDeg);
    void targetChanged(double bearingDeg);                  // compass bearing
    void targetReached(double bearingDeg);
    void targetCleared();
    // A command was rejected (rprt != 0) or timed out (rprt = -5).
    void commandFailed(const QString& command, int rprt, const QString& message);
    void errorOccurred(const QString& message);

private slots:
    void onBackendConnected();
    void onBackendDisconnected(const QString& reason);
    void onBackendPosition(double az, double el);
    void onBackendCommandFailed(const QString& command, int rprt, const QString& message);

private:
    void setState(State s);
    void createBackend();
    void clearTarget();

    // Backends are torn down with deleteLater(): stop()/setConfig() may be
    // called from a slot connected to one of the backend's own signals.
    struct BackendDeleter {
        void operator()(RotatorBackend* b) const;
    };

    RotatorConfig m_cfg;
    std::unique_ptr<RotatorBackend, BackendDeleter> m_backend;
    QTimer  m_pollTimer;
    State   m_state{State::Disabled};
    bool    m_running{false};

    bool    m_hasPosition{false};
    double  m_rawAz{0.0};
    double  m_el{0.0};

    bool    m_hasTarget{false};
    double  m_targetBearing{0.0};

    QString m_stationGrid;
    const CtyDatParser* m_cty{nullptr};
    mutable std::unique_ptr<CtyDatParser> m_ownCty;
};

} // namespace AetherSDR
