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
            break;
        }
        if (ret == 0)
            continue; // timeout, check interruption flag

        // Re-check the interruption flag after poll() returns, before touching
        // revents/read(): stop(false) can run between the loop-top flag check
        // and this point — it closes this fd, and the next session's forkpty()
        // may have already reused the number. poll() bound to the OLD file, so
        // revents below (and the read()) must not run against the reused fd.
        if (isInterruptionRequested())
            break;

        if (pfd.revents & (POLLERR | POLLNVAL))
            break;

        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = ::read(m_fd, buf, sizeof(buf));
            if (n > 0) {
                // Backpressure: wait if consumer is behind (cap = 64 chunks ≈ 1 MB).
                // load() is the relaxed load on Qt < 5.14; acquire/release ordering
                // isn't needed since the counter carries no other memory (the actual
                // data flows through Qt's queued connection which provides its own barriers).
                while (m_pendingEmits.load() >= 64) {
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
    // Suppress on interruption: stop() has already disconnected, so the emit
    // would be discarded and the reaper isn't needed.
    if (!isInterruptionRequested())
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

bool PtyManager::forkPtyProcess(uint16_t cols, uint16_t rows, int execPipe[2], pid_t &pid,
                                const char *workingDir, const char *homeDir)
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
        // No child exists; clear any stale pid left by the previous session so
        // it can't be signaled/parsed later (the kernel may recycle it). The
        // generation bump above would otherwise make the stop(false) reap
        // timer's same-generation clear unreachable.
        m_childPid = -1;
        return false;
    }
    fcntl(execPipe[1], F_SETFD, FD_CLOEXEC);

    // Set child env in the parent: forkpty() copies environ into the child, and
    // execlp/execvp pass it on. glibc setenv mallocs — not async-signal-safe — so
    // it must not run between fork and exec (QSG render thread / a still-live prior
    // PtyReaderThread can hold the allocator lock across the fork).
    setenv("TERM", "xterm-256color", 1);
    {
        // Point at the parent of shell-integration/ so shell-integration scripts
        // resolve via ${GHOSTTY_RESOURCES_DIR}/shell-integration/<shell>/...
        QByteArray resourceDir = QDir::toNativeSeparators(
            QStringLiteral("/usr/share/" APP_NAME)).toUtf8();
        setenv("GHOSTTY_RESOURCES_DIR", resourceDir.constData(), 1);
    }

    pid = forkpty(&m_ptyFd, nullptr, nullptr, &ws);
    if (pid < 0) {
        qWarning() << "forkpty failed:" << strerror(errno);
        ::close(execPipe[0]);
        ::close(execPipe[1]);
        // No child exists; clear any stale pid left by the previous session so
        // it can't be signaled/parsed later (the kernel may recycle it). The
        // generation bump above would otherwise make the stop(false) reap
        // timer's same-generation clear unreachable.
        m_childPid = -1;
        return false;
    }
    if (pid == 0) {
        ::close(execPipe[0]);
        setupChildProcess(workingDir, homeDir);
    } else {
        // Parent: mark the master PTY fd close-on-exec so future session forks
        // don't inherit it. Without this, each new session's forkpty() child
        // keeps prior sessions' master fds open in the new shell, preventing
        // clean EOF on teardown and leaking descriptors that grow with the
        // number of sessions. (Done in the parent branch only — in the child,
        // m_ptyFd is stale because forkpty writes *amaster in the parent.)
        fcntl(m_ptyFd, F_SETFD, FD_CLOEXEC);
    }
    return true;
}

