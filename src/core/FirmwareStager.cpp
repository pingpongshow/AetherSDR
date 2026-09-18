#include "FirmwareStager.h"
#include "LogManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace AetherSDR {

FirmwareStager::FirmwareStager(QObject* parent)
    : QObject(parent)
{
}

QString FirmwareStager::stagingDir()
{
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation) + "/firmware";
    QDir().mkpath(dir);
    return dir;
}

QString FirmwareStager::modelToFamily(const QString& model)
{
    // Platform codenames (from FlexLib ModelInfo.cs):
    //   Microburst  = FLEX-6300, 6500, 6700, 6700R       → FLEX-6x00 firmware
    //   DeepEddy    = FLEX-6400, 6400M, 6600, 6600M      → FLEX-6x00 firmware
    //   BigBend     = FLEX-8400, 8400M, 8600, 8600M,     → FLEX-6x00 firmware
    //                 AU-510, AU-510M, AU-520, AU-520M
    //   DragonFire  = FLEX-9600 (government only)         → FLEX-9600 firmware
    //
    // ALL consumer radios use FLEX-6x00 firmware.
    if (model.contains("9600"))
        return "9600";
    return "6x00";
}

// ─── Step 1: Check for update ────────────────────────────────────────────────

void FirmwareStager::checkForUpdate(const QString& currentVersion)
{
    emit stageProgress(0, "Checking for updates...");

    auto* reply = m_nam.get(QNetworkRequest(QUrl(SOFTWARE_PAGE)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, currentVersion]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit updateCheckFailed("Cannot reach FlexRadio: " + reply->errorString());
            return;
        }

        const QString html = QString::fromUtf8(reply->readAll());

        // Find latest version from the software page
        // Pattern: SmartSDR v4.1.5 or smartsdr-v4-1-5
        QRegularExpression re(R"(SmartSDR[- _]v?(\d+\.\d+\.\d+))",
                              QRegularExpression::CaseInsensitiveOption);
        QString latest;
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            auto m = it.next();
            const QString v = m.captured(1);
            if (latest.isEmpty() || v > latest)
                latest = v;
        }

        if (latest.isEmpty()) {
            emit updateCheckFailed("Could not determine latest version from FlexRadio website.");
            return;
        }

        // Compare major.minor.patch only (ignore build number like .39794)
        auto normalize = [](const QString& v) {
            const auto parts = v.split('.');
            if (parts.size() >= 3)
                return QString("%1.%2.%3").arg(parts[0], parts[1], parts[2]);
            return v;
        };
        const QString normLatest  = normalize(latest);
        const QString normCurrent = normalize(currentVersion);
        const bool updateAvailable = (normLatest > normCurrent);
        emit updateCheckComplete(latest, updateAvailable);
    });
}

// ─── Step 2: Download installer ──────────────────────────────────────────────

