#pragma once

// WSJT-X UDP "NetworkMessage" codec — pure, no sockets, unit-testable.
//
// Wire format reference (authoritative):
//   WSJT-X Network/NetworkMessage.hpp  (header comment block = the spec)
//   WSJT-X Network/NetworkMessage.cpp  (Builder/Reader: magic, schema,
//                                       QDataStream version selection)
//   WSJT-X Network/MessageClient.cpp   (what WSJT-X actually serialises)
//   WSJT-X UDPExamples/MessageServer.cpp (reference server: schema
//                                         negotiation, reply addressing)
// Upstream: https://sourceforge.net/p/wsjt/wsjtx/ (master, fetched
// 2026-09; adds AnnotationInfo=16 and the "CLEARALL!" highlight rule over
// the 2.x text mirrored at github.com/saitohirga/WSJT-X).
//
// Summary of the framing:
//   quint32 magic 0xadbccbda, quint32 schema, quint32 type, utf8 id, payload
//   All big-endian QDataStream.  Schema 2 = QDataStream::Qt_5_2,
//   schema 3 = QDataStream::Qt_5_4.  "utf8" = QByteArray (quint32 length,
//   0xffffffff = null).  Floating point is written at double precision
//   (QDataStream default), so the "float" Delta-time field is 8 bytes.
//
// Compatibility rules from the spec that this codec honours:
//   * Unknown message types are ignored (decode() returns Type::Unknown).
//   * New fields are only ever appended — trailing fields we don't know are
//     ignored, and trailing fields that are *missing* (older WSJT-X, JTDX,
//     wsjtx-improved variants) are left at their defaults.
//
// JTDX speaks the same binary protocol (it is a WSJT-X fork).  JS8Call's
// UDP API is JSON (default port 2242) and is NOT decoded here; such
// datagrams fail the magic check and are ignored (but still relayed).

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QTime>

#include <optional>
#include <variant>

namespace AetherSDR::Wsjtx {

inline constexpr quint32 kMagic = 0xadbccbdaU;       // never changes
inline constexpr quint32 kMaxSchema = 3;             // Qt_5_4 encoding
inline constexpr quint32 kPreNegotiationSchema = 2;  // schema 1 is broken
inline constexpr quint16 kDefaultPort = 2237;
inline constexpr int kPulseSeconds = 15;             // NetworkMessage::pulse
inline constexpr quint32 kNotApplicable = 0xffffffffU; // "max quint32" sentinel
inline constexpr const char* kClearAllHighlights = "CLEARALL!";

// Enum values are the on-wire type numbers — NEVER reorder.
enum class Type : quint32 {
    Heartbeat = 0,
    Status = 1,
    Decode = 2,
    Clear = 3,
    Reply = 4,
    QsoLogged = 5,
    Close = 6,
    Replay = 7,
    HaltTx = 8,
    FreeText = 9,
    WsprDecode = 10,
    Location = 11,
    LoggedAdif = 12,
    HighlightCallsign = 13,
    SwitchConfiguration = 14,
    Configure = 15,
    AnnotationInfo = 16,           // upstream master only (Fox-mode hound sort order)
    Unknown = 0xffffffffU,
};

QString typeName(Type t);

// QColor as serialised by QDataStream (spec byte + 5×quint16).  Kept here
// so the codec only needs QtCore.  valid=false encodes an invalid QColor,
// which WSJT-X interprets as "clear this highlight".
struct Color {
    bool valid{false};
    quint8 r{0};
    quint8 g{0};
    quint8 b{0};
    quint8 a{255};

