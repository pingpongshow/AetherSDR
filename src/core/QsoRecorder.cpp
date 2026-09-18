#include "QsoRecorder.h"
#include "AppSettings.h"
#include "LogManager.h"
#include "Resampler.h"
#include "../models/SliceModel.h"

#include <QDir>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr int kDefaultIdleTimeoutSecs = 120;
constexpr int kMinIdleTimeoutSecs = 10;
constexpr int kMaxIdleTimeoutSecs = 3600;

QString defaultRecordingDir()
{
    // Must match the default shown in RadioSetupDialog.
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
           + "/AetherSDR/Recordings";
}

// AppSettings stores booleans as "True"/"False"; accept any case so values
// written via QVariant(bool) ("true"/"false") also parse correctly.
bool readBool(const AppSettings& s, const QString& key, bool defaultValue)
{
    const QString raw = s.value(key, defaultValue ? "True" : "False").toString().trimmed();
    return raw.compare("True", Qt::CaseInsensitive) == 0 || raw == "1";
}

QString boolString(bool on)
{
    return on ? QStringLiteral("True") : QStringLiteral("False");
}

} // namespace

QsoRecorder::QsoRecorder(QObject* parent)
    : QObject(parent)
{
    reloadSettings();

    // Idle timer — fires when no TX activity for m_idleTimeoutSecs
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this]() {
        if (m_recording) {
            qCInfo(lcAudio) << "QsoRecorder: idle timeout, stopping recording";
            stopRecording();
        }
    });
}

QsoRecorder::~QsoRecorder()
{
    if (m_recording)
        finalizeFile();
}

void QsoRecorder::reloadSettings()
{
    // Key names and defaults must match RadioSetupDialog (Recording group).
    const auto& s = AppSettings::instance();

    const QString dir = s.value("QsoRecordingDir", defaultRecordingDir()).toString().trimmed();
    m_recordingDir = dir.isEmpty() ? defaultRecordingDir() : dir;

    bool ok = false;
    const int timeout = s.value("QsoRecordingIdleTimeout",
                                QString::number(kDefaultIdleTimeoutSecs)).toString().toInt(&ok);
    m_idleTimeoutSecs = ok ? qBound(kMinIdleTimeoutSecs, timeout, kMaxIdleTimeoutSecs)
                           : kDefaultIdleTimeoutSecs;

    m_autoRecord  = readBool(s, "QsoRecordingAutoRecord", false);
    m_includeDate = readBool(s, "QsoRecordingIncludeDate", true);
    m_includeTime = readBool(s, "QsoRecordingIncludeTime", true);
    m_includeFreq = readBool(s, "QsoRecordingIncludeFreq", true);
    m_includeMode = readBool(s, "QsoRecordingIncludeMode", true);
    // Callsign has no persisted key; it is only set via setCallsign().
}

void QsoRecorder::setRecordingDir(const QString& path)
{
    m_recordingDir = path;
    auto& s = AppSettings::instance();
    s.setValue("QsoRecordingDir", path);
    s.save();
}

void QsoRecorder::setIdleTimeoutSecs(int secs)
{
    m_idleTimeoutSecs = qBound(kMinIdleTimeoutSecs, secs, kMaxIdleTimeoutSecs);
    auto& s = AppSettings::instance();
    s.setValue("QsoRecordingIdleTimeout", QString::number(m_idleTimeoutSecs));
    s.save();
}

void QsoRecorder::setAutoRecordEnabled(bool on)
{
    m_autoRecord = on;
    auto& s = AppSettings::instance();
    s.setValue("QsoRecordingAutoRecord", boolString(on));
    s.save();
}

// Include* setters persist so reloadSettings() does not revert them.
void QsoRecorder::setIncludeDate(bool on)
{
    m_includeDate = on;
    auto& s = AppSettings::instance();
    s.setValue("QsoRecordingIncludeDate", boolString(on));
    s.save();
}

void QsoRecorder::setIncludeTime(bool on)
{
    m_includeTime = on;
    auto& s = AppSettings::instance();
    s.setValue("QsoRecordingIncludeTime", boolString(on));
    s.save();
}

void QsoRecorder::setIncludeFrequency(bool on)
{
    m_includeFreq = on;
    auto& s = AppSettings::instance();
    s.setValue("QsoRecordingIncludeFreq", boolString(on));
    s.save();
}

void QsoRecorder::setIncludeMode(bool on)
{
    m_includeMode = on;
    auto& s = AppSettings::instance();
    s.setValue("QsoRecordingIncludeMode", boolString(on));
    s.save();
}

