#include "Ale2gFrame.h"

#include <algorithm>

namespace AetherSDR::Ale2g {

namespace Frames {

int wordsForDurationMs(int ms)
{
    return std::max(0, (ms + kTrwMs - 1) / kTrwMs);
}

int scanWordsForScan(int channels, int dwellMs)
{
    const int ts = std::max(1, channels) * std::max(0, dwellMs);
    return std::max(2, wordsForDurationMs(ts + 2 * kTrwMs));
}

namespace {

void append(QVector<std::uint32_t>& out, const QVector<std::uint32_t>& words, int times = 1)
{
    for (int i = 0; i < times; ++i) {
        out += words;
    }
}

QVector<std::uint32_t> callWithAddress(const QString& to, const QString& self, bool tis,
                                       int scanWords, const QString& amdText)
{
    QVector<std::uint32_t> out;
    const QVector<std::uint32_t> toWords = addressWords(Preamble::To, to);
    const QVector<std::uint32_t> selfWords = addressWords(tis ? Preamble::Tis : Preamble::Twas, self);
    if (toWords.isEmpty() || selfWords.isEmpty()) {
        return out;
    }
    // Scanning call: first word only, TO preamble (A.5.5.3.1).
    for (int i = 0; i < scanWords; ++i) {
        out.push_back(toWords.first());
    }
    // Leading call: whole address sent twice.
    append(out, toWords, 2);
    // Message section.
    append(out, amdWords(amdText));
    // Conclusion: whole calling-station address once.
    append(out, selfWords);
    return out;
}

} // namespace

QVector<std::uint32_t> buildCall(const QString& to, const QString& self, bool tis,
                                 int scanWords, const QString& amdText)
{
    return callWithAddress(to, self, tis, scanWords, amdText);
}

QVector<std::uint32_t> buildAllCall(const QString& self, bool tis, int scanWords,
                                    const QString& amdText)
{
    return callWithAddress(QStringLiteral("@?@"), self, tis, scanWords, amdText);
}

QVector<std::uint32_t> buildAnyCall(const QString& self, int scanWords, const QString& amdText)
{
    return callWithAddress(QStringLiteral("@@?"), self, true, scanWords, amdText);
}

QVector<std::uint32_t> buildResponse(const QString& to, const QString& self, bool tis,
                                     const QString& amdText)
{
    return callWithAddress(to, self, tis, 0, amdText);
}

QVector<std::uint32_t> buildSound(const QString& self, bool tis, int minimumWords)
{
    QVector<std::uint32_t> out;
    const QVector<std::uint32_t> selfWords = addressWords(tis ? Preamble::Tis : Preamble::Twas, self);
    if (selfWords.isEmpty()) {
        return out;
    }
    int reps = 0;
    while (reps < 2 || out.size() < minimumWords) {
        append(out, selfWords);
        ++reps;
    }
    return out;
}

} // namespace Frames

QString Frame::summary() const
{
    QStringList parts;
    if (allCall) {
        parts << QStringLiteral("ALLCALL");
    }
    if (anyCall) {
        parts << QStringLiteral("ANYCALL");
    }
    if (!toAddresses.isEmpty()) {
        parts << QStringLiteral("TO %1").arg(toAddresses.join(QLatin1Char(',')));
    }
    for (const QString& amd : amdMessages) {
        parts << QStringLiteral("AMD \"%1\"").arg(amd);
    }
    if (!otherCmdWords.isEmpty()) {
        parts << QStringLiteral("CMD x%1").arg(otherCmdWords.size());
    }
    if (!fromAddress.isEmpty()) {
        parts << QStringLiteral("%1 %2").arg(twas ? QStringLiteral("TWAS")
                                                  : (tis ? QStringLiteral("TIS")
                                                         : QStringLiteral("FROM")),
                                             fromAddress);
    }
    if (!complete) {
        parts << QStringLiteral("(incomplete)");
    }
    return QStringLiteral("%1  [BER %2, SNR %3 dB]")
        .arg(parts.join(QStringLiteral(" | ")))
        .arg(berCode())
        .arg(QString::number(meanSnrDb(), 'f', 1));
}

void FrameAssembler::reset()
{
    m_active = false;
    m_frame = Frame{};
    m_field = Field::None;
    m_fieldChars.clear();
    m_fieldWords = 0;
    m_lastPreamble = Preamble::Data;
    m_toFirstWords.clear();
}

void FrameAssembler::beginFrameIfNeeded(const Demodulator::Word& word)
{
    if (m_active) {
        return;
    }
    m_active = true;
    m_frame = Frame{};
    m_frame.startSample = word.endSample - kSamplesPerRedundantWord;
    m_field = Field::None;
    m_fieldChars.clear();
    m_fieldWords = 0;
    m_toFirstWords.clear();
}

void FrameAssembler::closeField()
{
    switch (m_field) {
    case Field::To: {
        const QString raw = m_fieldChars;
        const QString first = raw.left(3);
        if (isAllCallAddress(first)) {
            m_frame.allCall = true;
            if (first != QStringLiteral("@?@") && !m_frame.toAddresses.contains(first)) {
                m_frame.toAddresses << first;   // selective AllCall pattern
            }
        } else if (isAnyCallAddress(first)) {
            m_frame.anyCall = true;
            if (first != QStringLiteral("@@?") && !m_frame.toAddresses.contains(first)) {
                m_frame.toAddresses << first;
            }
        } else if (first != QStringLiteral("@@@")) {
            const QString addr = unstuffAddress(raw);
            if (!addr.isEmpty() && !m_frame.toAddresses.contains(addr)) {
                m_frame.toAddresses << addr;
            }
        }
        break;
    }
    case Field::Tis:
    case Field::Twas:
    case Field::From:
        if (!m_fieldChars.isEmpty()) {
            m_frame.fromAddress = unstuffAddress(m_fieldChars);
        }
        break;
    case Field::Amd: {
        QString text = m_fieldChars;
        while (text.endsWith(QLatin1Char(' '))) {
            text.chop(1);
        }
        m_frame.amdMessages << text;
        break;
    }
    case Field::Thru:
    case Field::Cmd:
    case Field::None:
        break;
    }
    m_field = Field::None;
    m_fieldChars.clear();
    m_fieldWords = 0;
}

std::optional<Frame> FrameAssembler::takeFrame()
{
    if (!m_active) {
        return std::nullopt;
    }
    closeField();
    Frame f = m_frame;
    // Scanning-call first words ("TO SAM") are superseded by the whole address
    // ("TO SAM DATA UEL") of the leading call.
    QStringList pruned;
    for (const QString& a : f.toAddresses) {
        bool prefixOfLonger = false;
        if (a.size() <= 3) {
            for (const QString& b : f.toAddresses) {
                if (b.size() > 3 && b.startsWith(a)) {
                    prefixOfLonger = true;
                    break;
                }
            }
        }
        if (!prefixOfLonger) {
            pruned << a;
        }
    }
    f.toAddresses = pruned;
    reset();
    if (f.wordCount == 0) {
        return std::nullopt;
    }
    return f;
}

std::optional<Frame> FrameAssembler::flush()
{
    return takeFrame();
}

std::optional<Frame> FrameAssembler::addWord(const Demodulator::Word& word)
{
    const Preamble pre = wordPreamble(word.word);
    const QString chars = wordChars(word.word);
    std::optional<Frame> finished;

    // A new calling cycle / message after a conclusion starts a new frame; a
    // different TIS/TWAS address after a conclusion is a different transmitter.
    const bool concluded = m_active && m_frame.complete;
    if (concluded) {
        const bool conclusionField = m_field == Field::Tis || m_field == Field::Twas;
        const bool sameConclusionRepeat =
            ((pre == Preamble::Tis && m_frame.tis) || (pre == Preamble::Twas && m_frame.twas))
            && m_frame.fromAddress.left(3) == unstuffAddress(chars).left(3);
        const bool sameFirstWordRepeat =
            conclusionField && ((pre == Preamble::Tis && m_field == Field::Tis)
                                || (pre == Preamble::Twas && m_field == Field::Twas))
            && m_fieldChars.left(3) == chars;
        if (pre == Preamble::To || pre == Preamble::Thru || pre == Preamble::Cmd
            || pre == Preamble::From
            || ((pre == Preamble::Tis || pre == Preamble::Twas) && !sameConclusionRepeat
                && !sameFirstWordRepeat)) {
            finished = takeFrame();
        }
    }

    if (word.repeat && pre != Preamble::To && pre != Preamble::Thru && pre != Preamble::Tis
        && pre != Preamble::Twas) {
        return finished;   // identical extension/message word re-read; ignore
    }

    beginFrameIfNeeded(word);
    ++m_frame.wordCount;
    m_frame.nonUnanimousSum += word.nonUnanimous;
    m_frame.snrSum += word.snrDb;
    m_frame.endSample = word.endSample;

    const bool addressField = m_field == Field::To || m_field == Field::Tis
                              || m_field == Field::Twas || m_field == Field::From;
    switch (pre) {
    case Preamble::To:
        closeField();
        m_field = Field::To;
        m_fieldChars = chars;
        m_fieldWords = 1;
        ++m_frame.toWordCount;
        if (!m_toFirstWords.contains(chars)) {
            m_toFirstWords << chars;
        }
        break;
    case Preamble::Thru:
        closeField();
        m_field = Field::Thru;
        m_fieldChars = chars;
        ++m_frame.toWordCount;
        if (!m_toFirstWords.contains(chars)) {
            m_toFirstWords << chars;
        }
        break;
    case Preamble::Data:
        if (addressField && m_fieldWords < 5) {
            m_fieldChars += chars;
            ++m_fieldWords;
        } else if (m_field == Field::Amd) {
            m_fieldChars += chars;
        } else if (m_field == Field::Cmd) {
            m_frame.otherCmdWords << word.word;
        }
        break;
    case Preamble::Rep:
        if (addressField && m_lastPreamble == Preamble::Data && m_fieldWords < 5) {
            m_fieldChars += chars;
            ++m_fieldWords;
        } else if (m_field == Field::To) {
            // REP directly after a TO address: another called address (group).
            closeField();
            m_field = Field::To;
            m_fieldChars = chars;
            m_fieldWords = 1;
            ++m_frame.toWordCount;
        } else if (m_field == Field::Thru) {
            closeField();
            m_field = Field::Thru;
            m_fieldChars = chars;
            ++m_frame.toWordCount;
            if (!m_toFirstWords.contains(chars)) {
                m_toFirstWords << chars;
            }
        } else if (m_field == Field::Amd) {
            m_fieldChars += chars;
        } else if (m_field == Field::Cmd) {
            m_frame.otherCmdWords << word.word;
        }
        break;
    case Preamble::Cmd:
        closeField();
        if (allExpanded64(word.word)) {
            m_field = Field::Amd;
            m_fieldChars = chars;
        } else {
            m_field = Field::Cmd;
            m_frame.otherCmdWords << word.word;
        }
        break;
    case Preamble::From:
        closeField();
        m_field = Field::From;
        m_fieldChars = chars;
        m_fieldWords = 1;
        break;
    case Preamble::Tis:
    case Preamble::Twas:
        closeField();
        m_field = pre == Preamble::Tis ? Field::Tis : Field::Twas;
        m_fieldChars = chars;
        m_fieldWords = 1;
        m_frame.tis = pre == Preamble::Tis;
        m_frame.twas = pre == Preamble::Twas;
        m_frame.complete = true;
        m_frame.fromAddress = unstuffAddress(chars);
        break;
    }
    m_lastPreamble = pre;
    return finished;
}

} // namespace AetherSDR::Ale2g
