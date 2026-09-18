#pragma once

// Transmit interlock for GUI test builds (-DAETHER_GUI_TEST=ON).
// Test radios may have no antenna, so any command that could key the
// transmitter is refused at the connection layer, below all UI/CAT/TCI paths.

#include <QRegularExpression>
#include <QString>

namespace AetherSDR::TxSafety {

inline bool isTransmitCommand(const QString& command)
{
    // SmartSDR API commands that key the PA or start keyed playback
    // (protocol v1.4.0.0 / fw 4.1.5): MOX, tune carrier, ATU/TGXL tune
    // cycles, CW keying, CWX/DVK transmission, VOX enable.
    static const QRegularExpression kTx(
        QStringLiteral(R"(^\s*(xmit\s+(1|on|true)\b)"
                       R"(|transmit\s+tune\s+(1|on|true)\b)"
                       R"(|transmit\s+set\s+.*\b(mox|tune)=(1|on|true)\b)"
                       R"(|transmit\s+set\s+.*\bvox_enable=1\b)"
                       R"(|atu\s+start\b)"
                       R"(|tgxl\s+.*autotune)"
                       R"(|cw\s+(key|ptt)\b)"
                       R"(|cwx\s+(send|macro\s+send)\b)"
                       R"(|dvk\s+(play|playback|send))"
                       R"())"),
        QRegularExpression::CaseInsensitiveOption);
    return kTx.match(command).hasMatch();
}

} // namespace AetherSDR::TxSafety
