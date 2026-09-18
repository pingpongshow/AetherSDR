#pragma once

// 2G ALE frame construction and parsing (MIL-STD-188-141B App. A, A.5.2.5,
// A.5.3, A.5.5.3, A.5.7.2).
//
// Frame = calling cycle (scanning call Tsc + leading call Tlc)
//         + optional message section (CMD AMD / other CMDs)
//         + conclusion (TIS or TWAS + whole address).
// Every element of the word vectors below is a *redundant* word (Trw, 392 ms).

#include "Ale2gModem.h"
#include "Ale2gWord.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>

namespace AetherSDR::Ale2g {

namespace Frames {

// Number of redundant words needed for a scanning call / scanning sound that
// must capture a receiver scanning `channels` channels with `dwellMs` dwell:
// Tsc >= Ts (+ one Tdrw = 2 Trw spare for detection, A.5.5.3.1 / annex B).
int scanWordsForScan(int channels, int dwellMs);
int wordsForDurationMs(int ms);

// Individual (or net) call: TO first-word x scanWords, whole address x 2,
// optional AMD, TIS/TWAS self.
QVector<std::uint32_t> buildCall(const QString& to, const QString& self, bool tis,
                                 int scanWords, const QString& amdText = {});
// AllCall "@?@" / AnyCall "@@?" use the same structure with the special address.
QVector<std::uint32_t> buildAllCall(const QString& self, bool tis, int scanWords,
                                    const QString& amdText = {});
QVector<std::uint32_t> buildAnyCall(const QString& self, int scanWords,
                                    const QString& amdText = {});
// Response / acknowledgment / link termination: single-channel call form
// (leading call x 2 + conclusion), A.5.5.3.3-A.5.5.3.5, figures A-21a/A-30/A-31.
QVector<std::uint32_t> buildResponse(const QString& to, const QString& self, bool tis,
                                     const QString& amdText = {});
// Sound: whole self address repeated for Tsrs = Tss + Trs (A.5.3, fig. A-22).
QVector<std::uint32_t> buildSound(const QString& self, bool tis, int minimumWords);

} // namespace Frames

struct Frame {
    QStringList toAddresses;     // called addresses (unstuffed), duplicates removed
    QString fromAddress;         // TIS / TWAS (or FROM quick-ID) address
    bool tis{false};
    bool twas{false};
    bool allCall{false};
    bool anyCall{false};
    QStringList amdMessages;
    QVector<std::uint32_t> otherCmdWords;   // non-AMD CMD words (LQA etc.), raw
    int wordCount{0};
    int toWordCount{0};          // TO/THRU words (scanning + leading call length)
    int nonUnanimousSum{0};
    double snrSum{0.0};
    qint64 startSample{0};
    qint64 endSample{0};
    bool complete{false};        // conclusion received

    int berCode() const { return wordCount > 0 ? nonUnanimousSum / wordCount : 48; }
    float meanSnrDb() const { return wordCount > 0 ? static_cast<float>(snrSum / wordCount) : 0.0f; }
    bool isSound() const { return toAddresses.isEmpty() && !allCall && !anyCall && complete; }
    QString summary() const;
};

class FrameAssembler {
public:
    void reset();
    bool inFrame() const { return m_active; }
    // First words of TO/THRU addresses seen so far in the current frame
    // (lets a scanning receiver stop on "TO <self first word>").
    const QStringList& partialToFirstWords() const { return m_toFirstWords; }

    // Feed a demodulated word; returns a completed frame when one ends
    // (a new frame starting after a conclusion ends the previous one).
    std::optional<Frame> addWord(const Demodulator::Word& word);
    // Demodulator lost sync: flush the current frame (complete or not).
    std::optional<Frame> flush();

private:
    enum class Field { None, To, Thru, From, Tis, Twas, Amd, Cmd };

    void beginFrameIfNeeded(const Demodulator::Word& word);
    void closeField();
    std::optional<Frame> takeFrame();

    bool m_active{false};
    Frame m_frame;
    Field m_field{Field::None};
    QString m_fieldChars;
    Preamble m_lastPreamble{Preamble::Data};
    int m_fieldWords{0};
    QStringList m_toFirstWords;
};

} // namespace AetherSDR::Ale2g