void QsoRecorder::setCallsign(const QString& call)
{
    m_callsign = call.trimmed().toUpper();
}

void QsoRecorder::setSlice(SliceModel* slice)
{
    m_slice = slice;
}

int QsoRecorder::recordingDurationSecs() const
{
    if (!m_recording) return 0;
    return static_cast<int>(m_startTime.secsTo(QDateTime::currentDateTimeUtc()));
}

// ── Manual control ──────────────────────────────────────────────────────────

void QsoRecorder::startRecording()
{
    if (m_recording) {
        return;
    }
    // Settings may have changed via Radio Setup dialog since last use
    reloadSettings();
    startFile();
}

void QsoRecorder::stopRecording()
{
    if (!m_recording) return;
    m_idleTimer->stop();
    finalizeFile();
}

// ── Audio feeds ─────────────────────────────────────────────────────────────

// Convert float32 stereo PCM to int16 stereo PCM for WAV output.
static QByteArray float32ToInt16(const QByteArray& pcm)
{
    const int numFloats = pcm.size() / static_cast<int>(sizeof(float));
    QByteArray out(numFloats * static_cast<int>(sizeof(qint16)), Qt::Uninitialized);
    const float* src = reinterpret_cast<const float*>(pcm.constData());
    qint16* dst = reinterpret_cast<qint16*>(out.data());
    for (int i = 0; i < numFloats; ++i) {
        float clamped = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<qint16>(clamped * 32767.0f);
    }
    return out;
}

void QsoRecorder::feedRxAudio(const QByteArray& pcm)
{
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_recording || !m_file) return;
    // During an over that is producing TX audio, the TX feed owns the
    // timeline. If no TX tap is delivering audio (e.g. a TX path that does
    // not emit a tap), keep recording RX so the over isn't silently dropped.
    if (m_txActive.load(std::memory_order_relaxed) && m_txAudioThisOver) {
        return;
    }
    writePcmLocked(float32ToInt16(pcm));
}

void QsoRecorder::feedTxAudio(const QByteArray& pcm)
{
    feedTxPcm(pcm, SAMPLE_RATE, NUM_CHANNELS, true);
}

void QsoRecorder::feedTxPcm(const QByteArray& pcm, int sampleRate, int channels, bool float32)
{
    if (pcm.isEmpty() || sampleRate <= 0 || (channels != 1 && channels != 2)) {
        return;
    }
    if (!m_txActive.load(std::memory_order_relaxed)) {
        return;  // only record TX audio while locally transmitting
    }
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_recording || !m_file) return;

    // Normalise to float32 interleaved.
    QByteArray f32;
    if (float32) {
        f32 = pcm;
    } else {
        const int n = pcm.size() / static_cast<int>(sizeof(qint16));
        f32.resize(n * static_cast<int>(sizeof(float)));
        const auto* src = reinterpret_cast<const qint16*>(pcm.constData());
        auto* dst = reinterpret_cast<float*>(f32.data());
        for (int i = 0; i < n; ++i) {
            dst[i] = src[i] / 32768.0f;
        }
    }
    const int numSamples = f32.size() / static_cast<int>(sizeof(float));
    const auto* fin = reinterpret_cast<const float*>(f32.constData());

    QByteArray stereo24k;
    if (sampleRate == SAMPLE_RATE) {
        if (channels == NUM_CHANNELS) {
            stereo24k = f32;
        } else {
            stereo24k.resize(numSamples * 2 * static_cast<int>(sizeof(float)));
            auto* dst = reinterpret_cast<float*>(stereo24k.data());
            for (int i = 0; i < numSamples; ++i) {
                dst[i * 2] = fin[i];
                dst[i * 2 + 1] = fin[i];
            }
        }
    } else {
        if (!m_txResampler || m_txResamplerRate != sampleRate
            || m_txResamplerChannels != channels) {
            m_txResampler = std::make_unique<Resampler>(
                static_cast<double>(sampleRate), static_cast<double>(SAMPLE_RATE));
            m_txResamplerRate = sampleRate;
            m_txResamplerChannels = channels;
        }
        // TX mic audio is mono content; Resampler downmixes stereo input.
        stereo24k = (channels == 1)
            ? m_txResampler->processMonoToStereo(fin, numSamples)
            : m_txResampler->processStereoToStereo(fin, numSamples / 2);
    }

    m_txAudioThisOver = true;
    writePcmLocked(float32ToInt16(stereo24k));
}

