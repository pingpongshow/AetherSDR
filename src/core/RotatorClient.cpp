#include "RotatorClient.h"
#include "RotatorBackends.h"
#include "CtyDatParser.h"

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcRotator, "aether.rotator", QtWarningMsg)

namespace AetherSDR {

// ── RotatorConfig ───────────────────────────────────────────────────────────

QString RotatorConfig::backendToString(Backend b)
{
    switch (b) {
    case Backend::Rotctld:    return QStringLiteral("rotctld");
    case Backend::PstRotator: return QStringLiteral("pstrotator");
    case Backend::N1mm:       return QStringLiteral("n1mm");
    }
    return QStringLiteral("rotctld");
}

RotatorConfig::Backend RotatorConfig::backendFromString(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("pstrotator")) {
        return Backend::PstRotator;
    }
    if (v == QLatin1String("n1mm")) {
        return Backend::N1mm;
    }
    return Backend::Rotctld;
}

quint16 RotatorConfig::defaultPort(Backend b)
{
    switch (b) {
    case Backend::Rotctld:    return 4533;    // rotctld(1) default
    case Backend::PstRotator: return 12000;   // PstRotator UDP control default
    case Backend::N1mm:       return 12040;   // N1MM rotor command port
    }
    return 4533;
}

// ── RotatorClient ───────────────────────────────────────────────────────────

void RotatorClient::BackendDeleter::operator()(RotatorBackend* b) const
{
    if (b) {
        b->deleteLater();
    }
}

RotatorClient::RotatorClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_pollTimer, &QTimer::timeout, this, &RotatorClient::requestPosition);
}

RotatorClient::~RotatorClient()
{
    if (m_backend) {
        disconnect(m_backend.get(), nullptr, this, nullptr);
        m_backend->close();
    }
}

void RotatorClient::setConfig(const RotatorConfig& cfg)
{
    m_cfg = cfg;
    m_cfg.pollIntervalMs = std::max(100, m_cfg.pollIntervalMs);
    m_cfg.commandTimeoutMs = std::max(200, m_cfg.commandTimeoutMs);
    m_cfg.reconnectMinMs = std::max(50, m_cfg.reconnectMinMs);
    m_cfg.reconnectMaxMs = std::max(m_cfg.reconnectMinMs, m_cfg.reconnectMaxMs);
    if (m_cfg.maxAz < m_cfg.minAz) {
        std::swap(m_cfg.minAz, m_cfg.maxAz);
    }
    if (m_running) {
        stop();
        start();
    }
}

void RotatorClient::setStationGrid(const QString& grid)
{
    m_stationGrid = grid.trimmed().toUpper();
}

std::optional<LatLon> RotatorClient::stationLocation() const
{
    return GeoUtils::gridToLatLon(m_stationGrid);
}

const CtyDatParser* RotatorClient::ctyParser() const
{
    if (m_cty) {
        return m_cty;
    }
    if (!m_ownCty) {
        m_ownCty = std::make_unique<CtyDatParser>();
        if (!m_ownCty->loadFromResource(QStringLiteral(":/cty.dat"))) {
            qCWarning(lcRotator) << "cty.dat not available — callsign bearings disabled";
        }
    }
    return m_ownCty->isLoaded() ? m_ownCty.get() : nullptr;
}

void RotatorClient::createBackend()
{
    switch (m_cfg.backend) {
    case RotatorConfig::Backend::Rotctld:
        m_backend.reset(new RotctldBackend(m_cfg, this));
        break;
    case RotatorConfig::Backend::PstRotator:
        m_backend.reset(new PstRotatorBackend(m_cfg, this));
        break;
    case RotatorConfig::Backend::N1mm:
        m_backend.reset(new N1mmRotorBackend(m_cfg, this));
        break;
    }
    connect(m_backend.get(), &RotatorBackend::connected,
            this, &RotatorClient::onBackendConnected);
    connect(m_backend.get(), &RotatorBackend::disconnected,
            this, &RotatorClient::onBackendDisconnected);
    connect(m_backend.get(), &RotatorBackend::positionReported,
            this, &RotatorClient::onBackendPosition);
    connect(m_backend.get(), &RotatorBackend::commandFailed,
            this, &RotatorClient::onBackendCommandFailed);
}

