#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace AetherSDR {

// ─────────────────────────────────────────────────────────────────────────────
// SmartSDR-CAT-compatible Kenwood / FlexRadio "ZZ" CAT protocol.
//
// Emulates the CAT language spoken by FlexRadio's Windows "SmartSDR CAT"
// (SmartSDR CAT User Guide rev v4.1.5, ch. 3): the Kenwood TS-2000 style
// 2-letter compatibility commands plus the FlexRadio 4-letter ZZ commands.
// Loggers / contest programs that are configured for "FlexRadio 6xxx"
// (Hamlib model 2036, used by WSJT-X, Log4OM, ...) or "Kenwood TS-2000"
// work unchanged against this implementation.
//
// Framing: every command and every answer is terminated by ';'. Set commands
// produce no answer. Malformed / unsupported commands, and commands whose
// target slice does not exist, answer "?;" (Kenwood convention, also what
// SmartSDR CAT returns, e.g. "Setting FR1; without VFO B will result in ?;").
//
// This file is deliberately free of RadioModel/SliceModel dependencies so it
// can be unit tested against a fake radio (tests/cat_kenwood_test.cpp).
// CatKenwoodRadioAdapter binds CatRadioInterface to the real models.
// ─────────────────────────────────────────────────────────────────────────────

// Snapshot of one slice as seen by the CAT layer.
struct CatSliceState {
    int         id{-1};
    long long   freqHz{0};
    QString     mode;              // SmartSDR mode name: USB, LSB, CW, DIGU, ...
    QStringList modeList;          // modes the radio offers (empty = unknown/any)
    int         filterLow{0};      // Hz offset from carrier
    int         filterHigh{0};
    bool        active{false};
    bool        tx{false};         // this slice carries the TX flag
    int         audioGain{50};     // 0-100
    int         audioPan{50};      // 0-100 (0 = full left)
    bool        mute{false};
    bool        ritOn{false};
    int         ritHz{0};
    bool        xitOn{false};
    int         xitHz{0};
    QString     agcMode;           // off / slow / med / fast
    int         agcThreshold{50};  // 0-100
    bool        nr{false};
    bool        wnb{false};        // wide noise blanker (panadapter-level)
    int         wnbLevel{50};      // 0-100
    bool        diversity{false};
    bool        squelchOn{false};
    int         squelchLevel{20};  // 0-100
    float       sMeterDbm{-127.0f};
};

// Abstract radio facade the CAT protocol talks to. Implemented by
// CatKenwoodRadioAdapter (RadioModel) and by a fake in the unit tests.
// All calls happen on the thread that owns the radio models (main thread).
class CatRadioInterface {
public:
    virtual ~CatRadioInterface() = default;

    // ── State ────────────────────────────────────────────────────────────
    virtual bool isConnected() const = 0;
    virtual std::vector<int> sliceIds() const = 0;
    virtual std::optional<CatSliceState> slice(int id) const = 0;
    virtual int  activeSliceId() const = 0;   // -1 if none
    virtual bool isTransmitting() const = 0;  // interlock state, NOT mox (fw v1.4.0.0 quirk)
    virtual bool isTuning() const = 0;
    virtual int  rfPower() const = 0;         // 0-100
    virtual int  micLevel() const = 0;        // 0-100
    virtual int  cwSpeed() const = 0;         // WPM
    virtual int  cwPitch() const = 0;         // Hz
    virtual bool voxEnabled() const = 0;
    virtual bool binaural() const = 0;
    virtual QString radioModelName() const = 0;  // e.g. "FLEX-6700"

