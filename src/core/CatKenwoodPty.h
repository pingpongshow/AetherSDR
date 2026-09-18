#pragma once

#include "CatKenwoodServer.h"   // CatKenwoodPortConfig

#include <QObject>
#include <QString>

#include <memory>

class QSocketNotifier;

namespace AetherSDR {

class CatKenwoodProtocol;

// Virtual serial port speaking the SmartSDR-CAT Kenwood/ZZ protocol.
//
// macOS / Linux: openpty() pair (same approach as RigctlPty). The slave
// path (e.g. /dev/ttys012) is symlinked to /tmp/AetherSDR-KCAT-<letter> so
// loggers can be pointed at a stable name. Baud/parity chosen by the client
// are irrelevant on a pty.
//
// Windows: not implemented here. The equivalent of SmartSDR's FlexVSP pairs
// is a com0com null-modem pair (e.g. CNCA0 <-> CNCB0): AetherSDR would open
// CNCA0 with QSerialPort and feed bytes into CatKenwoodProtocol exactly like
// onDataReady() below, and the logger opens CNCB0. That needs Qt6::SerialPort
// (a new dependency) and a signed driver install, so it is left to a
// maintainer decision; start() returns false on Windows.
class CatKenwoodPty : public QObject {
    Q_OBJECT

public:
    explicit CatKenwoodPty(CatRadioInterface* radio, QObject* parent = nullptr);
    ~CatKenwoodPty() override;

    bool start();
    void stop();

    bool    isRunning() const   { return m_masterFd >= 0; }
    QString slavePath() const   { return m_slavePath; }
    QString symlinkPath() const { return m_symlinkPath; }
    void    setSymlinkPath(const QString& path) { m_symlinkPath = path; }

    void setPortConfig(const CatKenwoodPortConfig& cfg);
    CatKenwoodPortConfig portConfig() const { return m_cfg; }

public slots:
    // Connect CatKenwoodRadioAdapter::stateChanged here for AI pushes.
    void notifyRadioStateChanged();

signals:
    void started(const QString& path);
    void stopped();

private slots:
    void onDataReady();

private:
    void writeOut(const QByteArray& data);

    CatRadioInterface*                  m_radio;
    std::unique_ptr<CatKenwoodProtocol> m_protocol;
    CatKenwoodPortConfig                m_cfg;
    int                                 m_masterFd{-1};
    int                                 m_slaveFd{-1};
    QString                             m_slavePath;
    QString                             m_symlinkPath{"/tmp/AetherSDR-KCAT-A"};
    std::unique_ptr<QSocketNotifier>    m_notifier;
};

} // namespace AetherSDR
