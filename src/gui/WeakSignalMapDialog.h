#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QLabel;
class QPushButton;

namespace AetherSDR {

class WeakSignalMapWidget;

struct WeakSignalMapStation {
    QString callsign;
    QString country;
    QString grid;
    double latitude{0.0};
    double longitude{0.0};
    bool hasLocation{false};
    bool isHome{false};
    bool selected{false};
};

class WeakSignalMapDialog : public QDialog {
    Q_OBJECT

public:
    explicit WeakSignalMapDialog(QWidget* parent = nullptr);

    void setContext(const QString& titleText);
    void setHomeStation(const WeakSignalMapStation& home);
    void setStations(const QVector<WeakSignalMapStation>& stations);

private slots:
    void toggleProjection();

private:
    void refreshLegend();

    WeakSignalMapWidget* m_mapWidget{nullptr};
    QPushButton* m_projectionButton{nullptr};
    QLabel* m_legendLabel{nullptr};
    QString m_contextTitle;
    WeakSignalMapStation m_home;
    QVector<WeakSignalMapStation> m_stations;
};

} // namespace AetherSDR
