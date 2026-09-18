#include "WeakSignalMapDialog.h"

#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

const QVector<QVector<QPointF>>& landPolygons();
QPointF projectFlat(const QRectF& rect, double latitude, double longitude);
bool projectGlobe(const QRectF& rect,
                  double centerLatDeg,
                  double centerLonDeg,
                  double latitudeDeg,
                  double longitudeDeg,
                  QPointF* outPoint);
double normalizeLongitude(double longitude);
double greatCircleDistanceKm(double latitudeA, double longitudeA,
                             double latitudeB, double longitudeB);
double initialBearingDegrees(double latitudeA, double longitudeA,
                             double latitudeB, double longitudeB);
QString markerLabelForStation(const WeakSignalMapStation& station,
                              const WeakSignalMapStation& home);
void drawMarker(QPainter& painter, const QPointF& point, const QColor& fill, const QString& label, bool emphasize);

} // namespace

class WeakSignalMapWidget final : public QWidget {
public:
    enum class Projection {
        Flat,
        Globe
    };

    explicit WeakSignalMapWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(900, 520);
    }

    Projection projection() const
    {
        return m_projection;
    }

    void setProjection(Projection projection)
    {
        if (m_projection == projection)
            return;
        m_projection = projection;
        m_dragging = false;
        setCursor(m_projection == Projection::Globe ? Qt::OpenHandCursor : Qt::ArrowCursor);
        update();
    }

    void setHomeStation(const WeakSignalMapStation& home)
    {
        m_home = home;
        update();
    }

    void setStations(const QVector<WeakSignalMapStation>& stations)
    {
        m_stations = stations;
        update();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (m_projection != Projection::Globe || event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }

        const QRectF frame = rect().adjusted(12, 12, -12, -12);
        const QPointF center = frame.center();
        const double radius = std::min(frame.width(), frame.height()) * 0.47;
        const QPointF delta = event->position() - center;
        if ((delta.x() * delta.x()) + (delta.y() * delta.y()) > radius * radius) {
            QWidget::mousePressEvent(event);
            return;
        }

        m_manualView = true;
        m_dragging = true;
        m_lastDragPos = event->position();
        m_viewCenterLat = globeCenterLatitude();
        m_viewCenterLon = globeCenterLongitude();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_projection != Projection::Globe || !m_dragging) {
            QWidget::mouseMoveEvent(event);
            return;
        }

        const QRectF frame = rect().adjusted(12, 12, -12, -12);
        const double radius = std::min(frame.width(), frame.height()) * 0.47;
        if (radius <= 1.0) {
            QWidget::mouseMoveEvent(event);
            return;
        }

        const QPointF delta = event->position() - m_lastDragPos;
        m_lastDragPos = event->position();
        m_viewCenterLon = normalizeLongitude(m_viewCenterLon - (delta.x() * 90.0 / radius));
        m_viewCenterLat = std::clamp(m_viewCenterLat + (delta.y() * 90.0 / radius), -85.0, 85.0);
        update();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_projection == Projection::Globe && m_dragging && event->button() == Qt::LeftButton) {
            m_dragging = false;
            setCursor(Qt::OpenHandCursor);
            event->accept();
            return;
        }

        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF frame = rect().adjusted(12, 12, -12, -12);

        if (m_projection == Projection::Flat)
            paintFlat(painter, frame);
        else
            paintGlobe(painter, frame);
    }

