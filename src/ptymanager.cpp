#include "ptymanager.h"

#include <QDebug>
#include <QDir>
#include <QTimer>

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pty.h>
#include <fcntl.h>
#include <poll.h>

#include <QSocketNotifier>

// PtyReaderThread
PtyReaderThread::PtyReaderThread(int fd, QObject *parent)
    : QThread(parent)
    , m_fd(fd)
{
}

void PtyReaderThread::run()
{
    char buf[4096];
    struct pollfd pfd;
    pfd.fd = m_fd;
    pfd.events = POLLIN;

    while (!isInterruptionRequested()) {
        int ret = poll(&pfd, 1, 200); // 200ms timeout for clean shutdown check
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break; // error
        }
        if (ret == 0)
            continue; // timeout, check interruption flag

        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = ::read(m_fd, buf, sizeof(buf));
            if (n > 0) {
                Q_EMIT dataReady(QByteArray(buf, n));
            } else if (n == 0) {
                break; // EOF — shell exited
            } else {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                break; // error
            }
        }
    }
    Q_EMIT readFinished();
}

// PtyManager
PtyManager::PtyManager(QObject *parent)
    : QObject(parent)
{
}

PtyManager::~PtyManager()
{
    stop();
}

bool PtyManager::startShell(uint16_t cols, uint16_t rows)
{
    // Increment generation so stale timers from previous sessions bail out
    m_sessionGeneration++;

    struct winsize ws = {};
    ws.ws_col = cols;
    ws.ws_row = rows;

    // Determine shell — prefer configured command, then $SHELL, then /bin/sh
    const char *shell = nullptr;
    QByteArray shellBytes;
    if (!m_shellCommand.isEmpty()) {
        shellBytes = m_shellCommand.toUtf8();
        shell = shellBytes.constData();
    }
    if (!shell || !shell[0]) {
        shell = getenv("SHELL");
        if (!shell || !shell[0])
            shell = "/bin/sh";
    }

    // Create pipe for exec failure detection.
    // The write end has FD_CLOEXEC, so it is closed automatically on successful exec.
    // If exec fails, the child writes errno to the pipe before _exit(127).
    int execPipe[2];
    if (pipe(execPipe) < 0) {
        qWarning() << "pipe() failed:" << strerror(errno);
        return false;
    }
    fcntl(execPipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = forkpty(&m_ptyFd, nullptr, nullptr, &ws);
    if (pid < 0) {
        qWarning() << "forkpty failed:" << strerror(errno);
        ::close(execPipe[0]);
        ::close(execPipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child process
        ::close(execPipe[0]); // Close read end of exec pipe

        setsid();

        // Change to working directory if specified (for session restore)
        if (!m_workingDirectory.isEmpty()) {
            QByteArray dirBytes = m_workingDirectory.toUtf8();
            if (chdir(dirBytes.constData()) != 0) {
                // Fallback to home directory if saved path no longer exists
                const char *home = getenv("HOME");
                if (home)
                    (void)chdir(home);
            }
        }

        // Set TERM
        setenv("TERM", "xterm-256color", 1);

        // Set GHOSTTY_RESOURCES_DIR for shell integration scripts.
        // The scripts are installed at /usr/share/<APP_NAME>/shell-integration/
        // but GHOSTTY_RESOURCES_DIR should point to the parent so that
        // scripts reference ${GHOSTTY_RESOURCES_DIR}/shell-integration/bash/...
        QByteArray resourceDir = QDir::toNativeSeparators(
            QStringLiteral("/usr/share/" APP_NAME)).toUtf8();
        setenv("GHOSTTY_RESOURCES_DIR", resourceDir.constData(), 1);

        execlp(shell, shell, nullptr);
        // If exec fails, try sh
        execlp("sh", "sh", nullptr);
        // Both exec attempts failed — write errno to pipe so parent knows
        int execErr = errno;
        ssize_t written = ::write(execPipe[1], &execErr, sizeof(execErr));
        (void)written;
        _exit(127);
    }

    // Parent process
    ::close(execPipe[1]); // Close write end in parent
    m_execPipeReadFd = execPipe[0];
    m_childPid = pid;

    // Set non-blocking for writes to prevent UI freezes on large pastes
    int flags = fcntl(m_ptyFd, F_GETFL, 0);
    fcntl(m_ptyFd, F_SETFL, flags | O_NONBLOCK);

    // Start reader thread
    m_readerThread = new PtyReaderThread(m_ptyFd, this);
    connect(m_readerThread, &PtyReaderThread::dataReady,
            this, &PtyManager::dataReady, Qt::QueuedConnection);
    connect(m_readerThread, &PtyReaderThread::readFinished, this, [this]() {
        // Capture generation by value — if startShell() is called again,
        // the old timer will see a stale generation and bail out.
        uint32_t gen = m_sessionGeneration;

        // Cancel any existing waitpid timer (safety)
        if (m_waitPidTimer) {
            m_waitPidTimer->stop();
            m_waitPidTimer->deleteLater();
            m_waitPidTimer = nullptr;
        }

        // Use WNOHANG to avoid blocking the main thread.
        // If the child hasn't exited yet (grandchildren holding PTY),
        // retry periodically until waitpid succeeds.
        auto *timer = new QTimer(this);
        m_waitPidTimer = timer;
        connect(timer, &QTimer::timeout, this, [this, timer, gen]() {
            // Bail if child PID is invalid (stop() was called or already reaped)
            if (m_childPid <= 0) return;
            // Bail if this timer belongs to a previous session
            if (gen != m_sessionGeneration) {
                timer->stop();
                timer->deleteLater();
                if (m_waitPidTimer == timer)
                    m_waitPidTimer = nullptr;
                return;
            }
            int status = 0;
            pid_t result = waitpid(m_childPid, &status, WNOHANG);
            if (result > 0 || result < 0) {
                // Child exited or error (e.g., already reaped)
                timer->stop();
                timer->deleteLater();
                if (m_waitPidTimer == timer)
                    m_waitPidTimer = nullptr;
                int exitCode = (result > 0 && WIFEXITED(status))
                    ? WEXITSTATUS(status) : -1;
                Q_EMIT shellExited(exitCode);
            }
            // else: child still running, retry on next tick
        });
        timer->start(100); // Check every 100ms
    }, Qt::QueuedConnection);
    m_readerThread->start();

    // Monitor exec pipe for failure detection.
    // If exec succeeds, FD_CLOEXEC closes the write end and we get EOF.
    // If exec fails, the child writes errno before _exit(127).
    fcntl(m_execPipeReadFd, F_SETFL, O_NONBLOCK);
    m_execNotifier = new QSocketNotifier(m_execPipeReadFd, QSocketNotifier::Read, this);
    connect(m_execNotifier, &QSocketNotifier::activated, this, [this]() {
        m_execNotifier->setEnabled(false);
        m_execNotifier->deleteLater();
        m_execNotifier = nullptr;

        int execErr = 0;
        ssize_t n = ::read(m_execPipeReadFd, &execErr, sizeof(execErr));
        ::close(m_execPipeReadFd);
        m_execPipeReadFd = -1;

        if (n > 0) {
            // exec failed — we received the errno from the child
            qWarning() << "Shell exec failed:" << strerror(execErr);
            int status = 0;
            waitpid(m_childPid, &status, WNOHANG);
            m_childPid = -1;
            Q_EMIT shellExited(-127);
        }
        // else: n == 0 means EOF → exec succeeded (pipe closed by CLOEXEC)
    });

    return true;
}

void PtyManager::stop()
{
    // Clean up exec pipe resources
    if (m_execNotifier) {
        m_execNotifier->setEnabled(false);
        m_execNotifier->deleteLater();
        m_execNotifier = nullptr;
    }
    if (m_execPipeReadFd >= 0) {
        ::close(m_execPipeReadFd);
        m_execPipeReadFd = -1;
    }

    // Cancel any pending waitpid timer before changing state
    if (m_waitPidTimer) {
        m_waitPidTimer->stop();
        m_waitPidTimer->deleteLater();
        m_waitPidTimer = nullptr;
    }

    // Kill child FIRST — this causes the slave side of the PTY to close,
    // which makes read() on the master fd return 0 (EOF), reliably waking
    // the reader thread. Closing the fd while another thread is blocking
    // on read() of the same fd is undefined behavior on Linux.
    if (m_childPid > 0) {
        kill(m_childPid, SIGHUP);
        int status = 0;
        waitpid(m_childPid, &status, WNOHANG);
        m_childPid = -1;
    }

    if (m_readerThread) {
        // Disconnect signals first — prevents queued dataReady/readFinished
        // from being delivered to a destroyed PtyManager after we return.
        disconnect(m_readerThread, nullptr, this, nullptr);

        // Without requestInterruption(), the thread only exits on EOF or
        // poll/read error — neither is guaranteed if grandchildren hold the
        // PTY open after SIGHUP kills the shell.
        m_readerThread->requestInterruption();

        if (!m_readerThread->wait(3000)) {
            // Close the PTY fd to make poll()/read() return errors, forcing
            // the thread out. Technically undefined per POSIX, but reliable
            // on Linux: poll() returns POLLNVAL, read() returns EBADF. The
            // child is dead and the reader thread is the only other consumer.
            if (m_ptyFd >= 0) {
                if (m_writeNotifier) {
                    m_writeNotifier->setEnabled(false);
                    delete m_writeNotifier;
                    m_writeNotifier = nullptr;
                }
                m_writeBuffer.clear();
                ::close(m_ptyFd);
                m_ptyFd = -1;
            }

            if (!m_readerThread->wait(1000)) {
                qWarning() << "PtyReaderThread did not exit after closing PTY, terminating";
                m_readerThread->terminate();
                m_readerThread->wait(1000);
            }
        }
        delete m_readerThread;
        m_readerThread = nullptr;
    }

    if (m_ptyFd >= 0) {
        if (m_writeNotifier) {
            m_writeNotifier->setEnabled(false);
            delete m_writeNotifier;
            m_writeNotifier = nullptr;
        }
        m_writeBuffer.clear();

        ::close(m_ptyFd);
        m_ptyFd = -1;
    }
}

bool PtyManager::writeData(const char *data, size_t len)
{
    if (m_ptyFd < 0)
        return false;

    // If there's already buffered data, append to it
    if (!m_writeBuffer.isEmpty()) {
        m_writeBuffer.append(data, len);
        return true;
    }

    const char *ptr = data;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = ::write(m_ptyFd, ptr, remaining);
        if (n > 0) {
            ptr += n;
            remaining -= n;
        } else if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // PTY buffer full — save remainder for async drain
                m_writeBuffer.append(ptr, remaining);
                ensureWriteNotifier();
                return true;
            }
            qWarning() << "PTY write failed:" << strerror(errno);
            return false;
        } else {
            qWarning() << "PTY write returned 0";
            return false;
        }
    }
    return true;
}

