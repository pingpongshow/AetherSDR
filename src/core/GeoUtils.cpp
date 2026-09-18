#include "GeoUtils.h"
#include "CtyDatParser.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR::GeoUtils {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

bool inRange(QChar c, char lo, char hi)
{
    return c.unicode() >= static_cast<ushort>(lo) && c.unicode() <= static_cast<ushort>(hi);
}

} // namespace

bool isValidGrid(const QString& grid)
{
    const QString g = grid.trimmed().toUpper();
    const qsizetype n = g.size();
    if (n != 2 && n != 4 && n != 6 && n != 8) {
        return false;
    }
    if (!inRange(g[0], 'A', 'R') || !inRange(g[1], 'A', 'R')) {
        return false;
    }
    if (n >= 4 && (!inRange(g[2], '0', '9') || !inRange(g[3], '0', '9'))) {
        return false;
    }
    if (n >= 6 && (!inRange(g[4], 'A', 'X') || !inRange(g[5], 'A', 'X'))) {
        return false;
    }
    if (n >= 8 && (!inRange(g[6], '0', '9') || !inRange(g[7], '0', '9'))) {
        return false;
    }
    return true;
}

std::optional<LatLon> gridToLatLon(const QString& grid)
{
    if (!isValidGrid(grid)) {
        return std::nullopt;
    }
    const QString g = grid.trimmed().toUpper();
    const qsizetype n = g.size();

    // Field: 20° lon × 10° lat
    double lon = (g[0].unicode() - 'A') * 20.0 - 180.0;
    double lat = (g[1].unicode() - 'A') * 10.0 - 90.0;
    double lonCell = 20.0;
    double latCell = 10.0;

    if (n >= 4) {   // Square: 2° × 1°
        lon += (g[2].unicode() - '0') * 2.0;
        lat += (g[3].unicode() - '0') * 1.0;
        lonCell = 2.0;
        latCell = 1.0;
    }
    if (n >= 6) {   // Subsquare: 5' × 2.5'
        lonCell = 2.0 / 24.0;
        latCell = 1.0 / 24.0;
        lon += (g[4].unicode() - 'A') * lonCell;
        lat += (g[5].unicode() - 'A') * latCell;
    }
    if (n >= 8) {   // Extended square: 30" × 15"
        lonCell /= 10.0;
        latCell /= 10.0;
        lon += (g[6].unicode() - '0') * lonCell;
        lat += (g[7].unicode() - '0') * latCell;
    }

    return LatLon{lat + latCell / 2.0, lon + lonCell / 2.0};
}

QString latLonToGrid(const LatLon& pos, int precision)
{
    precision = std::clamp(precision / 2 * 2, 2, 8);
    // Nudge exact +180/+90 inside the last cell.
    double lon = std::clamp(pos.lon + 180.0, 0.0, 359.999999);
    double lat = std::clamp(pos.lat + 90.0, 0.0, 179.999999);

    QString out;
    const int fLon = static_cast<int>(lon / 20.0);
    const int fLat = static_cast<int>(lat / 10.0);
    out += QChar('A' + fLon);
    out += QChar('A' + fLat);
    lon -= fLon * 20.0;
    lat -= fLat * 10.0;
    if (precision >= 4) {
        const int sLon = static_cast<int>(lon / 2.0);
        const int sLat = static_cast<int>(lat / 1.0);
        out += QChar('0' + sLon);
        out += QChar('0' + sLat);
        lon -= sLon * 2.0;
        lat -= sLat * 1.0;
    }
    if (precision >= 6) {
        const int ssLon = static_cast<int>(lon / (2.0 / 24.0));
        const int ssLat = static_cast<int>(lat / (1.0 / 24.0));
        out += QChar('a' + ssLon);
        out += QChar('a' + ssLat);
        lon -= ssLon * (2.0 / 24.0);
        lat -= ssLat * (1.0 / 24.0);
    }
    if (precision >= 8) {
        out += QChar('0' + static_cast<int>(lon / (2.0 / 240.0)));
        out += QChar('0' + static_cast<int>(lat / (1.0 / 240.0)));
    }
    return out;
}

double normalizeBearing(double deg)
{
    double r = std::fmod(deg, 360.0);
    if (r < 0.0) {
        r += 360.0;
    }
    // fmod can return 360 - epsilon rounding to 360.0 after the add.
    if (r >= 360.0) {
        r -= 360.0;
    }
    return r;
}