    // ── Slice actions ────────────────────────────────────────────────────
    virtual void setFrequency(int sliceId, long long hz) = 0;
    virtual void setMode(int sliceId, const QString& mode) = 0;
    virtual void setFilter(int sliceId, int low, int high) = 0;
    virtual void setAudioGain(int sliceId, int gain) = 0;
    virtual void setAudioPan(int sliceId, int pan) = 0;
    virtual void setMute(int sliceId, bool mute) = 0;
    virtual void setRit(int sliceId, bool on, int hz) = 0;
    virtual void setXit(int sliceId, bool on, int hz) = 0;
    virtual void setAgcMode(int sliceId, const QString& mode) = 0;
    virtual void setAgcThreshold(int sliceId, int value) = 0;
    virtual void setNr(int sliceId, bool on) = 0;
    virtual void setWnb(int sliceId, bool on) = 0;
    virtual void setWnbLevel(int sliceId, int level) = 0;
    virtual void setDiversity(int sliceId, bool on) = 0;
    virtual void setSquelch(int sliceId, bool on, int level) = 0;
    virtual void setTxSlice(int sliceId, bool on) = 0;
    virtual void setActiveSlice(int sliceId) = 0;
    // Ask the GUI to enter/leave split for rxSliceId (creates / removes the
    // split TX slice — same path as the VFO SPLIT button and rigctld).
    virtual void requestSplit(int rxSliceId, bool on, double txFreqMhz) = 0;

    // ── Radio actions ────────────────────────────────────────────────────
    virtual void setTransmit(bool tx) = 0;   // RadioModel::setTransmit
    virtual void setTune(bool on) = 0;       // transmit tune 1/0
    virtual void setRfPower(int power) = 0;
    virtual void setMicLevel(int level) = 0;
    virtual void setCwSpeed(int wpm) = 0;
    virtual void setCwPitch(int hz) = 0;
    virtual void setVox(bool on) = 0;
    virtual void setBinaural(bool on) = 0;
    virtual void sendCw(const QString& text) = 0;
    virtual void clearCw() = 0;
};

// Per-port protocol state machine. One instance per TCP client / PTY.
// No I/O: feed() bytes in, get answer bytes out.
class CatKenwoodProtocol {
public:
    // VFO A binding: a fixed slice id (0-7 = Slice A-H) or the GUI's
    // currently active slice.
    static constexpr int kActiveSlice = -1;

    // What FT0; / ZZSW0; does with the split slice (SmartSDR CAT "Split Mode").
    enum class SplitMode {
        So2r,   // remove the split slice (SmartSDR default)
        So2v,   // leave it open as a regular VFO B
    };

    explicit CatKenwoodProtocol(CatRadioInterface* radio);

    // ── Port configuration ───────────────────────────────────────────────
    void setVfoASlice(int sliceIdOrActive) { m_vfoABinding = sliceIdOrActive; }
    int  vfoASlice() const                 { return m_vfoABinding; }
    // SmartSDR CAT "Auto Switch TX Slice": keying via this port moves the TX
    // flag to this port's VFO A slice (unless the port is in split).
    void setAutoSwitchTxSlice(bool on)     { m_autoSwitchTx = on; }
    bool autoSwitchTxSlice() const         { return m_autoSwitchTx; }
    void setSplitMode(SplitMode m)         { m_splitMode = m; }
    SplitMode splitMode() const            { return m_splitMode; }

    // ── I/O ──────────────────────────────────────────────────────────────
    // Append raw bytes from the client; every complete ';'-terminated
    // command is executed. Returns the concatenated answers (may be empty).
    QByteArray feed(const QByteArray& bytes);

    // Execute one command (without the trailing ';'). Returns the answer
    // including ';', or an empty array for set commands.
    QByteArray handleCommand(const QByteArray& cmd);

    // Auto-information: when AI is on, returns unsolicited FA/FB/MD/IF
    // answers for whatever changed since the last call. Call after any
    // radio state change (the servers coalesce model signals into this).
    QByteArray pollAutoInfo();
    bool autoInfoEnabled() const { return m_aiMode != 0; }

    // Resolved logical VFOs (slice ids, or -1). Exposed for UI/diagnostics
    // (e.g. SmartSDR's "Slice Not Present" indicator).
    int vfoASliceId() const;
    int vfoBSliceId() const;

    // ── Pure helpers (public for tests) ──────────────────────────────────
    static int      modeToMd(const QString& mode);            // MD digit
    static QString  mdToMode(int md);                         // "" if invalid
    static int      modeToZzmd(const QString& mode);          // ZZMD 2-digit code
    static QString  zzmdToMode(int code);                     // "" if invalid
    static int      filterIndexForWidth(const QString& mode, int widthHz);
    static int      filterWidthForIndex(const QString& mode, int index); // -1 if invalid
    static int      sMeterKenwood(float dbm);                 // 0-30, S9 = 15
    static int      sMeterZz(float dbm);                      // 0-260, (dBm+140)*2
    static int      radioIdForModel(const QString& model);    // ID; table
    static QByteArray formatFreq(long long hz, int digits = 11);

