#include "PskVaricode.h"

#include <QHash>

namespace AetherSDR {

namespace {

struct VaricodeEntry {
    char ch;
    const char* bits;
};

constexpr VaricodeEntry kVaricodeEntries[] = {
    {'\n', "11101"},
    {'\r', "11111"},
    {' ', "1"},
    {'!', "111111111"},
    {'"', "101011111"},
    {'#', "111110101"},
    {'$', "111011011"},
    {'%', "1011010101"},
    {'&', "1010111011"},
    {'\'', "101111111"},
    {'(', "11111011"},
    {')', "11110111"},
    {'*', "101101111"},
    {'+', "111011111"},
    {',', "1110101"},
    {'-', "110101"},
    {'.', "1010111"},
    {'/', "110101111"},
    {'0', "10110111"},
    {'1', "10111101"},
    {'2', "11101101"},
    {'3', "11111111"},
    {'4', "101110111"},
    {'5', "101011011"},
    {'6', "101101011"},
    {'7', "110101101"},
    {'8', "110101011"},
    {'9', "110110111"},
    {':', "11110101"},
    {';', "110111101"},
    {'<', "111101101"},
    {'=', "1010101"},
    {'>', "111010111"},
    {'?', "1010101111"},
    {'@', "1010111101"},
    {'A', "1111101"},
    {'B', "11101011"},
    {'C', "10101101"},
    {'D', "10110101"},
    {'E', "1110111"},
    {'F', "11011011"},
    {'G', "11111101"},
    {'H', "101010101"},
    {'I', "1111111"},
    {'J', "111111101"},
    {'K', "101111101"},
    {'L', "11010111"},
    {'M', "10111011"},
    {'N', "11011101"},
    {'O', "10101011"},
    {'P', "11010101"},
    {'Q', "111011101"},
    {'R', "10101111"},
    {'S', "1101111"},
    {'T', "1101101"},
    {'U', "101010111"},
    {'V', "110110101"},
    {'W', "101011101"},
    {'X', "101110101"},
    {'Y', "101111011"},
    {'Z', "1010101101"},
    {'[', "111110111"},
    {'\\', "111101111"},
    {']', "111111011"},
    {'^', "1010111111"},
    {'_', "101101101"},
    {'`', "1011011111"},
    {'a', "1011"},
    {'b', "1011111"},
    {'c', "101111"},
    {'d', "101101"},
    {'e', "11"},
    {'f', "111101"},
    {'g', "1011011"},
    {'h', "101011"},
    {'i', "1101"},
    {'j', "111101011"},
    {'k', "10111111"},
    {'l', "11011"},
    {'m', "111011"},
    {'n', "1111"},
    {'o', "111"},
    {'p', "111111"},
    {'q', "110111111"},
    {'r', "10101"},
    {'s', "10111"},
    {'t', "101"},
    {'u', "110111"},
    {'v', "1111011"},
    {'w', "1101011"},
    {'x', "11011111"},
    {'y', "1011101"},
    {'z', "111010101"},
    {'{', "1010110111"},
    {'|', "110111011"},
    {'}', "1010110101"},
    {'~', "1011010111"},
};

const QHash<QByteArray, QChar>& decodeMap()
{
    static const QHash<QByteArray, QChar> map = [] {
        QHash<QByteArray, QChar> h;
        for (const auto& entry : kVaricodeEntries)
            h.insert(QByteArray(entry.bits), QChar::fromLatin1(entry.ch));
        return h;
    }();
    return map;
}

const char* encodeChar(QChar ch)
{
    const ushort uc = ch.unicode();
    if (uc == '\r')
        return "11111";
    if (uc == '\n')
        return "11101";

    char latin = 0;
    if (uc < 0x80)
        latin = static_cast<char>(uc);
    else
        latin = '?';

    for (const auto& entry : kVaricodeEntries) {
        if (entry.ch == latin)
            return entry.bits;
    }

    return "1010101111"; // '?'
}

} // namespace

QByteArray PskVaricode::encodeText(const QString& text)
{
    QByteArray bits;
    bits.reserve(text.size() * 12);

    for (const QChar ch : text) {
        const char* code = encodeChar(ch);
        bits.append(code);
        bits.append("00");
    }

    return bits;
}

bool PskVaricode::decodeCode(const QByteArray& bits, QChar* out)
{
    const auto it = decodeMap().constFind(bits);
    if (it == decodeMap().constEnd())
        return false;
    if (out)
        *out = it.value();
    return true;
}

} // namespace AetherSDR
