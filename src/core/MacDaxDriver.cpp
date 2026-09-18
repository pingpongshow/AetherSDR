#include "MacDaxDriver.h"

#include "LogManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace AetherSDR {

namespace {

constexpr const char* kHalDir = "/Library/Audio/Plug-Ins/HAL";
constexpr const char* kDriverName = "AetherSDRDAX.driver";

bool looksLikeDriverBundle(const QString& bundle)
{
    return QFileInfo(bundle + "/Contents/MacOS/AetherSDRDAX").isFile()
        && QFileInfo(bundle + "/Contents/Info.plist").isFile();
}

} // namespace

MacDaxDriver::MacDaxDriver(QObject* parent)
    : QObject(parent)
{
}

QString MacDaxDriver::installedPath()
{
    return QStringLiteral("%1/%2").arg(QLatin1String(kHalDir), QLatin1String(kDriverName));
}

QString MacDaxDriver::bundledPath()
{
    // <App>.app/Contents/MacOS/AetherSDR → <App>.app/Contents/Resources/AetherSDRDAX.driver
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp();
    return dir.absoluteFilePath(QStringLiteral("Resources/%1").arg(QLatin1String(kDriverName)));
}

bool MacDaxDriver::isInstalled()
{
    return looksLikeDriverBundle(installedPath());
}

QByteArray MacDaxDriver::driverFingerprint(const QString& bundle)
{
    QFile f(bundle + "/Contents/MacOS/AetherSDRDAX");
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&f);
    return hash.result();
}

MacDaxDriver::State MacDaxDriver::state()
{
    const bool bundled = looksLikeDriverBundle(bundledPath());
    const bool installed = isInstalled();
    if (!bundled) {
        return installed ? State::InstalledOnly : State::NotBundled;
    }
    if (!installed) {
        return State::NotInstalled;
    }
    // The plug-in's version string isn't bumped per build, so compare the
    // binaries themselves to detect a stale driver after an app update.
    return driverFingerprint(installedPath()) == driverFingerprint(bundledPath())
        ? State::Current : State::Outdated;
}

void MacDaxDriver::install()
{
    if (m_process) {
        return;
    }
    const QString src = bundledPath();
    if (!looksLikeDriverBundle(src)) {
        emit installFinished(false, QStringLiteral("This build of AetherSDR does not include the DAX driver."));
        return;
    }

    // Paths are passed as osascript argv and quoted with "quoted form of", so
    // spaces/parentheses in the app location can't break the shell command.
    // coreaudiod must be restarted for the new HAL plug-in to be loaded.
    const QStringList script{
        QStringLiteral("on run argv"),
        QStringLiteral("set src to quoted form of (item 1 of argv)"),
        QStringLiteral("set halDir to quoted form of (item 2 of argv)"),
        QStringLiteral("set dst to quoted form of (item 3 of argv)"),
        QStringLiteral("do shell script \"/bin/mkdir -p \" & halDir & \" && /bin/rm -rf \" & dst & "
                       "\" && /bin/cp -R \" & src & \" \" & halDir & "
                       "\" && /usr/sbin/chown -R root:wheel \" & dst & "
                       "\" && (/bin/launchctl kickstart -kp system/com.apple.audio.coreaudiod "
                       "|| /usr/bin/killall coreaudiod)\" with prompt "
                       "\"AetherSDR wants to install its DAX virtual audio driver.\" "
                       "with administrator privileges"),
        QStringLiteral("end run"),
    };
    QStringList args;
    for (const QString& line : script) {
        args << QStringLiteral("-e") << line;
    }
    args << src << QLatin1String(kHalDir) << installedPath();

    m_process = new QProcess(this);
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const QString err = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        m_process->deleteLater();
        m_process = nullptr;
        if (status != QProcess::NormalExit || exitCode != 0) {
            // -128 = user cancelled the administrator prompt
            const bool cancelled = err.contains(QLatin1String("-128"));
            qCWarning(lcGui) << "MacDaxDriver: install failed" << exitCode << err;
            emit installFinished(false, cancelled
                ? QStringLiteral("Installation cancelled.")
                : QStringLiteral("Driver installation failed: %1").arg(err));
            return;
        }
        const bool ok = isInstalled();
        qCInfo(lcGui) << "MacDaxDriver: install finished, installed =" << ok;
        emit installFinished(ok, ok ? QStringLiteral("DAX audio driver installed.")
                                    : QStringLiteral("The driver was not found after installation."));
    });
    // If osascript can't even start, QProcess emits errorOccurred but never
    // finished — clear m_process so later install attempts aren't ignored.
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart || !m_process) {
            return;
        }
        m_process->deleteLater();
        m_process = nullptr;
        emit installFinished(false, QStringLiteral("Could not start the macOS installer helper (osascript)."));
    });
    qCInfo(lcGui) << "MacDaxDriver: installing" << src;
    m_process->start(QStringLiteral("/usr/bin/osascript"), args);
}

} // namespace AetherSDR
