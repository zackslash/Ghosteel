#ifndef PTYMANAGER_H
#define PTYMANAGER_H

#include <QObject>
#include <QThread>
#include <QByteArray>
#include <QAtomicInt>

class QTimer;
class QSocketNotifier;

class PtyReaderThread : public QThread
{
    Q_OBJECT
public:
    explicit PtyReaderThread(int fd, QObject *parent = nullptr);
    void consumeChunk();

Q_SIGNALS:
    void dataReady(const QByteArray &data);
    void readFinished();

protected:
    void run() override;

private:
    int m_fd;
    QAtomicInt m_pendingEmits;
};

class PtyManager : public QObject
{
    Q_OBJECT
public:
    explicit PtyManager(QObject *parent = nullptr);
    ~PtyManager();

    // Exit code emitted by shellExited() when exec() fails in the child
    static constexpr int kExecFailedExitCode = -127;

    bool startShell(uint16_t cols, uint16_t rows);
    bool startCommand(const QString &command, const QStringList &args, uint16_t cols, uint16_t rows);

    // synchronous=true  — block until child is reaped (destructor path).
    // synchronous=false — SIGHUP + async reap; use from restartShell() to
    //                     avoid blocking the GUI thread.
    void stop(bool synchronous = true);

    bool writeData(const char *data, size_t len);
    void setShellCommand(const QString &cmd) { m_shellCommand = cmd; }
    void setWorkingDirectory(const QString &dir) { m_workingDirectory = dir; }
    int ptyFd() const { return m_ptyFd; }
    pid_t childPid() const { return m_childPid; }

Q_SIGNALS:
    void dataReady(const QByteArray &data);
    void shellExited(int exitCode);

private:
    void ensureWriteNotifier();
    void drainWriteBuffer();
    void resetWriteBuffer();
    void setupChildProcess(const char *workingDir, const char *homeDir);
    bool forkPtyProcess(uint16_t cols, uint16_t rows, int execPipe[2], pid_t &pid, const char *workingDir, const char *homeDir);
    bool startParentProcess(pid_t pid, int execPipe[2]);

    void reapPidBounded(pid_t pid);

    int m_ptyFd = -1;
    pid_t m_childPid = -1;
    pid_t m_pendingReapPid = -1;
    PtyReaderThread *m_readerThread = nullptr;
    QString m_shellCommand;
    QString m_workingDirectory;

    // Timer-based waitpid reap with generation tracking
    QTimer *m_waitPidTimer = nullptr;
    uint32_t m_sessionGeneration = 0;
    int m_execPipeReadFd = -1;
    QSocketNotifier *m_execNotifier = nullptr;

    // Non-blocking write buffer
    QByteArray m_writeBuffer;
    int m_writeOffset = 0;
    QSocketNotifier *m_writeNotifier = nullptr;
};

#endif // PTYMANAGER_H
