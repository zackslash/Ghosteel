#include <QtTest>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QFile>

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

        // Give the reader thread time to pick up the data
        QVERIFY(dataSpy.wait(2000));
        QCOMPARE(dataSpy.count(), 1);
        QCOMPARE(dataSpy.at(0).at(0).toByteArray(), QByteArray("hello terminal\n"));

        // Close write end to trigger EOF
        ::close(pipefd[1]);

        // readFinished may already have fired (200ms poll cycle); check the
        // count first — plain wait() would time out intermittently.
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

        // Write two chunks back-to-back — no sleep needed.
        // The pipe buffers them; the reader may deliver as 1 or 2 signals.
        ::write(pipefd[1], "chunk1", 6);
        ::write(pipefd[1], "chunk2", 6);

        // Wait for at least one data signal (both chunks may arrive together)
        QVERIFY(dataSpy.wait(2000));

        // Drain any additional signals that arrived in the meantime
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

        // Close write end immediately — should trigger EOF
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

        // Write a byte so the thread wakes from poll() and we know
        // it's actively looping, then request interruption.
        ::write(pipefd[1], "x", 1);
        QVERIFY(dataSpy.wait(2000));

        reader.requestInterruption();

        // readFinished must not fire on interruption (only EOF/error).
        QVERIFY(reader.wait(3000));
        QCOMPARE(finishedSpy.count(), 0);

        ::close(pipefd[1]);
    }

    // --- PtyManager integration: child pid lifecycle ---

    void testShellExitClearsChildPid()
    {
        PtyManager pm;
        QSignalSpy exitedSpy(&pm, &PtyManager::shellExited);

        QVERIFY(pm.startCommand(QStringLiteral("/bin/sh"),
                                QStringList() << "-c" << "exit 0", 80, 24));
        QVERIFY(pm.childPid() > 0);

        // Shell exits immediately; the reap timer reports the exit code and
        // clears the pid.
        QTRY_COMPARE(exitedSpy.count(), 1);
        QCOMPARE(exitedSpy.at(0).at(0).toInt(), 0);
        QCOMPARE(pm.childPid(), -1);
    }

    void testAsyncStopReapsChildPid()
    {
        PtyManager pm;
        QSignalSpy exitedSpy(&pm, &PtyManager::shellExited);

        QVERIFY(pm.startCommand(QStringLiteral("/bin/sh"),
                                QStringList() << "-c" << "sleep 30", 80, 24));
        QVERIFY(pm.childPid() > 0);

        pm.stop(false);

        // Spin the event loop until the async pending-reap timer resolves.
        QTRY_VERIFY(pm.childPid() == -1);
        QCOMPARE(exitedSpy.count(), 1); // SIGHUP-terminated: one shellExited
    }

    void testExecFailureReportsFailureCode()
    {
        PtyManager pm;
        QSignalSpy exitedSpy(&pm, &PtyManager::shellExited);

        QVERIFY(pm.startCommand(QStringLiteral("/nonexistent/ghosteel-binary"),
                                QStringList(), 80, 24));
        QVERIFY(pm.childPid() > 0);

        // Exec failure is reported via the exec pipe with the documented code.
        QTRY_COMPARE(exitedSpy.count(), 1);
        QCOMPARE(exitedSpy.at(0).at(0).toInt(), PtyManager::kExecFailedExitCode);
        QCOMPARE(pm.childPid(), -1);
    }

    void testStopRestartInterleaving()
    {
        // restartShell() = stop(false) + immediate startCommand on the same
        // manager. The async stop leaves the pending-reap timer armed for the
        // OLD child; the immediate restart bumps the session generation and
        // re-gates bookkeeping to the new session. The old timer must reap
        // its SIGHUP'd child without reporting shellExited (generation
        // guard), and the new session must report exactly one when it too is
        // asynchronously stopped — one report for the current session, no
        // duplicate from the stale timer, no lost report.
        PtyManager pm;
        QSignalSpy exitedSpy(&pm, &PtyManager::shellExited);

        QVERIFY(pm.startCommand(QStringLiteral("/bin/sh"),
                                QStringList() << "-c" << "sleep 30", 80, 24));
        const pid_t firstPid = pm.childPid();
        QVERIFY(firstPid > 0);

        pm.stop(false); // arms the pending-reap timer for firstPid

        // Immediate restart: the new child must get a fresh positive pid,
        // different from the SIGHUP'd one.
        QVERIFY(pm.startCommand(QStringLiteral("/bin/sh"),
                                QStringList() << "-c" << "sleep 30", 80, 24));
        const pid_t secondPid = pm.childPid();
        QVERIFY(secondPid > 0);
        QVERIFY(secondPid != firstPid);

        // Asynchronously stop the current session too: its pending-reap
        // timer reports exactly one shellExited. The first session's timer
        // resolved earlier (SIGHUP kills `sleep 30` promptly; 100 ms poll
        // cadence) and must have been generation-suppressed — a duplicate
        // would push the count to 2.
        pm.stop(false);
        QTRY_COMPARE(exitedSpy.count(), 1);

        // The pending-reap timer must dispose of the old child: a zombie
        // would mean the transfer logic regressed. QTRY bounds the wait; we
        // must not waitpid() here (that would steal the manager's reap).
        QTRY_VERIFY(!QFile::exists(QStringLiteral("/proc/%1").arg(firstPid)));

        // Synchronous stop after the async sequence is still clean.
        pm.stop(true);
        QCOMPARE(pm.childPid(), -1);
    }
};

QTEST_MAIN(TestPtyReader)
#include "tst_pty_reader.moc"