void RotatorClient::start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    createBackend();
    setState(State::Connecting);
    m_backend->open();
    m_pollTimer.start(m_cfg.pollIntervalMs);
}

void RotatorClient::stop()
{
    m_running = false;
    m_pollTimer.stop();
    if (m_backend) {
        disconnect(m_backend.get(), nullptr, this, nullptr);
        m_backend->close();
        m_backend.reset();
    }
    m_hasPosition = false;
    if (m_hasTarget) {
        clearTarget();
    }
    setState(State::Disabled);
}

void RotatorClient::setState(State s)
{
    if (m_state == s) {
        return;
    }
    m_state = s;
    emit stateChanged(s);
}

void RotatorClient::onBackendConnected()
{
    setState(State::Connected);
    requestPosition();
}

void RotatorClient::onBackendDisconnected(const QString& reason)
{
    if (!m_running) {
        return;
    }
    setState(State::Reconnecting);
    emit errorOccurred(reason);
}

void RotatorClient::onBackendPosition(double az, double el)
{
    const bool changed = !m_hasPosition || std::abs(az - m_rawAz) > 0.05
                         || std::abs(el - m_el) > 0.05;
    m_rawAz = az;
    m_el = el;
    m_hasPosition = true;
    if (changed) {
        emit positionChanged(azimuth(), el);
    }

    if (m_hasTarget) {
        const double diff = std::abs(GeoUtils::normalizeBearing(azimuth() - m_targetBearing + 180.0) - 180.0);
        if (diff <= m_cfg.reachedToleranceDeg) {
            const double t = m_targetBearing;
            m_hasTarget = false;
            emit targetReached(t);
        }
    }
}

void RotatorClient::onBackendCommandFailed(const QString& command, int rprt, const QString& message)
{
    qCWarning(lcRotator) << "rotator command failed:" << command << "RPRT" << rprt << message;
    if (command.startsWith(QLatin1String("set_pos")) && m_hasTarget) {
        clearTarget();
    }
    emit commandFailed(command, rprt, message);
}

void RotatorClient::clearTarget()
{
    m_hasTarget = false;
    emit targetCleared();
}

std::optional<double> RotatorClient::mapToRotatorRange(double bearingDeg, double minAz, double maxAz,
                                                      std::optional<double> current)
{
    if (!std::isfinite(bearingDeg) || !std::isfinite(minAz) || !std::isfinite(maxAz)) {
        return std::nullopt;
    }
    constexpr double kEps = 1e-6;
    const double b = GeoUtils::normalizeBearing(bearingDeg);

    std::optional<double> best;
    for (int k = -2; k <= 2; ++k) {
        const double c = b + 360.0 * k;
        if (c < minAz - kEps || c > maxAz + kEps) {
            continue;
        }
        if (!best) {
            best = c;
            continue;
        }
        if (current) {
            if (std::abs(c - *current) < std::abs(*best - *current)) {
                best = c;
            }
        } else if (std::abs(c - b) < std::abs(*best - b)) {
            best = c;   // no position yet: prefer the plain 0–360 value
        }
    }
    if (best) {
        best = std::clamp(*best, minAz, maxAz);
    }
    return best;
}

bool RotatorClient::turnTo(double bearingDeg, double elevationDeg)
{
    if (!m_running || !m_backend) {
        emit errorOccurred(QStringLiteral("Rotator is not enabled"));
        return false;
    }
    const double bearing = GeoUtils::normalizeBearing(bearingDeg);
    double commandAz = bearing;
    if (!m_backend->wantsCompassBearing()) {
        const std::optional<double> mapped = mapToRotatorRange(
            bearing, m_cfg.minAz, m_cfg.maxAz,
            m_hasPosition ? std::optional<double>(m_rawAz) : std::nullopt);
        if (!mapped) {
            emit errorOccurred(QStringLiteral("Bearing %1° is outside the rotator range %2°–%3°")
                                   .arg(bearing, 0, 'f', 0)
                                   .arg(m_cfg.minAz, 0, 'f', 0)
                                   .arg(m_cfg.maxAz, 0, 'f', 0));
            return false;
        }
        commandAz = *mapped;
    }

    m_targetBearing = bearing;
    m_hasTarget = true;
    emit targetChanged(bearing);
    m_backend->setPosition(commandAz, elevationDeg);
    return true;
}

