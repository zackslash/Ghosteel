#include "ptymanager.h"

#include <QDebug>
#include <QDir>
#include <QTimer>

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
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
    , m_pendingEmits(0)
{
}

void PtyReaderThread::consumeChunk()
{
    m_pendingEmits.fetchAndAddRelaxed(-1);
}

void PtyReaderThread::run()
{
    char buf[16 * 1024];
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

        if (pfd.revents & (POLLERR | POLLNVAL))
            break;

        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = ::read(m_fd, buf, sizeof(buf));
            if (n > 0) {
                // Backpressure: wait if consumer is behind (cap = 64 chunks ≈ 1 MB).
                // loadRelaxed: counter only — no other memory published, so relaxed
                // ordering is correct on both producer and consumer sides.
                while (m_pendingEmits.loadRelaxed() >= 64) {
                    usleep(1000); // 1 ms
                    if (isInterruptionRequested())
                        return;
                }
                Q_EMIT dataReady(QByteArray(buf, n));
                m_pendingEmits.fetchAndAddRelaxed(1);
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

bool PtyManager::forkPtyProcess(uint16_t cols, uint16_t rows, int execPipe[2], pid_t &pid)
{
    m_sessionGeneration++;

    struct winsize ws = {};
    ws.ws_col = cols;
    ws.ws_row = rows;

    // Create pipe for exec failure detection.
    // The write end has FD_CLOEXEC, so it is closed automatically on successful exec.
    // If exec fails, the child writes errno to the pipe before _exit.
    if (pipe(execPipe) < 0) {
        qWarning() << "pipe() failed:" << strerror(errno);
        return false;
    }
    fcntl(execPipe[1], F_SETFD, FD_CLOEXEC);

    pid = forkpty(&m_ptyFd, nullptr, nullptr, &ws);
    if (pid < 0) {
        qWarning() << "forkpty failed:" << strerror(errno);
        ::close(execPipe[0]);
        ::close(execPipe[1]);
        return false;
    }

    if (pid == 0) {
        ::close(execPipe[0]);
        setupChildProcess();
    }

    return true;
}

bool PtyManager::startShell(uint16_t cols, uint16_t rows)
{
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

    int execPipe[2];
    pid_t pid;
    if (!forkPtyProcess(cols, rows, execPipe, pid))
        return false;

    if (pid == 0) {
        execlp(shell, shell, nullptr);
        execlp("sh", "sh", nullptr);  // fallback
        int execErr = errno;
        ssize_t written = ::write(execPipe[1], &execErr, sizeof(execErr));
        (void)written;
        _exit(127);
    }

    return startParentProcess(pid, execPipe);
}

void PtyManager::setupChildProcess()
{
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

    setenv("TERM", "xterm-256color", 1);

    // Set GHOSTTY_RESOURCES_DIR for shell integration scripts.
    // The scripts are installed at /usr/share/<APP_NAME>/shell-integration/
    // but GHOSTTY_RESOURCES_DIR should point to the parent so that
    // scripts reference ${GHOSTTY_RESOURCES_DIR}/shell-integration/bash/...
    QByteArray resourceDir = QDir::toNativeSeparators(
        QStringLiteral("/usr/share/" APP_NAME)).toUtf8();
    setenv("GHOSTTY_RESOURCES_DIR", resourceDir.constData(), 1);
}

bool PtyManager::startCommand(const QString &command, const QStringList &args, uint16_t cols, uint16_t rows)
{
    int execPipe[2];
    pid_t pid;
    if (!forkPtyProcess(cols, rows, execPipe, pid))
        return false;

    if (pid == 0) {
        QByteArray cmdBytes = command.toUtf8();
        QList<QByteArray> argBytes;
        argBytes.append(cmdBytes);
        for (const QString &arg : args)
            argBytes.append(arg.toUtf8());

        QVector<const char *> argv(argBytes.size() + 1);
        for (int i = 0; i < argBytes.size(); ++i)
            argv[i] = argBytes[i].constData();
        argv[argBytes.size()] = nullptr;

        execvp(cmdBytes.constData(), const_cast<char *const *>(argv.data()));

        int execErr = errno;
        ssize_t written = ::write(execPipe[1], &execErr, sizeof(execErr));
        (void)written;
        _exit(127);
    }

    return startParentProcess(pid, execPipe);
}

bool PtyManager::startParentProcess(pid_t pid, int execPipe[2])
{
    ::close(execPipe[1]);
    m_execPipeReadFd = execPipe[0];
    m_childPid = pid;

    // Set non-blocking for writes to prevent UI freezes on large pastes
    int flags = fcntl(m_ptyFd, F_GETFL, 0);
    fcntl(m_ptyFd, F_SETFL, flags | O_NONBLOCK);

    m_readerThread = new PtyReaderThread(m_ptyFd, this);
    connect(m_readerThread, &PtyReaderThread::dataReady,
            this, &PtyManager::dataReady, Qt::QueuedConnection);
    connect(m_readerThread, &PtyReaderThread::dataReady,
            this, [this]() {
                if (m_readerThread)
                    m_readerThread->consumeChunk();
            }, Qt::QueuedConnection);
    connect(m_readerThread, &PtyReaderThread::readFinished, this, [this]() {
        // Capture generation by value — if startShell()/startCommand() is called again,
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
        });
        timer->start(100);
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
            qWarning() << "Process exec failed:" << strerror(execErr);
            int status = 0;
            waitpid(m_childPid, &status, WNOHANG);
            m_childPid = -1;
            Q_EMIT shellExited(kExecFailedExitCode);
        }
        // else: n == 0 means EOF → exec succeeded (pipe closed by CLOEXEC)
    });

    return true;
}

