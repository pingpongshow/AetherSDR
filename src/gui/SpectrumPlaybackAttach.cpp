// GUI half of SpectrumPlayer: binds a player to a SpectrumWidget.
// Kept out of src/core/SpectrumPlayer.cpp so the core class (and its unit
// tests) build without the GUI/RHI stack.

#include "core/SpectrumPlayer.h"
#include "gui/SpectrumWidget.h"

namespace AetherSDR {

void SpectrumPlayer::attach(SpectrumWidget* widget)
{
    if (attachedWidget() == widget) { return; }
    detach();
    if (!widget) { return; }

    m_widget = widget;
    m_widgetGuard = widget;
    widget->setPlaybackMode(true, playbackLabel());

    // Push the current keyframe so the display is framed before first play.
    if (isOpen()) {
        const SpecRec::PanState ps = currentPanState();
        if (ps.bandwidthMhz > 0.0) {
            widget->playbackFrequencyRange(ps.centerMhz, ps.bandwidthMhz);
            widget->playbackDbmRange(ps.minDbm, ps.maxDbm);
        }
    }

    QPointer<SpectrumWidget> w(widget);
    m_widgetConns << connect(this, &SpectrumPlayer::spectrumFrame, widget,
                             &SpectrumWidget::playbackSpectrum);
    m_widgetConns << connect(this, &SpectrumPlayer::waterfallRow, widget,
                             &SpectrumWidget::playbackWaterfallRow);
    m_widgetConns << connect(this, &SpectrumPlayer::panStateChanged, widget,
        [w](double c, double bw, float mn, float mx) {
            if (w) {
                w->playbackFrequencyRange(c, bw);
                w->playbackDbmRange(mn, mx);
            }
        });
    m_widgetConns << connect(this, &SpectrumPlayer::seeked, widget,
                             &SpectrumWidget::playbackClear);
    m_widgetConns << connect(this, &SpectrumPlayer::labelChanged, widget,
                             &SpectrumWidget::setPlaybackLabel);
    // If the player dies while attached, hand the widget back to the live feed.
    m_widgetConns << connect(this, &QObject::destroyed, widget, [w] {
        if (w) { w->setPlaybackMode(false); }
    });
}

void SpectrumPlayer::detach()
{
    for (const QMetaObject::Connection& c : std::as_const(m_widgetConns)) {
        disconnect(c);
    }
    m_widgetConns.clear();
    if (SpectrumWidget* w = attachedWidget()) {
        w->setPlaybackMode(false);   // restores stashed live range, resumes live feed
    }
    m_widget = nullptr;
    m_widgetGuard.clear();
}

} // namespace AetherSDR