    static constexpr qsizetype kMaxBufferBytes = 4096;

private:
    // Command groups
    QByteArray cmdFreq(const QByteArray& name, const QByteArray& arg, bool vfoB);
    QByteArray cmdMode(const QByteArray& name, const QByteArray& arg, bool zz, bool vfoB);
    QByteArray cmdInfo(bool zz);
    QByteArray cmdRxVfo(const QByteArray& arg);               // FR
    QByteArray cmdTxVfo(const QByteArray& name, const QByteArray& arg); // FT / ZZSW
    QByteArray cmdToggleRx();                                 // ZZFR
    QByteArray cmdToggleTx();                                 // ZZFT
    QByteArray cmdTx(const QByteArray& arg);                  // TX
    QByteArray cmdZzTx(const QByteArray& arg);                // ZZTX
    QByteArray cmdRx();                                       // RX / ZZRX1
    QByteArray cmdRitStep(const QByteArray& arg, int sign);   // RU / RD
    QByteArray cmdRitClear();                                 // RC
    QByteArray cmdOffsetValue(const QByteArray& name, const QByteArray& arg,
                              bool vfoB, bool xit);           // ZZRG / ZZRW / ZZXG
    QByteArray cmdAudioGain(const QByteArray& arg);           // AG (SmartSDR + TS-2000 forms)
    QByteArray cmdAgc(const QByteArray& name, const QByteArray& arg);
    QByteArray cmdFilterIndex(const QByteArray& name, const QByteArray& arg, bool vfoB);
    QByteArray cmdFilterCut(const QByteArray& arg, bool high); // SH / SL
    QByteArray cmdSMeter(const QByteArray& arg, bool zz);
    QByteArray cmdKey(const QByteArray& rawArg);               // KY
    QByteArray cmdSquelch(const QByteArray& arg);              // SQ
    QByteArray cmdAi(const QByteArray& name, const QByteArray& arg);

    // Generic helpers for "Get → NAME + fixed-width number / Set NAME + digits".
    template <typename Getter, typename Setter>
    QByteArray numeric(const QByteArray& name, const QByteArray& arg, int width,
                       int minVal, int maxVal, Getter get, Setter set);
    template <typename Getter, typename Setter>
    QByteArray boolean(const QByteArray& name, const QByteArray& arg,
                       Getter get, Setter set);

    std::optional<CatSliceState> vfoA() const;
    std::optional<CatSliceState> vfoB() const;
    bool splitActive() const;                 // TX flag on VFO B
    void adoptSplitSlice();
    bool transmitting() const;
    int  defaultRitStep(const QString& mode) const;
    void keyTransmit();

    static QByteArray err() { return QByteArrayLiteral("?;"); }
    static bool parseUInt(const QByteArray& s, int& out);
    static bool parseSignedOffset(const QByteArray& s, int& out);  // "+nnnnn" / "-nnnnn"
    static QByteArray pad(long long v, int width);
    static QByteArray formatOffset(int hz);                        // "+nnnnn"

    struct AiSnapshot {
        bool      valid{false};
        long long freqA{-1};
        long long freqB{-1};
        int       md{-1};
        bool      tx{false};
    };
    AiSnapshot takeSnapshot() const;

    CatRadioInterface* m_radio;
    int        m_vfoABinding{0};
    bool       m_autoSwitchTx{true};
    SplitMode  m_splitMode{SplitMode::So2r};

    QByteArray m_buffer;
    int        m_freqDigits{11};     // "May switch to 8 digit mode by doing an 8 digit set"
    int        m_aiMode{0};
    AiSnapshot m_aiLast;
    bool       m_rxOnVfoB{false};    // FR1 state
    bool       m_splitRequested{false};
    int        m_lastVfoB{-1};       // remembered split slice (SO2V)
    long long  m_pendingVfoBHz{0};   // FB set before VFO B exists
};

} // namespace AetherSDR