    static Color rgb(quint8 r, quint8 g, quint8 b, quint8 a = 255)
    {
        return Color{true, r, g, b, a};
    }
    static Color invalid() { return Color{}; }
    bool operator==(const Color&) const = default;
};

// ── Message bodies (field order == wire order) ─────────────────────────────

struct Heartbeat {                 // Out/In 0
    quint32 maxSchema{kPreNegotiationSchema}; // absent in very old clients → 2
    QString version;
    QString revision;
};

struct Status {                    // Out 1
    quint64 dialFrequencyHz{0};
    QString mode;
    QString dxCall;
    QString report;
    QString txMode;
    bool txEnabled{false};
    bool transmitting{false};
    bool decoding{false};
    quint32 rxDf{0};
    quint32 txDf{0};
    QString deCall;
    QString deGrid;
    QString dxGrid;
    bool txWatchdog{false};
    QString subMode;
    bool fastMode{false};
    quint8 specialOperationMode{0}; // 0 NONE,1 NA VHF,2 EU VHF,3 FD,4 RTTY RU,5 WW DIGI,6 FOX,7 HOUND,8 ARRL DIGI
    quint32 frequencyTolerance{kNotApplicable};
    quint32 trPeriod{kNotApplicable}; // seconds
    QString configurationName;
    QString txMessage;
};

struct Decode {                    // Out 2
    bool isNew{true};
    QTime time;                    // UTC, ms since midnight
    qint32 snr{0};
    double deltaTime{0.0};         // seconds
    quint32 deltaFrequency{0};     // Hz audio offset
    QString mode;                  // one-char mode symbol, e.g. "~" = FT8
    QString message;
    bool lowConfidence{false};
    bool offAir{false};
};

struct Clear {                     // Out/In 3
    std::optional<quint8> window;  // In only: 0 band activity, 1 rx freq, 2 both
};

struct Reply {                     // In 4
    QTime time;
    qint32 snr{0};
    double deltaTime{0.0};
    quint32 deltaFrequency{0};
    QString mode;
    QString message;
    bool lowConfidence{false};
    quint8 modifiers{0};           // 0x02 SHIFT, 0x04 CTRL, 0x08 ALT, 0x10 META, 0x20 KEYPAD, 0x40 GROUP
};

struct QsoLogged {                 // Out 5
    QDateTime dateTimeOff;
    QString dxCall;
    QString dxGrid;
    quint64 txFrequencyHz{0};      // WSJT-X actually sends the *dial* frequency
    QString mode;
    QString reportSent;
    QString reportReceived;
    QString txPower;
    QString comments;
    QString name;
    QDateTime dateTimeOn;
    QString operatorCall;
    QString myCall;
    QString myGrid;
    QString exchangeSent;
    QString exchangeReceived;
    QString propagationMode;
};

struct Close {};                   // Out/In 6
struct Replay {};                  // In 7

struct HaltTx {                    // In 8
    bool autoTxOnly{false};
};

struct FreeText {                  // In 9
    QString text;
    bool send{false};
};

struct WsprDecode {                // Out 10
    bool isNew{true};
    QTime time;
    qint32 snr{0};
    double deltaTime{0.0};
    quint64 frequencyHz{0};
    qint32 drift{0};
    QString callsign;
    QString grid;
    qint32 powerDbm{0};
    bool offAir{false};
};

struct Location {                  // In 11
    QString location;              // 4- or 6-char Maidenhead
};

struct LoggedAdif {                // Out 12
    QString adif;
};

struct HighlightCallsign {         // In 13
    // Callsign "CLEARALL!" (any colours) clears WSJT-X's internal highlight
    // table (upstream master); on-screen highlighting is not removed.
    QString callsign;
    Color background;
    Color foreground;
    bool highlightLast{false};
};

struct SwitchConfiguration {       // In 14
    QString configurationName;
};

struct Configure {                 // In 15
    QString mode;
    quint32 frequencyTolerance{kNotApplicable};
    QString subMode;
    bool fastMode{false};
    quint32 trPeriod{kNotApplicable};
    quint32 rxDf{kNotApplicable};
    QString dxCall;
    QString dxGrid;
    bool generateMessages{false};
};

struct AnnotationInfo {            // In 16
    QString dxCall;
    bool sortOrderProvided{false};
    quint32 sortOrder{kNotApplicable}; // 0xffffffff removes the entry
};

using Body = std::variant<std::monostate, Heartbeat, Status, Decode, Clear, Reply,
                          QsoLogged, Close, Replay, HaltTx, FreeText, WsprDecode,
                          Location, LoggedAdif, HighlightCallsign,
                          SwitchConfiguration, Configure, AnnotationInfo>;

struct Message {
    quint32 schema{kMaxSchema};
    Type type{Type::Unknown};
    QString id;                    // WSJT-X instance id ("WSJT-X", "WSJT-X - rig2", "JTDX", ...)
    Body body;
    bool truncated{false};         // some trailing fields were absent (older client)
};

// Decode one datagram.  Returns std::nullopt on bad magic / unsupported
// schema / corrupt header.  Unknown types return a Message with
// type == Type::Unknown and an empty body.
std::optional<Message> decode(const QByteArray& datagram, QString* error = nullptr);

// Encode a message.  The Type is inferred from the body alternative; the
// schema is clamped to [2, kMaxSchema].  Returns an empty array for
// std::monostate.
QByteArray encode(const QString& id, const Body& body, quint32 schema = kMaxSchema);

// Cheap header peek (no payload parse) — used by the relay path.
bool peekHeader(const QByteArray& datagram, quint32* schema, Type* type, QString* id);

// ── Helpers ────────────────────────────────────────────────────────────────

// Decode.mode is a single symbol in WSJT-X ("~" FT8, "+" FT4, "#" JT65,
// "@" JT9, "&" MSK144, "$" JT4, "`" FST4, ":" Q65).  Returns "" if unknown.
QString modeFromSymbol(const QString& symbol);

// Nominal T/R period in seconds for a mode name (FT8 15, FT4 7.5, ...).
double nominalPeriodSeconds(const QString& mode);

// Structured view of an FT8/FT4-style standard message text.
struct ParsedText {
    bool isCq{false};              // "CQ ..." or "QRZ ..."
    QString cqModifier;            // "DX", "POTA", "NA", "123", ...
    QString deCall;                // the transmitting station (spot callsign)
    QString toCall;                // addressee for directed messages
    QString grid;                  // 4-char grid if present
    QString report;                // "-12", "R-05", "RR73", "RRR", "73"
    bool isFreeText{false};        // could not be parsed as a standard message
};
ParsedText parseMessageText(const QString& text);

// Rough callsign plausibility test (A-Z/0-9 and '/', has a digit and a letter).
bool looksLikeCallsign(const QString& token);

// Band name ("20m") for a frequency in Hz; "" if outside amateur bands.
QString bandForFrequencyHz(quint64 hz);

// Build a single ADIF record (terminated by <EOR>) from a QSOLogged message,
// mirroring the fields WSJT-X writes in its own LoggedADIF output.
QString adifFromQsoLogged(const QsoLogged& q);

// Extract the record(s) portion (after <EOH>) of a LoggedADIF payload.
// If there is no header the whole text is returned trimmed.
QString adifRecordFromLoggedAdif(const QString& adifFile);

} // namespace AetherSDR::Wsjtx
