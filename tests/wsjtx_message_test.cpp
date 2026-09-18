// Offline tests for the WSJT-X UDP NetworkMessage codec (no sockets).
// Wire format per WSJT-X Network/NetworkMessage.hpp (schema 2 = Qt_5_2,
// schema 3 = Qt_5_4 QDataStream).  Golden byte sequences below are built by
// hand, independent of the encoder, to pin the exact on-wire layout.
// Exit code 0 on all pass.

#include "core/WsjtxMessage.h"

#include <QTimeZone>

#include <cstdio>
#include <cstring>

using namespace AetherSDR;
namespace W = AetherSDR::Wsjtx;

namespace {
int g_failures = 0;

void check(bool ok, const char* what, const QString& detail = QString())
{
    if (!ok) {
        ++g_failures;
    }
    std::printf("[%s] %s %s\n", ok ? " OK " : "FAIL", what, qPrintable(detail));
}

// Big-endian primitive writers for golden datagrams.
void be32(QByteArray& b, quint32 v)
{
    b.append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8)).append(char(v));
}
void be64(QByteArray& b, quint64 v)
{
    be32(b, quint32(v >> 32));
    be32(b, quint32(v));
}
void utf8(QByteArray& b, const QByteArray& s)
{
    be32(b, quint32(s.size()));
    b.append(s);
}
void header(QByteArray& b, quint32 schema, quint32 type, const QByteArray& id)
{
    be32(b, 0xadbccbdaU);
    be32(b, schema);
    be32(b, type);
    utf8(b, id);
}

// Returns a copy so it is safe on temporaries from W::decode().
template <typename T>
std::optional<T> bodyAs(const std::optional<W::Message>& m)
{
    if (!m) {
        return std::nullopt;
    }
    if (const T* p = std::get_if<T>(&m->body)) {
        return *p;
    }
    return std::nullopt;
}

void testGoldenHeartbeat()
{
    QByteArray g;
    header(g, 2, 0, "WSJT-X");
    be32(g, 3);
    utf8(g, "2.7.0");
    utf8(g, "abc123");

    W::Heartbeat hb{3, QStringLiteral("2.7.0"), QStringLiteral("abc123")};
    const QByteArray enc = W::encode(QStringLiteral("WSJT-X"), hb, 2);
    check(enc == g, "Heartbeat encodes to golden bytes", QString::fromLatin1(enc.toHex()));

    const auto m = W::decode(g);
    const auto d = bodyAs<W::Heartbeat>(m);
    check(m && m->type == W::Type::Heartbeat && m->schema == 2 && m->id == "WSJT-X",
          "Heartbeat header decodes");
    check(d && d->maxSchema == 3 && d->version == "2.7.0" && d->revision == "abc123",
          "Heartbeat body decodes");
}

void testGoldenDecode()
{
    // Decode: bool new, QTime, qint32 snr, double dt, quint32 df, utf8 mode,
    // utf8 message, bool lowconf, bool offair.
    QByteArray g;
    header(g, 3, 2, "WSJT-X");
    g.append(char(1));                                  // new
    be32(g, (12 * 3600 + 34 * 60 + 45) * 1000);         // 12:34:45.000
    be32(g, quint32(-12));                              // snr
    double dt = 0.3;
    quint64 dtBits;
    std::memcpy(&dtBits, &dt, sizeof dt);
    be64(g, dtBits);                                    // double precision
    be32(g, 1234);                                      // df
    utf8(g, "~");
    utf8(g, "CQ DX K1ABC FN42");
    g.append(char(0));
    g.append(char(0));

    const auto m = W::decode(g);
    const auto d = bodyAs<W::Decode>(m);
    check(d.has_value(), "golden Decode parses");
    if (d) {
        check(d->isNew && d->time == QTime(12, 34, 45) && d->snr == -12, "Decode new/time/snr");
        check(qAbs(d->deltaTime - 0.3) < 1e-9 && d->deltaFrequency == 1234, "Decode dt(double)/df");
        check(d->mode == "~" && d->message == "CQ DX K1ABC FN42" && !d->lowConfidence && !d->offAir,
              "Decode mode/message/flags");
        check(!m->truncated, "Decode not truncated");
    }
    W::Decode rt = d ? *d : W::Decode{};
    check(W::encode(QStringLiteral("WSJT-X"), rt, 3) == g, "Decode re-encodes byte-identical");
}

