#include "SliceAudioRouting.h"

#include "Resampler.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// ── SliceAudioRoute ─────────────────────────────────────────────────────────

QString SliceAudioRoute::toSetting() const
{
    QJsonObject o;
    o[QStringLiteral("main")] = toMain;
    o[QStringLiteral("device")] = outputDevice;
    o[QStringLiteral("dax")] = daxChannel;
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

SliceAudioRoute SliceAudioRoute::fromSetting(const QString& s)
{
    SliceAudioRoute r;
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
    if (!doc.isObject()) {
        return r;
    }
    const QJsonObject o = doc.object();
    r.toMain = o.value(QStringLiteral("main")).toBool(true);
    r.outputDevice = o.value(QStringLiteral("device")).toString().trimmed();
    const int dax = o.value(QStringLiteral("dax")).toInt(0);
    r.daxChannel = (dax >= 1 && dax <= 4) ? dax : 0;
    // A slice routed nowhere would silently vanish: fall back to main.
    if (!r.toMain && r.outputDevice.isEmpty() && r.daxChannel == 0) {
        r.toMain = true;
    }
    return r;
}

QString SliceAudioRoute::settingsKey(QChar sliceLetter)
{
    return QStringLiteral("SliceAudioRoute_") + sliceLetter.toUpper();
}

// ── Planning ────────────────────────────────────────────────────────────────

QStringList SliceAudioPlan::outputDevices() const
{
    QStringList out;
    for (const SliceAudioPlanEntry& e : entries) {
        if (!e.outputDevice.isEmpty() && !out.contains(e.outputDevice)) {
            out.append(e.outputDevice);
        }
    }
    return out;
}

const SliceAudioPlanEntry* SliceAudioPlan::entryForDaxChannel(int daxChannel) const
{
    for (const SliceAudioPlanEntry& e : entries) {
        if (e.sourceDaxChannel == daxChannel) {
            return &e;
        }
    }
    return nullptr;
}

SliceAudioPlan planSliceAudioRouting(const QMap<QChar, SliceAudioRoute>& routes,
                                     const QVector<SliceAudioInfo>& slices,
                                     bool bridgeFeedsRadioDax)
{
    SliceAudioPlan plan;

    // How many slices share each radio DAX channel (a DAX channel carries
    // the sum of every slice assigned to it).
    QMap<int, int> daxUsers;
    for (const SliceAudioInfo& s : slices) {
        if (s.radioDaxChannel >= 1 && s.radioDaxChannel <= 4) {
            ++daxUsers[s.radioDaxChannel];
        }
    }

    QSet<int> usedBridgeChannels;
    for (const SliceAudioInfo& s : slices) {
        const SliceAudioRoute route = routes.value(s.letter.toUpper());
        if (route.isDefault() || s.sliceId < 0 || s.sliceId >= kMaxRoutedSlices) {
            continue;
        }
        if (s.radioDaxChannel < 1 || s.radioDaxChannel > 4) {
            plan.slicesNeedingDax.append(s.sliceId);
            plan.warnings.append(QStringLiteral("Slice %1: assign a DAX channel to route its audio "
                                                "separately.").arg(s.letter));
            continue;
        }
        if (daxUsers.value(s.radioDaxChannel) > 1) {
            plan.warnings.append(QStringLiteral("Slice %1 shares DAX channel %2 with another slice — "
                                                "its audio cannot be separated; route ignored.")
                                     .arg(s.letter).arg(s.radioDaxChannel));
            continue;
        }

        SliceAudioPlanEntry e;
        e.sliceId = s.sliceId;
        e.letter = s.letter.toUpper();
        e.sourceDaxChannel = s.radioDaxChannel;
        e.outputDevice = route.outputDevice;
        e.removeFromMain = !route.toMain;

        if (route.daxChannel != 0 && route.daxChannel != s.radioDaxChannel) {
            const bool ownedByRadio = bridgeFeedsRadioDax && daxUsers.contains(route.daxChannel);
            if (ownedByRadio) {
                plan.warnings.append(QStringLiteral("Slice %1: DAX %2 already carries another "
                                                    "slice's audio; DAX target ignored.")
                                         .arg(s.letter).arg(route.daxChannel));
            } else if (usedBridgeChannels.contains(route.daxChannel)) {
                plan.warnings.append(QStringLiteral("Slice %1: DAX %2 is already a target of "
                                                    "another slice; DAX target ignored.")
                                         .arg(s.letter).arg(route.daxChannel));
            } else {
                e.bridgeChannel = route.daxChannel;
                usedBridgeChannels.insert(route.daxChannel);
            }
        }
        // route.daxChannel == radio channel: the existing DAX bridge path
        // already delivers it there — nothing extra to feed.

        if (e.outputDevice.isEmpty() && e.bridgeChannel == 0 && !e.removeFromMain) {
            continue;   // nothing left to do for this slice
        }
        plan.entries.append(e);
    }
    return plan;
}

StereoGain sliceOutputGains(float audioLevel, int audioPan, bool muted)
{
    if (muted) {
        return {0.0f, 0.0f};
    }
    const float g = std::clamp(audioLevel, 0.0f, 100.0f) / 100.0f;
    const int pan = std::clamp(audioPan, 0, 100);
    StereoGain out;
    out.left = g * std::min(1.0f, static_cast<float>(100 - pan) / 50.0f);
    out.right = g * std::min(1.0f, static_cast<float>(pan) / 50.0f);
    return out;
}

// ── SliceMixer ──────────────────────────────────────────────────────────────

struct SliceMixer::Slot {
    Slot(int deviceRate, int capacityPow2)
        : resampler(std::make_unique<Resampler>(kSliceDaxRate, deviceRate, 4096,
                                                Resampler::Profile::LowLatencyVoice))
        , ring(static_cast<size_t>(capacityPow2), 0.0f)
        , mask(static_cast<uint64_t>(capacityPow2) - 1)
    {}

    // Producer side.
    std::unique_ptr<Resampler> resampler;   // 24 kHz mono → device rate
    std::vector<float> ring;                 // mono frames at device rate
    uint64_t mask;
    alignas(64) std::atomic<uint64_t> writePos{0};
    alignas(64) std::atomic<uint64_t> readPos{0};
    std::atomic<float> gainL{1.0f};
    std::atomic<float> gainR{1.0f};
    // Consumer side.
    bool priming{true};

    int available() const
    {
        return static_cast<int>(writePos.load(std::memory_order_acquire)
                                - readPos.load(std::memory_order_acquire));
    }
};

SliceMixer::SliceMixer(const QVector<int>& sliceIds, int deviceRate, int targetLatencyMs)
    : m_deviceRate(std::max(8000, deviceRate))
    , m_targetFrames(std::max(64, m_deviceRate * std::max(10, targetLatencyMs) / 1000))
{
    m_slotIndex.fill(-1);
    // Ring: comfortably above the 3x-target skip threshold (≥ ~1 s).
    int cap = 1;
    while (cap < std::max(m_targetFrames * 8, m_deviceRate)) {
        cap <<= 1;
    }
    for (int id : sliceIds) {
        if (id < 0 || id >= kMaxRoutedSlices || m_slotIndex[static_cast<size_t>(id)] >= 0) {
            continue;
        }
        m_slotIndex[static_cast<size_t>(id)] = static_cast<int>(m_slots.size());
        m_slots.push_back(std::make_unique<Slot>(m_deviceRate, cap));
    }
}

SliceMixer::~SliceMixer() = default;

SliceMixer::Slot* SliceMixer::slot(int sliceId) const
{
    if (sliceId < 0 || sliceId >= kMaxRoutedSlices) {
        return nullptr;
    }
    const int idx = m_slotIndex[static_cast<size_t>(sliceId)];
    return idx >= 0 ? m_slots[static_cast<size_t>(idx)].get() : nullptr;
}

bool SliceMixer::hasSlice(int sliceId) const
{
    return slot(sliceId) != nullptr;
}

void SliceMixer::setGain(int sliceId, StereoGain g)
{
    if (Slot* s = slot(sliceId)) {
        s->gainL.store(g.left, std::memory_order_relaxed);
        s->gainR.store(g.right, std::memory_order_relaxed);
    }
}

int SliceMixer::bufferedFrames(int sliceId) const
{
    const Slot* s = slot(sliceId);
    return s ? s->available() : 0;
}

int SliceMixer::push(int sliceId, const float* stereo24k, int frames)
{
    Slot* s = slot(sliceId);
    if (!s) {
        return -1;
    }
    if (!stereo24k || frames <= 0) {
        return 0;
    }
    const QByteArray mono = s->resampler->processStereoToMono(stereo24k, frames);
    const auto* src = reinterpret_cast<const float*>(mono.constData());
    const int n = static_cast<int>(mono.size() / static_cast<qsizetype>(sizeof(float)));
    const uint64_t w = s->writePos.load(std::memory_order_relaxed);
    const uint64_t r = s->readPos.load(std::memory_order_acquire);
    const int freeFrames = static_cast<int>(s->ring.size() - (w - r));
    const int count = std::min(n, freeFrames);   // full: drop newest (consumer resyncs)
    for (int i = 0; i < count; ++i) {
        s->ring[static_cast<size_t>((w + static_cast<uint64_t>(i)) & s->mask)] = src[i];
    }
    s->writePos.store(w + static_cast<uint64_t>(count), std::memory_order_release);
    return count;
}

void SliceMixer::pull(float* out, int frames, int channels)
{
    if (!out || frames <= 0 || channels <= 0) {
        return;
    }
    std::fill(out, out + static_cast<size_t>(frames) * static_cast<size_t>(channels), 0.0f);
    if (m_scratch.size() < static_cast<size_t>(frames)) {
        m_scratch.resize(static_cast<size_t>(frames));   // grows once; not in steady state
    }

    for (const std::unique_ptr<Slot>& sp : m_slots) {
        Slot& s = *sp;
        int avail = s.available();

        // Overflow / drift: skip back to the target.
        if (avail > m_targetFrames * 3) {
            const int skip = avail - m_targetFrames;
            s.readPos.fetch_add(static_cast<uint64_t>(skip), std::memory_order_acq_rel);
            m_skipped.fetch_add(static_cast<uint64_t>(skip), std::memory_order_relaxed);
            avail -= skip;
        }
        if (s.priming) {
            if (avail < m_targetFrames) {
                continue;   // silence until primed
            }
            s.priming = false;
        }

        const int n = std::min(avail, frames);
        const uint64_t r = s.readPos.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            m_scratch[static_cast<size_t>(i)] = s.ring[static_cast<size_t>((r + static_cast<uint64_t>(i)) & s.mask)];
        }
        s.readPos.store(r + static_cast<uint64_t>(n), std::memory_order_release);
        if (n < frames) {
            s.priming = true;   // underrun: pad with silence and re-prime
            m_underruns.fetch_add(1, std::memory_order_relaxed);
        }

        const float gl = s.gainL.load(std::memory_order_relaxed);
        const float gr = s.gainR.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            const float v = m_scratch[static_cast<size_t>(i)];
            float* frame = out + static_cast<size_t>(i) * static_cast<size_t>(channels);
            if (channels == 1) {
                frame[0] += 0.5f * (gl + gr) * v;
            } else {
                frame[0] += gl * v;
                frame[1] += gr * v;
            }
        }
    }
}

} // namespace AetherSDR