void FirmwareStager::downloadAndStage(const QString& version, const QString& modelFamily)
{
    m_targetVersion = version;
    m_modelFamily = modelFamily;
    m_cancelled = false;
    m_stagedPath.clear();
    m_stagedVersion.clear();

    // First, fetch the MD5 hash file
    emit stageProgress(0, "Fetching integrity hash...");

    const QString md5Url = QString(MD5_URL_FMT).arg(version);
    auto* md5Reply = m_nam.get(QNetworkRequest(QUrl(md5Url)));
    connect(md5Reply, &QNetworkReply::finished, this, [this, md5Reply, version]() {
        md5Reply->deleteLater();
        if (m_cancelled) return;

        if (md5Reply->error() == QNetworkReply::NoError) {
            // Parse MD5 from the hash file
            // Format: "MD-5:	17b7880f646dbaec9e387c2db35a997c"
            const QString body = QString::fromUtf8(md5Reply->readAll());
            QRegularExpression re(R"(MD-5:\s*([0-9a-fA-F]{32}))");
            auto m = re.match(body);
            if (m.hasMatch()) {
                m_expectedMd5 = m.captured(1).toLower();
                qCDebug(lcFirmware) << "FirmwareStager: expected installer MD5:" << m_expectedMd5;
            } else {
                qCWarning(lcFirmware) << "FirmwareStager: could not parse MD5 from hash file";
            }
        } else {
            qCWarning(lcFirmware) << "FirmwareStager: MD5 hash file not available (non-fatal)";
        }

        // Now download the installer
        const QString installerUrl = QString(INSTALLER_URL_FMT).arg(version);
        m_installerPath = stagingDir() + "/SmartSDR_v" + version + "_Installer.exe";

        // Check if we already have it cached
        if (QFileInfo::exists(m_installerPath) && !m_expectedMd5.isEmpty()) {
            emit stageProgress(50, "Verifying cached installer...");
            QFile f(m_installerPath);
            if (f.open(QIODevice::ReadOnly)) {
                QCryptographicHash hash(QCryptographicHash::Md5);
                hash.addData(&f);
                const QString md5 = hash.result().toHex().toLower();
                f.close();
                if (md5 == m_expectedMd5) {
                    emit stageProgress(50, "Using cached installer (MD5 verified) ✓");
                    verifyAndExtract();
                    return;
                }
                qCDebug(lcFirmware) << "FirmwareStager: cached installer MD5 mismatch, re-downloading";
            }
        }

        emit stageProgress(1, "Downloading SmartSDR installer...");

        QNetworkRequest req{QUrl{installerUrl}};
        m_downloadReply = m_nam.get(req);
        connect(m_downloadReply, &QNetworkReply::downloadProgress,
                this, &FirmwareStager::onInstallerDownloadProgress);
        connect(m_downloadReply, &QNetworkReply::finished,
                this, &FirmwareStager::onInstallerDownloadFinished);
    });
}

void FirmwareStager::cancel()
{
    m_cancelled = true;
    if (m_downloadReply) {
        m_downloadReply->abort();
        m_downloadReply = nullptr;
    }
    emit stageFailed("Cancelled.");
}

void FirmwareStager::onInstallerDownloadProgress(qint64 received, qint64 total)
{
    if (m_cancelled) return;
    if (total <= 0) {
        emit stageProgress(1, QString("Downloading... %1 MB").arg(received / (1024*1024)));
        return;
    }
    // Download is 0-50% of total progress
    const int pct = static_cast<int>(received * 50 / total);
    emit stageProgress(pct, QString("Downloading... %1 / %2 MB")
                           .arg(received / (1024*1024))
                           .arg(total / (1024*1024)));
}

void FirmwareStager::onInstallerDownloadFinished()
{
    auto* reply = m_downloadReply;
    m_downloadReply = nullptr;
    if (!reply) return;
    reply->deleteLater();

    if (m_cancelled) return;

    if (reply->error() != QNetworkReply::NoError) {
        emit stageFailed("Download failed: " + reply->errorString());
        return;
    }

    // Save to disk
    emit stageProgress(50, "Saving installer...");
    QFile f(m_installerPath);
    if (!f.open(QIODevice::WriteOnly)) {
        emit stageFailed("Cannot write installer: " + f.errorString());
        return;
    }
    f.write(reply->readAll());
    f.close();

    verifyAndExtract();
}

// ─── Steps 3-4: Verify and extract ──────────────────────────────────────────