void testGoldenHighlightColor()
{
    // QColor on the wire: qint8 spec(1=Rgb) + alpha,r,g,b,pad as quint16 (x*0x101).
    W::HighlightCallsign h;
    h.callsign = QStringLiteral("K1ABC");
    h.background = W::Color::rgb(0xff, 0x00, 0x80);
    h.foreground = W::Color::invalid();
    h.highlightLast = true;
    const QByteArray enc = W::encode(QStringLiteral("WSJT-X"), h, 3);

    QByteArray g;
    header(g, 3, 13, "WSJT-X");
    utf8(g, "K1ABC");
    g.append(char(1));
    for (quint16 v : {quint16(0xffff), quint16(0xffff), quint16(0x0000), quint16(0x8080), quint16(0)}) {
        g.append(char(v >> 8)).append(char(v));
    }
    // Invalid QColor: spec 0, alpha 0xffff, r/g/b/pad 0 (verified against
    // Qt's own QDataStream << QColor() output).
    g.append(char(0));
    g.append(char(0xff)).append(char(0xff));
    for (int i = 0; i < 4; ++i) {
        g.append(char(0)).append(char(0));
    }
    g.append(char(1));
    check(enc == g, "HighlightCallsign colour layout matches QDataStream QColor",
          QString::fromLatin1(enc.toHex()));
    const auto d = bodyAs<W::HighlightCallsign>(W::decode(enc));
    check(d && d->background == h.background && !d->foreground.valid && d->highlightLast,
          "HighlightCallsign round trip");
}

