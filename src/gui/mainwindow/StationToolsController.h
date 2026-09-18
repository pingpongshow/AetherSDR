#pragma once

#include "gui/MainWindow.h"

#include <QObject>
#include <QPointer>

class QTimer;

namespace AetherSDR {

class RecordingPanel;
class Logbook;
class ContestEngine;
class CredentialStore;
class LogUploadQueue;
class So2rController;
class LogbookDialog;
struct SpotData;
struct SpotAnnotation;
class CwSkimmer;
class CwSkimmerPanel;
class RotatorClient;

// Station tools that are not tied to a single slice or panadapter:
// spectrum/IQ recording + playback and antenna rotator control.
// Same pattern as the other MainWindow controllers: a QObject child of the
// window that reaches shared window state through its MainWindow reference.
class StationToolsController : public QObject {
public:
    explicit StationToolsController(MainWindow& mw);

    // Rotator client + applet (call after the applet panel exists).
    void createRotator();
    RotatorClient* rotatorClient() const { return m_rotatorClient; }
    // Turn the rotator toward a callsign; a Maidenhead grid found in `hint`
    // (spot comment / logbook grid) takes precedence over the cty.dat entity.
    void turnRotatorToCallsign(const QString& call, const QString& hint, bool longPath);

    // Logbook + contest engine + online upload queue + SO2R helper.
    void createLogbook();
    void showLogbook();
    void focusLogQuickEntry();
    Logbook* logbook() const { return m_logbook; }
    So2rController* so2r() const { return m_so2r; }
    // Spot overlay annotation (dupe / new mult / worked) when enabled.
    bool spotAnnotationsEnabled() const;
    SpotAnnotation annotateSpot(const SpotData& spot) const;
    void setLogbookActiveSlice(SliceModel* slice);
    void ingestAdifRecord(const QString& adif);

    // CW skimmer over a DAX IQ stream (the user enables DAX IQ themselves).
    void createCwSkimmer();
    void showCwSkimmerPanel();
    void shutdownCwSkimmer();

    // Recording panel (lazy, top-level window).
    void showRecordingPanel();
    // Called from closeEvent before the pan stream is torn down.
    void shutdownRecording();

private:
    void ensureRecordingPanel();
    void syncRecordingPans();
    void wireRecordingPan(PanadapterModel* pan);
    void pushSliceMarkers();
    void updateSkimmerCenter();

    MainWindow& m_mw;
    RotatorClient* m_rotatorClient{nullptr};
    QPointer<RecordingPanel> m_recordingPanel;
    QTimer* m_markerTimer{nullptr};
    CwSkimmer* m_cwSkimmer{nullptr};
    Logbook* m_logbook{nullptr};
    ContestEngine* m_contest{nullptr};
    CredentialStore* m_logCreds{nullptr};
    LogUploadQueue* m_logUploads{nullptr};
    So2rController* m_so2r{nullptr};
    QPointer<LogbookDialog> m_logbookDialog;
    QPointer<CwSkimmerPanel> m_cwSkimmerPanel;
};

} // namespace AetherSDR
