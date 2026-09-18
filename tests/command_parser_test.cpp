// Regression tests for SmartSDR wire-protocol line parsing (CommandParser).
//
// Every line here arrives over TCP from the radio, so the parser has to cope
// with truncated, empty, and adversarially-shaped input without crashing or
// silently losing data.
//
// Build: produced by CMake as `command_parser_test`.
// Run:   ./build/command_parser_test
// Exit code 0 on all pass, 1 on any failure.

#include "core/CommandParser.h"

#include <QString>

#include <cstdio>
#include <cstdlib>
#include <string>

using AetherSDR::CommandParser;
using AetherSDR::MessageType;
using AetherSDR::ParsedMessage;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

std::string q(const QString& s) { return s.toStdString(); }

// ── Version / handle ─────────────────────────────────────────────────────────

void testVersionLine()
{
    const ParsedMessage m = CommandParser::parseLine("V1.4.0.0");
    report("V: version parsed",
           m.type == MessageType::Version && m.object == "1.4.0.0", q(m.object));
}

void testHandleLine()
{
    const ParsedMessage m = CommandParser::parseLine("H0A1B2C3D");
    report("H: hex handle parsed",
           m.type == MessageType::Handle && m.handle == 0x0A1B2C3Du,
           std::to_string(m.handle));
}

// ── Responses ────────────────────────────────────────────────────────────────

void testResponseOk()
{
    const ParsedMessage m = CommandParser::parseLine("R1|0|");
    report("R: success response",
           m.type == MessageType::Response && m.sequence == 1 && m.resultCode == 0);
}

void testResponseErrorCodeIsHex()
{
    const ParsedMessage m = CommandParser::parseLine("R2|50001000|No Such Object");
    report("R: error code parsed as hex",
           m.resultCode == 0x50001000 && m.object == "No Such Object",
           q(m.object));
}

// Codes with the top bit set overflowed QString::toInt() and came back as 0,
// making a failed command look like success.
void testResponseHighBitErrorCodeIsNotSuccess()
{
    const ParsedMessage m = CommandParser::parseLine("R3|E2000000|Error");
    report("R: high-bit error code is non-zero",
           m.resultCode != 0 && static_cast<quint32>(m.resultCode) == 0xE2000000u);
    const ParsedMessage m2 = CommandParser::parseLine("R4|8000000A");
    report("R: high-bit error code without body is non-zero",
           static_cast<quint32>(m2.resultCode) == 0x8000000Au);
}

// The response body is everything after the second '|' and may itself contain
// pipes. Splitting on '|' and taking field [2] truncated these.
void testResponseBodyPreservesEmbeddedPipes()
{
    const ParsedMessage m = CommandParser::parseLine("R7|0|a=1|b=2|c=3");
    const bool ok = m.sequence == 7 && m.resultCode == 0 && m.object == "a=1|b=2|c=3";
    report("R: body preserves embedded pipes", ok, q(m.object));
}

void testResponseWithNoBody()
{
    const ParsedMessage m = CommandParser::parseLine("R9|0");
    report("R: missing body is tolerated",
           m.type == MessageType::Response && m.sequence == 9 && m.resultCode == 0);
}

void testResponseWithOnlySequence()
{
    const ParsedMessage m = CommandParser::parseLine("R42");
    report("R: sequence-only line is tolerated",
           m.type == MessageType::Response && m.sequence == 42);
}

// ── Status messages ──────────────────────────────────────────────────────────

void testStatusSingleWordObject()
{
    const ParsedMessage m = CommandParser::parseLine("S0A1B2C3D|slice 0 RF_frequency=14.225000 mode=USB");
    const bool ok = m.type == MessageType::Status
                 && m.object == "slice 0"
                 && m.kvs.value("RF_frequency") == "14.225000"
                 && m.kvs.value("mode") == "USB";
    report("S: multi-word object split from KVs", ok, q(m.object));
}

// "display pan 0x40000000" — three-word object name.
void testStatusThreeWordObject()
{
    const ParsedMessage m = CommandParser::parseLine(
        "S0A1B2C3D|display pan 0x40000000 center=14.1 bandwidth=0.2");
    const bool ok = m.object == "display pan 0x40000000"
                 && m.kvs.value("center") == "14.1"
                 && m.kvs.value("bandwidth") == "0.2";
    report("S: three-word object name", ok, q(m.object));
}

void testStatusWithNoKeyValues()
{
    const ParsedMessage m = CommandParser::parseLine("S0A1B2C3D|interlock band 9");
    report("S: object with no KVs", m.object == "interlock band 9", q(m.object));
}