void testRoundTripAll()
{
    const QString id = QStringLiteral("WSJT-X - rig2");
    for (quint32 schema : {2U, 3U}) {
        const QString sfx = QStringLiteral("schema %1").arg(schema);

        W::Status st;
        st.dialFrequencyHz = 14074000;
        st.mode = "FT8";
        st.dxCall = "K1ABC";
        st.report = "-12";
        st.txMode = "FT8";
        st.txEnabled = true;
        st.transmitting = false;
        st.decoding = true;
        st.rxDf = 1500;
        st.txDf = 1200;
        st.deCall = "W9XYZ";
        st.deGrid = "EN52";
        st.dxGrid = "FN42";
        st.txWatchdog = false;
        st.subMode = "";
        st.fastMode = false;
        st.specialOperationMode = 5;
        st.frequencyTolerance = W::kNotApplicable;
        st.trPeriod = 15;
        st.configurationName = "Default";
        st.txMessage = "K1ABC W9XYZ EN52";
        {
            const auto m = W::decode(W::encode(id, st, schema));
            const auto d = bodyAs<W::Status>(m);
            check(d && m->id == id && d->dialFrequencyHz == 14074000 && d->mode == "FT8"
                      && d->dxCall == "K1ABC" && d->txEnabled && d->decoding && !d->transmitting
                      && d->rxDf == 1500 && d->txDf == 1200 && d->deCall == "W9XYZ"
                      && d->specialOperationMode == 5 && d->trPeriod == 15
                      && d->frequencyTolerance == W::kNotApplicable
                      && d->configurationName == "Default" && d->txMessage == "K1ABC W9XYZ EN52"
                      && !m->truncated,
                  "Status round trip", sfx);
        }

        W::Clear clrOut;
        {
            const auto m = W::decode(W::encode(id, clrOut, schema));
            const auto d = bodyAs<W::Clear>(m);
            check(d && !d->window.has_value(), "Clear (out, no window) round trip", sfx);
            W::Clear clrIn;
            clrIn.window = 2;
            const auto d2 = bodyAs<W::Clear>(W::decode(W::encode(id, clrIn, schema)));
            check(d2 && d2->window == 2, "Clear (in, window=2) round trip", sfx);
        }

        W::Reply rp;
        rp.time = QTime(1, 2, 3);
        rp.snr = -5;
        rp.deltaTime = -0.2;
        rp.deltaFrequency = 800;
        rp.mode = "~";
        rp.message = "CQ K1ABC FN42";
        rp.lowConfidence = false;
        rp.modifiers = 0x02;
        {
            const auto d = bodyAs<W::Reply>(W::decode(W::encode(id, rp, schema)));
            check(d && d->time == rp.time && d->snr == -5 && qAbs(d->deltaTime + 0.2) < 1e-9
                      && d->deltaFrequency == 800 && d->mode == "~" && d->message == rp.message
                      && d->modifiers == 0x02,
                  "Reply round trip", sfx);
        }

        W::QsoLogged q;
        q.dateTimeOff = QDateTime(QDate(2026, 9, 17), QTime(18, 1, 30), QTimeZone::UTC);
        q.dateTimeOn = QDateTime(QDate(2026, 9, 17), QTime(18, 0, 0), QTimeZone::UTC);
        q.dxCall = "K1ABC";
        q.dxGrid = "FN42";
        q.txFrequencyHz = 14074000;
        q.mode = "FT8";
        q.reportSent = "-10";
        q.reportReceived = "-08";
        q.txPower = "50";
        q.comments = "tnx";
        q.name = "Bob";
        q.operatorCall = "W9XYZ";
        q.myCall = "W9XYZ";
        q.myGrid = "EN52";
        q.exchangeSent = "";
        q.exchangeReceived = "";
        q.propagationMode = "";
        {
            const auto d = bodyAs<W::QsoLogged>(W::decode(W::encode(id, q, schema)));
            check(d && d->dateTimeOff == q.dateTimeOff && d->dateTimeOn == q.dateTimeOn
                      && d->dateTimeOff.timeSpec() == Qt::UTC && d->dxCall == "K1ABC"
                      && d->txFrequencyHz == 14074000 && d->reportReceived == "-08"
                      && d->myGrid == "EN52",
                  "QSOLogged round trip (QDateTime UTC)", sfx);
        }

        check(bodyAs<W::Close>(W::decode(W::encode(id, W::Close{}, schema))).has_value(),
              "Close round trip", sfx);
        check(bodyAs<W::Replay>(W::decode(W::encode(id, W::Replay{}, schema))).has_value(),
              "Replay round trip", sfx);
        {
            const auto d = bodyAs<W::HaltTx>(W::decode(W::encode(id, W::HaltTx{true}, schema)));
            check(d && d->autoTxOnly, "HaltTx round trip", sfx);
        }
        {
            const auto d = bodyAs<W::FreeText>(
                W::decode(W::encode(id, W::FreeText{QStringLiteral("TNX 73 GL"), true}, schema)));
            check(d && d->text == "TNX 73 GL" && d->send, "FreeText round trip", sfx);
        }
        W::WsprDecode w;
        w.time = QTime(12, 0);
        w.snr = -22;
        w.deltaTime = 0.5;
        w.frequencyHz = 14097071;
        w.drift = -1;
        w.callsign = "K1ABC";
        w.grid = "FN42";
        w.powerDbm = 37;
        {
            const auto d = bodyAs<W::WsprDecode>(W::decode(W::encode(id, w, schema)));
            check(d && d->frequencyHz == 14097071 && d->drift == -1 && d->callsign == "K1ABC"
                      && d->powerDbm == 37 && d->snr == -22,
                  "WSPRDecode round trip", sfx);
        }
        {
            const auto d = bodyAs<W::Location>(
                W::decode(W::encode(id, W::Location{QStringLiteral("EN52wa")}, schema)));
            check(d && d->location == "EN52wa", "Location round trip", sfx);
        }
        {
            const QString adif = QStringLiteral("\n<adif_ver:5>3.1.0\n<programid:6>WSJT-X\n<EOH>\n"
                                                "<call:5>K1ABC <mode:3>FT8 <EOR>");
            const auto d = bodyAs<W::LoggedAdif>(W::decode(W::encode(id, W::LoggedAdif{adif}, schema)));
            check(d && d->adif == adif, "LoggedADIF round trip", sfx);
        }
        {
            const auto d = bodyAs<W::SwitchConfiguration>(W::decode(
                W::encode(id, W::SwitchConfiguration{QStringLiteral("6m")}, schema)));
            check(d && d->configurationName == "6m", "SwitchConfiguration round trip", sfx);
        }
        W::Configure c;
        c.mode = "FT4";
        c.frequencyTolerance = 50;
        c.subMode = "";
        c.fastMode = false;
        c.trPeriod = W::kNotApplicable;
        c.rxDf = 1500;
        c.dxCall = "K1ABC";
        c.dxGrid = "FN42";
        c.generateMessages = true;
        {
            const auto d = bodyAs<W::Configure>(W::decode(W::encode(id, c, schema)));
            check(d && d->mode == "FT4" && d->frequencyTolerance == 50 && d->rxDf == 1500
                      && d->trPeriod == W::kNotApplicable && d->dxCall == "K1ABC"
                      && d->generateMessages,
                  "Configure round trip", sfx);
        }
        {
            const auto d = bodyAs<W::AnnotationInfo>(W::decode(
                W::encode(id, W::AnnotationInfo{QStringLiteral("K1ABC"), true, 42}, schema)));
            check(d && d->dxCall == "K1ABC" && d->sortOrderProvided && d->sortOrder == 42,
                  "AnnotationInfo (16) round trip", sfx);
        }
    }
}