void FirmwareStager::verifyAndExtract()
{
    if (m_cancelled) return;

    // Step 3: Verify MD5
    if (!m_expectedMd5.isEmpty()) {
        emit stageProgress(55, "Verifying installer integrity...");

        QFile f(m_installerPath);
        if (!f.open(QIODevice::ReadOnly)) {
            emit stageFailed("Cannot read installer for verification.");
            return;
        }
        QCryptographicHash hash(QCryptographicHash::Md5);
        hash.addData(&f);
        const QString md5 = hash.result().toHex().toLower();
        f.close();

        if (md5 != m_expectedMd5) {
            emit stageFailed(QString("Installer integrity check failed.\n"
                "Expected: %1\nGot: %2").arg(m_expectedMd5, md5));
            QFile::remove(m_installerPath);
            return;
        }
        emit stageProgress(65, "Installer integrity verified ✓");
    } else {
        emit stageProgress(65, "MD5 hash not available — skipping verification.");
    }

    // Step 4: Extract .ssdr firmware
    emit stageProgress(70, "Extracting firmware...");

    QFile installer(m_installerPath);
    if (!installer.open(QIODevice::ReadOnly)) {
        emit stageFailed("Cannot open installer for extraction.");
        return;
    }

    const QByteArray data = installer.readAll();
    installer.close();

    // Find all "Salted__" signatures
    const QByteArray saltSig("Salted__");
    QList<qint64> saltOffsets;
    qint64 pos = 0;
    while (true) {
        qint64 idx = data.indexOf(saltSig, pos);
        if (idx < 0) break;
        saltOffsets.append(idx);
        pos = idx + 1;
    }

    if (saltOffsets.size() < 2) {
        emit stageFailed(QString("Expected 2 firmware files in installer, found %1.\n"
            "The installer format may have changed.")
            .arg(saltOffsets.size()));
        return;
    }

    emit stageProgress(75, QString("Found %1 firmware files.").arg(saltOffsets.size()));

    // Determine file boundaries
    // File 1 (6x00): offset[0] to offset[1]
    // File 2 (9600): offset[1] to next InnoSetup LZMA header
    const qint64 off1 = saltOffsets[0];
    const qint64 off2 = saltOffsets[1];
    const qint64 size1 = off2 - off1;

    // Find end of file 2: search for InnoSetup LZMA block header after off2
    const QByteArray zlbSig("\x7a\x6c\x62\x1a\x5d\x00\x00\x80", 8);
    qint64 size2 = -1;
    pos = off2 + 1024;  // skip past the Salted__ header
    while (true) {
        qint64 idx = data.indexOf(zlbSig, pos);
        if (idx < 0) break;
        qint64 candidate = idx - off2;
        // The .ssdr file should be > 50MB
        if (candidate > 50 * 1024 * 1024) {
            size2 = candidate;
            break;
        }
        pos = idx + 1;
    }

    if (size2 < 0) {
        // Fallback: use everything from off2 to EOF minus a reasonable trailer
        size2 = data.size() - off2;
        qCWarning(lcFirmware) << "FirmwareStager: could not find LZMA boundary, using" << size2 << "bytes";
    }

    emit stageProgress(80, "Extracting firmware files...");

    // Determine which file we need
    struct FwFile {
        QString name;
        qint64 offset;
        qint64 size;
    };
    QList<FwFile> files = {
        {"FLEX-6x00_v" + m_targetVersion + ".ssdr", off1, size1},
        {"FLEX-9600_v" + m_targetVersion + ".ssdr", off2, size2},
    };

    // Find the one matching our model family
    int targetIdx = -1;
    for (int i = 0; i < files.size(); ++i) {
        if (files[i].name.contains(m_modelFamily)) {
            targetIdx = i;
            break;
        }
    }
    if (targetIdx < 0) {
        emit stageFailed("Cannot find firmware for model family: " + m_modelFamily);
        return;
    }

    const auto& target = files[targetIdx];
    emit stageProgress(85, QString("Extracting %1 (%2 MB)...")
                           .arg(target.name)
                           .arg(target.size / (1024*1024)));

    // Extract to staging directory
    const QString outPath = stagingDir() + "/" + target.name;
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly)) {
        emit stageFailed("Cannot write firmware file: " + out.errorString());
        return;
    }
    out.write(data.constData() + target.offset, target.size);
    out.close();

    // Validate: first 8 bytes must be "Salted__"
    emit stageProgress(92, "Validating extracted firmware...");
    QFile check(outPath);
    if (!check.open(QIODevice::ReadOnly)) {
        emit stageFailed("Cannot read extracted firmware for validation.");
        return;
    }
    const QByteArray header = check.read(8);
    check.close();

    if (header != saltSig) {
        emit stageFailed("Extracted firmware failed validation (bad header).");
        QFile::remove(outPath);
        return;
    }

    // Compute MD5 of extracted file
    QFile hashFile(outPath);
    hashFile.open(QIODevice::ReadOnly);
    QCryptographicHash fwHash(QCryptographicHash::Md5);
    fwHash.addData(&hashFile);
    const QString fwMd5 = fwHash.result().toHex().toLower();
    hashFile.close();

    m_stagedPath = outPath;
    m_stagedVersion = m_targetVersion;

    emit stageProgress(100, QString("Firmware staged and ready ✓\n"
        "%1 (%2 MB)\nMD5: %3")
        .arg(target.name)
        .arg(target.size / (1024*1024))
        .arg(fwMd5));
    emit stageComplete(outPath, m_targetVersion);
}

} // namespace AetherSDR