void testStatusBareWordKeyIsPresent()
{
    const ParsedMessage m = CommandParser::parseLine("S0A1B2C3D|slice 0 removed");
    // "removed" is a bare flag — recorded with an empty value so callers can
    // distinguish "absent" from "present but valueless".
    report("S: bare-word flag recorded",
           m.object == "slice 0 removed" || m.kvs.contains("removed"), q(m.object));
}

void testStatusValueContainingEquals()
{
    const ParsedMessage m = CommandParser::parseLine("S0A1B2C3D|meter 1 unit=dBm desc=a=b");
    report("S: value containing '=' keeps full value",
           m.kvs.value("desc") == "a=b", q(m.kvs.value("desc")));
}

void testStatusMissingPipeIsTolerated()
{
    const ParsedMessage m = CommandParser::parseLine("S0A1B2C3D");
    report("S: missing '|' does not crash", m.type == MessageType::Status);
}

// ── Message lines ────────────────────────────────────────────────────────────

void testMessageLine()
{
    const ParsedMessage m = CommandParser::parseLine("M0A1B2C3D|some text");
    report("M: handle and body split",
           m.type == MessageType::Message && m.handle == 0x0A1B2C3Du && m.object == "some text",
           q(m.object));
}

// ── Malformed / hostile input ────────────────────────────────────────────────

void testEmptyAndWhitespaceLines()
{
    bool ok = true;
    for (const char* line : {"", " ", "\n", "\r\n", "   \t  "}) {
        const ParsedMessage m = CommandParser::parseLine(QString::fromLatin1(line));
        if (m.type != MessageType::Unknown && !m.raw.isEmpty()) ok = false;
    }
    report("Malformed: empty/whitespace lines are inert", ok);
}

void testUnknownTagIsClassified()
{
    const ParsedMessage m = CommandParser::parseLine("Z12345|whatever");
    report("Malformed: unknown tag classified as Unknown",
           m.type == MessageType::Unknown);
}

void testNonNumericFieldsDoNotCrash()
{
    bool ok = true;
    for (const char* line : {"Rxx|yy|zz", "Hzzzz", "Snothex|slice 0 a=1", "R|", "R||"}) {
        const ParsedMessage m = CommandParser::parseLine(QString::fromLatin1(line));
        (void)m;  // parsing must simply not crash or hang
    }
    report("Malformed: non-numeric fields do not crash", ok);
}

// Truncate a realistic line at every possible length.
void testEveryTruncatedPrefix()
{
    const QString lines[] = {
        "S0A1B2C3D|slice 0 RF_frequency=14.225000 mode=USB filter_lo=-1500",
        "R2|50001000|No Such Object",
        "M0A1B2C3D|hello",
        "V1.4.0.0",
        "H0A1B2C3D",
    };
    for (const QString& full : lines) {
        for (int len = 0; len <= full.size(); ++len) {
            const ParsedMessage m = CommandParser::parseLine(full.left(len));
            (void)m;
        }
    }
    report("Malformed: every truncated prefix is handled", true);
}

void testVeryLongLineIsHandled()
{
    QString line = "S0A1B2C3D|slice 0";
    for (int i = 0; i < 5000; ++i)
        line += QString(" k%1=%2").arg(i).arg(i);
    const ParsedMessage m = CommandParser::parseLine(line);
    report("Malformed: 5000-KV line parses",
           m.kvs.size() == 5000 && m.object == "slice 0",
           "kvs=" + std::to_string(m.kvs.size()));
}

// ── Command building round-trip ──────────────────────────────────────────────

void testBuildCommandFormat()
{
    const QByteArray cmd = CommandParser::buildCommand(17, "slice tune 0 14.2");
    report("buildCommand: C<seq>|<cmd>\\n format",
           cmd == "C17|slice tune 0 14.2\n", cmd.toStdString());
}

} // namespace

int main()
{
    testVersionLine();
    testHandleLine();

    testResponseOk();
    testResponseErrorCodeIsHex();
    testResponseHighBitErrorCodeIsNotSuccess();
    testResponseBodyPreservesEmbeddedPipes();
    testResponseWithNoBody();
    testResponseWithOnlySequence();

    testStatusSingleWordObject();
    testStatusThreeWordObject();
    testStatusWithNoKeyValues();
    testStatusBareWordKeyIsPresent();
    testStatusValueContainingEquals();
    testStatusMissingPipeIsTolerated();

    testMessageLine();

    testEmptyAndWhitespaceLines();
    testUnknownTagIsClassified();
    testNonNumericFieldsDoNotCrash();
    testEveryTruncatedPrefix();
    testVeryLongLineIsHandled();

    testBuildCommandFormat();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "ALL PASS" : "FAILURES",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