void PtyManager::ensureWriteNotifier()
{
    if (!m_writeNotifier) {
        m_writeNotifier = new QSocketNotifier(m_ptyFd, QSocketNotifier::Write, this);
        connect(m_writeNotifier, &QSocketNotifier::activated, this, &PtyManager::drainWriteBuffer);
    }
    m_writeNotifier->setEnabled(true);
}

void PtyManager::drainWriteBuffer()
{
    if (m_ptyFd < 0 || m_writeBuffer.isEmpty()) {
        if (m_writeNotifier)
            m_writeNotifier->setEnabled(false);
        return;
    }

    const char *ptr = m_writeBuffer.constData();
    size_t remaining = m_writeBuffer.size();

    while (remaining > 0) {
        ssize_t n = ::write(m_ptyFd, ptr, remaining);
        if (n > 0) {
            ptr += n;
            remaining -= n;
        } else if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Still full — keep remaining in buffer
                m_writeBuffer.remove(0, ptr - m_writeBuffer.constData());
                return;
            }
            qWarning() << "PTY drain write failed:" << strerror(errno);
            m_writeBuffer.clear();
            if (m_writeNotifier)
                m_writeNotifier->setEnabled(false);
            return;
        }
    }

    // All data written — clear buffer and disable notifier
    m_writeBuffer.clear();
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(false);
}
