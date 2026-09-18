#pragma once

#include "gui/MainWindow.h"

#include <QObject>

namespace AetherSDR {

// Slice lifecycle, active slice, VFO widgets, split, click/step tuning and pan-follow-VFO.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class SliceController : public QObject {
public:
    explicit SliceController(MainWindow& mw);

    void onSliceAdded(SliceModel* slice);
    void onSliceRemoved(int id);
    // Spectrum click-to-tune
    void onFrequencyChanged(double mhz);
    // Pan-follow-VFO (#989): if mhz is outside the visible pan window, apply
    // the new center locally (immediate repaint) and send the radio command.
    void panFollowVfo(SliceModel* s, double mhz);
    void setActiveSlice(int sliceId);
    void updateFilterLimitsForMode(const QString& mode);
    void centerActiveSliceInPanadapter(bool forceRadioCenter, double centerMhz = -1.0);
    void pushSliceOverlay(SliceModel* s);
    void updateSplitState();
    void disableSplit();
    void enableSplit(int rxSliceId, double txFreqMhz = 0.0);
    void wireVfoWidget(VfoWidget* w, SliceModel* s);
    VfoWidget* createVfoForSlice(SpectrumWidget* sw, SliceModel* s);
    void stepBand(int direction);
    BandSnapshot captureCurrentBandState() const;
    void restoreBandState(const BandSnapshot& snap);
    void wireSliceApplets();
    void wireStepSize();
    void toggleSplitFromShortcut();

private:
    MainWindow& m_mw;

    // Audio stream re-creation flag (after profile load)
    bool             m_needAudioStream{false};
    bool m_splitActive{false};
    int  m_splitRxSliceId{-1};
    double m_pendingSplitTxFreqMhz{0.0};  // CAT-requested TX freq, applied when the TX slice appears
    int  m_splitTxSliceId{-1};
    // Guard: set true while updating controls from the model, so that
    // onFrequencyChanged doesn't echo the change back to the radio.
    bool m_updatingFromModel{false};
};

} // namespace AetherSDR
