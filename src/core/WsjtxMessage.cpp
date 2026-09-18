#include "WsjtxMessage.h"

#include <QDataStream>
#include <QIODevice>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <array>

namespace AetherSDR::Wsjtx {

namespace {

// Schema → QDataStream version, exactly as NetworkMessage.cpp
// common_initialization() does (schema 1 = Qt_5_0 is broken, not supported).
QDataStream::Version streamVersionForSchema(quint32 schema)
{
    if (schema <= 2) {
        return QDataStream::Qt_5_2;
    }
    return QDataStream::Qt_5_4;
}

quint32 clampSchema(quint32 schema)
{
    return std::clamp<quint32>(schema, kPreNegotiationSchema, kMaxSchema);
}

// ── Reading helpers ─────────────────────────────────────────────────────────
// Each reader returns false once the stream is exhausted so that trailing
// fields that an older client never sent simply keep their defaults.

class FieldReader {
public:
    explicit FieldReader(QDataStream& s) : m_s(s) {}

    bool ok() const { return m_ok; }
    bool truncated() const { return m_truncated; }

    template <typename T>
    FieldReader& operator>>(T& out)
    {
        if (!m_ok) {
            return *this;
        }
        if (m_s.atEnd()) {
            m_ok = false;
            m_truncated = true;
            return *this;
        }
        T tmp{};
        m_s >> tmp;
        if (m_s.status() != QDataStream::Ok) {
            m_ok = false;
            m_truncated = true;
            return *this;
        }
        out = tmp;
        return *this;
    }

    FieldReader& operator>>(QString& out)
    {
        QByteArray utf8;
        const bool before = m_ok;
        *this >> utf8;
        if (before && m_ok) {
            out = QString::fromUtf8(utf8);
        }
        return *this;
    }

    FieldReader& operator>>(Color& out)
    {
        // QDataStream QColor layout (stream version >= 7):
        //   qint8 spec, quint16 alpha, red, green, blue, pad
        qint8 spec = 0;
        std::array<quint16, 5> v{};
        *this >> spec;
        for (quint16& c : v) {
            *this >> c;
        }
        if (!m_ok) {
            return *this;
        }
        if (spec == 1) {  // QColor::Rgb
            out = Color::rgb(static_cast<quint8>(v[1] >> 8), static_cast<quint8>(v[2] >> 8),
                             static_cast<quint8>(v[3] >> 8), static_cast<quint8>(v[0] >> 8));
        } else if (spec == 0) {
            out = Color::invalid();
        } else {
            // Other specs (HSV/CMYK/...) are not produced by our peers; treat
            // as valid-but-unknown by keeping the alpha only.
            out = Color::rgb(0, 0, 0, static_cast<quint8>(v[0] >> 8));
        }
        return *this;
    }

private:
    QDataStream& m_s;
    bool m_ok{true};
    bool m_truncated{false};
};

// ── Writing helpers ─────────────────────────────────────────────────────────

void writeUtf8(QDataStream& s, const QString& str)
{
    // WSJT-X always writes QString::toUtf8(); a null QString therefore goes
    // out as a null QByteArray (length 0xffffffff), an empty one as length 0.
    s << str.toUtf8();
}

void writeColor(QDataStream& s, const Color& c)
{
    if (!c.valid) {
        // Byte-identical to QDataStream << QColor(): Invalid spec, alpha 0xffff.
        s << qint8(0) << quint16(0xffff) << quint16(0) << quint16(0) << quint16(0) << quint16(0);
        return;
    }
    auto wide = [](quint8 v) { return static_cast<quint16>(v * 0x101); };
    s << qint8(1) << wide(c.a) << wide(c.r) << wide(c.g) << wide(c.b) << quint16(0);
}

struct Encoder {
    QDataStream& s;

    Type operator()(const std::monostate&) const { return Type::Unknown; }

    Type operator()(const Heartbeat& m) const
    {
        s << m.maxSchema;
        writeUtf8(s, m.version);
        writeUtf8(s, m.revision);
        return Type::Heartbeat;
    }

    Type operator()(const Status& m) const
    {
        s << m.dialFrequencyHz;
        writeUtf8(s, m.mode);
        writeUtf8(s, m.dxCall);
        writeUtf8(s, m.report);
        writeUtf8(s, m.txMode);
        s << m.txEnabled << m.transmitting << m.decoding << m.rxDf << m.txDf;
        writeUtf8(s, m.deCall);
        writeUtf8(s, m.deGrid);
        writeUtf8(s, m.dxGrid);
        s << m.txWatchdog;
        writeUtf8(s, m.subMode);
        s << m.fastMode << m.specialOperationMode << m.frequencyTolerance << m.trPeriod;
        writeUtf8(s, m.configurationName);
        writeUtf8(s, m.txMessage);
        return Type::Status;
    }

