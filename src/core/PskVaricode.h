#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR {

class PskVaricode {
public:
    static QByteArray encodeText(const QString& text);
    static bool decodeCode(const QByteArray& bits, QChar* out);
};

} // namespace AetherSDR
