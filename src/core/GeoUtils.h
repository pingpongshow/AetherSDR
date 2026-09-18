#pragma once

#include <QString>

#include <optional>

namespace AetherSDR {

class CtyDatParser;
struct DxccEntity;

// ---------------------------------------------------------------------------
// GeoUtils — Maidenhead locator conversion and great-circle path math.
//
// Conventions used throughout:
//   * latitude  in degrees, +North / -South
//   * longitude in degrees, +East  / -West   (ISO 6709 — NOT cty.dat's sign)
//   * bearings  in degrees true, [0, 360)
//   * distances in kilometres on a spherical Earth (R = 6371.0 km, IUGG mean)
//
// NOTE on cty.dat: AD1C's cty.dat stores longitude as +West (e.g. "United
// States: ... 37.60: 91.87:"). CtyDatParser keeps the raw file value, so
// callers must go through latLonFromCtyEntity() which flips the sign.
// ---------------------------------------------------------------------------

struct LatLon {
    double lat{0.0};
    double lon{0.0};
};

struct GreatCirclePath {
    double shortPathDeg{0.0};   // initial bearing, short path
    double longPathDeg{0.0};    // short path + 180 (mod 360)
    double shortPathKm{0.0};
    double longPathKm{0.0};     // Earth circumference − short path
};

namespace GeoUtils {

constexpr double kEarthRadiusKm = 6371.0;

// True for a syntactically valid 2/4/6/8-character Maidenhead locator
// (field AA-RR, square 00-99, subsquare AA-XX, extended square 00-99).
bool isValidGrid(const QString& grid);

// Centre of the given locator cell.  std::nullopt if the grid is invalid.
std::optional<LatLon> gridToLatLon(const QString& grid);

// Encode lat/lon as a locator of 2, 4, 6 or 8 characters.
QString latLonToGrid(const LatLon& pos, int precision = 6);

// Normalise any angle into [0, 360).
double normalizeBearing(double deg);

// Initial great-circle bearing from `from` to `to` (short path).
double initialBearing(const LatLon& from, const LatLon& to);

// Great-circle (haversine) distance in km.
double distanceKm(const LatLon& from, const LatLon& to);

// Short/long path bearing + distance.
GreatCirclePath pathBetween(const LatLon& from, const LatLon& to);

// DXCC entity centroid from cty.dat, with the +West longitude flipped to +East.
LatLon latLonFromCtyEntity(const DxccEntity& entity);

// First 4/6-char Maidenhead locator appearing as a whole word in free text
// (e.g. a DX-cluster / FT8 spot comment "CQ DX FN31"), upper-cased; "" if none.
QString findGridInText(const QString& text);

// Look up a callsign in cty.dat and return the entity centroid.
std::optional<LatLon> latLonForCallsign(const CtyDatParser& cty, const QString& callsign);

// Parse a free-form "turn to" target: a bearing ("275", "275.5", "275°"),
// a Maidenhead locator ("IO91", "IO91wm"), or a callsign (resolved via cty).
// Numeric input is a bearing; the other two resolve to a position.
struct TurnTarget {
    enum class Kind { Invalid, Bearing, Grid, Callsign };
    Kind    kind{Kind::Invalid};
    double  bearingDeg{0.0};     // valid for Bearing
    LatLon  position;            // valid for Grid / Callsign
    QString label;               // normalised text for display (e.g. "IO91WM", "VK2ABC (Australia)")
};
TurnTarget parseTurnTarget(const QString& text, const CtyDatParser* cty);

} // namespace GeoUtils
} // namespace AetherSDR