private:
    void paintFlat(QPainter& painter, const QRectF& frame)
    {
        painter.fillRect(rect(), QColor("#08131f"));
        QLinearGradient ocean(frame.topLeft(), frame.bottomRight());
        ocean.setColorAt(0.0, QColor("#0e2a46"));
        ocean.setColorAt(1.0, QColor("#061625"));
        painter.fillRect(frame, ocean);

        painter.setPen(QPen(QColor(180, 220, 255, 38), 1.0));
        for (int lon = -150; lon <= 150; lon += 30) {
            const QPointF top = projectFlat(frame, 85.0, static_cast<double>(lon));
            const QPointF bottom = projectFlat(frame, -85.0, static_cast<double>(lon));
            painter.drawLine(top, bottom);
        }
        for (int lat = -60; lat <= 60; lat += 30) {
            const QPointF left = projectFlat(frame, static_cast<double>(lat), -180.0);
            const QPointF right = projectFlat(frame, static_cast<double>(lat), 180.0);
            painter.drawLine(left, right);
        }

        painter.setPen(QPen(QColor(45, 77, 64), 0.7));
        painter.setBrush(QColor("#4f6d4a"));
        for (const auto& polygon : landPolygons()) {
            QPolygonF projected;
            projected.reserve(polygon.size());
            for (const auto& point : polygon)
                projected.append(projectFlat(frame, point.y(), point.x()));
            painter.drawPolygon(projected);
        }

        drawStationsFlat(painter, frame);
    }

    void paintGlobe(QPainter& painter, const QRectF& frame)
    {
        painter.fillRect(rect(), QColor("#02040a"));

        const QPointF center = frame.center();
        const double radius = std::min(frame.width(), frame.height()) * 0.47;

        QRadialGradient globe(center, radius * 1.08, QPointF(center.x() - radius * 0.28, center.y() - radius * 0.35));
        globe.setColorAt(0.0, QColor("#2a5d84"));
        globe.setColorAt(0.65, QColor("#183d5d"));
        globe.setColorAt(1.0, QColor("#08131f"));
        painter.setPen(Qt::NoPen);
        painter.setBrush(globe);
        painter.drawEllipse(center, radius, radius);

        painter.save();
        QPainterPath clipPath;
        clipPath.addEllipse(center, radius, radius);
        painter.setClipPath(clipPath);

        const double centerLat = globeCenterLatitude();
        const double centerLon = globeCenterLongitude();

        painter.setPen(QPen(QColor(210, 235, 255, 28), 1.0));
        for (int lon = -150; lon <= 180; lon += 30)
            drawGlobeMeridian(painter, frame, centerLat, centerLon, static_cast<double>(lon));
        for (int lat = -60; lat <= 60; lat += 30)
            drawGlobeParallel(painter, frame, centerLat, centerLon, static_cast<double>(lat));

        painter.setPen(QPen(QColor(65, 100, 76), 0.8));
        painter.setBrush(QColor("#5f8258"));
        for (const auto& polygon : landPolygons()) {
            QPainterPath path;
            bool open = false;
            for (const auto& point : polygon) {
                QPointF projected;
                if (projectGlobe(frame, centerLat, centerLon, point.y(), point.x(), &projected)) {
                    if (!open) {
                        path.moveTo(projected);
                        open = true;
                    } else {
                        path.lineTo(projected);
                    }
                } else {
                    open = false;
                }
            }
            painter.drawPath(path);
        }

        drawStationsGlobe(painter, frame, centerLat, centerLon);
        painter.restore();

        painter.setPen(QPen(QColor("#d6e4f3"), 1.6));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius, radius);
    }

    void drawGlobeMeridian(QPainter& painter,
                           const QRectF& frame,
                           double centerLat,
                           double centerLon,
                           double meridianLon)
    {
        QPainterPath path;
        bool open = false;
        for (int lat = -85; lat <= 85; lat += 3) {
            QPointF projected;
            if (projectGlobe(frame, centerLat, centerLon, static_cast<double>(lat), meridianLon, &projected)) {
                if (!open) {
                    path.moveTo(projected);
                    open = true;
                } else {
                    path.lineTo(projected);
                }
            } else {
                open = false;
            }
        }
        painter.drawPath(path);
    }

    void drawGlobeParallel(QPainter& painter,
                           const QRectF& frame,
                           double centerLat,
                           double centerLon,
                           double parallelLat)
    {
        QPainterPath path;
        bool open = false;
        for (int lon = -180; lon <= 180; lon += 3) {
            QPointF projected;
            if (projectGlobe(frame, centerLat, centerLon, parallelLat, static_cast<double>(lon), &projected)) {
                if (!open) {
                    path.moveTo(projected);
                    open = true;
                } else {
                    path.lineTo(projected);
                }
            } else {
                open = false;
            }
        }
        painter.drawPath(path);
    }

    void drawStationsFlat(QPainter& painter, const QRectF& frame)
    {
        if (m_home.hasLocation) {
            drawMarker(painter, projectFlat(frame, m_home.latitude, m_home.longitude),
                       QColor("#2f8fff"),
                       m_home.callsign.isEmpty() ? QStringLiteral("Home") : QStringLiteral("H %1").arg(m_home.callsign),
                       true);
        }

        int labelsDrawn = 0;
        for (const auto& station : m_stations) {
            if (!station.hasLocation || station.isHome)
                continue;
            const QString label = (station.selected || labelsDrawn < 10)
                                      ? markerLabelForStation(station, m_home)
                                      : QString{};
            drawMarker(painter, projectFlat(frame, station.latitude, station.longitude),
                       QColor("#ffca55"), label, station.selected);
            if (!label.isEmpty())
                ++labelsDrawn;
        }
    }

    void drawStationsGlobe(QPainter& painter, const QRectF& frame, double centerLat, double centerLon)
    {
        if (m_home.hasLocation) {
            QPointF projected;
            if (projectGlobe(frame, centerLat, centerLon, m_home.latitude, m_home.longitude, &projected)) {
                drawMarker(painter, projected,
                           QColor("#2f8fff"),
                           m_home.callsign.isEmpty() ? QStringLiteral("Home") : QStringLiteral("H %1").arg(m_home.callsign),
                           true);
            }
        }

        int labelsDrawn = 0;
        for (const auto& station : m_stations) {
            if (!station.hasLocation || station.isHome)
                continue;
            QPointF projected;
            if (!projectGlobe(frame, centerLat, centerLon, station.latitude, station.longitude, &projected))
                continue;
            const QString label = (station.selected || labelsDrawn < 10)
                                      ? markerLabelForStation(station, m_home)
                                      : QString{};
            drawMarker(painter, projected, QColor("#ffca55"), label, station.selected);
            if (!label.isEmpty())
                ++labelsDrawn;
        }
    }

    double globeCenterLatitude() const
    {
        if (m_manualView)
            return m_viewCenterLat;
        if (m_home.hasLocation)
            return m_home.latitude;
        for (const auto& station : m_stations) {
            if (station.selected && station.hasLocation)
                return station.latitude;
        }
        return 0.0;
    }

    double globeCenterLongitude() const
    {
        if (m_manualView)
            return m_viewCenterLon;
        if (m_home.hasLocation)
            return m_home.longitude;
        for (const auto& station : m_stations) {
            if (station.selected && station.hasLocation)
                return station.longitude;
        }
        return 0.0;
    }

    Projection m_projection{Projection::Flat};
    WeakSignalMapStation m_home;
    QVector<WeakSignalMapStation> m_stations;
    bool m_manualView{false};
    bool m_dragging{false};
    QPointF m_lastDragPos;
    double m_viewCenterLat{0.0};
    double m_viewCenterLon{0.0};
};

