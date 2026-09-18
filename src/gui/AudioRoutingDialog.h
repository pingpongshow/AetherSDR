#pragma once

#include "core/SliceAudioRouting.h"

#include <QDialog>
#include <QMap>
#include <QStringList>

class QGridLayout;
class QLabel;

namespace AetherSDR {

class RadioModel;

// "Per-slice audio routing" panel.  Pure view over the client-side routing
// choice per slice letter (AppSettings SliceAudioRoute_<letter>); the
// controller (MainWindow) turns the saved routes into a plan and applies it
// (AudioEngine::setSliceAudioRoutes, DAX feeds, radio mutes), then pushes
// the plan's warnings back with setPlanWarnings().
//
// Volume, pan and mute stay on the radio (slice audio_level / audio_pan /
// audio_mute) and are not shown or persisted here.
class AudioRoutingDialog : public QDialog {
    Q_OBJECT

public:
    explicit AudioRoutingDialog(RadioModel* model, QWidget* parent = nullptr);

    // Persistence helpers (AppSettings).  Default routes are not stored.
    static QMap<QChar, SliceAudioRoute> loadRoutes();
    static void saveRoute(QChar sliceLetter, const SliceAudioRoute& route);

public slots:
    void refresh();                              // slices / devices changed
    void setPlanWarnings(const QStringList& warnings);

signals:
    void routesChanged();                        // something was saved

protected:
    void showEvent(QShowEvent* ev) override;

private:
    void rebuildRows();

    RadioModel*  m_model;
    QGridLayout* m_grid{nullptr};
    QLabel*      m_warnLabel{nullptr};
};

} // namespace AetherSDR