    Type operator()(const Decode& m) const
    {
        s << m.isNew << m.time << m.snr << m.deltaTime << m.deltaFrequency;
        writeUtf8(s, m.mode);
        writeUtf8(s, m.message);
        s << m.lowConfidence << m.offAir;
        return Type::Decode;
    }

    Type operator()(const Clear& m) const
    {
        if (m.window.has_value()) {
            s << *m.window;
        }
        return Type::Clear;
    }

    Type operator()(const Reply& m) const
    {
        s << m.time << m.snr << m.deltaTime << m.deltaFrequency;
        writeUtf8(s, m.mode);
        writeUtf8(s, m.message);
        s << m.lowConfidence << m.modifiers;
        return Type::Reply;
    }

    Type operator()(const QsoLogged& m) const
    {
        s << m.dateTimeOff;
        writeUtf8(s, m.dxCall);
        writeUtf8(s, m.dxGrid);
        s << m.txFrequencyHz;
        writeUtf8(s, m.mode);
        writeUtf8(s, m.reportSent);
        writeUtf8(s, m.reportReceived);
        writeUtf8(s, m.txPower);
        writeUtf8(s, m.comments);
        writeUtf8(s, m.name);
        s << m.dateTimeOn;
        writeUtf8(s, m.operatorCall);
        writeUtf8(s, m.myCall);
        writeUtf8(s, m.myGrid);
        writeUtf8(s, m.exchangeSent);
        writeUtf8(s, m.exchangeReceived);
        writeUtf8(s, m.propagationMode);
        return Type::QsoLogged;
    }

    Type operator()(const Close&) const { return Type::Close; }
    Type operator()(const Replay&) const { return Type::Replay; }

    Type operator()(const HaltTx& m) const
    {
        s << m.autoTxOnly;
        return Type::HaltTx;
    }

    Type operator()(const FreeText& m) const
    {
        writeUtf8(s, m.text);
        s << m.send;
        return Type::FreeText;
    }

    Type operator()(const WsprDecode& m) const
    {
        s << m.isNew << m.time << m.snr << m.deltaTime << m.frequencyHz << m.drift;
        writeUtf8(s, m.callsign);
        writeUtf8(s, m.grid);
        s << m.powerDbm << m.offAir;
        return Type::WsprDecode;
    }

    Type operator()(const Location& m) const
    {
        writeUtf8(s, m.location);
        return Type::Location;
    }

    Type operator()(const LoggedAdif& m) const
    {
        writeUtf8(s, m.adif);
        return Type::LoggedAdif;
    }

    Type operator()(const HighlightCallsign& m) const
    {
        writeUtf8(s, m.callsign);
        writeColor(s, m.background);
        writeColor(s, m.foreground);
        s << m.highlightLast;
        return Type::HighlightCallsign;
    }

    Type operator()(const SwitchConfiguration& m) const
    {
        writeUtf8(s, m.configurationName);
        return Type::SwitchConfiguration;
    }

    Type operator()(const Configure& m) const
    {
        writeUtf8(s, m.mode);
        s << m.frequencyTolerance;
        writeUtf8(s, m.subMode);
        s << m.fastMode << m.trPeriod << m.rxDf;
        writeUtf8(s, m.dxCall);
        writeUtf8(s, m.dxGrid);
        s << m.generateMessages;
        return Type::Configure;
    }