void QsoRecorder::writePcmLocked(const QByteArray& int16pcm)
{
    // Whole stereo frames only, so RX/TX writes never shift the channel
    // alignment of everything that follows.
    const quint32 blockAlign = NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    quint32 bytes = static_cast<quint32>(int16pcm.size()) / blockAlign * blockAlign;
    bytes = std::min(bytes, (kMaxDataBytes - m_dataBytes) / blockAlign * blockAlign);
    if (bytes == 0) {
        return;
    }
    const qint64 written = m_file->write(int16pcm.constData(), bytes);
    if (written <= 0) {
        return;
    }
    // A short write (disk full) may leave a partial frame; count what landed.
    m_dataBytes += static_cast<quint32>(written);
    if (m_dataBytes - m_dataBytesAtLastPatch >= kHeaderPatchIntervalBytes) {
        patchWavHeader();
    }
}

// ── TX state tracking ───────────────────────────────────────────────────────

void QsoRecorder::onMoxChanged(bool mox)
{
    // Settings may have changed via Radio Setup dialog at runtime
    reloadSettings();

    // Only auto-record when in client-side recording mode
    const bool clientSide =
        AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        m_txAudioThisOver = false;
    }
    m_txActive.store(mox, std::memory_order_relaxed);
    if (mox) {
        // TX started — begin recording if auto-record is on and not already recording
        if (clientSide && m_autoRecord && !m_recording) {
            startRecording();
        }

        // Reset idle timer on each TX
        m_idleTimer->stop();
    } else {
        // TX ended — start idle countdown (timeout re-read above)
        if (m_recording) {
            m_idleTimer->start(m_idleTimeoutSecs * 1000);
        }
    }
}

// ── File management ─────────────────────────────────────────────────────────

void QsoRecorder::startFile()
{
    // Capture metadata from active slice at recording start
    if (m_slice) {
        m_freqMhz = m_slice->frequency();
        m_mode = m_slice->mode();
    } else {
        m_freqMhz = 0.0;
        m_mode.clear();
    }

    m_startTime = QDateTime::currentDateTimeUtc();
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        m_dataBytes = 0;
        m_dataBytesAtLastPatch = 0;
    }

    // Ensure directory exists
    QDir dir(m_recordingDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit recordingError("Cannot create recording directory: " + m_recordingDir);
            return;
        }
    }

    QString filePath = m_recordingDir + "/" + buildFilename();

    auto* file = new QFile(filePath, this);
    if (!file->open(QIODevice::WriteOnly)) {
        emit recordingError("Cannot create recording file: " + file->errorString());
        delete file;
        return;
    }

    {
        // Publish the file and the recording flag together: the feed slots
        // read both under this mutex.
        std::lock_guard<std::mutex> lock(m_writeMutex);
        m_file = file;
        writeWavHeader();
        m_recording = true;
    }

    qCInfo(lcAudio) << "QsoRecorder: started recording to" << filePath;
    emit recordingStarted(filePath);
}

void QsoRecorder::finalizeFile()
{
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        m_recording = false;
        if (m_file) {
            patchWavHeader();
        }
    }

    if (!m_file) return;

    QString filePath = m_file->fileName();
    m_lastRecordingPath = filePath;
    int durationSecs = static_cast<int>(m_startTime.secsTo(QDateTime::currentDateTimeUtc()));

    m_file->close();
    m_file->deleteLater();
    m_file = nullptr;

    qCInfo(lcAudio) << "QsoRecorder: stopped recording," << durationSecs << "seconds,"
                     << m_dataBytes << "bytes";
    emit recordingStopped(filePath, durationSecs);
}

QString QsoRecorder::buildFilename() const
{
    QStringList parts;

    if (m_includeDate)
        parts << m_startTime.toString("yyyy-MM-dd");

    if (m_includeTime)
        parts << m_startTime.toString("HHmmss") + "Z";

    if (m_includeFreq && m_freqMhz > 0.0) {
        // Format frequency: e.g. 14.200 MHz → "14.200"
        parts << QString::number(m_freqMhz, 'f', 3) + "MHz";
    }

    if (m_includeMode && !m_mode.isEmpty())
        parts << m_mode;

    if (!m_callsign.isEmpty())
        parts << m_callsign;

    if (parts.isEmpty())
        parts << "QSO";

    return parts.join("_") + ".wav";
}