bool PtyManager::startShell(uint16_t cols, uint16_t rows)
{
    // Pre-fork precompute: between forkpty() and exec() the child may only use
    // async-signal-safe calls. The QSG render thread and an exiting previous
    // PtyReaderThread can both be live across the fork, so Qt/glibc mallocs
    // (including setenv) here can deadlock. Build all child inputs on this stack.

    // Build the hop chain: user-configured shell, then $SHELL, then sh. Each
    // failed hop is reported via execPipe so the parent can notify, and an
    // in-terminal notice is written to the pty before the fallback exec.
    // $SHELL is copied into a QByteArray because forkPtyProcess() calls
    // setenv(), which can realloc environ and invalidate the getenv() pointer.
    QVector<QString> hopNames;
    QByteArray shellCmdBytes;
    QByteArray shellEnvBytes;
    const char *hops[3];
    int hopCount = 0;

    if (!m_shellCommand.isEmpty()) {
        shellCmdBytes = m_shellCommand.toUtf8();
        hops[hopCount++] = shellCmdBytes.constData();
        hopNames.append(m_shellCommand);
    }
    const char *shellEnv = getenv("SHELL");
    if (shellEnv && shellEnv[0]) {
        bool duplicate = false;
        for (int i = 0; i < hopCount; ++i) {
            if (strcmp(hops[i], shellEnv) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            shellEnvBytes = QByteArray(shellEnv);
            hops[hopCount++] = shellEnvBytes.constData();
            hopNames.append(QString::fromUtf8(shellEnv));
        }
    }
    bool shPresent = false;
    for (int i = 0; i < hopCount; ++i) {
        if (strcmp(hops[i], "sh") == 0) {
            shPresent = true;
            break;
        }
    }
    if (!shPresent) {
        hops[hopCount++] = "sh";
        hopNames.append(QStringLiteral("sh"));
    }

    // One notice line per adjacent hop pair, written to the pty (fd 2) before
    // the fallback exec. Plain ASCII; strerror is not async-signal-safe.
    QByteArray noticeLines[2];
    for (int i = 0; i + 1 < hopCount; ++i) {
        noticeLines[i] = QByteArray("\r\nghosteel: ") + hops[i]
            + " could not be started, using " + hops[i + 1] + "\r\n";
    }

    const char *homeDir = getenv("HOME");
    QByteArray workingDirBytes;
    const char *workingDir = nullptr;
    if (!m_workingDirectory.isEmpty()) {
        workingDirBytes = m_workingDirectory.toUtf8();
        workingDir = workingDirBytes.constData();
    }

    int execPipe[2];
    pid_t pid;
    if (!forkPtyProcess(cols, rows, execPipe, pid, workingDir, homeDir))
        return false;

    if (pid == 0) {
        for (int i = 0; i < hopCount; i++) {
            execlp(hops[i], hops[i], nullptr);
            int execErr = errno;
            if (i + 1 < hopCount) {
                ssize_t noticeWritten = ::write(2, noticeLines[i].constData(), noticeLines[i].size());
                (void)noticeWritten;
            }
            int32_t rec[3] = { i, execErr, (i + 1 < hopCount) ? i + 1 : -1 };
            ssize_t written = ::write(execPipe[1], rec, sizeof rec);
            (void)written;
        }
        _exit(127);
    }

    return startParentProcess(pid, execPipe, hopNames);
}

// Runs in CHILD between fork and exec — async-signal-safe calls only.
void PtyManager::setupChildProcess(const char *workingDir, const char *homeDir)
{
    setsid();
    if (workingDir && workingDir[0]) {
        // Fall back to HOME; if that fails too the child keeps the
        // inherited cwd — exec proceeds either way.
        if (chdir(workingDir) != 0 && homeDir && chdir(homeDir) != 0) {
        }
    }
}

bool PtyManager::startCommand(const QString &command, const QStringList &args, uint16_t cols, uint16_t rows)
{
    // Pre-fork precompute: between forkpty() and exec() the child may only use
    // async-signal-safe calls. The QSG render thread and an exiting previous
    // PtyReaderThread can both be live across the fork, so Qt/glibc mallocs
    // (including setenv) here can deadlock. Build all child inputs on this stack.
    QByteArray cmdBytes = command.toUtf8();
    QList<QByteArray> argBytes;
    argBytes.append(cmdBytes);
    for (const QString &arg : args)
        argBytes.append(arg.toUtf8());

    QVector<const char *> argv(argBytes.size() + 1);
    for (int i = 0; i < argBytes.size(); ++i)
        argv[i] = argBytes[i].constData();
    argv[argBytes.size()] = nullptr;

    const char *homeDir = getenv("HOME");
    QByteArray workingDirBytes;
    const char *workingDir = nullptr;
    if (!m_workingDirectory.isEmpty()) {
        workingDirBytes = m_workingDirectory.toUtf8();
        workingDir = workingDirBytes.constData();
    }

    int execPipe[2];
    pid_t pid;
    if (!forkPtyProcess(cols, rows, execPipe, pid, workingDir, homeDir))
        return false;

    if (pid == 0) {
        execvp(cmdBytes.constData(), const_cast<char *const *>(argv.data()));

        int execErr = errno;
        int32_t rec[3] = { -1, execErr, -1 };
        ssize_t written = ::write(execPipe[1], rec, sizeof rec);
        (void)written;
        _exit(127);
    }

    return startParentProcess(pid, execPipe);
}

bool PtyManager::startParentProcess(pid_t pid, int execPipe[2],
                                    const QVector<QString> &hopNames)
{
    // Defensive cleanup mirroring stop(): a double-start would otherwise leave
    // a stale exec notifier busy-polling a leaked fd (the notifier-capture
    // bail for stale activations would silently mask it). Tear down any prior
    // exec pipe/notifier before wiring the new one.
    if (m_execNotifier) {
        m_execNotifier->setEnabled(false);
        m_execNotifier->deleteLater();
        m_execNotifier = nullptr;
    }
    if (m_execPipeReadFd >= 0) {
        ::close(m_execPipeReadFd);
        m_execPipeReadFd = -1;
    }
    m_execMsgLen = 0;

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

        // Reap a pid queued by a previous stop(false) BEFORE cancelling its
        // timer — otherwise the cancel orphans it as a zombie until the next
        // stop() / ~PtyManager(). WNOHANG so we don't block the GUI thread.
        // If the old child is still alive (result == 0), leave the pending
        // timer running — its lambda captured the correct oldPid and its
        // generation guard already prevents it emitting shellExited for the
        // old session — and keep m_pendingReapPid set so a later readFinished
        // (or stop()) reaps it. Only cancel the timer once the pending reap
        // actually resolved.
        bool pendingReapStillRunning = false;
        if (m_pendingReapPid > 0) {
            int pendingStatus = 0;
            pid_t pendingResult = ::waitpid(m_pendingReapPid, &pendingStatus, WNOHANG);
            if (pendingResult != 0) {
                m_pendingReapPid = -1; // reaped (>0) or already gone (<0)
            } else {
                pendingReapStillRunning = true;
            }
        }

        // Cancel any existing waitpid timer (safety) — unless it is the
        // pending-reap timer for a still-running old child, which must keep
        // polling until the old pid is reaped.
        if (m_waitPidTimer && !pendingReapStillRunning) {
            m_waitPidTimer->stop();
            m_waitPidTimer->deleteLater();
            m_waitPidTimer = nullptr;
        }

        // If the child was already reaped (e.g., exec failure set m_childPid
        // to -1), don't create a reap timer — nothing to waitpid.
        if (m_childPid <= 0)
            return;

        // Use WNOHANG to avoid blocking the main thread.
        // If the child hasn't exited yet (grandchildren holding PTY),
        // retry periodically until waitpid succeeds.
        auto *timer = new QTimer(this);
        m_waitPidTimer = timer;
        connect(timer, &QTimer::timeout, this, [this, timer, gen]() {
            // Bail if child PID is invalid (stop() was called or already reaped)
            if (m_childPid <= 0) {
                timer->stop();
                timer->deleteLater();
                if (m_waitPidTimer == timer)
                    m_waitPidTimer = nullptr;
                return;
            }
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
                // The pid is reaped (or already gone); clear it before emitting
                // so a later stop() can't SIGHUP a stale (possibly recycled)
                // pid and workingDirectory() can't parse a foreign /proc/<pid>/cwd.
                // Safe: the generation guard above guarantees same-generation
                // context, i.e. m_childPid is this session's child.
                m_childPid = -1;
                Q_EMIT shellExited(exitCode);
            }
        });
        timer->start(100);
    }, Qt::QueuedConnection);
    m_readerThread->start();

    // Monitor exec pipe for failure detection.
    // If exec succeeds, FD_CLOEXEC closes the write end and we get EOF.
    // If exec fails, the child writes one 12-byte failure record per failed
    // hop before _exit(127).
    fcntl(m_execPipeReadFd, F_SETFL, O_NONBLOCK);
    m_execNotifier = new QSocketNotifier(m_execPipeReadFd, QSocketNotifier::Read, this);
    // Capture the generation at creation, mirroring the reap timers' guard:
    // if a newer session started while this notifier's delivery was pending,
    // the exit reporting belongs to the newer session's timers.
    const uint32_t gen = m_sessionGeneration;
    QSocketNotifier *notifier = m_execNotifier;
    connect(notifier, &QSocketNotifier::activated, this, [this, notifier, gen, hopNames]() {
        // Stale activation: a newer session's startCommand() replaced
        // m_execNotifier while this delivery was pending — this activation
        // belongs to the old pipe and must not touch the member.
        if (m_execNotifier != notifier)
            return;

        // Drain the exec pipe. The child writes one 12-byte record per failed
        // hop; on success CLOEXEC closes the write end and read() returns 0.
        // EAGAIN means the child is mid-hop: keep the notifier armed and wait
        // for the next activation (EOF or more records).
        while (true) {
            ssize_t n = ::read(m_execPipeReadFd, m_execMsgBuf + m_execMsgLen,
                               sizeof(m_execMsgBuf) - m_execMsgLen);
            if (n > 0) {
                m_execMsgLen += static_cast<int>(n);
                if (m_execMsgLen >= static_cast<int>(sizeof(m_execMsgBuf)))
                    break; // buffer full: finalize with what we have
            } else if (n == 0) {
                break; // EOF: exec succeeded or child done writing
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; // child mid-hop; next activation delivers EOF or more records
            } else {
                break; // read error: finalize
            }
        }

        ::close(m_execPipeReadFd);
        m_execPipeReadFd = -1;
        m_execNotifier->setEnabled(false);
        m_execNotifier->deleteLater();
        m_execNotifier = nullptr;

        // Bail if this notifier belongs to a previous session, or the
        // child was already reaped (e.g. readFinished's reap timer beat a
        // delayed delivery of this notifier): the exit was reported
        // elsewhere, so emitting again would duplicate shellExited and
        // overwrite the final exit code (shellFallbackNotice is likewise
        // dropped).
        if (gen != m_sessionGeneration || m_childPid <= 0)
            return;

        // Parse 12-byte records {failed, errsv, target}. A record with
        // failed == -1 (raw command) or target == -1 (last hop failed) marks
        // total failure.
        constexpr int kRecordSize = 3 * static_cast<int>(sizeof(int32_t));
        int firstFailed = -1;
        int firstErrsv = 0;
        int lastTarget = -1;
        int recordCount = 0;
        bool totalFailure = false;
        for (int off = 0; off + kRecordSize <= m_execMsgLen; off += kRecordSize) {
            int32_t rec[3];
            memcpy(rec, m_execMsgBuf + off, kRecordSize);
            if (recordCount == 0) {
                firstFailed = rec[0];
                firstErrsv = rec[1];
            }
            lastTarget = rec[2];
            recordCount++;
            if (rec[0] == -1 || rec[2] == -1)
                totalFailure = true;
        }

        if (totalFailure) {
            // exec failed — we received the errno from the child
            qWarning() << "Process exec failed:" << strerror(firstErrsv);
            // Bounded reap: the child may not have _exit'd yet (it writes the
            // failure records then calls _exit(127); the parent may read the
            // pipe before the _exit lands). reapPidBounded retries to avoid
            // leaving a zombie.
            reapPidBounded(m_childPid);
            m_childPid = -1;
            Q_EMIT shellExited(kExecFailedExitCode);
        } else if (recordCount > 0) {
            // Out-of-range hop indices (corrupt record) silently skip the emit.
            if (firstFailed >= 0 && firstFailed < hopNames.size()
                && lastTarget >= 0 && lastTarget < hopNames.size()) {
                Q_EMIT shellFallbackNotice(hopNames.at(firstFailed),
                                           hopNames.at(lastTarget),
                                           QString::fromLocal8Bit(strerror(firstErrsv)));
            }
        }
    });

    return true;
}

