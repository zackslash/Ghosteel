#include <QtTest>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QSignalSpy>
#include <QDir>

// Pull in the stub TerminalView (QObject-based, no Qt Quick dependency)
#include "terminalview.h"

// SessionManager under test
#include "sessionmanager.h"

class TestSessionPersistence : public QObject
{
    Q_OBJECT

private:
    QString m_settingsPath;
    QTemporaryDir m_tempDir;

    // Debounce timer interval in SessionManager is 500ms.
    // We wait 600ms to ensure the timer fires. If the debounce changes,
    // update this constant to match (debounce + ~100ms margin).
    static constexpr int DEBOUNCE_WAIT_MS = 600;

    // Helper: write raw session data to settings file.
    // Each session is a QStringList: {name, workingDirectory[, autorunCommand[, keybarOpen[, keyboardVisible]]]}
    void writeRawSessions(const QList<QStringList> &sessions, int activeIndex = 0)
    {
        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.remove(QString()); // clear all

        s.beginGroup(QStringLiteral("sessions"));
        s.setValue(QStringLiteral("count"), sessions.size());
        s.setValue(QStringLiteral("nextId"), sessions.size() + 1);
        s.setValue(QStringLiteral("activeIndex"), activeIndex);
        s.endGroup();

        for (int i = 0; i < sessions.size(); i++) {
            s.beginGroup(QStringLiteral("sessionData/session_%1").arg(i));
            s.setValue(QStringLiteral("name"), sessions[i].at(0));
            s.setValue(QStringLiteral("workingDirectory"), sessions[i].at(1));
            if (sessions[i].size() > 2)
                s.setValue(QStringLiteral("autorunCommand"), sessions[i].at(2));
            if (sessions[i].size() > 3)
                s.setValue(QStringLiteral("keybarOpen"), sessions[i].at(3));
            if (sessions[i].size() > 4)
                s.setValue(QStringLiteral("keyboardVisible"), sessions[i].at(4));
            s.setValue(QStringLiteral("id"), i + 1);
            s.endGroup();
        }
        s.sync();
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(m_tempDir.isValid(), qUtf8Printable(m_tempDir.errorString()));
        m_settingsPath = m_tempDir.path() + "/test.conf";
    }

    void init()
    {
        // Clean settings file before each test
        QFile::remove(m_settingsPath);
    }

    // --- Restore tests ---

    void testRestoreNoSavedData()
    {
        SessionManager mgr(m_settingsPath);
        QSignalSpy createdSpy(&mgr, &SessionManager::sessionCreated);
        QSignalSpy restoredSpy(&mgr, &SessionManager::sessionsRestored);

        bool restored = mgr.restoreSessions();

        QCOMPARE(restored, false);
        QCOMPARE(mgr.sessionCount(), 0);
        QCOMPARE(createdSpy.count(), 0);
        QCOMPARE(restoredSpy.count(), 0); // not emitted on failure path
    }

