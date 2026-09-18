#pragma once

#include "gui/MainWindow.h"

#include <QHash>
#include <QObject>
#include <QPointer>

namespace AetherSDR {

// Audio routing: TX audio streams and mic source, RX audio taps (QSO recorder), title-bar PC audio / master / headphone volume.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class AudioRoutingController : public QObject {
public:
    explicit AudioRoutingController(MainWindow& mw);

    // Master volume routing shared by the title bar and MIDI (PC Audio on →
    // AudioEngine RX volume; off → radio lineout gain).
    void applyMasterVolume(int pct);
    int  masterVolumePercent() const;
    void syncMasterVolumeControl();
    void wireTxAudioStreams();
    void wireRxAudioTaps();
    void wireTitleBarAudio();

    // Per-slice RX audio routing (SliceAudioRouting.h).  Idempotent: call on
    // start-up, when the dialog saves, and when slices / DAX channels change.
    void applySliceAudioRouting();
    void showSliceAudioRoutingDialog();
    void restoreSliceRoutingMutes();   // on disconnect / shutdown
    // Re-apply routing on connect / slice add+remove / DAX channel change.
    void wireSliceRoutingTriggers();

private:
    void ensureSliceDaxStream(int daxChannel);

    MainWindow& m_mw;
    QPointer<class AudioRoutingDialog> m_sliceRoutingDialog;
    QHash<int, bool> m_routingMutedSlices;         // sliceId → mute before we muted it
    QMetaObject::Connection m_sliceDaxFeedConn;    // daxAudioReady → AudioEngine
    QMetaObject::Connection m_sliceBridgeConn;     // AudioEngine → DAX bridge
    QMetaObject::Connection m_sliceDaxStatusConn;
    QList<QMetaObject::Connection> m_sliceGainConns;
};

} // namespace AetherSDR
