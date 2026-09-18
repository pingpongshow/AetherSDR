#pragma once

// Built-in DAX virtual audio driver management (macOS).
//
// Like SmartSDR's DAX, AetherSDR ships its own virtual audio devices
// ("AetherSDR DAX 1-4" RX, "AetherSDR DAX TX") so WSJT-X, fldigi, VARA etc.
// can use them without third-party loopback drivers. macOS only exposes
// virtual devices through a CoreAudio HAL plug-in in
// /Library/Audio/Plug-Ins/HAL, so the driver is bundled inside
// AetherSDR.app/Contents/Resources and installed on first use with a
// standard macOS administrator prompt.

#include <QObject>
#include <QString>

class QProcess;

namespace AetherSDR {

class MacDaxDriver : public QObject {
    Q_OBJECT
public:
    enum class State {
        NotBundled,   // this build has no driver in Resources (can't install)
        NotInstalled, // bundled, not in /Library/Audio/Plug-Ins/HAL
        Outdated,     // installed driver binary differs from the bundled one
        Current,      // installed and identical to the bundled driver
        InstalledOnly // installed, but this build has nothing to compare with
    };

    explicit MacDaxDriver(QObject* parent = nullptr);

    static QString installedPath();
    static QString bundledPath();
    static bool isInstalled();
    static State state();

    // Copies the bundled driver into the HAL folder via an administrator
    // prompt and restarts coreaudiod. Asynchronous; emits installFinished.
    void install();
    bool isBusy() const { return m_process != nullptr; }

signals:
    void installFinished(bool ok, const QString& message);

private:
    static QByteArray driverFingerprint(const QString& bundle);

    QProcess* m_process{nullptr};
};

} // namespace AetherSDR