void testRobustness()
{
    // Old client: Status that stops after "Decoding" (pre-2.0 WSJT-X / JTDX).
    QByteArray g;
    header(g, 2, 1, "JTDX");
    be64(g, 7074000);
    utf8(g, "FT8");
    utf8(g, "");
    utf8(g, "");
    utf8(g, "FT8");
    g.append(char(1)).append(char(0)).append(char(1));
    const auto m = W::decode(g);
    const auto st = bodyAs<W::Status>(m);
    check(st && st->dialFrequencyHz == 7074000 && st->txEnabled && st->decoding
              && st->trPeriod == W::kNotApplicable && m->truncated,
          "truncated Status keeps defaults + truncated flag");

    // Heartbeat without max-schema field → assume schema 2.
    QByteArray hb;
    header(hb, 2, 0, "old");
    const auto h = bodyAs<W::Heartbeat>(W::decode(hb));
    check(h && h->maxSchema == 2, "Heartbeat without max schema defaults to 2");

    // Extra trailing fields from a future version are ignored.
    QByteArray fut = W::encode(QStringLiteral("x"), W::HaltTx{true}, 3);
    fut.append("future-extra-data");
    const auto ht = bodyAs<W::HaltTx>(W::decode(fut));
    check(ht && ht->autoTxOnly, "trailing unknown fields ignored");

    QByteArray bad = W::encode(QStringLiteral("x"), W::Replay{}, 3);
    bad[0] = 0x00;
    check(!W::decode(bad).has_value(), "bad magic rejected");

    QByteArray s4;
    header(s4, 4, 0, "x");
    check(!W::decode(s4).has_value(), "schema 4 rejected");
    QByteArray s1;
    header(s1, 1, 0, "x");
    check(!W::decode(s1).has_value(), "schema 1 (broken Qt_5_0) rejected");

    QByteArray unk;
    header(unk, 3, 99, "x");
    unk.append("whatever");
    const auto u = W::decode(unk);
    check(u && u->type == W::Type::Unknown && u->id == "x", "unknown type → Type::Unknown");

    check(!W::decode(QByteArray("{\"type\":\"RX.ACTIVITY\"}")).has_value(),
          "JS8Call JSON datagram rejected cleanly");
    check(!W::decode(QByteArray()).has_value(), "empty datagram rejected");
    check(!W::decode(QByteArray::fromHex("adbccbda0000")).has_value(), "short header rejected");

    QString id;
    W::Type t = W::Type::Unknown;
    quint32 sc = 0;
    check(W::peekHeader(W::encode(QStringLiteral("abc"), W::Location{QStringLiteral("FN42")}, 2),
                        &sc, &t, &id)
              && sc == 2 && t == W::Type::Location && id == "abc",
          "peekHeader");

    // Null id round trip and schema clamping.
    const auto clamped = W::decode(W::encode(QStringLiteral("x"), W::Replay{}, 9));
    check(clamped && clamped->schema == 3, "encode clamps schema to 3");
    check(W::encode(QStringLiteral("x"), W::Body{}, 3).isEmpty(), "monostate encodes empty");
}