namespace {

constexpr double kPi = 3.14159265358979323846;

double degToRad(double degrees)
{
    return degrees * kPi / 180.0;
}

double radToDeg(double radians)
{
    return radians * 180.0 / kPi;
}

double normalizeLongitude(double longitude)
{
    while (longitude <= -180.0)
        longitude += 360.0;
    while (longitude > 180.0)
        longitude -= 360.0;
    return longitude;
}

QVector<QVector<QPointF>> loadLandPolygons()
{
    QFile file(QStringLiteral(":/maps/ne_110m_land.geojson"));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = doc.object();
    const QJsonArray features = root.value(QStringLiteral("features")).toArray();

    QVector<QVector<QPointF>> polygons;
    for (const auto& featureValue : features) {
        const QJsonObject feature = featureValue.toObject();
        const QJsonObject geometry = feature.value(QStringLiteral("geometry")).toObject();
        const QString type = geometry.value(QStringLiteral("type")).toString();
        const QJsonValue coordsValue = geometry.value(QStringLiteral("coordinates"));

        auto addRing = [&polygons](const QJsonArray& ringArray) {
            QVector<QPointF> ring;
            ring.reserve(ringArray.size());
            for (const auto& pointValue : ringArray) {
                const QJsonArray pointArray = pointValue.toArray();
                if (pointArray.size() < 2)
                    continue;
                ring.append(QPointF(pointArray.at(0).toDouble(), pointArray.at(1).toDouble()));
            }
            if (ring.size() >= 3)
                polygons.append(ring);
        };

        if (type == QLatin1String("Polygon")) {
            const QJsonArray polygonArray = coordsValue.toArray();
            if (!polygonArray.isEmpty())
                addRing(polygonArray.at(0).toArray());
        } else if (type == QLatin1String("MultiPolygon")) {
            const QJsonArray multiPolygonArray = coordsValue.toArray();
            for (const auto& polygonValue : multiPolygonArray) {
                const QJsonArray polygonArray = polygonValue.toArray();
                if (!polygonArray.isEmpty())
                    addRing(polygonArray.at(0).toArray());
            }
        }
    }
    return polygons;
}

const QVector<QVector<QPointF>>& landPolygons()
{
    static const QVector<QVector<QPointF>> polygons = loadLandPolygons();
    return polygons;
}

QPointF projectFlat(const QRectF& rect, double latitude, double longitude)
{
    const double x = rect.left() + ((longitude + 180.0) / 360.0) * rect.width();
    const double y = rect.top() + ((90.0 - latitude) / 180.0) * rect.height();
    return QPointF(x, y);
}

bool projectGlobe(const QRectF& rect,
                  double centerLatDeg,
                  double centerLonDeg,
                  double latitudeDeg,
                  double longitudeDeg,
                  QPointF* outPoint)
{
    const double centerLat = degToRad(centerLatDeg);
    const double centerLon = degToRad(centerLonDeg);
    const double lat = degToRad(latitudeDeg);
    const double lon = degToRad(longitudeDeg);
    const double deltaLon = lon - centerLon;

    const double cosc = std::sin(centerLat) * std::sin(lat)
                        + std::cos(centerLat) * std::cos(lat) * std::cos(deltaLon);
    if (cosc <= 0.0)
        return false;

    const double radius = std::min(rect.width(), rect.height()) * 0.47;
    const QPointF center = rect.center();
    const double x = radius * std::cos(lat) * std::sin(deltaLon);
    const double y = -radius * (std::cos(centerLat) * std::sin(lat)
                                - std::sin(centerLat) * std::cos(lat) * std::cos(deltaLon));
    *outPoint = QPointF(center.x() + x, center.y() + y);
    return true;
}

double greatCircleDistanceKm(double latitudeA, double longitudeA,
                             double latitudeB, double longitudeB)
{
    constexpr double kEarthRadiusKm = 6371.0;

    const double lat1 = degToRad(latitudeA);
    const double lon1 = degToRad(longitudeA);
    const double lat2 = degToRad(latitudeB);
    const double lon2 = degToRad(longitudeB);
    const double dLat = lat2 - lat1;
    const double dLon = lon2 - lon1;

    const double sinHalfLat = std::sin(dLat / 2.0);
    const double sinHalfLon = std::sin(dLon / 2.0);
    const double a = (sinHalfLat * sinHalfLat)
                     + std::cos(lat1) * std::cos(lat2) * (sinHalfLon * sinHalfLon);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
    return kEarthRadiusKm * c;
}

double initialBearingDegrees(double latitudeA, double longitudeA,
                             double latitudeB, double longitudeB)
{
    const double lat1 = degToRad(latitudeA);
    const double lon1 = degToRad(longitudeA);
    const double lat2 = degToRad(latitudeB);
    const double lon2 = degToRad(longitudeB);
    const double dLon = lon2 - lon1;

    const double y = std::sin(dLon) * std::cos(lat2);
    const double x = std::cos(lat1) * std::sin(lat2)
                     - std::sin(lat1) * std::cos(lat2) * std::cos(dLon);
    double bearing = radToDeg(std::atan2(y, x));
    if (bearing < 0.0)
        bearing += 360.0;
    return bearing;
}

QString markerLabelForStation(const WeakSignalMapStation& station,
                              const WeakSignalMapStation& home)
{
    if (!station.selected || !home.hasLocation || !station.hasLocation)
        return station.callsign;

    const int distanceKm = static_cast<int>(std::lround(
        greatCircleDistanceKm(home.latitude, home.longitude, station.latitude, station.longitude)));
    const int bearing = static_cast<int>(std::lround(
        initialBearingDegrees(home.latitude, home.longitude, station.latitude, station.longitude)));
    return QStringLiteral("%1  %2 km  %3°").arg(station.callsign).arg(distanceKm).arg(bearing);
}

void drawMarker(QPainter& painter, const QPointF& point, const QColor& fill, const QString& label, bool emphasize)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int radius = emphasize ? 9 : 7;
    painter.setPen(QPen(emphasize ? QColor("#ffffff") : QColor(0, 0, 0, 180), emphasize ? 2.0 : 1.2));
    painter.setBrush(fill);
    painter.drawEllipse(point, radius, radius);