double initialBearing(const LatLon& from, const LatLon& to)
{
    const double phi1 = from.lat * kDegToRad;
    const double phi2 = to.lat * kDegToRad;
    const double dLambda = (to.lon - from.lon) * kDegToRad;
    const double y = std::sin(dLambda) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2)
                   - std::sin(phi1) * std::cos(phi2) * std::cos(dLambda);
    return normalizeBearing(std::atan2(y, x) * kRadToDeg);
}

double distanceKm(const LatLon& from, const LatLon& to)
{
    const double phi1 = from.lat * kDegToRad;
    const double phi2 = to.lat * kDegToRad;
    const double dPhi = phi2 - phi1;
    const double dLambda = (to.lon - from.lon) * kDegToRad;
    const double a = std::sin(dPhi / 2.0) * std::sin(dPhi / 2.0)
                   + std::cos(phi1) * std::cos(phi2)
                     * std::sin(dLambda / 2.0) * std::sin(dLambda / 2.0);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
    return kEarthRadiusKm * c;
}

GreatCirclePath pathBetween(const LatLon& from, const LatLon& to)
{
    GreatCirclePath p;
    p.shortPathDeg = initialBearing(from, to);
    p.longPathDeg = normalizeBearing(p.shortPathDeg + 180.0);
    p.shortPathKm = distanceKm(from, to);
    p.longPathKm = 2.0 * std::numbers::pi * kEarthRadiusKm - p.shortPathKm;
    return p;
}

LatLon latLonFromCtyEntity(const DxccEntity& entity)
{
    // cty.dat longitude is +West (AD1C format) — flip to ISO +East.
    return LatLon{entity.latitude, -entity.longitude};
}

QString findGridInText(const QString& text)
{
    static const QRegularExpression re(
        QStringLiteral(R"((?<![A-Za-z0-9])([A-Ra-r]{2}[0-9]{2}(?:[A-Xa-x]{2})?)(?![A-Za-z0-9]))"));
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QString g = it.next().captured(1).toUpper();
        // "RR73" is an FT8 sign-off, not a grid (it is a valid, but polar, locator).
        if (g != QLatin1String("RR73") && isValidGrid(g)) {
            return g;
        }
    }
    return {};
}

std::optional<LatLon> latLonForCallsign(const CtyDatParser& cty, const QString& callsign)
{
    const QString call = callsign.trimmed().toUpper();
    if (call.isEmpty() || !cty.isLoaded()) {
        return std::nullopt;
    }
    const DxccEntity* e = cty.entityByPrefix(cty.resolvePrimaryPrefix(call));
    if (!e) {
        return std::nullopt;
    }
    return latLonFromCtyEntity(*e);
}

TurnTarget parseTurnTarget(const QString& text, const CtyDatParser* cty)
{
    TurnTarget t;
    QString s = text.trimmed();
    s.remove(QChar(0x00B0));   // degree sign
    if (s.isEmpty()) {
        return t;
    }

    bool ok = false;
    const double deg = s.toDouble(&ok);
    if (ok && std::isfinite(deg)) {
        t.kind = TurnTarget::Kind::Bearing;
        t.bearingDeg = normalizeBearing(deg);
        t.label = QString::number(t.bearingDeg, 'f', 0) + QChar(0x00B0);
        return t;
    }

    const QString upper = s.toUpper();
    // 4/6/8-char locators take precedence over callsigns; a 2-char field
    // ("FN") is too easily confused with a prefix so require >= 4.
    if (upper.size() >= 4 && isValidGrid(upper)) {
        if (const std::optional<LatLon> pos = gridToLatLon(upper)) {
            t.kind = TurnTarget::Kind::Grid;
            t.position = *pos;
            t.label = upper;
            return t;
        }
    }

    static const QRegularExpression callRe(QStringLiteral(R"(^[A-Z0-9/]{2,}$)"));
    if (cty && callRe.match(upper).hasMatch()) {
        const DxccEntity* e = cty->entityByPrefix(cty->resolvePrimaryPrefix(upper));
        if (e) {
            t.kind = TurnTarget::Kind::Callsign;
            t.position = latLonFromCtyEntity(*e);
            t.label = QStringLiteral("%1 (%2)").arg(upper, e->name);
            return t;
        }
    }
    return t;
}

} // namespace AetherSDR::GeoUtils
