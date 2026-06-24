#ifndef PTYMANAGER_H
#define PTYMANAGER_H

#include <QObject>
#include <QThread>
#include <QByteArray>

class QTimer;
class QSocketNotifier;

class PtyReaderThread : public QThread
{
    Q_OBJECT
public:
    explicit PtyReaderThread(int fd, QObject *parent = nullptr);

Q_SIGNALS:
    void dataReady(const QByteArray &data);
    void readFinished();

protected:
    void run() override;

private:
    int m_fd;
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
    void stop();
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
    void setupChildProcess();
    bool forkPtyProcess(uint16_t cols, uint16_t rows, int execPipe[2], pid_t &pid);
    bool startParentProcess(pid_t pid, int execPipe[2]);

    int m_ptyFd = -1;
    pid_t m_childPid = -1;
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
    QSocketNotifier *m_writeNotifier = nullptr;
};

#endif // PTYMANAGER_H