    if (!label.isEmpty()) {
        const QFontMetrics metrics(painter.font());
        const qreal bubbleWidth = std::clamp<qreal>(metrics.horizontalAdvance(label) + 20.0, 96.0, 280.0);
        const QRectF bubble(point.x() + 10.0, point.y() - 16.0, bubbleWidth, 24.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(8, 24, 48, 210));
        painter.drawRoundedRect(bubble, 6.0, 6.0);
        painter.setPen(QColor("#f8fbff"));
        painter.drawText(bubble.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    }

    painter.restore();
}

} // namespace

WeakSignalMapDialog::WeakSignalMapDialog(QWidget* parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1220, 760);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* header = new QHBoxLayout;
    m_legendLabel = new QLabel(QStringLiteral("Home station in blue, received stations in amber."));
    m_legendLabel->setWordWrap(true);
    m_projectionButton = new QPushButton(QStringLiteral("Globe"));
    m_projectionButton->setStyleSheet(
        "QPushButton { background: #30455d; color: white; padding: 8px 18px; "
        "border-radius: 6px; font-weight: bold; }");
    header->addWidget(m_legendLabel, 1);
    header->addWidget(m_projectionButton, 0);
    root->addLayout(header);

    m_mapWidget = new WeakSignalMapWidget(this);
    root->addWidget(m_mapWidget, 1);

    connect(m_projectionButton, &QPushButton::clicked, this, &WeakSignalMapDialog::toggleProjection);

    setContext(QStringLiteral("Weak Signal"));
    refreshLegend();
}

