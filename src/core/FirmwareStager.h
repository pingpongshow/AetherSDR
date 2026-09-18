#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace AetherSDR {

// Downloads SmartSDR installer, verifies integrity, extracts .ssdr firmware
// files, and stages them for upload.
//
// Workflow:
//   1. checkForUpdate()  — compare radio version vs latest available
//   2. downloadAndStage() — download installer, verify MD5, extract .ssdr
//   3. stagedFilePath()   — path to extracted .ssdr ready for upload

class FirmwareStager : public QObject {
    Q_OBJECT
public:
    explicit FirmwareStager(QObject* parent = nullptr);

    // Check FlexRadio website for latest version
    void checkForUpdate(const QString& currentVersion);

    // Download installer, verify, extract .ssdr for the given model family
    // modelFamily: "6x00" or "9600"
    void downloadAndStage(const QString& version, const QString& modelFamily);

    // Cancel in-progress download
    void cancel();

    // Path to the staged .ssdr file (empty if not staged)
    QString stagedFilePath() const { return m_stagedPath; }
    QString stagedVersion()  const { return m_stagedVersion; }
    bool    isStaged()       const { return !m_stagedPath.isEmpty(); }

    // Map radio model string to firmware model family
    static QString modelToFamily(const QString& model);

    // Staging directory
    static QString stagingDir();

signals:
    // Step 1: version check
    void updateCheckComplete(const QString& latestVersion, bool updateAvailable);
    void updateCheckFailed(const QString& error);

    // Steps 2-4: download, verify, extract
    void stageProgress(int percent, const QString& status);
    void stageComplete(const QString& ssdrPath, const QString& version);
    void stageFailed(const QString& error);

private:
    void onInstallerDownloadProgress(qint64 received, qint64 total);
    void onInstallerDownloadFinished();
    void verifyAndExtract();

    QNetworkAccessManager m_nam;
    QNetworkReply*  m_downloadReply{nullptr};
    QString         m_installerPath;
    QString         m_expectedMd5;
    QString         m_modelFamily;
    QString         m_targetVersion;
    QString         m_stagedPath;
    QString         m_stagedVersion;
    bool            m_cancelled{false};

    static constexpr const char* INSTALLER_URL_FMT =
        "https://smartsdr.flexradio.com/SmartSDR_v%1_Installer.exe";
    static constexpr const char* MD5_URL_FMT =
        "https://edge.flexradio.com/www/offload/20251215133656/"
        "SmartSDR-v%1-Installer-MD5-Hash-File.txt";
    static constexpr const char* SOFTWARE_PAGE =
        "https://www.flexradio.com/software/";
};

} // namespace AetherSDR