    void testRestoreSingleSession()
    {
        writeRawSessions({{"Projects", "/tmp"}});

        SessionManager mgr(m_settingsPath);
        QSignalSpy createdSpy(&mgr, &SessionManager::sessionCreated);
        QSignalSpy restoredSpy(&mgr, &SessionManager::sessionsRestored);
        QSignalSpy switchedSpy(&mgr, &SessionManager::sessionSwitched);

        bool restored = mgr.restoreSessions();

        QCOMPARE(restored, true);
        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("Projects"));
        QCOMPARE(mgr.activeSessionIndex(), 0);
        QCOMPARE(createdSpy.count(), 0);   // sessionCreated not emitted during restore
        QCOMPARE(restoredSpy.count(), 1);  // sessionsRestored emitted once
        QCOMPARE(switchedSpy.count(), 1); // setActiveSessionIndex fires this
    }

    void testRestoreMultipleSessions()
    {
        writeRawSessions({
            {"Terminal", "/home"},
            {"Logs", "/var/log"},
            {"Temp", "/tmp"}
        }, 1);

        SessionManager mgr(m_settingsPath);
        QSignalSpy createdSpy(&mgr, &SessionManager::sessionCreated);
        QSignalSpy restoredSpy(&mgr, &SessionManager::sessionsRestored);
        QSignalSpy switchedSpy(&mgr, &SessionManager::sessionSwitched);

        bool restored = mgr.restoreSessions();

        QCOMPARE(restored, true);
        QCOMPARE(mgr.sessionCount(), 3);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("Terminal"));
        QCOMPARE(mgr.sessionName(1), QStringLiteral("Logs"));
        QCOMPARE(mgr.sessionName(2), QStringLiteral("Temp"));
        QCOMPARE(mgr.activeSessionIndex(), 1);
        QCOMPARE(createdSpy.count(), 0);   // sessionCreated not emitted during restore
        QCOMPARE(restoredSpy.count(), 1);  // sessionsRestored emitted once
        QCOMPARE(switchedSpy.count(), 1); // only the final setActiveSessionIndex
    }

    void testRestoreActiveIndexClamping()
    {
        writeRawSessions({{"A", "/tmp"}}, 5); // activeIndex out of range

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.activeSessionIndex(), 0); // clamped to valid range
    }

    void testRestoreDeletedDirectoryFallback()
    {
        writeRawSessions({{"Test", "/nonexistent/path/that/does/not/exist"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Session should still be created — directory validated on restore
        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("Test"));

        // The view's working directory should have been set to home (path doesn't exist)
        TerminalView *view = mgr.activeSession();
        QVERIFY(view != nullptr);
        QCOMPARE(view->workingDirectory(), QDir::homePath());
    }

    void testRestoreCorruptedCountCap()
    {
        // Write settings with absurdly high count
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessions");
            s.setValue("count", 9999);
            s.setValue("nextId", 10000);
            s.setValue("activeIndex", 0);
            s.endGroup();

            // Write only 2 actual sessions — the rest would be missing data
            for (int i = 0; i < 2; i++) {
                s.beginGroup(QStringLiteral("sessionData/session_%1").arg(i));
                s.setValue("name", QStringLiteral("Session %1").arg(i));
                s.setValue("workingDirectory", "/tmp");
                s.endGroup();
            }
            s.sync();
        }

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // restoreSessions caps at 50 and creates all count sessions (even without data).
        // With count=9999, only the first 50 are created (data exists for 2, defaults for 48).
        QCOMPARE(mgr.sessionCount(), 50);
    }

    void testRestoreCorruptedCountCapExactly50()
    {
        // Write settings with count = 50 (at the cap boundary)
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessions");
            s.setValue("count", 50);
            s.setValue("nextId", 51);
            s.setValue("activeIndex", 0);
            s.endGroup();

            for (int i = 0; i < 50; i++) {
                s.beginGroup(QStringLiteral("sessionData/session_%1").arg(i));
                s.setValue("name", QStringLiteral("S%1").arg(i));
                s.setValue("workingDirectory", "/tmp");
                s.endGroup();
            }
            s.sync();
        }

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionCount(), 50);
    }

    // --- Save tests ---

    void testSaveOnCreateSession()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions(); // initialize (returns false, sets m_sessionsLoaded)

        TerminalView *view = mgr.createSession();
        QVERIFY(view != nullptr);
        QCOMPARE(mgr.sessionCount(), 1);

        // Wait for debounce timer
        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Verify saved data
        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessions");
        QCOMPARE(s.value("count").toInt(), 1);
        s.endGroup();

        s.beginGroup("sessionData/session_0");
        QVERIFY(s.value("name").toString().startsWith("Session"));
        s.endGroup();
    }

    void testSaveOnRemoveSession()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 2);

        mgr.removeSession(0);
        QCOMPARE(mgr.sessionCount(), 1);

        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessions");
        QCOMPARE(s.value("count").toInt(), 1);
        s.endGroup();

        s.beginGroup("sessionData/session_0");
        QCOMPARE(s.value("name").toString(), QStringLiteral("B"));
        s.endGroup();
    }

    void testSaveOnRenameSession()
    {
        writeRawSessions({{"Original", "/tmp"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.setSessionName(0, "Renamed");
        QCOMPARE(mgr.sessionName(0), QStringLiteral("Renamed"));

        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessionData/session_0");
        QCOMPARE(s.value("name").toString(), QStringLiteral("Renamed"));
        s.endGroup();
    }

    void testSaveOnActiveSessionSwitch()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.activeSessionIndex(), 0);

        mgr.setActiveSessionIndex(1);
        QCOMPARE(mgr.activeSessionIndex(), 1);

        // Wait for debounce timer (500ms) to fire
        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessions");
        QCOMPARE(s.value("activeIndex").toInt(), 1);
        s.endGroup();
    }

    void testRemoveSessionById()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 3);

        int idToRemove = mgr.sessionId(1); // "B"
        mgr.removeSessionById(idToRemove);

        QCOMPARE(mgr.sessionCount(), 2);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("A"));
        QCOMPARE(mgr.sessionName(1), QStringLiteral("C"));
    }

    void testRemoveSessionByIdNotFound()
    {
        writeRawSessions({{"A", "/tmp"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.removeSessionById(999); // doesn't exist
        QCOMPARE(mgr.sessionCount(), 1); // unchanged
    }

    void testRemoveLastSession()
    {
        writeRawSessions({{"Only", "/tmp"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.activeSessionIndex(), 0);

        mgr.removeSession(0);

        QCOMPARE(mgr.sessionCount(), 0);
        QCOMPARE(mgr.activeSessionIndex(), -1);
        QVERIFY(mgr.activeSession() == nullptr);
    }

    void testRemoveSessionBeforeActive()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 2);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.activeSessionIndex(), 2); // "C" is active

        mgr.removeSession(0); // remove "A" (before active)
        QCOMPARE(mgr.activeSessionIndex(), 1); // active shifts down
        QCOMPARE(mgr.sessionName(mgr.activeSessionIndex()), QStringLiteral("C"));
    }

    void testRemoveSessionAfterActive()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 0);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 3);
        QCOMPARE(mgr.activeSessionIndex(), 0); // "A" is active

        mgr.removeSession(2); // remove "C" (after active)

        QCOMPARE(mgr.sessionCount(), 2);
        QCOMPARE(mgr.activeSessionIndex(), 0); // active unchanged
        QCOMPARE(mgr.sessionName(0), QStringLiteral("A"));
        QCOMPARE(mgr.sessionName(1), QStringLiteral("B"));
    }

    void testRemoveActiveSessionNotLast()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 1);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 3);
        QCOMPARE(mgr.activeSessionIndex(), 1); // "B" is active

        mgr.removeSession(1); // remove "B" (the active session)

        QCOMPARE(mgr.sessionCount(), 2);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("A"));
        QCOMPARE(mgr.sessionName(1), QStringLiteral("C"));
        QCOMPARE(mgr.activeSessionIndex(), 1); // points to "C" which shifted into index 1
    }

    void testRemoveActiveSessionFirstOfThree()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 0);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 3);
        QCOMPARE(mgr.activeSessionIndex(), 0); // "A" is active

        mgr.removeSession(0); // remove "A" (the active session, first of three)

        QCOMPARE(mgr.sessionCount(), 2);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("B"));
        QCOMPARE(mgr.sessionName(1), QStringLiteral("C"));
        QCOMPARE(mgr.activeSessionIndex(), 0); // stays 0 (0 < size 2)
    }

    void testRemoveActiveSessionSignalOrdering()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 1);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.activeSessionIndex(), 1); // "B" is active

        // Track emission order via direct connections
        QList<QString> signalOrder;
        connect(&mgr, &SessionManager::sessionSwitched, [&]() { signalOrder << "switched"; });
        connect(&mgr, &SessionManager::sessionRemoved, [&]() { signalOrder << "removed"; });

        QSignalSpy switchedSpy(&mgr, &SessionManager::sessionSwitched);
        QSignalSpy removedSpy(&mgr, &SessionManager::sessionRemoved);

        mgr.removeSession(1); // remove active "B"

        QCOMPARE(switchedSpy.count(), 1);
        QCOMPARE(removedSpy.count(), 1);

        // sessionSwitched must fire BEFORE sessionRemoved so that the view
        // is still alive when handlers react to the switch
        QCOMPARE(signalOrder.size(), 2);
        QCOMPARE(signalOrder.at(0), QStringLiteral("switched"));
        QCOMPARE(signalOrder.at(1), QStringLiteral("removed"));
    }

    void testRemoveSessionMultipleTimes()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}, {"D", "/opt"}}, 3);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 4);
        QCOMPARE(mgr.activeSessionIndex(), 3); // "D" is active

        mgr.removeSession(0); // remove "A" (before active → active shifts down)
        QCOMPARE(mgr.sessionCount(), 3);
        QCOMPARE(mgr.activeSessionIndex(), 2); // shifted down from 3
        QCOMPARE(mgr.sessionName(0), QStringLiteral("B"));
        QCOMPARE(mgr.sessionName(1), QStringLiteral("C"));
        QCOMPARE(mgr.sessionName(2), QStringLiteral("D"));

        mgr.removeSession(0); // remove "B" (now at index 0, before active)
        QCOMPARE(mgr.sessionCount(), 2);
        QCOMPARE(mgr.activeSessionIndex(), 1); // shifted down from 2
        QCOMPARE(mgr.sessionName(0), QStringLiteral("C"));
        QCOMPARE(mgr.sessionName(1), QStringLiteral("D"));
    }

    void testRemoveActiveSessionFromTwo()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}}, 0);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 2);
        QCOMPARE(mgr.activeSessionIndex(), 0);

        mgr.removeSession(0); // remove active "A" from 2-session list

        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.activeSessionIndex(), 0);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("B"));
    }

    void testRestoreWithAutorunCommand()
    {
        writeRawSessions({{"Htop", "/tmp", "htop"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("Htop"));
        QCOMPARE(mgr.sessionAutorunCommand(0), QStringLiteral("htop"));
    }

    void testRestoreWithEmptyAutorunCommand()
    {
        writeRawSessions({{"Terminal", "/tmp"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.sessionAutorunCommand(0), QString());
    }

    void testSaveAutorunCommand()
    {
        writeRawSessions({{"Htop", "/tmp"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.setSessionAutorunCommand(0, "htop");
        QCOMPARE(mgr.sessionAutorunCommand(0), QStringLiteral("htop"));

        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessionData/session_0");
        QCOMPARE(s.value("autorunCommand").toString(), QStringLiteral("htop"));
        s.endGroup();
    }

    void testSetAutorunCommandNoOp()
    {
        writeRawSessions({{"Htop", "/tmp", "htop"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QSignalSpy spy(&mgr, &SessionManager::sessionAutorunCommandChanged);

        // Set same value — should not emit signal
        mgr.setSessionAutorunCommand(0, "htop");
        QCOMPARE(spy.count(), 0);
    }

    void testSetAutorunCommandChanged()
    {
        writeRawSessions({{"Htop", "/tmp", "htop"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QSignalSpy spy(&mgr, &SessionManager::sessionAutorunCommandChanged);

        mgr.setSessionAutorunCommand(0, "vim");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(mgr.sessionAutorunCommand(0), QStringLiteral("vim"));
    }

    void testFullSaveRestoreCycleWithAutorun()
    {
        // Create fresh manager, set autorun, save
        {
            SessionManager mgr(m_settingsPath);
            mgr.restoreSessions();

            mgr.createSession();
            mgr.setSessionName(0, "Htop");
            mgr.setSessionAutorunCommand(0, "htop");
        }

        // New instance — restore
        SessionManager mgr2(m_settingsPath);
        mgr2.restoreSessions();

        QCOMPARE(mgr2.sessionCount(), 1);
        QCOMPARE(mgr2.sessionName(0), QStringLiteral("Htop"));
        QCOMPARE(mgr2.sessionAutorunCommand(0), QStringLiteral("htop"));
    }

    void testAutorunWithSpecialCharacters()
    {
        writeRawSessions({{"Logs", "/tmp", "tail -f /var/log/syslog | grep error"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionAutorunCommand(0), QStringLiteral("tail -f /var/log/syslog | grep error"));
    }

    void testClearAutorunCommand()
    {
        // Start with autorun set
        writeRawSessions({{"Htop", "/tmp", "htop"}});

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionAutorunCommand(0), QStringLiteral("htop"));

        // Clear it
        mgr.setSessionAutorunCommand(0, "");
        QCOMPARE(mgr.sessionAutorunCommand(0), QString());

        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Verify cleared on disk
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessionData/session_0");
            QCOMPARE(s.value("autorunCommand").toString(), QString());
            s.endGroup();
        }

        // Verify stays cleared after restore
        SessionManager mgr2(m_settingsPath);
        mgr2.restoreSessions();
        QCOMPARE(mgr2.sessionAutorunCommand(0), QString());
    }

    // --- Keybar & keyboard state tests ---

    void testRestoreKeybarOpen()
    {
        writeRawSessions({{"Test", "/tmp", "", "true"}});
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionKeybarOpen(0), true);
    }

    void testRestoreKeybarClosed()
    {
        writeRawSessions({{"Test", "/tmp", "", "false"}});
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionKeybarOpen(0), false);
    }

    void testRestoreKeyboardVisible()
    {
        writeRawSessions({{"Test", "/tmp", "", "", "true"}});
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionKeyboardVisible(0), true);
    }

    void testRestoreKeyboardHidden()
    {
        writeRawSessions({{"Test", "/tmp", "", "", "false"}});
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionKeyboardVisible(0), false);
    }

    void testSaveKeybarState()
    {
        SessionManager mgr(m_settingsPath);
        mgr.createSession();
        mgr.setSessionKeybarOpen(0, false);
        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("sessionData/session_0"));
        QCOMPARE(s.value("keybarOpen").toBool(), false);
        s.endGroup();
    }

    void testSaveKeyboardState()
    {
        SessionManager mgr(m_settingsPath);
        mgr.createSession();
        mgr.setSessionKeyboardVisible(0, false);
        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("sessionData/session_0"));
        QCOMPARE(s.value("keyboardVisible").toBool(), false);
        s.endGroup();
    }

    void testKeybarNoOpSuppressesSignal()
    {
        SessionManager mgr(m_settingsPath);
        mgr.createSession();
        QSignalSpy spy(&mgr, &SessionManager::sessionKeybarOpenChanged);
        mgr.setSessionKeybarOpen(0, true); // default is true
        QCOMPARE(spy.count(), 0);
    }

    void testKeyboardNoOpSuppressesSignal()
    {
        SessionManager mgr(m_settingsPath);
        mgr.createSession();
        QSignalSpy spy(&mgr, &SessionManager::sessionKeyboardVisibleChanged);
        mgr.setSessionKeyboardVisible(0, true); // default is true
        QCOMPARE(spy.count(), 0);
    }

    void testKeybarSignalOnChange()
    {
        SessionManager mgr(m_settingsPath);
        mgr.createSession();
        QSignalSpy spy(&mgr, &SessionManager::sessionKeybarOpenChanged);
        mgr.setSessionKeybarOpen(0, false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);
    }

    void testKeyboardSignalOnChange()
    {
        SessionManager mgr(m_settingsPath);
        mgr.createSession();
        QSignalSpy spy(&mgr, &SessionManager::sessionKeyboardVisibleChanged);
        mgr.setSessionKeyboardVisible(0, false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);
    }

    void testKeybarDefaultValue()
    {
        SessionManager mgr(m_settingsPath);
        mgr.createSession();
        QCOMPARE(mgr.sessionKeybarOpen(0), true);
    }

    void testKeyboardDefaultValue()
    {
        SessionManager mgr(m_settingsPath);
        mgr.createSession();
        QCOMPARE(mgr.sessionKeyboardVisible(0), true);
    }

    void testKeybarKeyboardFullCycle()
    {
        // Save sessions with custom UI state
        {
            SessionManager mgr(m_settingsPath);
            mgr.restoreSessions(); // initialize (sets m_sessionsLoaded)
            mgr.createSession();
            mgr.createSession();
            mgr.setSessionKeybarOpen(0, false);
            mgr.setSessionKeyboardVisible(0, false);
            mgr.setSessionKeybarOpen(1, true);
            mgr.setSessionKeyboardVisible(1, true);
            QTest::qWait(DEBOUNCE_WAIT_MS);
        }

        // Restore and verify
        SessionManager mgr(m_settingsPath);
        QVERIFY(mgr.restoreSessions());
        QCOMPARE(mgr.sessionKeybarOpen(0), false);
        QCOMPARE(mgr.sessionKeyboardVisible(0), false);
        QCOMPARE(mgr.sessionKeybarOpen(1), true);
        QCOMPARE(mgr.sessionKeyboardVisible(1), true);
    }

    void testSavePreservesCachedCwdForInactiveSessions()
    {
        // Simulate: 3 sessions saved with known directories
        writeRawSessions({
            {"S1", "/tmp"},
            {"S2", "/var"},
            {"S3", "/home"}
        }, 0);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 3);

        // Force a save — inactive sessions should preserve their cached CWD
        mgr.setSessionName(0, "Trigger Save");
        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Read back and verify all directories preserved
        QSettings s(m_settingsPath, QSettings::IniFormat);

        s.beginGroup("sessionData/session_0");
        QCOMPARE(s.value("workingDirectory").toString(), QStringLiteral("/tmp"));
        s.endGroup();

        s.beginGroup("sessionData/session_1");
        QCOMPARE(s.value("workingDirectory").toString(), QStringLiteral("/var"));
        s.endGroup();

        s.beginGroup("sessionData/session_2");
        QCOMPARE(s.value("workingDirectory").toString(), QStringLiteral("/home"));
        s.endGroup();
    }

    // --- Full cycle tests ---

    void testFullSaveRestoreCycle()
    {
        // Create fresh manager, create sessions, save
        {
            SessionManager mgr(m_settingsPath);
            mgr.restoreSessions();

            mgr.createSession();
            mgr.createSession();
            mgr.setSessionName(0, "Projects");
            mgr.setSessionName(1, "Logs");
            mgr.setActiveSessionIndex(1);

            // Trigger save via destructor
        }

        // New manager instance — restore from saved state
        SessionManager mgr2(m_settingsPath);
        bool restored = mgr2.restoreSessions();

        QCOMPARE(restored, true);
        QCOMPARE(mgr2.sessionCount(), 2);
        QCOMPARE(mgr2.sessionName(0), QStringLiteral("Projects"));
        QCOMPARE(mgr2.sessionName(1), QStringLiteral("Logs"));
        QCOMPARE(mgr2.activeSessionIndex(), 1);
    }

    void testNoSaveDuringRestore()
    {
        writeRawSessions({{"A", "/tmp"}});

        // Add a marker key that would be lost if saveSessions() rewrote the file
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.setValue("test/marker", "should-survive");
            s.sync();
        }

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // The restore itself should NOT trigger a save (m_sessionsLoaded is false during restore)
        // Verify the marker key survived — if saveSessions() ran, it would clear
        // sessionData but not preserve arbitrary keys outside its groups.
        QSettings s(m_settingsPath, QSettings::IniFormat);
        QCOMPARE(s.value("test/marker").toString(), QStringLiteral("should-survive"));

        // Also verify the session data is intact
        s.beginGroup("sessions");
        QCOMPARE(s.value("count").toInt(), 1);
        s.endGroup();
    }

    void testZeroSessionsRestore()
    {
        // Write count=0
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessions");
            s.setValue("count", 0);
            s.setValue("nextId", 1);
            s.setValue("activeIndex", 0);
            s.endGroup();
            s.sync();
        }

        SessionManager mgr(m_settingsPath);
        bool restored = mgr.restoreSessions();

        QCOMPARE(restored, false);
        QCOMPARE(mgr.sessionCount(), 0);
    }

    void testNextIdPreserved()
    {
        // Simulate sessions with non-contiguous IDs (e.g., some were deleted)
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessions");
            s.setValue("count", 2);
            s.setValue("nextId", 10); // high nextId from previous deletions
            s.setValue("activeIndex", 0);
            s.endGroup();

            s.beginGroup("sessionData/session_0");
            s.setValue("name", "A");
            s.setValue("workingDirectory", "/tmp");
            s.endGroup();

            s.beginGroup("sessionData/session_1");
            s.setValue("name", "B");
            s.setValue("workingDirectory", "/home");
            s.endGroup();
            s.sync();
        }

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Restore assigns IDs 10, 11 (m_nextSessionId increments to 12).
        // createSession() should use 12, not restart from 1.
        TerminalView *view = mgr.createSession();
        QVERIFY(view != nullptr);
        QCOMPARE(mgr.sessionId(2), 12);
    }

    // --- Session ID persistence tests ---

    void testSessionIdPersistence()
    {
        // Write 3 sessions with known IDs
        writeRawSessions({
            {"Alpha", QDir::homePath()},
            {"Beta", QDir::homePath()},
            {"Gamma", QDir::homePath()}
        });
        // Manually set IDs in the settings file
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessionData/session_0");
            s.setValue("id", 10);
            s.endGroup();
            s.beginGroup("sessionData/session_1");
            s.setValue("id", 20);
            s.endGroup();
            s.beginGroup("sessionData/session_2");
            s.setValue("id", 30);
            s.endGroup();
            s.beginGroup("sessions");
            s.setValue("nextId", 31);
            s.endGroup();
            s.sync();
        }

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionCount(), 3);
        QCOMPARE(mgr.sessionId(0), 10);
        QCOMPARE(mgr.sessionId(1), 20);
        QCOMPARE(mgr.sessionId(2), 30);
    }

    void testSessionIndexById()
    {
        writeRawSessions({
            {"A", QDir::homePath()},
            {"B", QDir::homePath()},
            {"C", QDir::homePath()}
        });
        // Set known IDs
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessionData/session_0"); s.setValue("id", 5); s.endGroup();
            s.beginGroup("sessionData/session_1"); s.setValue("id", 10); s.endGroup();
            s.beginGroup("sessionData/session_2"); s.setValue("id", 15); s.endGroup();
            s.beginGroup("sessions"); s.setValue("nextId", 16); s.endGroup();
            s.sync();
        }

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionIndexById(5), 0);
        QCOMPARE(mgr.sessionIndexById(10), 1);
        QCOMPARE(mgr.sessionIndexById(15), 2);
        QCOMPARE(mgr.sessionIndexById(99), -1);
        QCOMPARE(mgr.sessionIndexById(-1), -1);
    }

    void testSessionIndexByIdAfterRemoval()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions(); // initialize

        mgr.createSession(); // id=1, index=0
        mgr.createSession(); // id=2, index=1
        mgr.createSession(); // id=3, index=2

        QCOMPARE(mgr.sessionIndexById(1), 0);
        QCOMPARE(mgr.sessionIndexById(2), 1);
        QCOMPARE(mgr.sessionIndexById(3), 2);

        // Remove middle session (id=2)
        mgr.removeSession(1);

        // After removal: id=1 at index 0, id=3 at index 1
        QCOMPARE(mgr.sessionIndexById(1), 0);
        QCOMPARE(mgr.sessionIndexById(2), -1); // removed
        QCOMPARE(mgr.sessionIndexById(3), 1);
    }

    void testDesktopNotificationSignal()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions(); // initialize

        TerminalView *view1 = mgr.createSession(); // id=1
        TerminalView *view2 = mgr.createSession(); // id=2

        QSignalSpy spy(&mgr, &SessionManager::desktopNotification);

        // Emit notification from session 1's view
        emit view1->desktopNotification("title1", "body1");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 1); // sessionId
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("title1"));
        QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("body1"));

        // Emit notification from session 2's view
        emit view2->desktopNotification("title2", "body2");
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toInt(), 2); // sessionId
        QCOMPARE(spy.at(1).at(1).toString(), QStringLiteral("title2"));
        QCOMPARE(spy.at(1).at(2).toString(), QStringLiteral("body2"));
    }

    void testNextSessionIdAfterRestore()
    {
        // Write sessions with high IDs
        writeRawSessions({
            {"A", QDir::homePath()},
            {"B", QDir::homePath()}
        });
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessionData/session_0"); s.setValue("id", 100); s.endGroup();
            s.beginGroup("sessionData/session_1"); s.setValue("id", 200); s.endGroup();
            s.beginGroup("sessions"); s.setValue("nextId", 201); s.endGroup();
            s.sync();
        }

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create a new session — its ID should be >= 201
        mgr.createSession();
        QCOMPARE(mgr.sessionId(2), 201); // nextId was 201
    }
};

QTEST_MAIN(TestSessionPersistence)
#include "tst_session_persistence.moc"