    Type operator()(const AnnotationInfo& m) const
    {
        writeUtf8(s, m.dxCall);
        s << m.sortOrderProvided << m.sortOrder;
        return Type::AnnotationInfo;
    }
};

Type typeForBody(const Body& body)
{
    // Map the variant index to the wire type without serialising anything.
    static constexpr std::array<Type, 18> kTypes{
        Type::Unknown, Type::Heartbeat, Type::Status, Type::Decode, Type::Clear,
        Type::Reply, Type::QsoLogged, Type::Close, Type::Replay, Type::HaltTx,
        Type::FreeText, Type::WsprDecode, Type::Location, Type::LoggedAdif,
        Type::HighlightCallsign, Type::SwitchConfiguration, Type::Configure,
        Type::AnnotationInfo};
    static_assert(std::variant_size_v<Body> == kTypes.size());
    return kTypes[body.index()];
}

bool readHeader(QDataStream& s, quint32* schema, quint32* rawType, QByteArray* id, QString* error)
{
    s.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0;
    s >> magic;
    if (s.status() != QDataStream::Ok || magic != kMagic) {
        if (error) {
            *error = QStringLiteral("bad magic");
        }
        return false;
    }
    s >> *schema;
    if (s.status() != QDataStream::Ok || *schema < kPreNegotiationSchema || *schema > kMaxSchema) {
        if (error) {
            *error = QStringLiteral("unsupported schema %1").arg(*schema);
        }
        return false;
    }
    s.setVersion(streamVersionForSchema(*schema));
    s >> *rawType >> *id;
    if (s.status() != QDataStream::Ok) {
        if (error) {
            *error = QStringLiteral("truncated header");
        }
        return false;
    }
    return true;
}

} // namespace

QString typeName(Type t)
{
    switch (t) {
    case Type::Heartbeat:           return QStringLiteral("Heartbeat");
    case Type::Status:              return QStringLiteral("Status");
    case Type::Decode:              return QStringLiteral("Decode");
    case Type::Clear:               return QStringLiteral("Clear");
    case Type::Reply:               return QStringLiteral("Reply");
    case Type::QsoLogged:           return QStringLiteral("QSOLogged");
    case Type::Close:               return QStringLiteral("Close");
    case Type::Replay:              return QStringLiteral("Replay");
    case Type::HaltTx:              return QStringLiteral("HaltTx");
    case Type::FreeText:            return QStringLiteral("FreeText");
    case Type::WsprDecode:          return QStringLiteral("WSPRDecode");
    case Type::Location:            return QStringLiteral("Location");
    case Type::LoggedAdif:          return QStringLiteral("LoggedADIF");
    case Type::HighlightCallsign:   return QStringLiteral("HighlightCallsign");
    case Type::SwitchConfiguration: return QStringLiteral("SwitchConfiguration");
    case Type::Configure:           return QStringLiteral("Configure");
    case Type::AnnotationInfo:      return QStringLiteral("AnnotationInfo");
    case Type::Unknown:             break;
    }
    return QStringLiteral("Unknown");
}

bool peekHeader(const QByteArray& datagram, quint32* schema, Type* type, QString* id)
{
    QDataStream s(datagram);
    quint32 sc = 0;
    quint32 rawType = 0;
    QByteArray rawId;
    if (!readHeader(s, &sc, &rawType, &rawId, nullptr)) {
        return false;
    }
    if (schema) {
        *schema = sc;
    }
    if (type) {
        *type = rawType <= static_cast<quint32>(Type::AnnotationInfo) ? static_cast<Type>(rawType)
                                                                   : Type::Unknown;
    }
    if (id) {
        *id = QString::fromUtf8(rawId);
    }
    return true;
}

std::optional<Message> decode(const QByteArray& datagram, QString* error)
{
    QDataStream s(datagram);
    quint32 schema = 0;
    quint32 rawType = 0;
    QByteArray rawId;
    if (!readHeader(s, &schema, &rawType, &rawId, error)) {
        return std::nullopt;
    }

    Message msg;
    msg.schema = schema;
    msg.id = QString::fromUtf8(rawId);
    if (rawType > static_cast<quint32>(Type::AnnotationInfo)) {
        msg.type = Type::Unknown;   // spec rule 1: ignore unknown types
        return msg;
    }
    msg.type = static_cast<Type>(rawType);

    FieldReader r(s);
    switch (msg.type) {
    case Type::Heartbeat: {
        Heartbeat m;
        r >> m.maxSchema >> m.version >> m.revision;
        msg.body = m;
        break;
    }
    case Type::Status: {
        Status m;
        r >> m.dialFrequencyHz >> m.mode >> m.dxCall >> m.report >> m.txMode
          >> m.txEnabled >> m.transmitting >> m.decoding >> m.rxDf >> m.txDf
          >> m.deCall >> m.deGrid >> m.dxGrid >> m.txWatchdog >> m.subMode
          >> m.fastMode >> m.specialOperationMode >> m.frequencyTolerance
          >> m.trPeriod >> m.configurationName >> m.txMessage;
        msg.body = m;
        break;
    }
    case Type::Decode: {
        Decode m;
        r >> m.isNew >> m.time >> m.snr >> m.deltaTime >> m.deltaFrequency >> m.mode
          >> m.message >> m.lowConfidence >> m.offAir;
        msg.body = m;
        break;
    }
    case Type::Clear: {
        Clear m;
        quint8 window = 0;
        if (!s.atEnd()) {
            r >> window;
            if (r.ok()) {
                m.window = window;
            }
        }
        msg.body = m;
        break;
    }
    case Type::Reply: {
        Reply m;
        r >> m.time >> m.snr >> m.deltaTime >> m.deltaFrequency >> m.mode >> m.message
          >> m.lowConfidence >> m.modifiers;
        msg.body = m;
        break;
    }
    case Type::QsoLogged: {
        QsoLogged m;
        r >> m.dateTimeOff >> m.dxCall >> m.dxGrid >> m.txFrequencyHz >> m.mode
          >> m.reportSent >> m.reportReceived >> m.txPower >> m.comments >> m.name
          >> m.dateTimeOn >> m.operatorCall >> m.myCall >> m.myGrid >> m.exchangeSent
          >> m.exchangeReceived >> m.propagationMode;
        msg.body = m;
        break;
    }
    case Type::Close:
        msg.body = Close{};
        break;
    case Type::Replay:
        msg.body = Replay{};
        break;
    case Type::HaltTx: {
        HaltTx m;
        r >> m.autoTxOnly;
        msg.body = m;
        break;
    }
    case Type::FreeText: {
        FreeText m;
        r >> m.text >> m.send;
        msg.body = m;
        break;
    }
    case Type::WsprDecode: {
        WsprDecode m;
        r >> m.isNew >> m.time >> m.snr >> m.deltaTime >> m.frequencyHz >> m.drift
          >> m.callsign >> m.grid >> m.powerDbm >> m.offAir;
        msg.body = m;
        break;
    }
    case Type::Location: {
        Location m;
        r >> m.location;
        msg.body = m;
        break;
    }
    case Type::LoggedAdif: {
        LoggedAdif m;
        r >> m.adif;
        msg.body = m;
        break;
    }
    case Type::HighlightCallsign: {
        HighlightCallsign m;
        r >> m.callsign >> m.background >> m.foreground >> m.highlightLast;
        msg.body = m;
        break;
    }
    case Type::SwitchConfiguration: {
        SwitchConfiguration m;
        r >> m.configurationName;
        msg.body = m;
        break;
    }
    case Type::Configure: {
        Configure m;
        r >> m.mode >> m.frequencyTolerance >> m.subMode >> m.fastMode >> m.trPeriod
          >> m.rxDf >> m.dxCall >> m.dxGrid >> m.generateMessages;
        msg.body = m;
        break;
    }
    case Type::AnnotationInfo: {
        AnnotationInfo m;
        r >> m.dxCall >> m.sortOrderProvided >> m.sortOrder;
        msg.body = m;
        break;
    }
    case Type::Unknown:
        break;
    }
    msg.truncated = r.truncated();
    return msg;
}

QByteArray encode(const QString& id, const Body& body, quint32 schema)
{
    const Type type = typeForBody(body);
    if (type == Type::Unknown) {
        return {};
    }
    schema = clampSchema(schema);
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::BigEndian);
    s.setVersion(streamVersionForSchema(schema));
    s << kMagic << schema << static_cast<quint32>(type);
    writeUtf8(s, id);
    std::visit(Encoder{s}, body);
    return out;
}

