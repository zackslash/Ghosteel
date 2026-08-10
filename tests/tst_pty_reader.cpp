#include <QtTest>
#include <QCoreApplication>
#include <QSignalSpy>

#include <unistd.h>
#include <cstring>

#include "ptymanager.h"

class TestPtyReader : public QObject
{
    Q_OBJECT

private slots:
    void testReadData()
    {
        int pipefd[2];
        QCOMPARE(pipe(pipefd), 0);

        PtyReaderThread reader(pipefd[0]);
        QSignalSpy dataSpy(&reader, &PtyReaderThread::dataReady);
        QSignalSpy finishedSpy(&reader, &PtyReaderThread::readFinished);

        reader.start();

        const char *msg = "hello terminal\n";
        ::write(pipefd[1], msg, strlen(msg));

        QVERIFY(dataSpy.wait(2000));
        QCOMPARE(dataSpy.count(), 1);
        QCOMPARE(dataSpy.at(0).at(0).toByteArray(), QByteArray("hello terminal\n"));

        ::close(pipefd[1]);

        // readFinished may already have arrived before we get here (the
        // reader thread's 200ms poll cycle can see EOF and emit before
        // this call), so check the spy count first and only wait if it
        // hasn't fired yet. Plain finishedSpy.wait() would time out and
        // fail intermittently when the signal already arrived.
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(2000));
        reader.wait(3000);
    }

    void testReadMultipleChunks()
    {
        int pipefd[2];
        QCOMPARE(pipe(pipefd), 0);

        PtyReaderThread reader(pipefd[0]);
        QSignalSpy dataSpy(&reader, &PtyReaderThread::dataReady);
        QSignalSpy finishedSpy(&reader, &PtyReaderThread::readFinished);

        reader.start();

        ::write(pipefd[1], "chunk1", 6);
        ::write(pipefd[1], "chunk2", 6);

        QVERIFY(dataSpy.wait(2000));

        while (dataSpy.wait(200))
            ;

        QByteArray allData;
        for (const auto &sig : dataSpy)
            allData += sig.at(0).toByteArray();
        QVERIFY(allData.contains("chunk1"));
        QVERIFY(allData.contains("chunk2"));

        ::close(pipefd[1]);
        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(2000));
        reader.wait(3000);
    }

    void testEOF()
    {
        int pipefd[2];
        QCOMPARE(pipe(pipefd), 0);

        PtyReaderThread reader(pipefd[0]);
        QSignalSpy finishedSpy(&reader, &PtyReaderThread::readFinished);

        reader.start();

        ::close(pipefd[1]);

        QVERIFY(finishedSpy.count() > 0 || finishedSpy.wait(2000));
        reader.wait(3000);
    }

    void testInterruption()
    {
        int pipefd[2];
        QCOMPARE(pipe(pipefd), 0);

        PtyReaderThread reader(pipefd[0]);
        QSignalSpy dataSpy(&reader, &PtyReaderThread::dataReady);
        QSignalSpy finishedSpy(&reader, &PtyReaderThread::readFinished);

        reader.start();

        ::write(pipefd[1], "x", 1);
        QVERIFY(dataSpy.wait(2000));

        reader.requestInterruption();

        QVERIFY(reader.wait(3000));
        QCOMPARE(finishedSpy.count(), 0);

        ::close(pipefd[1]);
    }

    void testInvalidUserShellSurfacesError()
    {
        // A user-configured shell that doesn't exist should emit
        // shellExited(kExecFailedExitCode) instead of silently
        // falling back to /bin/sh.
        PtyManager pty;
        pty.setShellCommand(QStringLiteral("/nonexistent/shell/path"));
        QSignalSpy exitSpy(&pty, &PtyManager::shellExited);

        QVERIFY(pty.startShell(80, 24));

        // Wait for shellExited with a generous timeout — the child
        // fails execlp, writes errno via execPipe, exits 127.
        QVERIFY(exitSpy.wait(5000));

        QCOMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.at(0).at(0).toInt(), PtyManager::kExecFailedExitCode);

        pty.stop();
    }

    void testSystemShellFallbackPreserved()
    {
        // Empty m_shellCommand + bad $SHELL should fall back to /bin/sh
        // (the fallback is preserved for system shells).
        PtyManager pty;
        // Save and corrupt $SHELL
        QByteArray savedShell = qgetenv("SHELL");
        qputenv("SHELL", "/nonexistent/default/shell");

        QSignalSpy exitSpy(&pty, &PtyManager::shellExited);

        QVERIFY(pty.startShell(80, 24));

        // /bin/sh should start successfully — shellExited should NOT
        // fire with kExecFailedExitCode. It may fire with a normal exit
        // when we stop the pty, or not at all within the timeout.
        // If it fires quickly, the exit code must NOT be kExecFailedExitCode.
        if (exitSpy.wait(1000)) {
            QVERIFY2(exitSpy.at(0).at(0).toInt() != PtyManager::kExecFailedExitCode,
                     "System shell fallback to /bin/sh should not emit exec-failed code");
        }

        pty.stop();

        // Restore $SHELL
        if (!savedShell.isEmpty())
            qputenv("SHELL", savedShell.constData());
        else
            qunsetenv("SHELL");
    }
};

QTEST_MAIN(TestPtyReader)
#include "tst_pty_reader.moc"
