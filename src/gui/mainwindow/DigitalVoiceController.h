#pragma once

#include "gui/MainWindow.h"

#include <QObject>

#include <functional>

namespace AetherSDR {

// Digital voice (RADE V1 / codec2 FreeDV): engine lifecycle, slice/DAX setup, FreeDV Reporter publishing and the Digital Voice dialog.
// Mechanically split out of MainWindow: a QObject child of the window that
// reaches shared window state through its MainWindow reference.
class DigitalVoiceController : public QObject {
public:
    explicit DigitalVoiceController(MainWindow& mw);

    DigitalVoiceDialog* showDigitalVoiceDialog();
#ifdef HAVE_DIGITAL_VOICE
    void activateDigitalVoice(int sliceId, DigitalVoiceMode mode);
    void deactivateDigitalVoice();
    void ensureDigitalVoiceDaxStream(SliceModel* s);
    // RxApplet/VfoWidget "RADE" selection handler (on=false only affects the
    // slice that is actually running digital voice).
    void onRadeSelection(bool on, int sliceId);
    bool digitalVoiceActive() const { return m_dvSliceId >= 0 && m_dvEngine && m_dvEngine->isActive(); }
    void setDigitalVoiceReporting(bool on);
    void reportDigitalVoiceFrequency();
    QString digitalVoiceTxCallsign() const;
    void updateForMox(bool tx);
    // RADE V1 end-of-over: call INSTEAD of dropping MOX when the operator
    // releases PTT.  Returns false (caller unkeys now) unless RADE is the
    // transmitting DV mode; otherwise the EOO frame (callsign) is generated
    // and `unkey` runs once it has had time to leave the DAX TX path.
    // UNTESTED ON AIR — maintainer review required before wiring.
    bool deferUnkeyForEndOfOver(std::function<void()> unkey);
    void shutdownDigitalVoice();
    void applyVfoIndicator(VfoWidget* vfo, SliceModel* s);
    void handleSliceRemoved(int id);
#endif
#ifdef HAVE_WEBSOCKETS
#ifdef HAVE_DIGITAL_VOICE
    void onFreeDvReporterStopped();
#endif
#endif
#if defined(HAVE_DAX_BRIDGE)
#ifdef HAVE_DIGITAL_VOICE
    void onTxSliceChanged(int txSliceId);
#endif
#endif

private:
    MainWindow& m_mw;

    // Digital voice (RADE V1 / codec2 FreeDV).  One engine, one slice at a
    // time; the engine lives on m_dvThread.
    QPointer<DigitalVoiceDialog> m_dvDialog;
#ifdef HAVE_DIGITAL_VOICE
    DigitalVoiceEngine* m_dvEngine{nullptr};
    QThread*            m_dvThread{nullptr};
    int                 m_dvSliceId{-1};
    DigitalVoiceMode    m_dvMode{DigitalVoiceMode::None};
    bool                m_dvPrevMute{false};
    // Slice state saved on DV activation and restored on deactivation, plus
    // the DAX RX stream DV created itself when the DAX bridge wasn't running.
    QString             m_dvPrevMode;
    int                 m_dvPrevFilterLo{0};
    int                 m_dvPrevFilterHi{0};
    int                 m_dvPrevDaxChannel{-1};
    bool                m_dvCreatedDaxStream{false};
    int                 m_dvDaxChannel{0};
    QMetaObject::Connection m_dvDaxStatusConn;
    float               m_dvLastSnr{0.0f};
    QString             m_dvLastReportedCall;
    quint64             m_dvLastReportedFreqHz{0};
    QMetaObject::Connection m_dvFreqConn;
    bool                m_dvReporting{false};             // "Report my station" on
    bool                m_dvReporterOpenedConnection{false};
    QTimer*             m_dvFreqReportTimer{nullptr};     // debounce freq_change
#endif
};

} // namespace AetherSDR