void PtyManager::reapPidBounded(pid_t pid)
{
    if (pid <= 0)
        return;

    constexpr int kMaxAttempts = 50;
    constexpr long kSleepNs = 10 * 1000 * 1000; // 10ms
    int status = 0;
    for (int i = 0; i < kMaxAttempts; ++i) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result > 0 || result < 0)
            return; // reaped or error/already gone
        // result == 0: child still alive. Sleep briefly and retry.
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = kSleepNs;
        nanosleep(&ts, nullptr);
    }
    qWarning("PtyManager::reapPidBounded: child %ld did not exit within %dms; "
             "may leak as zombie", (long)pid,
             kMaxAttempts * (int)(kSleepNs / 1000000));
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

    // Cancel any pending waitpid timer before changing state. Async-path
    // exception: a still-unresolved pending-reap timer is left running — it is
    // self-cleaning and generation-guarded; a newer session's stop reassigns
    // the m_pendingReapPid bookkeeping, and the old timer stays responsible
    // via its captured pid.
    if (m_waitPidTimer && (synchronous || m_pendingReapPid <= 0)) {
        m_waitPidTimer->stop();
        m_waitPidTimer->deleteLater();
        m_waitPidTimer = nullptr;
    }
    // The async reap timer may have been tracking an old pid; reap it here so
    // it can't be orphaned as a zombie. Synchronous path only: in the async
    // path this bounded reap would block the GUI thread up to 500ms when a
    // second restartShell arrives while the previous stop(false)'s pending
    // reap is unresolved (the still-running pending timer reaps it instead).
    if (synchronous && m_pendingReapPid > 0) {
        reapPidBounded(m_pendingReapPid);
        // If the pending pid is still this session's current child it is now
        // definitively reaped — clear it so a later stop() can't SIGHUP a
        // stale (possibly recycled) pid. Guarded by equality: a new session
        // would have overwritten m_childPid with a different pid.
        if (m_childPid == m_pendingReapPid)
            m_childPid = -1;
        m_pendingReapPid = -1;
    }

    // Kill child FIRST — this causes the slave side of the PTY to close,
    // which makes read() on the master fd return 0 (EOF), reliably waking
    // the reader thread. Closing the fd while another thread is blocking
    // on read() of the same fd is undefined behavior on Linux.
    if (m_childPid > 0) {
        kill(m_childPid, SIGHUP);
    }

    if (!synchronous) {
        // Async path (restartShell): tear down the reader thread and PTY fd with
        // only a short bounded wait (≤100 ms) for the thread to exit, then set up
        // an async timer to reap the child. Avoids the up-to-500 ms blocking reap
        // that the synchronous path performs via reapPidBounded().

        if (m_readerThread) {
            // Disconnect signals first — prevents queued dataReady/readFinished
            // from being delivered to a destroyed PtyManager after we return.
            disconnect(m_readerThread, nullptr, this, nullptr);

            // Ask the thread to exit at its next poll timeout (≤200 ms).
            m_readerThread->requestInterruption();
        }

        // Close the old PTY fd so it isn't leaked into the new session. (NB:
        // on Linux, close() in this thread does NOT wake the old reader's in-
        // flight poll() — fds are process-shared and poll binds to the file at
        // entry; POLLNVAL would surface only on a *subsequent* poll() call.)
        // The old thread instead exits via its 200 ms timeout + the flag set
        // by requestInterruption() above; the disconnect() above ensures any
        // straggling emissions go nowhere. Those, not the close(), neutralize
        // the fd-reuse race with the new PTY.
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

        if (m_readerThread) {
            // The old thread's poll timeout (200 ms) exceeds this 100 ms wait, so the
            // async deleteLater-on-finished path below is the expected restart path —
            // not an exceptional fallback (the thread's post-poll interruption
            // re-check gates read(), so it can't touch a reused fd number).
            if (m_readerThread->wait(100)) {
                m_readerThread->deleteLater();
            } else {
                qWarning() << "PtyReaderThread did not exit after fd close; async cleanup";
                // Detach from parent so ~PtyManager doesn't try to delete a
                // still-running QThread (would abort: "QThread: Destroyed
                // while thread is still running"). The finished->deleteLater
                // connection below owns lifecycle once the thread exits.
                m_readerThread->setParent(nullptr);
                connect(m_readerThread, &QThread::finished, m_readerThread, &QObject::deleteLater);
            }
            m_readerThread = nullptr;
        }

        // Set up an async reap timer — the child is dying from SIGHUP but
        // may not have exited yet. Poll with WNOHANG every 100 ms.
        // Capture the OLD pid BEFORE setupTerminal() can overwrite m_childPid
        // with the new child's PID, so we always reap the correct process.
        if (m_childPid > 0) {
            pid_t oldPid = m_childPid;
            m_pendingReapPid = oldPid;
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
                if (m_pendingReapPid == oldPid)
                    m_pendingReapPid = -1;
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
        reapPidBounded(m_childPid);
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
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Still full — offset already reflects bytes written
            return;
        } else {
            // Real error (n < 0) or n == 0 (no progress possible — retrying
            // the identical write would spin forever). Mirror writeData()'s
            // handling: drop the pending data and stop draining.
            if (n < 0)
                qWarning() << "PTY drain write failed:" << strerror(errno);
            else
                qWarning() << "PTY drain write returned 0";
            resetWriteBuffer();
            if (m_writeNotifier)
                m_writeNotifier->setEnabled(false);
            return;
        }
    }

    resetWriteBuffer();
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(false);
}