// ── Helpers ────────────────────────────────────────────────────────────────

QString modeFromSymbol(const QString& symbol)
{
    // Symbols per WSJT-X lib/decoder.f90 output formats and mainwindow.cpp.
    const QString sym = symbol.trimmed();
    if (sym == QLatin1String("~")) { return QStringLiteral("FT8"); }
    if (sym == QLatin1String("+")) { return QStringLiteral("FT4"); }
    if (sym == QLatin1String("#")) { return QStringLiteral("JT65"); }
    if (sym == QLatin1String("@")) { return QStringLiteral("JT9"); }
    if (sym == QLatin1String("&")) { return QStringLiteral("MSK144"); }
    if (sym == QLatin1String("$")) { return QStringLiteral("JT4"); }
    if (sym == QLatin1String("`")) { return QStringLiteral("FST4"); }
    if (sym == QLatin1String(":")) { return QStringLiteral("Q65"); }
    // Some forks send the mode name itself.
    if (sym.size() > 1) {
        return sym.toUpper();
    }
    return {};
}

double nominalPeriodSeconds(const QString& mode)
{
    const QString m = mode.toUpper();
    if (m == QLatin1String("FT4")) { return 7.5; }
    if (m == QLatin1String("FT8")) { return 15.0; }
    if (m == QLatin1String("MSK144")) { return 15.0; }
    if (m == QLatin1String("WSPR")) { return 120.0; }
    if (m == QLatin1String("Q65") || m == QLatin1String("FST4")) { return 60.0; }
    if (m == QLatin1String("JT65") || m == QLatin1String("JT9") || m == QLatin1String("JT4")) {
        return 60.0;
    }
    if (m == QLatin1String("JS8")) { return 15.0; }
    return 15.0;
}