void QsoRecorder::writeWavHeader()
{
    // Write a placeholder WAV header (44 bytes). The data size fields
    // will be patched in finalizeFile() once we know the total size.
    char header[WAV_HEADER_SIZE] = {};

    const int byteRate = SAMPLE_RATE * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    const int blockAlign = NUM_CHANNELS * (BITS_PER_SAMPLE / 8);

    // RIFF header
    memcpy(header + 0, "RIFF", 4);
    // header[4..7] = file size - 8 (patched later)
    memcpy(header + 8, "WAVE", 4);

    // fmt sub-chunk
    memcpy(header + 12, "fmt ", 4);
    qToLittleEndian<quint32>(16, header + 16);               // sub-chunk size
    qToLittleEndian<quint16>(1, header + 20);                 // audio format (1 = PCM)
    qToLittleEndian<quint16>(NUM_CHANNELS, header + 22);      // channels
    qToLittleEndian<quint32>(SAMPLE_RATE, header + 24);       // sample rate
    qToLittleEndian<quint32>(byteRate, header + 28);          // byte rate
    qToLittleEndian<quint16>(blockAlign, header + 32);        // block align
    qToLittleEndian<quint16>(BITS_PER_SAMPLE, header + 34);   // bits per sample

    // data sub-chunk
    memcpy(header + 36, "data", 4);
    // header[40..43] = data size (patched later)

    m_file->write(header, WAV_HEADER_SIZE);
}

void QsoRecorder::patchWavHeader()
{
    if (!m_file || !m_file->isOpen()) return;

    // Seek back and patch the two size fields in the WAV header
    m_file->seek(4);
    quint32 riffSize = m_dataBytes + WAV_HEADER_SIZE - 8;
    char buf[4];
    qToLittleEndian<quint32>(riffSize, buf);
    m_file->write(buf, 4);

    m_file->seek(40);
    qToLittleEndian<quint32>(m_dataBytes, buf);
    m_file->write(buf, 4);

    // Return to the end so later PCM appends after the data written so far.
    m_file->seek(WAV_HEADER_SIZE + static_cast<qint64>(m_dataBytes));
    m_file->flush();
    m_dataBytesAtLastPatch = m_dataBytes;
}

// ── Playback ───────────────────────────────────────────────────────────────

void QsoRecorder::startPlayback()
{
    if (m_playing || m_lastRecordingPath.isEmpty()) return;

    m_playFile = new QFile(m_lastRecordingPath, this);
    if (!m_playFile->open(QIODevice::ReadOnly)) {
        delete m_playFile;
        m_playFile = nullptr;
        return;
    }

    // Skip WAV header
    m_playFile->seek(WAV_HEADER_SIZE);
    m_playing = true;

    // Feed audio in chunks matching the recording format (24kHz stereo int16).
    // Convert int16 → float32 for AudioEngine. Timer paces at ~10ms intervals
    // (same as AudioEngine RX timer) to avoid buffer overrun.
    static constexpr int kChunkFrames = 240;  // 10ms at 24kHz
    static constexpr int kChunkBytes = kChunkFrames * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);

    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(10);
    connect(m_playTimer, &QTimer::timeout, this, [this]() {
        if (!m_playFile || !m_playFile->isOpen()) {
            stopPlayback();
            return;
        }

        QByteArray chunk = m_playFile->read(kChunkBytes);
        if (chunk.isEmpty()) {
            stopPlayback();
            return;
        }

        // Convert int16 → float32 for AudioEngine
        int numSamples = chunk.size() / static_cast<int>(sizeof(qint16));
        QByteArray floatPcm(numSamples * static_cast<int>(sizeof(float)), Qt::Uninitialized);
        const qint16* src = reinterpret_cast<const qint16*>(chunk.constData());
        float* dst = reinterpret_cast<float*>(floatPcm.data());
        for (int i = 0; i < numSamples; ++i) {
            dst[i] = static_cast<float>(src[i]) / 32768.0f;
        }

        emit playbackAudio(floatPcm);
    });
    m_playTimer->start();
    emit muteRxRequested(true);
    emit playbackStarted();
}

void QsoRecorder::stopPlayback()
{
    if (!m_playing) return;
    m_playing = false;

    if (m_playTimer) {
        m_playTimer->stop();
        m_playTimer->deleteLater();
        m_playTimer = nullptr;
    }

    if (m_playFile) {
        m_playFile->close();
        m_playFile->deleteLater();
        m_playFile = nullptr;
    }

    emit muteRxRequested(false);
    emit playbackStopped();
}

} // namespace AetherSDR
