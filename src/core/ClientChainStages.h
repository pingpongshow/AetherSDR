#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define AETHER_CLIENT_CHAIN_HAS_MXCSR 1
#endif

namespace AetherSDR {

// Stage identifiers shared by the client-side TX and RX DSP chains.
// Values are persisted indirectly (by name, see stageName()) and are
// packed one byte per slot into a uint64_t for lock-free handoff to
// the audio threads, so they must stay < 256 and None must stay 0.
enum class ClientChainStage : uint8_t {
    None   = 0,   // sentinel / end-of-list marker
    Gate   = 1,
    Eq     = 2,
    DeEss  = 3,
    Comp   = 4,
    Tube   = 5,
    Enh    = 6,   // PUDU slot (legacy enum name)
    Reverb = 7,
};

// Which client-side chain a stage / DSP instance belongs to.
//   Tx — mic → chain → VITA-49 encode (24 kHz stereo, TX audio thread)
//   Rx — post-NR → chain → resample/sink (24 kHz stereo float32)
enum class ClientChainPath : uint8_t {
    Tx = 0,
    Rx = 1,
};

// Header-only helpers so the packing / persistence logic can be unit
// tested without pulling in AudioEngine (QtMultimedia) — see
// tests/client_chain_test.cpp.
namespace ClientChain {

inline constexpr int kMaxStages = 8;  // packs into uint64_t

// Pack a stage list into the uint64_t atomic format used by the audio
// threads.  Unused slots are ClientChainStage::None (0).  Stages past
// kMaxStages are dropped.
inline uint64_t pack(const QVector<ClientChainStage>& stages)
{
    uint64_t v = 0;
    const int n = std::min(static_cast<int>(stages.size()), kMaxStages);
    for (int i = 0; i < n; ++i) {
        v |= static_cast<uint64_t>(static_cast<uint8_t>(stages[i])) << (i * 8);
    }
    return v;
}

inline ClientChainStage stageAt(uint64_t packed, int slot) noexcept
{
    return static_cast<ClientChainStage>((packed >> (slot * 8)) & 0xFF);
}

inline QVector<ClientChainStage> unpack(uint64_t v)
{
    QVector<ClientChainStage> out;
    out.reserve(kMaxStages);
    for (int i = 0; i < kMaxStages; ++i) {
        const ClientChainStage s = stageAt(v, i);
        if (s == ClientChainStage::None) {
            break;
        }
        out.append(s);
    }
    return out;
}

// Audio-thread walk of a packed chain.  Calls fn(stage) for each slot
// in order, stopping at the first None.  No allocation.
template <typename Fn>
inline void forEachStage(uint64_t packed, Fn&& fn)
{
    for (int i = 0; i < kMaxStages; ++i) {
        const ClientChainStage s = stageAt(packed, i);
        if (s == ClientChainStage::None) {
            return;
        }
        fn(s);
    }
}

// Human-readable names keep the XML settings file inspectable.
inline QString stageName(ClientChainStage s)
{
    switch (s) {
        case ClientChainStage::Gate:   return QStringLiteral("Gate");
        case ClientChainStage::Eq:     return QStringLiteral("Eq");
        case ClientChainStage::DeEss:  return QStringLiteral("DeEss");
        case ClientChainStage::Comp:   return QStringLiteral("Comp");
        case ClientChainStage::Tube:   return QStringLiteral("Tube");
        case ClientChainStage::Enh:    return QStringLiteral("Enh");
        case ClientChainStage::Reverb: return QStringLiteral("Reverb");
        case ClientChainStage::None:   return QString();
    }
    return QString();
}

inline ClientChainStage stageFromName(const QString& name)
{
    if (name == QLatin1String("Gate"))   { return ClientChainStage::Gate; }
    if (name == QLatin1String("Eq"))     { return ClientChainStage::Eq; }
    if (name == QLatin1String("DeEss"))  { return ClientChainStage::DeEss; }
    if (name == QLatin1String("Comp"))   { return ClientChainStage::Comp; }
    if (name == QLatin1String("Tube"))   { return ClientChainStage::Tube; }
    if (name == QLatin1String("Enh"))    { return ClientChainStage::Enh; }
    if (name == QLatin1String("Reverb")) { return ClientChainStage::Reverb; }
    return ClientChainStage::None;
}

// Canonical default order for a fresh install (both paths).
inline QVector<ClientChainStage> defaultOrder()
{
    return {
        ClientChainStage::Gate,
        ClientChainStage::Eq,
        ClientChainStage::DeEss,
        ClientChainStage::Comp,
        ClientChainStage::Tube,
        ClientChainStage::Enh,
        ClientChainStage::Reverb,
    };
}

// Append any canonical stages missing from `stages` and drop
// duplicates, so every processor box is always present exactly once.
inline QVector<ClientChainStage> normalize(const QVector<ClientChainStage>& stages)
{
    QVector<ClientChainStage> out;
    out.reserve(kMaxStages);
    for (ClientChainStage s : stages) {
        if (s != ClientChainStage::None && !out.contains(s)) {
            out.append(s);
        }
    }
    for (ClientChainStage canon : defaultOrder()) {
        if (!out.contains(canon)) {
            out.append(canon);
        }
    }
    return out;
}

// "Gate,Eq,DeEss" ⇄ stage list.  Unknown names are skipped.
inline QVector<ClientChainStage> parseList(const QString& stored)
{
    QVector<ClientChainStage> out;
    for (const QString& name : stored.split(',', Qt::SkipEmptyParts)) {
        const ClientChainStage s = stageFromName(name.trimmed());
        if (s != ClientChainStage::None) {
            out.append(s);
        }
    }
    return out;
}

inline QString serializeList(const QVector<ClientChainStage>& stages)
{
    QStringList names;
    for (ClientChainStage s : stages) {
        const QString n = stageName(s);
        if (!n.isEmpty()) {
            names.append(n);
        }
    }
    return names.join(',');
}

// RAII guard that enables flush-to-zero + denormals-are-zero on x86 for
// the duration of one chain block, restoring the caller's MXCSR after.
// The IIR stages (EQ biquads, PUDU/Tube filters, reverb combs) decay into
// the subnormal range for minutes after their input goes digitally silent
// (muted mic, muted slice), and x86 float ops on subnormals are 10-100x
// slower — enough to overrun the audio thread.  ARM64 handles subnormals
// in hardware, so this is a no-op there.
class ScopedNoDenormals {
public:
#ifdef AETHER_CLIENT_CHAIN_HAS_MXCSR
    ScopedNoDenormals() noexcept : m_saved(_mm_getcsr())
    {
        _mm_setcsr(m_saved | kFlushToZero | kDenormalsAreZero);
    }
    ~ScopedNoDenormals() { _mm_setcsr(m_saved); }
#else
    ScopedNoDenormals() noexcept = default;
#endif
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
#ifdef AETHER_CLIENT_CHAIN_HAS_MXCSR
    static constexpr unsigned int kFlushToZero      = 0x8000;
    static constexpr unsigned int kDenormalsAreZero = 0x0040;
    unsigned int m_saved;
#endif
};

} // namespace ClientChain

} // namespace AetherSDR