void PtyManager::stop(bool synchronous)
{
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
    }

    if (!synchronous) {
        // Async path (restartShell): tear down the reader thread and PTY fd
        // without blocking, then set up an async timer to reap the child.
        // This avoids freezing the GUI thread for up to 500 ms.

        if (m_readerThread) {
            // Disconnect signals first — prevents queued dataReady/readFinished
            // from being delivered to a destroyed PtyManager after we return.
            disconnect(m_readerThread, nullptr, this, nullptr);

            // Ask the thread to exit at its next poll timeout (≤200 ms).
            m_readerThread->requestInterruption();

            // The thread will exit shortly (POLLNVAL from closed fd or
            // interruption). deleteLater ensures the QThread object is
            // properly cleaned up after exit, even though we no longer track
            // it via m_readerThread.
            connect(m_readerThread, &QThread::finished, m_readerThread, &QObject::deleteLater);
            m_readerThread = nullptr;
        }

        if (m_ptyFd >= 0) {
            if (m_writeNotifier) {
                m_writeNotifier->setEnabled(false);
                delete m_writeNotifier;
                m_writeNotifier = nullptr;
            }
            resetWriteBuffer();
            ::close(m_ptyFd);
            m_ptyFd = -1;
        }

        // Set up an async reap timer — the child is dying from SIGHUP but
        // may not have exited yet. Poll with WNOHANG every 100 ms.
        // Capture the OLD pid BEFORE setupTerminal() can overwrite m_childPid
        // with the new child's PID, so we always reap the correct process.
        if (m_childPid > 0) {
            pid_t oldPid = m_childPid;
            uint32_t gen = m_sessionGeneration;
            auto *timer = new QTimer(this);
            m_waitPidTimer = timer;
            connect(timer, &QTimer::timeout, this, [this, timer, gen, oldPid]() {
                if (oldPid <= 0) {
                    timer->stop();
                    timer->deleteLater();
                    return;
                }
                int status = 0;
                pid_t result = ::waitpid(oldPid, &status, WNOHANG);
                if (result == 0)
                    return; // still running, try again next tick
                // Reaped (result > 0) or error (result < 0, e.g. already
                // reaped elsewhere). Clean up the timer either way.
                timer->stop();
                timer->deleteLater();
                if (m_waitPidTimer == timer)
                    m_waitPidTimer = nullptr;
                // Only emit if this is still the current session generation
                if (gen == m_sessionGeneration && result > 0) {
                    m_childPid = -1;
                    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    Q_EMIT shellExited(exitCode);
                }
            });
            timer->start(100);
        }
        return;
    }

    // --- Synchronous path (destructor / cleanup) ---

    if (m_childPid > 0) {
        // Bounded reaping loop. A single waitpid(WNOHANG) here would return 0
        // (child not yet dead from the asynchronous SIGHUP) and we'd then
        // clobber m_childPid, leaving a zombie the m_waitPidTimer can no
        // longer match. Poll briefly instead: give the child up to ~500ms to
        // terminate, then give up (kernel subreaper / init will reap if so
        // configured; otherwise we log and accept the rare leak rather than
        // block shutdown indefinitely).
        constexpr int kMaxAttempts = 50;
        constexpr long kSleepNs = 10 * 1000 * 1000; // 10ms
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < kMaxAttempts; ++i) {
            pid_t result = waitpid(m_childPid, &status, WNOHANG);
            if (result > 0 || result < 0) {
                reaped = true;
                break;
            }
            // result == 0: child still alive. Sleep briefly and retry.
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = kSleepNs;
            nanosleep(&ts, nullptr);
        }
        if (!reaped) {
            qWarning("PtyManager::stop: child %ld did not exit within %dms; "
                     "may leak as zombie", (long)m_childPid,
                     kMaxAttempts * (int)(kSleepNs / 1000000));
        }
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
                resetWriteBuffer();
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
        resetWriteBuffer();

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

void PtyManager::resetWriteBuffer()
{
    m_writeBuffer.clear();
    m_writeOffset = 0;
}

void PtyManager::drainWriteBuffer()
{
    if (m_ptyFd < 0 || m_writeOffset >= m_writeBuffer.size()) {
        if (m_writeNotifier)
            m_writeNotifier->setEnabled(false);
        return;
    }

    const char *ptr = m_writeBuffer.constData() + m_writeOffset;
    size_t remaining = m_writeBuffer.size() - m_writeOffset;

    while (remaining > 0) {
        ssize_t n = ::write(m_ptyFd, ptr, remaining);
        if (n > 0) {
            m_writeOffset += n;
            ptr += n;
            remaining -= n;
        } else if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Still full — offset already reflects bytes written
                return;
            }
            qWarning() << "PTY drain write failed:" << strerror(errno);
            resetWriteBuffer();
            if (m_writeNotifier)
                m_writeNotifier->setEnabled(false);
            return;
        }
    }

    // All data written — clear buffer and disable notifier
    resetWriteBuffer();
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(false);
}