std::optional<GreatCirclePath> RotatorClient::pathToGrid(const QString& grid) const
{
    const std::optional<LatLon> home = stationLocation();
    const std::optional<LatLon> dx = GeoUtils::gridToLatLon(grid);
    if (!home || !dx) {
        return std::nullopt;
    }
    return GeoUtils::pathBetween(*home, *dx);
}

std::optional<GreatCirclePath> RotatorClient::pathToCallsign(const QString& callsign,
                                                             const QString& gridHint) const
{
    if (GeoUtils::isValidGrid(gridHint) && gridHint.trimmed().size() >= 4) {
        return pathToGrid(gridHint);
    }
    const std::optional<LatLon> home = stationLocation();
    const CtyDatParser* cty = ctyParser();
    if (!home || !cty) {
        return std::nullopt;
    }
    const std::optional<LatLon> dx = GeoUtils::latLonForCallsign(*cty, callsign);
    if (!dx) {
        return std::nullopt;
    }
    return GeoUtils::pathBetween(*home, *dx);
}

bool RotatorClient::turnToGrid(const QString& grid, bool longPath)
{
    if (!stationLocation()) {
        emit errorOccurred(QStringLiteral("Station grid square is not set (Radio Setup)"));
        return false;
    }
    const std::optional<GreatCirclePath> p = pathToGrid(grid);
    if (!p) {
        emit errorOccurred(QStringLiteral("Invalid grid square: %1").arg(grid));
        return false;
    }
    return turnTo(longPath ? p->longPathDeg : p->shortPathDeg);
}

bool RotatorClient::turnToCallsign(const QString& callsign, bool longPath, const QString& gridHint)
{
    if (!stationLocation()) {
        emit errorOccurred(QStringLiteral("Station grid square is not set (Radio Setup)"));
        return false;
    }
    const std::optional<GreatCirclePath> p = pathToCallsign(callsign, gridHint);
    if (!p) {
        emit errorOccurred(QStringLiteral("No location for %1").arg(callsign));
        return false;
    }
    return turnTo(longPath ? p->longPathDeg : p->shortPathDeg);
}

void RotatorClient::stopRotation()
{
    if (!m_backend) {
        return;
    }
    m_backend->stop();
    if (m_hasTarget) {
        clearTarget();
    }
}

void RotatorClient::park()
{
    if (!m_backend) {
        return;
    }
    m_backend->park();
    if (m_hasTarget) {
        clearTarget();
    }
}

void RotatorClient::requestPosition()
{
    if (m_backend) {
        m_backend->poll();
    }
}

QString RotatorClient::rprtErrorString(int code)
{
    // Hamlib include/hamlib/rig.h, enum rig_errcode_e (Hamlib 4.x).
    switch (code) {
    case 0:   return QStringLiteral("Command completed successfully");
    case -1:  return QStringLiteral("Invalid parameter");
    case -2:  return QStringLiteral("Invalid configuration");
    case -3:  return QStringLiteral("Memory shortage");
    case -4:  return QStringLiteral("Function not implemented");
    case -5:  return QStringLiteral("Communication timed out");
    case -6:  return QStringLiteral("IO error");
    case -7:  return QStringLiteral("Internal Hamlib error");
    case -8:  return QStringLiteral("Protocol error");
    case -9:  return QStringLiteral("Command rejected by the rotator");
    case -10: return QStringLiteral("Command performed, but argument truncated");
    case -11: return QStringLiteral("Function not available");
    case -12: return QStringLiteral("VFO not targetable");
    case -13: return QStringLiteral("Error talking on the bus");
    case -14: return QStringLiteral("Collision on the bus");
    case -15: return QStringLiteral("NULL handle or invalid pointer parameter");
    case -16: return QStringLiteral("Invalid VFO");
    case -17: return QStringLiteral("Argument out of domain of function");
    case -18: return QStringLiteral("Function deprecated");
    case -19: return QStringLiteral("Security error");
    case -20: return QStringLiteral("Rotator not powered on");
    default:  break;
    }
    return QStringLiteral("Hamlib error %1").arg(code);
}

} // namespace AetherSDR
