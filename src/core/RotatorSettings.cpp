#include "RotatorSettings.h"
#include "AppSettings.h"

#include <QStringList>

#include <algorithm>

namespace AetherSDR::RotatorSettings {

namespace {
constexpr const char* kDefaultPresets = "0,45,90,135,180,225,270,315";
} // namespace

bool isEnabled()
{
    return AppSettings::instance().value(QStringLiteral("RotatorEnabled"), QStringLiteral("False"))
               .toString() == QStringLiteral("True");
}

void setEnabled(bool on)
{
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("RotatorEnabled"), on ? QStringLiteral("True") : QStringLiteral("False"));
    s.save();
}

RotatorConfig load()
{
    const auto& s = AppSettings::instance();
    RotatorConfig c;
    c.backend = RotatorConfig::backendFromString(
        s.value(QStringLiteral("RotatorBackend"), QStringLiteral("rotctld")).toString());
    c.host = s.value(QStringLiteral("RotatorHost"), QStringLiteral("127.0.0.1")).toString().trimmed();
    if (c.host.isEmpty()) {
        c.host = QStringLiteral("127.0.0.1");
    }
    const int port = s.value(QStringLiteral("RotatorPort"),
                             RotatorConfig::defaultPort(c.backend)).toInt();
    c.port = static_cast<quint16>((port > 0 && port <= 65535) ? port
                                                              : RotatorConfig::defaultPort(c.backend));
    const int listen = s.value(QStringLiteral("RotatorListenPort"), 0).toInt();
    c.listenPort = static_cast<quint16>((listen > 0 && listen <= 65535) ? listen : 0);
    c.pollIntervalMs = std::clamp(s.value(QStringLiteral("RotatorPollMs"), 1000).toInt(), 100, 60000);
    c.minAz = s.value(QStringLiteral("RotatorMinAz"), 0.0).toDouble();
    c.maxAz = s.value(QStringLiteral("RotatorMaxAz"), 360.0).toDouble();
    if (c.maxAz <= c.minAz) {
        c.minAz = 0.0;
        c.maxAz = 360.0;
    }
    c.n1mmRotorName = s.value(QStringLiteral("RotatorN1mmName"), QStringLiteral("Rotor")).toString();
    return c;
}

void save(const RotatorConfig& c)
{
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("RotatorBackend"), RotatorConfig::backendToString(c.backend));
    s.setValue(QStringLiteral("RotatorHost"), c.host);
    s.setValue(QStringLiteral("RotatorPort"), static_cast<int>(c.port));
    s.setValue(QStringLiteral("RotatorListenPort"), static_cast<int>(c.listenPort));
    s.setValue(QStringLiteral("RotatorPollMs"), c.pollIntervalMs);
    s.setValue(QStringLiteral("RotatorMinAz"), c.minAz);
    s.setValue(QStringLiteral("RotatorMaxAz"), c.maxAz);
    s.setValue(QStringLiteral("RotatorN1mmName"), c.n1mmRotorName);
    s.save();
}

QList<double> presets()
{
    const QString raw = AppSettings::instance()
                            .value(QStringLiteral("RotatorPresets"), QString::fromLatin1(kDefaultPresets))
                            .toString();
    QList<double> out;
    for (const QString& tok : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const double v = tok.trimmed().toDouble(&ok);
        if (ok) {
            out.append(GeoUtils::normalizeBearing(v));
        }
    }
    return out;
}

void setPresets(const QList<double>& list)
{
    QStringList parts;
    for (double v : list) {
        parts.append(QString::number(GeoUtils::normalizeBearing(v), 'f', 0));
    }
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("RotatorPresets"), parts.join(QLatin1Char(',')));
    s.save();
}

QString stationGrid()
{
    return AppSettings::instance().value(QStringLiteral("StationGridSquare"), QString())
        .toString().trimmed().toUpper();
}

} // namespace AetherSDR::RotatorSettings
