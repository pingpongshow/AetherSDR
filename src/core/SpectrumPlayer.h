#pragma once

#include "SpectrumRecordingFormat.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

namespace AetherSDR {

class SpectrumWidget;

// Plays an .aesr spectrum recording (all _partNNN segments) back as the same
// frame/row/range calls the live PanadapterStream → SpectrumWidget path uses.
//
// Lives on the main thread. Chunks are decoded lazily one at a time (~1 s of
// data), so memory is bounded regardless of recording length.
//
// Speeds: any positive factor (UI offers 1/2/4/8×). Above 1× only the newest
// FFT frame per tick is emitted (the spectrum trace shows one frame anyway);
// every waterfall row is emitted so the waterfall stays complete.
//
// attach(SpectrumWidget*) puts the widget into playback mode (live feed
// ignored + stashed, radio-bound interactions suppressed) and routes the
// player's signals into it; detach() restores the live view. attach/detach are
// implemented in src/gui/SpectrumPlaybackAttach.cpp so this class (and its
// tests) do not depend on the GUI library.
class SpectrumPlayer : public QObject {
    Q_OBJECT

public:
    enum class State { Closed, Stopped, Playing, Paused };
    Q_ENUM(State)

    explicit SpectrumPlayer(QObject* parent = nullptr);
    ~SpectrumPlayer() override;

    bool open(const QString& path, QString* err = nullptr);
    void close();
    bool isOpen() const { return m_reader.isOpen(); }
    QString path() const { return m_path; }
    const SpecRec::Reader& reader() const { return m_reader; }

    void play();
    void pause();
    void stop();              // pause + rewind to 0
    void seek(qint64 ms);     // media time from the start of the recording
    void setSpeed(double factor);
    void setLoop(bool on) { m_loop = on; }

    State  state() const { return m_state; }
    double speed() const { return m_speed; }
    bool   loop() const { return m_loop; }
    qint64 positionMs() const { return (m_posUs - m_reader.startUs()) / 1000; }
    qint64 durationMs() const { return m_reader.durationMs(); }
    SpecRec::PanState currentPanState() const { return m_pan; }

    // Overlay text for the widget, e.g. "PLAYBACK 2×" / "PLAYBACK ❚❚".
    QString playbackLabel() const;

    // Deterministic advance for tests / frame stepping: dispatches every
    // record with t <= start + ms. Honours speed-based coalescing rules.
    void advanceTo(qint64 ms);

    // GUI binding (see SpectrumPlaybackAttach.cpp).
    void attach(SpectrumWidget* widget);
    void detach();
    SpectrumWidget* attachedWidget() const { return m_widgetGuard ? m_widget : nullptr; }

signals:
    void spectrumFrame(const QVector<float>& binsDbm);
    void waterfallRow(const QVector<float>& bins, double lowFreqMhz, double highFreqMhz,
                      quint32 timecode);
    void panStateChanged(double centerMhz, double bandwidthMhz, float minDbm, float maxDbm);
    void sliceMarkersChanged(const QVector<AetherSDR::SpecRec::SliceMarker>& markers);
    void noteReached(const QString& text);
    void seeked(qint64 ms);   // display should be cleared
    void positionChanged(qint64 ms);
    void stateChanged(AetherSDR::SpectrumPlayer::State state);
    void labelChanged(const QString& label);
    void finished();
    void errorOccurred(const QString& message);

private:
    void onTick();
    bool loadChunk(int index);
    void dispatchUntil(qint64 targetUs, bool emitFrames);
    void setState(State s);
    qint64 wallPositionUs() const;

    SpecRec::Reader          m_reader;
    QString                  m_path;
    QTimer                   m_timer;
    QElapsedTimer            m_wall;
    qint64                   m_wallBaseUs{0};   // media position when m_wall restarted
    qint64                   m_posUs{0};        // absolute µs (file clock)
    double                   m_speed{1.0};
    bool                     m_loop{false};
    State                    m_state{State::Closed};

    int                      m_chunkIndex{-1};
    QVector<SpecRec::Record> m_records;
    int                      m_recIndex{0};

    SpecRec::PanState        m_pan;
    bool                     m_havePan{false};

    QVector<SpecRec::SliceMarker> m_slices;
    qint64                   m_lastPosEmitUs{0};

    // SpectrumWidget is incomplete here (core must not depend on gui), so the
    // guard tracks it as a QObject and the raw pointer is only dereferenced in
    // SpectrumPlaybackAttach.cpp after checking the guard.
    SpectrumWidget*          m_widget{nullptr};
    QPointer<QObject>        m_widgetGuard;
    QVector<QMetaObject::Connection> m_widgetConns;
};

} // namespace AetherSDR
