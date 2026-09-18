#include "FreeDvEngine.h"
#include "LogManager.h"
#include "Resampler.h"

#include <algorithm>
#include <cmath>

#ifdef HAVE_CODEC2
extern "C" {
#include <freedv_api.h>
#include <modem_stats.h>
#include <reliable_text.h>
}
#endif

namespace AetherSDR {

namespace {
constexpr float kInt16Scale = 32767.0f;
constexpr int kMaxTextLineChars = 64;

short toShort(float v)
{
    return static_cast<short>(std::lround(std::clamp(v * kInt16Scale, -32768.0f, 32767.0f)));
}

#ifdef HAVE_CODEC2
int codec2ModeFor(DigitalVoiceMode m)
{
    switch (m) {
    case DigitalVoiceMode::FreeDv700D: return FREEDV_MODE_700D;
    case DigitalVoiceMode::FreeDv700E: return FREEDV_MODE_700E;
    case DigitalVoiceMode::FreeDv1600: return FREEDV_MODE_1600;
    case DigitalVoiceMode::FreeDv700C: return FREEDV_MODE_700C;
    default: break;
    }
    return -1;
}

bool usesReliableText(DigitalVoiceMode m)
{
    return m == DigitalVoiceMode::FreeDv700D || m == DigitalVoiceMode::FreeDv700E;
}
#endif
} // namespace

FreeDvEngine::FreeDvEngine(DigitalVoiceMode mode, QObject* parent)
    : DigitalVoiceEngine(parent)
    , m_mode(mode)
{}

FreeDvEngine::~FreeDvEngine()
{
    stop();
}

bool FreeDvEngine::isSupported()
{
#ifdef HAVE_CODEC2
    return true;
#else
    return false;
#endif
}

bool FreeDvEngine::start()
{
#ifdef HAVE_CODEC2
    if (m_fdv) {
        return true;
    }
    const int c2mode = codec2ModeFor(m_mode);
    if (c2mode < 0) {
        qCWarning(lcRade) << "FreeDvEngine: unsupported mode" << digitalVoiceModeName(m_mode);
        return false;
    }

    m_fdv = freedv_open(c2mode);
    if (!m_fdv) {
        qCWarning(lcRade) << "FreeDvEngine: freedv_open failed for" << digitalVoiceModeName(m_mode);
        return false;
    }

    // OFDM voice modes: enable TX clipper + BPF (freedv-gui defaults for
    // 700D/700E, improves average power without widening the signal).
    if (m_mode == DigitalVoiceMode::FreeDv700D || m_mode == DigitalVoiceMode::FreeDv700E) {
        freedv_set_clip(m_fdv, true);
        freedv_set_tx_bpf(m_fdv, 1);
    }

    m_modemRate  = freedv_get_modem_sample_rate(m_fdv);
    m_speechRate = freedv_get_speech_sample_rate(m_fdv);

    m_stats = std::make_unique<MODEM_STATS>();

    // Low-latency voice profile: ~16 ms per 8 kHz stage instead of ~297 ms
    // with the r8brain 24-bit defaults (see resampler_rate_test).
    constexpr auto kProfile = Resampler::Profile::LowLatencyVoice;
    m_rxDown = std::make_unique<Resampler>(24000, m_modemRate, 4096, kProfile);
    m_rxUp   = std::make_unique<Resampler>(m_speechRate, 24000, 4096, kProfile);
    m_txDown = std::make_unique<Resampler>(24000, m_speechRate, 4096, kProfile);
    m_txUp   = std::make_unique<Resampler>(m_modemRate, 24000, 4096, kProfile);

    m_demodIn.assign(static_cast<size_t>(freedv_get_n_max_modem_samples(m_fdv)), 0);
    m_speechOut.assign(static_cast<size_t>(freedv_get_n_max_speech_samples(m_fdv)), 0);
    m_speechIn.assign(static_cast<size_t>(freedv_get_n_speech_samples(m_fdv)), 0);
    m_modOut.assign(static_cast<size_t>(freedv_get_n_tx_modem_samples(m_fdv)), 0);
    m_rxAccum.clear();
    m_txAccum.clear();
    m_rxTextLine.clear();
    m_statsSampleCounter = 0;

    if (usesReliableText(m_mode)) {
        m_reliableText = reliable_text_create();
        reliable_text_use_with_freedv(m_reliableText, m_fdv,
                                      &FreeDvEngine::onReliableTextRx, this);
    } else {
        freedv_set_callback_txt(m_fdv, &FreeDvEngine::onPlainTextRx,
                                &FreeDvEngine::onPlainTextTx, this);
    }
    applyTxText();
    applySquelch();

    m_synced.store(false);
    m_active.store(true);
    qCInfo(lcRade) << "FreeDvEngine: started" << digitalVoiceModeName(m_mode)
                   << "modemRate=" << m_modemRate << "speechRate=" << m_speechRate
                   << "nSpeech=" << m_speechIn.size() << "nTxModem=" << m_modOut.size();
    return true;
#else
    qCWarning(lcRade) << "FreeDvEngine: built without codec2 (HAVE_CODEC2 not defined)";
    return false;
#endif
}

void FreeDvEngine::stop()
{
#ifdef HAVE_CODEC2
    if (!m_fdv) {
        return;
    }
    m_active.store(false);
    if (m_reliableText) {
        reliable_text_unlink_from_freedv(m_reliableText);
        reliable_text_destroy(m_reliableText);
        m_reliableText = nullptr;
    }
    freedv_close(m_fdv);
    m_fdv = nullptr;
    m_stats.reset();
    m_rxDown.reset();
    m_rxUp.reset();
    m_txDown.reset();
    m_txUp.reset();
    m_rxAccum.clear();
    m_txAccum.clear();
    if (m_synced.exchange(false)) {
        emit syncChanged(false);
    }
    qCInfo(lcRade) << "FreeDvEngine: stopped";
#endif
}

void FreeDvEngine::resetTx()
{
#ifdef HAVE_CODEC2
    m_txAccum.clear();
    m_txTextPos = 0;
#endif
}

void FreeDvEngine::setTxText(const QString& text)
{
    m_txText = text.trimmed().toUpper();
#ifdef HAVE_CODEC2
    if (m_fdv) {
        applyTxText();
    }
#endif
}

void FreeDvEngine::setSquelch(bool enabled, float snrThresholdDb)
{
    m_squelchEnabled = enabled;
    m_squelchThreshDb = snrThresholdDb;
#ifdef HAVE_CODEC2
    if (m_fdv) {
        applySquelch();
    }
#endif
}

void FreeDvEngine::feedTxAudio(const QByteArray& pcm)
{
#ifdef HAVE_CODEC2
    if (!m_fdv) {
        return;
    }
    const int frames = static_cast<int>(pcm.size() / (2 * sizeof(float)));
    if (frames <= 0) {
        return;
    }
    QByteArray mono = m_txDown->processStereoToMono(
        reinterpret_cast<const float*>(pcm.constData()), frames);
    const auto* mf = reinterpret_cast<const float*>(mono.constData());
    m_txAccum.insert(m_txAccum.end(), mf, mf + mono.size() / static_cast<int>(sizeof(float)));

    const size_t nSpeech = m_speechIn.size();
    const int nMod = static_cast<int>(m_modOut.size());
    size_t consumed = 0;
    QByteArray modemFloat;
    while (m_txAccum.size() - consumed >= nSpeech) {
        for (size_t i = 0; i < nSpeech; ++i) {
            m_speechIn[i] = toShort(m_txAccum[consumed + i]);
        }
        consumed += nSpeech;
        freedv_tx(m_fdv, m_modOut.data(), m_speechIn.data());

        const qsizetype start = modemFloat.size();
        modemFloat.resize(start + nMod * static_cast<qsizetype>(sizeof(float)));
        auto* dst = reinterpret_cast<float*>(modemFloat.data() + start);
        for (int i = 0; i < nMod; ++i) {
            dst[i] = static_cast<float>(m_modOut[static_cast<size_t>(i)]) / 32768.0f;
        }
    }
    if (consumed > 0) {
        m_txAccum.erase(m_txAccum.begin(), m_txAccum.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    if (!modemFloat.isEmpty()) {
        QByteArray stereo = m_txUp->processMonoToStereo(
            reinterpret_cast<const float*>(modemFloat.constData()),
            static_cast<int>(modemFloat.size() / sizeof(float)));
        if (!stereo.isEmpty()) {
            emit txModemReady(stereo);
        }
    }
#else
    Q_UNUSED(pcm);
#endif
}

void FreeDvEngine::feedRxAudio(int channel, const QByteArray& pcm)
{
    Q_UNUSED(channel);  // caller (MainWindow) already filters by the slice's DAX channel
#ifdef HAVE_CODEC2
    if (!m_fdv) {
        return;
    }
    const int frames = static_cast<int>(pcm.size() / (2 * sizeof(float)));
    if (frames <= 0) {
        return;
    }
    QByteArray mono = m_rxDown->processStereoToMono(
        reinterpret_cast<const float*>(pcm.constData()), frames);
    const auto* mf = reinterpret_cast<const float*>(mono.constData());
    const int nMono = static_cast<int>(mono.size() / sizeof(float));
    m_rxAccum.insert(m_rxAccum.end(), mf, mf + nMono);

    QByteArray speechFloat;
    size_t consumed = 0;
    int nin = freedv_nin(m_fdv);
    while (nin > 0 && m_rxAccum.size() - consumed >= static_cast<size_t>(nin)) {
        for (int i = 0; i < nin; ++i) {
            m_demodIn[static_cast<size_t>(i)] = toShort(m_rxAccum[consumed + static_cast<size_t>(i)]);
        }
        consumed += static_cast<size_t>(nin);
        const int nout = freedv_rx(m_fdv, m_speechOut.data(), m_demodIn.data());
        m_statsSampleCounter += nin;

        if (nout > 0) {
            const qsizetype start = speechFloat.size();
            speechFloat.resize(start + nout * static_cast<qsizetype>(sizeof(float)));
            auto* dst = reinterpret_cast<float*>(speechFloat.data() + start);
            for (int i = 0; i < nout; ++i) {
                dst[i] = static_cast<float>(m_speechOut[static_cast<size_t>(i)]) / 32768.0f;
            }
        }

        const bool synced = freedv_get_sync(m_fdv) != 0;
        if (synced != m_synced.load()) {
            m_synced.store(synced);
            if (!synced && m_reliableText) {
                reliable_text_reset(m_reliableText);
            }
            emit syncChanged(synced);
            publishStats(true);
        }
        nin = freedv_nin(m_fdv);
    }
    if (consumed > 0) {
        m_rxAccum.erase(m_rxAccum.begin(), m_rxAccum.begin() + static_cast<std::ptrdiff_t>(consumed));
    }

    // Stats at ~4 Hz of modem time.
    if (m_statsSampleCounter >= m_modemRate / 4) {
        publishStats(false);
    }

    if (!speechFloat.isEmpty()) {
        QByteArray stereo = m_rxUp->processMonoToStereo(
            reinterpret_cast<const float*>(speechFloat.constData()),
            static_cast<int>(speechFloat.size() / sizeof(float)));
        if (!stereo.isEmpty()) {
            emit rxSpeechReady(stereo);
        }
    }
#else
    Q_UNUSED(pcm);
#endif
}

#ifdef HAVE_CODEC2
void FreeDvEngine::publishStats(bool force)
{
    if (!m_fdv || !m_stats) {
        return;
    }
    m_statsSampleCounter = 0;
    if (!m_synced.load() && !force) {
        return;
    }
    freedv_get_modem_extended_stats(m_fdv, m_stats.get());
    int sync = 0;
    float snr = 0.0f;
    freedv_get_modem_stats(m_fdv, &sync, &snr);
    if (sync) {
        emit snrChanged(snr);
        emit freqOffsetChanged(m_stats->foff);
    }
}

void FreeDvEngine::applySquelch()
{
    freedv_set_squelch_en(m_fdv, m_squelchEnabled);
    freedv_set_snr_squelch_thresh(m_fdv, m_squelchThreshDb);
}

void FreeDvEngine::applyTxText()
{
    if (m_reliableText) {
        const QByteArray bytes = m_txText.toLatin1();
        reliable_text_set_string(m_reliableText, bytes.constData(), static_cast<int>(bytes.size()));
    } else {
        // Plain varicode channel: repeat "<text>\r" (freedv-gui convention).
        m_txTextBytes = m_txText.isEmpty() ? QByteArray() : m_txText.toLatin1() + '\r';
        m_txTextPos = 0;
    }
}

void FreeDvEngine::emitText(const QString& text)
{
    const QString t = text.trimmed();
    if (!t.isEmpty()) {
        emit textReceived(t);
    }
}

void FreeDvEngine::onReliableTextRx(void* rt, const char* txt, int length, void* state)
{
    auto* self = static_cast<FreeDvEngine*>(state);
    if (self && txt && length > 0) {
        self->emitText(QString::fromLatin1(txt, length));
    }
    // Ready for the next string (freedv-gui resets after each decode).
    if (rt) {
        reliable_text_reset(rt);
    }
}

void FreeDvEngine::onPlainTextRx(void* state, char c)
{
    auto* self = static_cast<FreeDvEngine*>(state);
    if (!self) {
        return;
    }
    if (c == '\r' || c == '\n') {
        if (!self->m_rxTextLine.isEmpty()) {
            self->emitText(QString::fromLatin1(self->m_rxTextLine));
            self->m_rxTextLine.clear();
        }
        return;
    }
    if (static_cast<unsigned char>(c) < 0x20) {
        return;
    }
    self->m_rxTextLine.append(c);
    if (self->m_rxTextLine.size() >= kMaxTextLineChars) {
        self->emitText(QString::fromLatin1(self->m_rxTextLine));
        self->m_rxTextLine.clear();
    }
}

char FreeDvEngine::onPlainTextTx(void* state)
{
    auto* self = static_cast<FreeDvEngine*>(state);
    if (!self || self->m_txTextBytes.isEmpty()) {
        return 0;
    }
    if (self->m_txTextPos >= self->m_txTextBytes.size()) {
        self->m_txTextPos = 0;
    }
    return self->m_txTextBytes.at(self->m_txTextPos++);
}
#endif

} // namespace AetherSDR
