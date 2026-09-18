#include "CommandParser.h"
#include <QStringList>

namespace AetherSDR {

// ─── Static helpers ──────────────────────────────────────────────────────────

QMap<QString, QString> CommandParser::parseKVs(const QString& body)
{
    QMap<QString, QString> result;
    // Body may look like: "freq=14.225000 mode=USB filter_lo=-1500 filter_hi=1500"
    for (const QString& token : body.split(' ', Qt::SkipEmptyParts)) {
        const int eq = token.indexOf('=');
        if (eq < 0) {
            // bare word — store with empty value so callers can detect presence
            result.insert(token, QString{});
        } else {
            result.insert(token.left(eq), token.mid(eq + 1));
        }
    }
    return result;
}

// ─── parseLine ───────────────────────────────────────────────────────────────
//
// Protocol summary (all lines are ASCII, terminated with \n):
//   V3.3.28.0                         → version
//   H0A1B2C3D                         → hex client handle
//   R1|0|                             → response to command seq 1, code 0 (OK)
//   R2|50001001|No Such Object        → error response
//   S0A1B2C3D|slice 0 freq=14.225000  → status update for slice
//   M0A1B2C3D|0053006F006D0065...     → encoded message (hex UTF-16)

ParsedMessage CommandParser::parseLine(const QString& rawLine)
{
    ParsedMessage msg;
    msg.raw = rawLine.trimmed();

    if (msg.raw.isEmpty()) return msg;

    const QChar tag = msg.raw[0];
    const QString body = msg.raw.mid(1);  // everything after the tag character

    switch (tag.toLatin1()) {
    case 'V':
        msg.type   = MessageType::Version;
        msg.object = body;  // version string
        break;

    case 'H':
        msg.type   = MessageType::Handle;
        msg.handle = body.toUInt(nullptr, 16);
        break;

    case 'R': {
        // R<seq>|<code>|<message_body>
        //
        // Only the first two fields are delimited; the body is everything after
        // the second '|' and may itself contain '|' (e.g. meter and profile
        // list responses). Splitting on '|' and taking parts[2] silently
        // truncated those at the first embedded pipe.
        msg.type = MessageType::Response;
        const int p1 = body.indexOf('|');
        if (p1 < 0) {
            msg.sequence = body.toUInt();
            break;
        }
        msg.sequence = body.left(p1).toUInt();

        const int p2 = body.indexOf('|', p1 + 1);
        if (p2 < 0) {
            msg.resultCode = static_cast<int>(body.mid(p1 + 1).toUInt(nullptr, 16));
            break;
        }
        msg.resultCode = static_cast<int>(body.mid(p1 + 1, p2 - p1 - 1).toUInt(nullptr, 16));

        msg.object = body.mid(p2 + 1);   // full remainder, embedded '|' preserved
        msg.kvs    = parseKVs(msg.object);
        break;
    }

    case 'S': {
        // S<handle>|<object_name> [key=val ...]
        //
        // Object names can be multi-word, e.g. "slice 0", "display pan 0x40000000".
        // KV tokens always contain '='; object tokens never do.
        // Find the boundary by locating the last space before the first '='.
        msg.type = MessageType::Status;
        const int pipe = body.indexOf('|');
        if (pipe < 0) break;
        msg.handle = body.left(pipe).toUInt(nullptr, 16);
        const QString statusBody = body.mid(pipe + 1);

        const int firstEq = statusBody.indexOf('=');
        if (firstEq < 0) {
            // No KVs — entire body is the object name
            msg.object = statusBody.trimmed();
        } else {
            const int kvStart = statusBody.lastIndexOf(' ', firstEq);
            if (kvStart < 0) {
                // Body starts directly with a KV (no object prefix)
                msg.kvs = parseKVs(statusBody);
            } else {
                msg.object = statusBody.left(kvStart).trimmed();
                msg.kvs    = parseKVs(statusBody.mid(kvStart + 1));
            }
        }
        break;
    }

    case 'M':
        msg.type = MessageType::Message;
        {
            const int pipe = body.indexOf('|');
            if (pipe >= 0) {
                msg.handle = body.left(pipe).toUInt(nullptr, 16);
                msg.object = body.mid(pipe + 1);
            }
        }
        break;

    default:
        msg.type = MessageType::Unknown;
        break;
    }

    return msg;
}

// ─── buildCommand ─────────────────────────────────────────────────────────────

QByteArray CommandParser::buildCommand(quint32 seq, const QString& command)
{
    return QString("C%1|%2\n").arg(seq).arg(command).toUtf8();
}

} // namespace AetherSDR