void WeakSignalMapDialog::setContext(const QString& titleText)
{
    m_contextTitle = titleText.trimmed();
    setWindowTitle(QStringLiteral("Map - %1").arg(m_contextTitle.isEmpty() ? QStringLiteral("Weak Signal") : m_contextTitle));
    refreshLegend();
}

void WeakSignalMapDialog::setHomeStation(const WeakSignalMapStation& home)
{
    m_home = home;
    m_mapWidget->setHomeStation(home);
    refreshLegend();
}

void WeakSignalMapDialog::setStations(const QVector<WeakSignalMapStation>& stations)
{
    m_stations = stations;
    m_mapWidget->setStations(stations);
    refreshLegend();
}

void WeakSignalMapDialog::toggleProjection()
{
    const auto nextProjection = m_mapWidget->projection() == WeakSignalMapWidget::Projection::Flat
                                    ? WeakSignalMapWidget::Projection::Globe
                                    : WeakSignalMapWidget::Projection::Flat;
    m_mapWidget->setProjection(nextProjection);
    m_projectionButton->setText(nextProjection == WeakSignalMapWidget::Projection::Flat
                                    ? QStringLiteral("Globe")
                                    : QStringLiteral("Map"));
    refreshLegend();
}

void WeakSignalMapDialog::refreshLegend()
{
    QStringList segments;

    QString homeText = QStringLiteral("Home: awaiting station locator");
    if (!m_home.callsign.trimmed().isEmpty() || !m_home.grid.trimmed().isEmpty() || m_home.hasLocation) {
        QStringList details;
        if (!m_home.callsign.trimmed().isEmpty())
            details.append(m_home.callsign.trimmed().toUpper());
        if (!m_home.grid.trimmed().isEmpty())
            details.append(m_home.grid.trimmed().toUpper());
        if (m_home.hasLocation) {
            details.append(QStringLiteral("%1, %2")
                               .arg(QString::number(m_home.latitude, 'f', 2))
                               .arg(QString::number(m_home.longitude, 'f', 2)));
        }
        homeText = QStringLiteral("Home: %1").arg(details.join(QStringLiteral("  |  ")));
    }
    segments.append(homeText);

    const auto selectedIt = std::find_if(m_stations.cbegin(), m_stations.cend(), [](const WeakSignalMapStation& station) {
        return station.selected && station.hasLocation;
    });
    if (selectedIt != m_stations.cend() && m_home.hasLocation) {
        const int distanceKm = static_cast<int>(std::lround(
            greatCircleDistanceKm(m_home.latitude, m_home.longitude, selectedIt->latitude, selectedIt->longitude)));
        const int bearing = static_cast<int>(std::lround(
            initialBearingDegrees(m_home.latitude, m_home.longitude, selectedIt->latitude, selectedIt->longitude)));
        QStringList selectedDetails;
        selectedDetails.append(selectedIt->callsign.trimmed().toUpper());
        if (!selectedIt->grid.trimmed().isEmpty())
            selectedDetails.append(selectedIt->grid.trimmed().toUpper());
        if (!selectedIt->country.trimmed().isEmpty())
            selectedDetails.append(selectedIt->country.trimmed());
        selectedDetails.append(QStringLiteral("%1 km").arg(distanceKm));
        selectedDetails.append(QStringLiteral("%1°").arg(bearing));
        segments.append(QStringLiteral("Selected: %1").arg(selectedDetails.join(QStringLiteral("  |  "))));
    } else {
        segments.append(QStringLiteral("Stations: %1 received").arg(m_stations.size()));
    }

    segments.append(m_mapWidget->projection() == WeakSignalMapWidget::Projection::Globe
                        ? QStringLiteral("Drag the globe to rotate.")
                        : QStringLiteral("Home station is blue and received stations are amber."));
    m_legendLabel->setText(segments.join(QStringLiteral("\n")));
}

} // namespace AetherSDR