void testParse()
{
    auto p = W::parseMessageText(QStringLiteral("CQ K1ABC FN42"));
    check(p.isCq && p.deCall == "K1ABC" && p.grid == "FN42" && p.cqModifier.isEmpty(), "CQ call grid");
    p = W::parseMessageText(QStringLiteral("CQ DX K1ABC FN42"));
    check(p.isCq && p.cqModifier == "DX" && p.deCall == "K1ABC", "CQ DX");
    p = W::parseMessageText(QStringLiteral("CQ POTA W1AW/P"));
    check(p.isCq && p.cqModifier == "POTA" && p.deCall == "W1AW/P", "CQ POTA portable");
    p = W::parseMessageText(QStringLiteral("CQ 290 K1ABC FN42"));
    check(p.isCq && p.cqModifier == "290" && p.deCall == "K1ABC", "CQ numeric modifier");
    p = W::parseMessageText(QStringLiteral("QRZ K1ABC FN42"));
    check(p.isCq && p.deCall == "K1ABC", "QRZ");
    p = W::parseMessageText(QStringLiteral("W9XYZ K1ABC -12"));
    check(!p.isCq && p.toCall == "W9XYZ" && p.deCall == "K1ABC" && p.report == "-12", "directed report");
    p = W::parseMessageText(QStringLiteral("W9XYZ K1ABC R-07"));
    check(p.deCall == "K1ABC" && p.report == "R-07", "R-report");
    p = W::parseMessageText(QStringLiteral("W9XYZ K1ABC RR73"));
    check(p.deCall == "K1ABC" && p.report == "RR73" && p.grid.isEmpty(), "RR73 not a grid");
    p = W::parseMessageText(QStringLiteral("W9XYZ K1ABC FN42"));
    check(p.deCall == "K1ABC" && p.grid == "FN42", "directed grid");
    p = W::parseMessageText(QStringLiteral("<PJ4/K1ABC> W9XYZ R 579 0013"));
    check(p.toCall == "PJ4/K1ABC" && p.deCall == "W9XYZ", "hashed compound call");
    p = W::parseMessageText(QStringLiteral("<...> K1ABC -05"));
    check(p.deCall == "K1ABC", "unresolved hash addressee");
    p = W::parseMessageText(QStringLiteral("TU; W9XYZ K1ABC 579 MA"));
    check(p.toCall == "W9XYZ" && p.deCall == "K1ABC", "RTTY roundup TU; prefix");
    p = W::parseMessageText(QStringLiteral("TNX BOB 73 GL"));
    check(p.isFreeText && p.deCall.isEmpty(), "free text");
    p = W::parseMessageText(QStringLiteral(""));
    check(p.isFreeText, "empty text");

    check(W::looksLikeCallsign("K1ABC") && W::looksLikeCallsign("VK9/W1AW")
              && !W::looksLikeCallsign("FN42") && !W::looksLikeCallsign("RR73")
              && !W::looksLikeCallsign("DX") && !W::looksLikeCallsign("-12"),
          "looksLikeCallsign");

    check(W::modeFromSymbol("~") == "FT8" && W::modeFromSymbol("+") == "FT4"
              && W::modeFromSymbol("`") == "FST4" && W::modeFromSymbol(":") == "Q65"
              && W::modeFromSymbol("#") == "JT65" && W::modeFromSymbol("@") == "JT9"
              && W::modeFromSymbol("&") == "MSK144" && W::modeFromSymbol("?").isEmpty(),
          "modeFromSymbol");
    check(W::nominalPeriodSeconds("FT4") == 7.5 && W::nominalPeriodSeconds("FT8") == 15.0,
          "nominalPeriodSeconds");
    check(W::bandForFrequencyHz(14074000) == "20m" && W::bandForFrequencyHz(50313000) == "6m"
              && W::bandForFrequencyHz(1840000) == "160m" && W::bandForFrequencyHz(12345).isEmpty(),
          "bandForFrequencyHz");
}

void testAdif()
{
    W::QsoLogged q;
    q.dateTimeOn = QDateTime(QDate(2026, 9, 17), QTime(18, 0, 0), QTimeZone::UTC);
    q.dateTimeOff = QDateTime(QDate(2026, 9, 17), QTime(18, 1, 30), QTimeZone::UTC);
    q.dxCall = "K1ABC";
    q.dxGrid = "FN42";
    q.txFrequencyHz = 14074000;
    q.mode = "FT4";
    q.reportSent = "-10";
    q.reportReceived = "-08";
    q.myCall = "W9XYZ";
    q.myGrid = "EN52";
    const QString a = W::adifFromQsoLogged(q);
    check(a.contains("<call:5>K1ABC") && a.contains("<gridsquare:4>FN42")
              && a.contains("<mode:4>MFSK") && a.contains("<submode:3>FT4")
              && a.contains("<qso_date:8>20260917") && a.contains("<time_on:6>180000")
              && a.contains("<time_off:6>180130") && a.contains("<band:3>20m")
              && a.contains("<freq:9>14.074000") && a.contains("<station_callsign:5>W9XYZ")
              && a.contains("<rst_rcvd:3>-08") && a.endsWith("<EOR>")
              && !a.contains("<comment:"),
          "ADIF from QSOLogged", a);

    const QString file = QStringLiteral("\n<adif_ver:5>3.1.0\n<programid:6>WSJT-X\n<EOH>\n"
                                        "<call:5>K1ABC <mode:3>FT8 <EOR>\n");
    check(W::adifRecordFromLoggedAdif(file) == "<call:5>K1ABC <mode:3>FT8 <EOR>",
          "LoggedADIF record extraction");
    check(W::adifRecordFromLoggedAdif("<call:5>K1ABC <eor>") == "<call:5>K1ABC <eor>",
          "LoggedADIF without header");
}

} // namespace

int main()
{
    testGoldenHeartbeat();
    testGoldenDecode();
    testGoldenHighlightColor();
    testRoundTripAll();
    testRobustness();
    testParse();
    testAdif();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