bool looksLikeCallsign(const QString& token)
{
    static const QRegularExpression kCall(QStringLiteral("^[A-Z0-9/]{3,13}$"));
    if (!kCall.match(token).hasMatch()) {
        return false;
    }
    bool hasDigit = false;
    bool hasLetter = false;
    for (const QChar c : token) {
        if (c.isDigit()) {
            hasDigit = true;
        } else if (c.isLetter()) {
            hasLetter = true;
        }
    }
    if (!hasDigit || !hasLetter) {
        return false;
    }
    // Reject 4-char grids like "FN42" masquerading as calls, and RR73.
    static const QRegularExpression kGrid(QStringLiteral("^[A-R]{2}[0-9]{2}$"));
    if (kGrid.match(token).hasMatch() || token == QLatin1String("RR73")) {
        return false;
    }
    return true;
}

ParsedText parseMessageText(const QString& text)
{
    ParsedText p;
    QStringList tok = text.simplified().toUpper().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    // Strip trailing WSJT-X annotations such as "a1"/"?" (not normally in
    // the UDP text, but harmless) and the RTTY-contest "TU;" prefix.
    if (!tok.isEmpty() && tok.first() == QLatin1String("TU;")) {
        tok.removeFirst();
    }
    auto unhash = [](QString t) {
        if (t.startsWith(QLatin1Char('<')) && t.endsWith(QLatin1Char('>'))) {
            t = t.mid(1, t.size() - 2);
        }
        return t;
    };
    static const QRegularExpression kGrid(QStringLiteral("^[A-R]{2}[0-9]{2}$"));
    static const QRegularExpression kModifier(QStringLiteral("^([A-Z]{1,4}|[0-9]{3})$"));
    // "RR73" is syntactically a grid (ocean square) but WSJT-X always means
    // the roger/73 acknowledgement by it.
    auto isGrid = [](const QString& t) {
        return kGrid.match(t).hasMatch() && t != QLatin1String("RR73");
    };

    if (tok.isEmpty()) {
        p.isFreeText = true;
        return p;
    }

    int i = 0;
    if (tok[0] == QLatin1String("CQ") || tok[0] == QLatin1String("QRZ")) {
        p.isCq = true;
        i = 1;
        // "CQ DX K1ABC FN42" — modifier is present when there are two more
        // tokens after it and the next token is not itself a callsign.
        if (i + 1 < tok.size() && kModifier.match(tok[i]).hasMatch()
            && !looksLikeCallsign(unhash(tok[i]))) {
            p.cqModifier = tok[i];
            ++i;
        }
        if (i < tok.size()) {
            const QString call = unhash(tok[i]);
            if (looksLikeCallsign(call)) {
                p.deCall = call;
            }
            ++i;
        }
        if (i < tok.size() && isGrid(tok[i])) {
            p.grid = tok[i];
        }
        p.isFreeText = p.deCall.isEmpty();
        return p;
    }

    if (tok.size() >= 2) {
        const QString to = unhash(tok[0]);
        const QString de = unhash(tok[1]);
        // "<...>" is an unresolved hashed callsign — still a directed message.
        if (looksLikeCallsign(de) && (looksLikeCallsign(to) || to == QLatin1String("..."))) {
            p.toCall = to;
            p.deCall = de;
            for (int k = 2; k < tok.size(); ++k) {
                const QString& t = tok[k];
                if (t == QLatin1String("R")) {
                    continue;
                }
                if (isGrid(t)) {
                    p.grid = t;
                } else if (p.report.isEmpty()) {
                    p.report = t;
                }
            }
            return p;
        }
    }
    p.isFreeText = true;
    return p;
}

