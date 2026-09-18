#include "CatKenwoodPty.h"

#include "CatKenwoodProtocol.h"
#include "LogManager.h"

#include <QSocketNotifier>

#ifndef Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#ifdef Q_OS_MAC
#include <util.h>       // openpty() on macOS
#else
#include <pty.h>        // openpty() on Linux
#endif
#endif

namespace AetherSDR {

CatKenwoodPty::CatKenwoodPty(CatRadioInterface* radio, QObject* parent)
    : QObject(parent)
    , m_radio(radio)
{}

CatKenwoodPty::~CatKenwoodPty()
{
    stop();
}

void CatKenwoodPty::setPortConfig(const CatKenwoodPortConfig& cfg)
{
    m_cfg = cfg;
    if (m_protocol) {
        m_protocol->setVfoASlice(cfg.vfoASlice);
        m_protocol->setAutoSwitchTxSlice(cfg.autoSwitchTxSlice);
        m_protocol->setSplitMode(cfg.splitMode);
    }
}

bool CatKenwoodPty::start()
{
#ifdef Q_OS_WIN
    qCWarning(lcCat) << "CatKenwoodPty: virtual serial ports need com0com on Windows (not implemented)";
    return false;
#else
    if (m_masterFd >= 0) {
        return true;
    }
    char slaveName[256] = {};
    if (::openpty(&m_masterFd, &m_slaveFd, slaveName, nullptr, nullptr) != 0) {
        qCWarning(lcCat) << "CatKenwoodPty: openpty() failed";
        m_masterFd = -1;
        m_slaveFd = -1;
        return false;
    }
    m_slavePath = QString::fromLocal8Bit(slaveName);

    const int flags = ::fcntl(m_masterFd, F_GETFL);
    ::fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

    // Raw mode: ';' framing, no echo, no CR/LF translation.
    struct termios tio {};
    if (::tcgetattr(m_slaveFd, &tio) == 0) {
        ::cfmakeraw(&tio);
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        ::tcsetattr(m_slaveFd, TCSANOW, &tio);
    }

    const QByteArray link = m_symlinkPath.toLocal8Bit();
    ::unlink(link.constData());
    if (::symlink(slaveName, link.constData()) != 0) {
        qCWarning(lcCat) << "CatKenwoodPty: symlink failed:" << m_symlinkPath;
    }

    m_protocol = std::make_unique<CatKenwoodProtocol>(m_radio);
    setPortConfig(m_cfg);

    m_notifier = std::make_unique<QSocketNotifier>(m_masterFd, QSocketNotifier::Read);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &CatKenwoodPty::onDataReady);

    qCInfo(lcCat) << "CatKenwoodPty: started on" << m_slavePath << "symlink:" << m_symlinkPath;
    emit started(m_symlinkPath);
    return true;
#endif
}

void CatKenwoodPty::stop()
{
#ifndef Q_OS_WIN
    if (m_masterFd < 0) {
        return;
    }
    m_notifier.reset();
    m_protocol.reset();
    ::close(m_masterFd);
    ::close(m_slaveFd);
    m_masterFd = -1;
    m_slaveFd = -1;
    ::unlink(m_symlinkPath.toLocal8Bit().constData());
    qCInfo(lcCat) << "CatKenwoodPty: stopped";
    emit stopped();
#endif
}

void CatKenwoodPty::writeOut(const QByteArray& data)
{
#ifndef Q_OS_WIN
    if (m_masterFd < 0 || data.isEmpty()) {
        return;
    }
    // Non-blocking: if nobody has the slave open and the pty buffer is full,
    // the answer is dropped rather than stalling the GUI thread.
    const ssize_t n = ::write(m_masterFd, data.constData(), static_cast<size_t>(data.size()));
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        qCWarning(lcCat) << "CatKenwoodPty: write failed, errno" << errno;
    }
#else
    Q_UNUSED(data);
#endif
}

void CatKenwoodPty::onDataReady()
{
#ifndef Q_OS_WIN
    char buf[1024];
    const ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
    if (n <= 0 || !m_protocol) {
        return;   // EAGAIN / client closed: keep the pty for reconnection
    }
    writeOut(m_protocol->feed(QByteArray(buf, static_cast<qsizetype>(n))));
#endif
}

void CatKenwoodPty::notifyRadioStateChanged()
{
    if (m_protocol && m_protocol->autoInfoEnabled()) {
        writeOut(m_protocol->pollAutoInfo());
    }
}

} // namespace AetherSDR