QString bandForFrequencyHz(quint64 hz)
{
    struct Band { quint64 lo; quint64 hi; const char* name; };
    static constexpr std::array<Band, 19> kBands{{
        {135700, 137800, "2190m"},   {472000, 479000, "630m"},
        {1800000, 2000000, "160m"},  {3500000, 4000000, "80m"},
        {5060000, 5450000, "60m"},   {7000000, 7300000, "40m"},
        {10100000, 10150000, "30m"}, {14000000, 14350000, "20m"},
        {18068000, 18168000, "17m"}, {21000000, 21450000, "15m"},
        {24890000, 24990000, "12m"}, {28000000, 29700000, "10m"},
        {40000000, 45000000, "8m"},  {50000000, 54000000, "6m"},
        {70000000, 71000000, "4m"},  {144000000, 148000000, "2m"},
        {222000000, 225000000, "1.25m"}, {420000000, 450000000, "70cm"},
        {902000000, 928000000, "33cm"},
    }};
    for (const Band& b : kBands) {
        if (hz >= b.lo && hz <= b.hi) {
            return QString::fromLatin1(b.name);
        }
    }
    return {};
}

namespace {

void adifField(QString& out, const char* name, const QString& value)
{
    if (value.isEmpty()) {
        return;
    }
    const QByteArray utf8 = value.toUtf8();
    out += QStringLiteral("<%1:%2>%3 ").arg(QLatin1String(name)).arg(utf8.size()).arg(value);
}

// ADIF 3.1 mode/submode mapping for WSJT-X mode names (FT4, Q65, FST4, JS8
// are MFSK submodes; FT8/JT65/JT9/MSK144/WSPR/JT4 are ADIF modes).
void adifMode(QString& out, const QString& mode)
{
    const QString m = mode.toUpper();
    if (m == QLatin1String("FT4") || m == QLatin1String("Q65") || m == QLatin1String("FST4")
        || m == QLatin1String("FST4W") || m == QLatin1String("JS8")) {
        adifField(out, "mode", QStringLiteral("MFSK"));
        adifField(out, "submode", m);
        return;
    }
    adifField(out, "mode", m);
}

} // namespace

QString adifFromQsoLogged(const QsoLogged& q)
{
    QString rec;
    adifField(rec, "call", q.dxCall);
    adifField(rec, "gridsquare", q.dxGrid);
    adifMode(rec, q.mode);
    adifField(rec, "rst_sent", q.reportSent);
    adifField(rec, "rst_rcvd", q.reportReceived);
    const QDateTime on = (q.dateTimeOn.isValid() ? q.dateTimeOn : q.dateTimeOff).toUTC();
    const QDateTime off = (q.dateTimeOff.isValid() ? q.dateTimeOff : q.dateTimeOn).toUTC();
    if (on.isValid()) {
        adifField(rec, "qso_date", on.toString(QStringLiteral("yyyyMMdd")));
        adifField(rec, "time_on", on.toString(QStringLiteral("HHmmss")));
    }
    if (off.isValid()) {
        adifField(rec, "qso_date_off", off.toString(QStringLiteral("yyyyMMdd")));
        adifField(rec, "time_off", off.toString(QStringLiteral("HHmmss")));
    }
    adifField(rec, "band", bandForFrequencyHz(q.txFrequencyHz));
    if (q.txFrequencyHz > 0) {
        adifField(rec, "freq", QString::number(static_cast<double>(q.txFrequencyHz) / 1e6, 'f', 6));
    }
    adifField(rec, "station_callsign", q.myCall);
    adifField(rec, "my_gridsquare", q.myGrid);
    adifField(rec, "tx_pwr", q.txPower);
    adifField(rec, "comment", q.comments);
    adifField(rec, "name", q.name);
    adifField(rec, "operator", q.operatorCall);
    adifField(rec, "stx_string", q.exchangeSent);
    adifField(rec, "srx_string", q.exchangeReceived);
    adifField(rec, "prop_mode", q.propagationMode);
    rec += QStringLiteral("<EOR>");
    return rec;
}

QString adifRecordFromLoggedAdif(const QString& adifFile)
{
    const int eoh = adifFile.indexOf(QStringLiteral("<EOH>"), 0, Qt::CaseInsensitive);
    if (eoh < 0) {
        return adifFile.trimmed();
    }
    return adifFile.mid(eoh + 5).trimmed();
}

} // namespace AetherSDR::Wsjtx
