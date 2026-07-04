#include <QtTest>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QSignalSpy>
#include <QThread>
#include <QDir>

// Pull in the stub TerminalView (QObject-based, no Qt Quick dependency)
#include "terminalview.h"

// SessionManager under test
#include "sessionmanager.h"
#include "settings.h"

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
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        QCOMPARE(mgr.activeSessionIndex(), 0); // clamped to valid range
    }

    void testRestoreDeletedDirectoryFallback()
    {
        writeRawSessions({{"Test", "/nonexistent/path/that/does/not/exist"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        // restoreSessions caps at 100 and creates all count sessions (even without data).
        // With count=9999, only the first 100 are created (data exists for 2, defaults for 98).
        QCOMPARE(mgr.sessionCount(), 100);
    }

    void testRestoreCorruptedCountCapBelowLimit()
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionCount(), 50);
    }

    // --- Save tests ---

    void testSaveOnCreateSession()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.activeSessionIndex(), 0);

        mgr.setActiveSessionIndex(1);
        QCOMPARE(mgr.activeSessionIndex(), 1);

        // Wait for debounce timer (500ms) to fire
        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessions");
        QCOMPARE(s.value("activeId").toInt(), 2);
        s.endGroup();
    }

    void testRemoveSessionById()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.removeSessionById(999); // doesn't exist
        QCOMPARE(mgr.sessionCount(), 1); // unchanged
    }

    void testRemoveLastSession()
    {
        writeRawSessions({{"Only", "/tmp"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.activeSessionIndex(), 0);

        mgr.removeSession(0);

        // Removing the last session creates a fallback shell session
        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.activeSessionIndex(), 0);
        QVERIFY(mgr.activeSession() != nullptr);
        QVERIFY(mgr.sessionName(0).isEmpty() == false); // fallback has a default name
    }

    void testRemoveSessionBeforeActive()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 2);

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.activeSessionIndex(), 2); // "C" is active

        mgr.removeSession(0); // remove "A" (before active)
        QCOMPARE(mgr.activeSessionIndex(), 1); // active shifts down
        QCOMPARE(mgr.sessionName(mgr.activeSessionIndex()), QStringLiteral("C"));
    }

    void testRemoveSessionAfterActive()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 0);

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 3);
        QCOMPARE(mgr.activeSessionIndex(), 1); // "B" is active

        mgr.removeSession(1); // remove "B" (the active session)

        QCOMPARE(mgr.sessionCount(), 2);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("A"));
        QCOMPARE(mgr.sessionName(1), QStringLiteral("C"));
        QCOMPARE(mgr.activeSessionIndex(), 0); // first in sort order (manual → raw 0)
    }

    void testRemoveActiveSessionFirstOfThree()
    {
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 0);

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.sessionName(0), QStringLiteral("Htop"));
        QCOMPARE(mgr.sessionAutorunCommand(0), QStringLiteral("htop"));
    }

    void testRestoreWithEmptyAutorunCommand()
    {
        writeRawSessions({{"Terminal", "/tmp"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.sessionAutorunCommand(0), QString());
    }

    void testSaveAutorunCommand()
    {
        writeRawSessions({{"Htop", "/tmp"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        QSignalSpy spy(&mgr, &SessionManager::sessionAutorunCommandChanged);

        // Set same value — should not emit signal
        mgr.setSessionAutorunCommand(0, "htop");
        QCOMPARE(spy.count(), 0);
    }

    void testSetAutorunCommandChanged()
    {
        writeRawSessions({{"Htop", "/tmp", "htop"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();

            mgr.createSession();
            mgr.setSessionName(0, "Htop");
            mgr.setSessionAutorunCommand(0, "htop");
        }

        // New instance — restore
        Settings settings2(m_settingsPath);
        SessionManager mgr2(&settings2);
        mgr2.restoreSessions();

        QCOMPARE(mgr2.sessionCount(), 1);
        QCOMPARE(mgr2.sessionName(0), QStringLiteral("Htop"));
        QCOMPARE(mgr2.sessionAutorunCommand(0), QStringLiteral("htop"));
    }

    void testAutorunWithSpecialCharacters()
    {
        writeRawSessions({{"Logs", "/tmp", "tail -f /var/log/syslog | grep error"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionAutorunCommand(0), QStringLiteral("tail -f /var/log/syslog | grep error"));
    }

    void testClearAutorunCommand()
    {
        // Start with autorun set
        writeRawSessions({{"Htop", "/tmp", "htop"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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
        Settings settings2(m_settingsPath);
        SessionManager mgr2(&settings2);
        mgr2.restoreSessions();
        QCOMPARE(mgr2.sessionAutorunCommand(0), QString());
    }

    // --- Keybar & keyboard state tests ---

    void testRestoreKeybarOpen()
    {
        writeRawSessions({{"Test", "/tmp", "", "true"}});
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionKeybarOpen(0), true);
    }

    void testRestoreKeybarClosed()
    {
        writeRawSessions({{"Test", "/tmp", "", "false"}});
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionKeybarOpen(0), false);
    }

    void testRestoreKeyboardVisible()
    {
        writeRawSessions({{"Test", "/tmp", "", "", "true"}});
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionKeyboardVisible(0), true);
    }

    void testRestoreKeyboardHidden()
    {
        writeRawSessions({{"Test", "/tmp", "", "", "false"}});
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionKeyboardVisible(0), false);
    }

    void testSaveKeybarState()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.createSession();
        QSignalSpy spy(&mgr, &SessionManager::sessionKeybarOpenChanged);
        mgr.setSessionKeybarOpen(0, true); // default is true
        QCOMPARE(spy.count(), 0);
    }

    void testKeyboardNoOpSuppressesSignal()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.createSession();
        QSignalSpy spy(&mgr, &SessionManager::sessionKeyboardVisibleChanged);
        mgr.setSessionKeyboardVisible(0, true); // default is true
        QCOMPARE(spy.count(), 0);
    }

    void testKeybarSignalOnChange()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.createSession();
        QSignalSpy spy(&mgr, &SessionManager::sessionKeybarOpenChanged);
        mgr.setSessionKeybarOpen(0, false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);
    }

    void testKeyboardSignalOnChange()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.createSession();
        QSignalSpy spy(&mgr, &SessionManager::sessionKeyboardVisibleChanged);
        mgr.setSessionKeyboardVisible(0, false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);
    }

    void testKeybarDefaultValue()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.createSession();
        QCOMPARE(mgr.sessionKeybarOpen(0), true);
    }

    void testKeyboardDefaultValue()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.createSession();
        QCOMPARE(mgr.sessionKeyboardVisible(0), true);
    }

    void testKeybarKeyboardFullCycle()
    {
        // Save sessions with custom UI state
        {
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
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
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();

            mgr.createSession();
            mgr.createSession();
            mgr.setSessionName(0, "Projects");
            mgr.setSessionName(1, "Logs");
            mgr.setActiveSessionIndex(1);

            // Trigger save via destructor
        }

        // New manager instance — restore from saved state
        Settings settings2(m_settingsPath);
        SessionManager mgr2(&settings2);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionIndexById(5), 0);
        QCOMPARE(mgr.sessionIndexById(10), 1);
        QCOMPARE(mgr.sessionIndexById(15), 2);
        QCOMPARE(mgr.sessionIndexById(99), -1);
        QCOMPARE(mgr.sessionIndexById(-1), -1);
    }

    void testSessionIndexByIdAfterRemoval()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
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

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        // Create a new session — its ID should be >= 201
        mgr.createSession();
        QCOMPARE(mgr.sessionId(2), 201); // nextId was 201
    }

    void testNavigateSessionSignal()
    {
        TerminalView tv;
        QSignalSpy spy(&tv, &TerminalView::navigateSession);

        emit tv.navigateSession(-1);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), -1);

        emit tv.navigateSession(1);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toInt(), 1);
    }

    // --- Timestamp tests ---

    void testCreateSessionSetsTimestamps()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession();

        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessionData/session_0");
        qint64 createdAt = s.value("createdAt").toLongLong();
        qint64 lastUsedAt = s.value("lastUsedAt").toLongLong();
        s.endGroup();

        QVERIFY(createdAt > 0);
        QVERIFY(lastUsedAt > 0);
        QCOMPARE(createdAt, lastUsedAt); // set at the same time
    }

    void testTimestampsPersistAndRestore()
    {
        // Create a session and save
        qint64 savedCreatedAt = 0;
        qint64 savedLastUsedAt = 0;
        {
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();
            mgr.createSession();
            QTest::qWait(DEBOUNCE_WAIT_MS);

            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessionData/session_0");
            savedCreatedAt = s.value("createdAt").toLongLong();
            savedLastUsedAt = s.value("lastUsedAt").toLongLong();
            s.endGroup();
        }

        QVERIFY(savedCreatedAt > 0);
        QVERIFY(savedLastUsedAt > 0);

        // Create new manager and restore
        Settings settings2(m_settingsPath);
        SessionManager mgr2(&settings2);
        mgr2.restoreSessions();
        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Read back and verify timestamps match
        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessionData/session_0");
        qint64 restoredCreatedAt = s.value("createdAt").toLongLong();
        qint64 restoredLastUsedAt = s.value("lastUsedAt").toLongLong();
        s.endGroup();

        QCOMPARE(restoredCreatedAt, savedCreatedAt);
        QCOMPARE(restoredLastUsedAt, savedLastUsedAt);
    }

    void testTimestampsDefaultZeroForLegacySessions()
    {
        // writeRawSessions doesn't write timestamps — simulates legacy data
        writeRawSessions({{"Legacy", "/tmp"}});

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessionData/session_0");
        qint64 createdAt = s.value("createdAt").toLongLong();
        qint64 lastUsedAt = s.value("lastUsedAt").toLongLong();
        s.endGroup();

        QCOMPARE(createdAt, static_cast<qint64>(0));
        QCOMPARE(lastUsedAt, static_cast<qint64>(0));
    }

    void testSwitchToSessionUpdatesLastUsedAt()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        // Create 2 sessions with a small delay between them
        mgr.createSession(); // session 0
        QTest::qWait(20);
        mgr.createSession(); // session 1 — has higher lastUsedAt

        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Read initial timestamps
        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessionData/session_0");
        qint64 session0LastUsedBefore = s.value("lastUsedAt").toLongLong();
        s.endGroup();
        s.beginGroup("sessionData/session_1");
        qint64 session1LastUsedBefore = s.value("lastUsedAt").toLongLong();
        s.endGroup();

        QVERIFY(session1LastUsedBefore >= session0LastUsedBefore);

        // Switch to session 0
        mgr.setActiveSessionIndex(0);
        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Read updated timestamps
        QSettings s2(m_settingsPath, QSettings::IniFormat);
        s2.beginGroup("sessionData/session_0");
        qint64 session0LastUsedAfter = s2.value("lastUsedAt").toLongLong();
        s2.endGroup();
        s2.beginGroup("sessionData/session_1");
        qint64 session1LastUsedAfter = s2.value("lastUsedAt").toLongLong();
        s2.endGroup();

        // Session 0's lastUsedAt should now be >= session 1's (we just switched to it)
        QVERIFY(session0LastUsedAfter >= session1LastUsedAfter);
    }

    // --- Sort mode tests ---

    void testSortModeDefault()
    {
        // SessionManager::sortMode() delegates to Settings::instance()
        // (production singleton).  Test the default via Settings directly
        // using a fresh temp path, since that's where the default is defined.
        QTemporaryDir dir;
        Settings s(dir.path() + "/sort_test.conf");
        QCOMPARE(s.sessionSortMode(), 1); // SortLastUsed is the default
    }

    void testDisplayToActualManualMode()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession();
        mgr.createSession();
        mgr.createSession();

        mgr.setSortMode(0); // SortManual

        QCOMPARE(mgr.displayToActual(0), 0);
        QCOMPARE(mgr.displayToActual(1), 1);
        QCOMPARE(mgr.displayToActual(2), 2);
    }

    void testDisplayToActualAlphabeticalSort()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession(); // actual 0
        mgr.createSession(); // actual 1
        mgr.createSession(); // actual 2

        mgr.setSessionName(0, "Charlie");
        mgr.setSessionName(1, "Alpha");
        mgr.setSessionName(2, "Bravo");

        mgr.setSortMode(3); // SortAlphabetical

        // Alphabetical order: Alpha, Bravo, Charlie
        QCOMPARE(mgr.sessionName(mgr.displayToActual(0)), QStringLiteral("Alpha"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(1)), QStringLiteral("Bravo"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(2)), QStringLiteral("Charlie"));
    }

    void testDisplayToActualLastUsedSort()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession(); // actual 0 — least recent
        QTest::qWait(20);
        mgr.createSession(); // actual 1
        QTest::qWait(20);
        mgr.createSession(); // actual 2 — most recent

        mgr.setSortMode(1); // SortLastUsed

        // LastUsed descending: most recent first
        QCOMPARE(mgr.displayToActual(0), 2); // most recent
        QCOMPARE(mgr.displayToActual(2), 0); // least recent
    }

    void testActualToDisplayRoundTrip()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession();
        mgr.createSession();
        mgr.createSession();

        mgr.setSessionName(0, "Charlie");
        mgr.setSessionName(1, "Alpha");
        mgr.setSessionName(2, "Bravo");

        mgr.setSortMode(3); // SortAlphabetical

        // For each display index, round-trip through actual and back
        for (int i = 0; i < 3; i++) {
            int actual = mgr.displayToActual(i);
            QCOMPARE(mgr.actualToDisplay(actual), i);
        }
    }

    void testSwitchToSessionByDisplayIndex()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession();
        mgr.createSession();

        mgr.setSessionName(0, "Bravo");
        mgr.setSessionName(1, "Alpha");

        mgr.setSortMode(3); // SortAlphabetical

        // Display 0 maps to "Alpha" (actual index 1)
        int expectedActual = mgr.displayToActual(0);

        mgr.switchToSession(0); // switch via display index
        QCOMPARE(mgr.activeSessionIndex(), expectedActual);
    }

    void testSortRebuildsOnNameChange()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession(); // actual 0 — "C"
        mgr.createSession(); // actual 1 — "A"
        mgr.createSession(); // actual 2 — "B"

        mgr.setSessionName(0, "C");
        mgr.setSessionName(1, "A");
        mgr.setSessionName(2, "B");

        mgr.setSortMode(3); // SortAlphabetical

        // Initial alphabetical order: A(1), B(2), C(0)
        QCOMPARE(mgr.sessionName(mgr.displayToActual(0)), QStringLiteral("A"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(1)), QStringLiteral("B"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(2)), QStringLiteral("C"));

        // Rename "A" (actual 1) to "Z" — should trigger rebuild
        mgr.setSessionName(1, "Z");

        // New alphabetical order: B(2), C(0), Z(1)
        QCOMPARE(mgr.sessionName(mgr.displayToActual(0)), QStringLiteral("B"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(1)), QStringLiteral("C"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(2)), QStringLiteral("Z"));
    }

    void testSortRebuildsOnRemove()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession(); // actual 0 — "C"
        mgr.createSession(); // actual 1 — "A"
        mgr.createSession(); // actual 2 — "B"

        mgr.setSessionName(0, "C");
        mgr.setSessionName(1, "A");
        mgr.setSessionName(2, "B");

        mgr.setSortMode(3); // SortAlphabetical

        // Initial alphabetical order: A(1), B(2), C(0)
        QCOMPARE(mgr.sessionName(mgr.displayToActual(0)), QStringLiteral("A"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(1)), QStringLiteral("B"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(2)), QStringLiteral("C"));

        // Remove the first-displayed session ("A" at actual index 1)
        int firstActual = mgr.displayToActual(0);
        mgr.removeSession(firstActual);

        // Remaining sessions should still be alphabetically sorted
        QCOMPARE(mgr.sessionCount(), 2);
        QCOMPARE(mgr.sessionName(mgr.displayToActual(0)), QStringLiteral("B"));
        QCOMPARE(mgr.sessionName(mgr.displayToActual(1)), QStringLiteral("C"));
    }

    void testSortedIndicesValidDuringSignalHandlers()
    {
        // Regression: when signals fire, displayToActual() must return
        // valid mappings because rebuildSortedIndices() now runs BEFORE
        // signal emissions.  Verify by reading displayToActual() inside
        // a signal handler.
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession(); // actual 0
        mgr.createSession(); // actual 1
        mgr.createSession(); // actual 2

        mgr.setSessionName(0, "Zebra");
        mgr.setSessionName(1, "Alpha");
        mgr.setSessionName(2, "Middle");

        mgr.setSortMode(3); // SortAlphabetical — Alpha(1), Middle(2), Zebra(0)

        // Capture displayToActual(0) from inside a sessionsChanged handler
        int display0ActualFromSignal = -1;

        connect(&mgr, &SessionManager::sessionsChanged, [&]() {
            // At this point, rebuildSortedIndices() must have already run
            display0ActualFromSignal = mgr.displayToActual(0);
        });

        // Switch active session — triggers rebuild + activeSessionIndexChanged
        // (sessionsChanged is not emitted by setActiveSessionIndex, so test
        // with setSortMode which does emit it)
        mgr.setSortMode(0); // SortManual — insertion order
        QCOMPARE(display0ActualFromSignal, mgr.displayToActual(0));

        // Now test removeSession — emits sessionsChanged after rebuild
        display0ActualFromSignal = -1;
        mgr.setSortMode(3); // Back to alphabetical
        int firstActual = mgr.displayToActual(0); // "Alpha"
        mgr.removeSession(firstActual);

        // During the sessionsChanged signal, displayToActual(0) should
        // have returned a valid (non-negative) index
        QVERIFY(display0ActualFromSignal >= 0);
        QCOMPARE(display0ActualFromSignal, mgr.displayToActual(0));
    }

    // --- -e / --exec and -s / --session tests ---

    // --- createSessionWithCommand() ---

    void testCreateCommandSessionSetsFields()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        TerminalView *view = mgr.createSessionWithCommand("htop", QStringList() << "htop");
        QVERIFY(view);

        int idx = mgr.sessionCount() - 1;
        QCOMPARE(mgr.sessionName(idx), QStringLiteral("htop"));
        QCOMPARE(mgr.sessionExecCommand(idx), QStringLiteral("htop"));
    }

    void testCreateCommandSessionAnonymous()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        TerminalView *view = mgr.createSessionWithCommand(QString(), QStringList() << "htop");
        QVERIFY(view);

        int idx = mgr.sessionCount() - 1;
        QCOMPARE(mgr.sessionName(idx), QString());
        QCOMPARE(mgr.sessionExecCommand(idx), QStringLiteral("htop"));
    }

    void testCreateCommandSessionSetsCommandArgs()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QStringList args = QStringList() << "python3" << "-m" << "http.server";
        TerminalView *view = mgr.createSessionWithCommand("python3", args);
        QVERIFY(view);
        QCOMPARE(view->commandArgs(), args);
    }

    // --- Anonymous auto-remove ---

    void testAnonymousAutoRemoveOnSuccess()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSessionWithCommand(QString(), QStringList() << "true");
        int countBefore = mgr.sessionCount();
        int idx = countBefore - 1;

        // Get the view to emit commandExited
        TerminalView *view = mgr.sessionById(mgr.sessionId(idx));
        QVERIFY(view);

        QSignalSpy removedSpy(&mgr, &SessionManager::sessionCountChanged);
        view->emitCommandExited(0); // success = immediate remove (0ms singleShot)

        // Process the queued singleShot(0) and verify removal
        QTest::qWait(50);
        // If this was the last session, a fallback shell session is created
        int expected = (countBefore == 1) ? 1 : countBefore - 1;
        QCOMPARE(mgr.sessionCount(), expected);
    }

    void testAnonymousAutoRemoveOnErrorDelayed()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSessionWithCommand(QString(), QStringList() << "badcommand");
        int countBefore = mgr.sessionCount();
        int idx = countBefore - 1;

        TerminalView *view = mgr.sessionById(mgr.sessionId(idx));
        QVERIFY(view);

        view->emitCommandExited(127); // error = delayed remove

        // Should NOT be removed immediately
        QCOMPARE(mgr.sessionCount(), countBefore);

        // Wait for the delay (kCommandExitDisplayDelayMs = 800 + margin)
        QTest::qWait(900);

        // If this was the last session, a fallback shell session is created
        int expected = (countBefore == 1) ? 1 : countBefore - 1;
        QCOMPARE(mgr.sessionCount(), expected);
    }

    void testAnonymousAutoRemoveSkippedAfterRename()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSessionWithCommand(QString(), QStringList() << "htop");
        int countBefore = mgr.sessionCount();
        int idx = countBefore - 1;
        int sessionId = mgr.sessionId(idx);

        // Rename the session — it's no longer anonymous
        mgr.setSessionName(idx, "My Htop");

        TerminalView *view = mgr.sessionById(sessionId);
        QVERIFY(view);
        view->emitCommandExited(0);

        QCOMPARE(mgr.sessionCount(), countBefore);
    }

    void testNamedCommandSessionNotAutoRemoved()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSessionWithCommand("editor", QStringList() << "nvim");
        int countBefore = mgr.sessionCount();
        int idx = countBefore - 1;

        TerminalView *view = mgr.sessionById(mgr.sessionId(idx));
        QVERIFY(view);
        view->emitCommandExited(0);

        QCOMPARE(mgr.sessionCount(), countBefore);
    }

    void testNamedCommandSessionAutoRemovedOnError()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSessionWithCommand("htop", QStringList() << "htop");
        int countBefore = mgr.sessionCount();
        int idx = countBefore - 1;

        TerminalView *view = mgr.sessionById(mgr.sessionId(idx));
        QVERIFY(view);
        view->emitCommandExited(127); // command not found

        QThread::msleep(900);
        QCoreApplication::processEvents();

        // Named session removed on error. If it was the last session,
        // removeSession() creates a fallback shell — so count may be 1.
        int expected = (countBefore == 1) ? 1 : countBefore - 1;
        QCOMPARE(mgr.sessionCount(), expected);
    }

    void testNamedSessionRerunsCommandAfterExit()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // First launch: create named session with command
        mgr.setCliArgs("htop", QStringList(), "htop");
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionName(mgr.sessionCount() - 1), QStringLiteral("htop"));
        QCOMPARE(mgr.sessionExecCommand(mgr.sessionCount() - 1), QStringLiteral("htop"));

        // Command exits successfully — named session stays alive
        int idx = mgr.sessionCount() - 1;
        TerminalView *view = mgr.sessionById(mgr.sessionId(idx));
        QVERIFY(view);
        view->emitCommandExited(0);
        QCoreApplication::processEvents();

        // Session still exists, but shellExited is true
        QVERIFY(view->shellExited());

        // Second launch: same named session, same command
        // Should remove the dead session and create a new one
        int countBefore = mgr.sessionCount();
        mgr.setCliArgs("htop", QStringList(), "htop");
        mgr.processCliArgs();

        // New session has the same name and command
        QCOMPARE(mgr.sessionName(mgr.sessionCount() - 1), QStringLiteral("htop"));
        QCOMPARE(mgr.sessionExecCommand(mgr.sessionCount() - 1), QStringLiteral("htop"));
    }

    void testProcessCliArgsExecOnly()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        int countBefore = mgr.sessionCount();

        mgr.setCliArgs("htop", QStringList(), QString());
        mgr.processCliArgs();

        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        int idx = mgr.sessionCount() - 1;
        QCOMPARE(mgr.sessionExecCommand(idx), QStringLiteral("htop"));
        QCOMPARE(mgr.sessionName(idx), QString());
    }

    void testProcessCliArgsSessionOnly()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        int countBefore = mgr.sessionCount();

        mgr.setCliArgs(QString(), QStringList(), "editor");
        mgr.processCliArgs();

        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        int idx = mgr.sessionCount() - 1;
        QCOMPARE(mgr.sessionName(idx), QStringLiteral("editor"));
        QCOMPARE(mgr.sessionExecCommand(idx), QString());
    }

    void testProcessCliArgsExecAndSession()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        int countBefore = mgr.sessionCount();

        mgr.setCliArgs("nvim", QStringList(), "editor");
        mgr.processCliArgs();

        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        int idx = mgr.sessionCount() - 1;
        QCOMPARE(mgr.sessionName(idx), QStringLiteral("editor"));
        QCOMPARE(mgr.sessionExecCommand(idx), QStringLiteral("nvim"));
    }

    void testProcessCliArgsIdempotent()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.setCliArgs("htop", QStringList(), QString());
        mgr.processCliArgs();
        int countAfterFirst = mgr.sessionCount();

        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), countAfterFirst);
    }

    void testProcessCliArgsExecWithArgs()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        int countBefore = mgr.sessionCount();

        QStringList execArgs = QStringList() << "-m" << "http.server";
        mgr.setCliArgs("python3", execArgs, QString());
        mgr.processCliArgs();

        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        TerminalView *view = mgr.sessionById(mgr.sessionId(mgr.sessionCount() - 1));
        QVERIFY(view);
        QCOMPARE(view->commandArgs(), QStringList() << "python3" << "-m" << "http.server");
    }

    void testProcessCliArgsReusesAnonymousSession()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // First call creates a new session
        mgr.setCliArgs("top", QStringList(), QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 1);
        int activeAfterFirst = mgr.activeSessionIndex();
        QCOMPARE(mgr.sessionExecCommand(activeAfterFirst), QStringLiteral("top"));

        // Second call with same command should reuse (not create)
        mgr.setCliArgs("top", QStringList(), QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 1); // still 1
        QCOMPARE(mgr.activeSessionIndex(), activeAfterFirst); // same session
    }

    void testProcessCliArgsReuseSkipsNamedSessions()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create a named session running "top"
        mgr.createSessionWithCommand("sysmon", QStringList() << "top");
        QCOMPARE(mgr.sessionCount(), 1);

        // Anonymous "-e top" should NOT reuse the named "sysmon" session
        mgr.setCliArgs("top", QStringList(), QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 2); // new session created
        int activeAfter = mgr.activeSessionIndex();
        QCOMPARE(mgr.sessionName(activeAfter), QString()); // anonymous
        QCOMPARE(mgr.sessionExecCommand(activeAfter), QStringLiteral("top"));
    }

    void testProcessCliArgsNamedReuseByName()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create a named session
        mgr.createSessionWithCommand("editor", QStringList() << "nvim");
        QCOMPARE(mgr.sessionCount(), 1);
        int namedIdx = mgr.activeSessionIndex();

        // Create another session to change active
        mgr.createSession();
        QCOMPARE(mgr.activeSessionIndex(), 1); // now on regular session

        // "-s editor -e nvim" should switch back to the named session (not create new)
        mgr.setCliArgs("nvim", QStringList(), "editor");
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 2); // no new session
        QCOMPARE(mgr.activeSessionIndex(), namedIdx); // switched to editor
    }

    void testProcessCliArgsDifferentCommandsNotReused()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create session with "htop"
        mgr.setCliArgs("htop", QStringList(), QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 1);

        // "btop" should NOT reuse "htop" session
        mgr.setCliArgs("btop", QStringList(), QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 2); // new session
    }

    void testProcessCliArgsSameBinaryDifferentArgsNotReused()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create session with "python3 -m http.server"
        mgr.setCliArgs("python3", QStringList() << "-m" << "http.server", QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 1);

        // "python3 -m http.server 8080" should NOT reuse (different args)
        mgr.setCliArgs("python3", QStringList() << "-m" << "http.server" << "8080", QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 2); // new session
    }

    void testProcessCliArgsSameFullCommandReuses()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create session with "python3 -m http.server"
        mgr.setCliArgs("python3", QStringList() << "-m" << "http.server", QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 1);
        int activeAfterFirst = mgr.activeSessionIndex();

        // Same full command should reuse
        mgr.setCliArgs("python3", QStringList() << "-m" << "http.server", QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 1); // still 1
        QCOMPARE(mgr.activeSessionIndex(), activeAfterFirst); // same session
    }

    void testProcessCliArgsNoArgsVsArgsNotReused()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create session with "top" (no extra args)
        mgr.setCliArgs("top", QStringList(), QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 1);

        // "top -d 5" should NOT reuse the plain "top" session
        mgr.setCliArgs("top", QStringList() << "-d" << "5", QString());
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), 2); // new session
    }

    // --- saveSessions skips anonymous ---

    void testSaveSkipsAnonymousCommandSessions()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create a regular session, a named command session, and an anonymous command session
        mgr.createSession(); // regular
        mgr.createSessionWithCommand("editor", QStringList() << "nvim"); // named command
        mgr.createSessionWithCommand(QString(), QStringList() << "htop"); // anonymous

        QCOMPARE(mgr.sessionCount(), 3);

        // Trigger save
        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Read settings and verify count
        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessions");
        int savedCount = s.value("count").toInt();
        s.endGroup();

        QCOMPARE(savedCount, 2); // anonymous should be skipped
    }

    void testSavePreservesNamedCommandSessions()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSession(); // regular session
        mgr.createSessionWithCommand("editor", QStringList() << "nvim");
        QTest::qWait(DEBOUNCE_WAIT_MS);

        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessions");
        QCOMPARE(s.value("count").toInt(), 2); // regular + named command
        s.endGroup();

        s.beginGroup("sessionData/session_1");
        QCOMPARE(s.value("name").toString(), QStringLiteral("editor"));
        s.endGroup();
    }

    // --- activeId with anonymous sessions ---

    void testActiveIdWhenAnonymousIsActive()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        mgr.createSession(); // session 0 (regular, id=1)
        TerminalView *view = mgr.createSessionWithCommand(QString(), QStringList() << "htop"); // session 1 (anonymous, id=2, now active)
        Q_UNUSED(view);

        QCOMPARE(mgr.activeSessionIndex(), 1); // anonymous is active

        // Save fires — anonymous session is skipped in persistence, but activeId
        // points to the anonymous session's ID (because it IS the active session).
        QTest::qWait(DEBOUNCE_WAIT_MS);

        int anonId = mgr.sessionId(1);

        // Verify activeId was saved pointing to the anonymous session's ID
        QSettings s(m_settingsPath, QSettings::IniFormat);
        s.beginGroup("sessions");
        int savedActiveId = s.value("activeId").toInt();
        int savedCount = s.value("count").toInt();
        s.endGroup();

        QCOMPARE(savedActiveId, anonId); // activeId tracks the real active session
        QCOMPARE(savedCount, 1);         // anonymous session is NOT persisted

        // On restore, the anonymous session doesn't exist — activeId has no match,
        // so the manager falls back to session 0.
        SessionManager mgr2(m_settingsPath);
        mgr2.restoreSessions();
        QCOMPARE(mgr2.activeSessionIndex(), 0);
    }

    void testActiveIdLegacyFallback()
    {
        // Write settings with only activeIndex (no activeId) — simulates pre-feature config
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}}, 1);

        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.activeSessionIndex(), 1); // should use legacy index
    }

    // --- findSessionByName (tested indirectly via switchToSessionByName) ---

    void testFindSessionByName()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSession(); // index 0
        mgr.setSessionName(0, "editor");

        mgr.createSession(); // index 1
        mgr.setSessionName(1, "logs");

        mgr.setActiveSessionIndex(1); // start on "logs"

        mgr.switchToSessionByName("editor");
        QCOMPARE(mgr.activeSessionIndex(), 0);
    }

    void testFindSessionByNameNotFound()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // switchToSessionByName with non-existent name should create a new session
        int countBefore = mgr.sessionCount();
        mgr.switchToSessionByName("nonexistent");
        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        QCOMPARE(mgr.sessionName(mgr.sessionCount() - 1), QStringLiteral("nonexistent"));
    }

    void testFindSessionByNameCaseSensitive()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        mgr.setSessionName(0, "editor");

        int countBefore = mgr.sessionCount();
        mgr.switchToSessionByName("Editor"); // different case
        QCOMPARE(mgr.sessionCount(), countBefore + 1); // should create new, not find existing
    }

    // --- switchToSessionByName ---

    void testSwitchToExistingNamedSession()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        mgr.createSession();
        mgr.setSessionName(0, "Terminal");
        mgr.setSessionName(1, "editor");
        mgr.setActiveSessionIndex(0);

        mgr.switchToSessionByName("editor");
        QCOMPARE(mgr.activeSessionIndex(), 1);
    }

    void testSwitchToNewNamedSession()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();
        int countBefore = mgr.sessionCount();

        mgr.switchToSessionByName("logs");
        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        QCOMPARE(mgr.sessionName(mgr.sessionCount() - 1), QStringLiteral("logs"));
        QCOMPARE(mgr.activeSessionIndex(), mgr.sessionCount() - 1);
    }

    // --- sessionExecCommand ---

    void testSessionExecCommandForCommandSession()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSessionWithCommand("htop", QStringList() << "htop");
        QCOMPARE(mgr.sessionExecCommand(mgr.sessionCount() - 1), QStringLiteral("htop"));
    }

    void testSessionExecCommandForRegularSession()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        QCOMPARE(mgr.sessionExecCommand(0), QString());
    }

    // --- isAnonymous() helper ---

    void testIsAnonymousHelper()
    {
        SessionInfo info;
        info.id = 1;
        info.name = "test";
        QCOMPARE(info.isAnonymous(), false); // named session, no command

        info.execArgs = QStringList() << "top";
        info.name = "test";
        QCOMPARE(info.isAnonymous(), false); // named command session

        info.execArgs = QStringList() << "top";
        info.name = "";
        QCOMPARE(info.isAnonymous(), true); // anonymous command session

        info.execArgs = QStringList();
        info.name = "";
        QCOMPARE(info.isAnonymous(), false); // regular session with empty name (edge case)
    }

    // --- Multiple anonymous sessions ---

    void testMultipleAnonymousSessionsIndependent()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        mgr.createSessionWithCommand(QString(), QStringList() << "htop");
        mgr.createSessionWithCommand(QString(), QStringList() << "btop");

        QCOMPARE(mgr.sessionCount(), 2);

        // Get the first anonymous session's view and emit exit
        int firstId = mgr.sessionId(0);
        TerminalView *view1 = mgr.sessionById(firstId);
        QVERIFY(view1);

        view1->emitCommandExited(0);

        // Process the queued singleShot(0) and verify only first removed
        QTest::qWait(50);
        QCOMPARE(mgr.sessionCount(), 1);
        QCOMPARE(mgr.sessionExecCommand(0), QStringLiteral("btop")); // second still exists
    }

    // --- Security: sanitization and rate limiting ---

    void testSessionNameSanitizedControlChars()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Newlines should be stripped
        int countBefore = mgr.sessionCount();
        mgr.setCliArgs(QString(), QStringList(), QStringLiteral("name\nwith\nnewlines"));
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        QString name = mgr.sessionName(mgr.sessionCount() - 1);
        QVERIFY(!name.contains('\n'));
        QVERIFY(!name.contains('\r'));

        // Colons should be stripped (IPC exec: protocol delimiter)
        countBefore = mgr.sessionCount();
        mgr.setCliArgs(QString(), QStringList(), QStringLiteral("foo:bar"));
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        QCOMPARE(mgr.sessionName(mgr.sessionCount() - 1), QStringLiteral("foobar"));

        // Long names should be truncated
        countBefore = mgr.sessionCount();
        QString longName(200, QChar('x'));
        mgr.setCliArgs(QString(), QStringList(), longName);
        mgr.processCliArgs();
        QCOMPARE(mgr.sessionCount(), countBefore + 1);
        QVERIFY(mgr.sessionName(mgr.sessionCount() - 1).length() <= 128);
    }

    void testRateLimitAtMaxSessions()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create sessions up to the limit
        // matches kMaxSessionCount in sessionmanager.cpp
        for (int i = 0; i < 100; i++) {
            auto *view = mgr.createSessionWithCommand(
                QStringLiteral("cmd%1").arg(i), QStringList() << "true");
            QVERIFY(view != nullptr);
        }
        QCOMPARE(mgr.sessionCount(), 100);

        // Next one should be rejected
        auto *view = mgr.createSessionWithCommand(
            QStringLiteral("overflow"), QStringList() << "true");
        QVERIFY(view == nullptr);
        QCOMPARE(mgr.sessionCount(), 100); // unchanged
    }

    void testRateLimitAnonymousSessions()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Fill up with anonymous sessions
        // matches kMaxSessionCount in sessionmanager.cpp
        for (int i = 0; i < 100; i++) {
            mgr.createSessionWithCommand(QString(), QStringList() << "true");
        }
        QCOMPARE(mgr.sessionCount(), 100);

        // Anonymous overflow should also be rejected
        auto *view = mgr.createSessionWithCommand(QString(), QStringList() << "htop");
        QVERIFY(view == nullptr);
        QCOMPARE(mgr.sessionCount(), 100);
    }

    void testLastAnonymousSessionExitCreatesFallback()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create a single anonymous command session
        mgr.createSessionWithCommand(QString(), QStringList() << "echo");
        QCOMPARE(mgr.sessionCount(), 1);

        // Simulate command exit (success) — triggers immediate auto-remove
        auto *view = mgr.activeSession();
        QVERIFY(view);
        view->emitCommandExited(0);
        QCoreApplication::processEvents();

        // Should have created a fallback shell session, not 0 sessions
        QCOMPARE(mgr.sessionCount(), 1);
        // The fallback session should be a regular session (no execArgs)
        QVERIFY(mgr.sessionExecCommand(0).isEmpty());
    }

    void testRestartShellDuringAutoRemoveDelayPreventsRemoval()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create an anonymous command session
        mgr.createSessionWithCommand(QString(), QStringList() << "failing-cmd");
        QCOMPARE(mgr.sessionCount(), 1);

        // Simulate command exit with error — schedules 800ms auto-remove
        auto *view = mgr.activeSession();
        QVERIFY(view);
        view->emitCommandExited(1);

        // User taps terminal before the 800ms timer fires — restarts shell
        view->restartShell();

        // Now let the timer fire
        QThread::msleep(900);
        QCoreApplication::processEvents();

        // Session should still exist — restartShell cleared execArgs,
        // so isAnonymous() returned false and auto-remove was skipped.
        QCOMPARE(mgr.sessionCount(), 1);
    }

    void testRestartShellBeforeImmediateAutoRemovePreventsRemoval()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create an anonymous command session
        mgr.createSessionWithCommand(QString(), QStringList() << "quick-cmd");
        QCOMPARE(mgr.sessionCount(), 1);

        // Simulate command exit with success — schedules singleShot(0, ...)
        auto *view = mgr.activeSession();
        QVERIFY(view);
        view->emitCommandExited(0);

        // User taps terminal BEFORE processEvents() fires the 0ms timer
        view->restartShell();

        // Now let the 0ms timer fire
        QCoreApplication::processEvents();

        // Session should survive — restartShell cleared execArgs
        QCOMPARE(mgr.sessionCount(), 1);
    }

    void testIpcMessageEncodeParseRoundTrip()
    {
        // Encode: ghosteel -e top -o %CPU
        QByteArray wire = IpcMessage::encode("top", QStringList() << "-o" << "%CPU", "");
        IpcMessage parsed = IpcMessage::parse(wire.trimmed());
        QCOMPARE(parsed.type, IpcMessage::Exec);
        QCOMPARE(parsed.command, QStringLiteral("top"));
        QCOMPARE(parsed.args.size(), 2);
        QCOMPARE(parsed.args.at(0), QStringLiteral("-o"));
        QCOMPARE(parsed.args.at(1), QStringLiteral("%CPU"));
        QCOMPARE(parsed.sessionName, QString());

        // Encode: ghosteel -e top -d 5
        QByteArray wire2 = IpcMessage::encode("top", QStringList() << "-d" << "5", "");
        IpcMessage parsed2 = IpcMessage::parse(wire2.trimmed());
        QCOMPARE(parsed2.type, IpcMessage::Exec);
        QCOMPARE(parsed2.command, QStringLiteral("top"));
        QCOMPARE(parsed2.args.size(), 2);
        QCOMPARE(parsed2.args.at(0), QStringLiteral("-d"));
        QCOMPARE(parsed2.args.at(1), QStringLiteral("5"));

        // These two must NOT be equal — different args = different sessions
        QVERIFY(parsed.args != parsed2.args);
    }

    void testIpcMessageExecWithSessionName()
    {
        QByteArray wire = IpcMessage::encode("htop", QStringList(), "htop");
        IpcMessage parsed = IpcMessage::parse(wire.trimmed());
        QCOMPARE(parsed.type, IpcMessage::Exec);
        QCOMPARE(parsed.command, QStringLiteral("htop"));
        QCOMPARE(parsed.args.size(), 0);
        QCOMPARE(parsed.sessionName, QStringLiteral("htop"));
    }

    void testIpcMessageRaiseAndSwitch()
    {
        QByteArray raise = IpcMessage::encode(QString(), QStringList(), QString());
        IpcMessage parsedRaise = IpcMessage::parse(raise.trimmed());
        QCOMPARE(parsedRaise.type, IpcMessage::Raise);

        QByteArray sw = IpcMessage::encode(QString(), QStringList(), "editor");
        IpcMessage parsedSwitch = IpcMessage::parse(sw.trimmed());
        QCOMPARE(parsedSwitch.type, IpcMessage::Switch);
        QCOMPARE(parsedSwitch.sessionName, QStringLiteral("editor"));
    }

    void testAutoRemoveEmitsShowSessionList()
    {
        SessionManager mgr(m_settingsPath);
        mgr.restoreSessions();

        // Create a regular session first, then an anonymous command session
        mgr.createSession();
        mgr.createSessionWithCommand(QString(), QStringList() << "failing-cmd");
        QCOMPARE(mgr.sessionCount(), 2);

        QSignalSpy spy(&mgr, &SessionManager::showSessionList);

        // Simulate command exit with error — schedules 800ms auto-remove
        auto *view = mgr.activeSession();
        QVERIFY(view);
        view->emitCommandExited(1);

        // Wait for the 800ms error delay to fire
        QThread::msleep(900);
        QCoreApplication::processEvents();

        // Anonymous session removed, regular session remains
        QCOMPARE(mgr.sessionCount(), 1);
        // showSessionList should have been emitted
        QCOMPARE(spy.count(), 1);
    }

    // --- Session-scoped font size tests ---

    void testSetSessionFontSizeSessionOnly()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        TerminalView *view = mgr.createSession();
        QCOMPARE(settings.fontSize(), 18); // global default

        mgr.setActiveSessionFontSize(20, false);

        QCOMPARE(mgr.activeSessionFontSize(), 20);
        QCOMPARE(settings.fontSize(), 18); // global unchanged
        QCOMPARE(view->fontSize(), 20);
    }

    void testSetSessionFontSizeUpdatesGlobal()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        TerminalView *view = mgr.createSession();
        QCOMPARE(settings.fontSize(), 18);

        mgr.setActiveSessionFontSize(20, true);

        QCOMPARE(mgr.activeSessionFontSize(), 20);
        QCOMPARE(settings.fontSize(), 20); // global updated
        QCOMPARE(view->fontSize(), 20);
    }

    void testSetSessionFontSizeZeroResetsToDefault()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        TerminalView *view = mgr.createSession();

        // Set explicit override with updateGlobal=false so global stays at 18
        mgr.setActiveSessionFontSize(20, false);
        QCOMPARE(mgr.activeSessionFontSize(), 20);
        QCOMPARE(settings.fontSize(), 18);

        // Reset to track global default
        mgr.setActiveSessionFontSize(0);

        QCOMPARE(mgr.activeSessionFontSize(), 0);
        QCOMPARE(settings.fontSize(), 18); // global unchanged
        QCOMPARE(view->fontSize(), settings.fontSize()); // view resolved to global
    }

    void testSetSessionFontSizeZeroNoOpWhenAlreadyZero()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        mgr.createSession(); // fontSize starts at 0 (track default)

        QSignalSpy spy(&mgr, &SessionManager::activeSessionFontSizeChanged);

        // Already 0 — should be a no-op with no signal
        mgr.setActiveSessionFontSize(0);

        QCOMPARE(spy.count(), 0);
    }

    void testSetSessionFontSizeDedupNoSignal()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        mgr.createSession();

        // Set to 20 with updateGlobal=true
        mgr.setActiveSessionFontSize(20, true);
        QCOMPARE(settings.fontSize(), 20);

        QSignalSpy spy(&mgr, &SessionManager::activeSessionFontSizeChanged);

        // Set same value again — no signal, but global still synced
        mgr.setActiveSessionFontSize(20, true);

        QCOMPARE(spy.count(), 0);
        QCOMPARE(settings.fontSize(), 20); // global still synced
    }

    void testActiveSessionFontSizeChangedSignal()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        mgr.createSession();

        QSignalSpy spy(&mgr, &SessionManager::activeSessionFontSizeChanged);

        // First set — signal emitted
        mgr.setActiveSessionFontSize(22, false);
        QCOMPARE(spy.count(), 1);

        // Same value again — no signal (dedup)
        mgr.setActiveSessionFontSize(22, false);
        QCOMPARE(spy.count(), 1);

        // Reset to default — signal emitted
        mgr.setActiveSessionFontSize(0);
        QCOMPARE(spy.count(), 2);
    }

    void testGlobalFontSizeChangePropagatesToTrackingSessions()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        // Create 2 sessions — both start with fontSize=0 (tracking default)
        TerminalView *view0 = mgr.createSession();
        TerminalView *view1 = mgr.createSession();
        QCOMPARE(mgr.sessionCount(), 2);

        // Change global default — both views should update
        settings.setFontSize(24);
        QCOMPARE(view0->fontSize(), 24);
        QCOMPARE(view1->fontSize(), 24);

        // Override session 0 with explicit size (don't touch global)
        mgr.setActiveSessionIndex(0);
        mgr.setActiveSessionFontSize(20, false);
        QCOMPARE(view0->fontSize(), 20);
        QCOMPARE(mgr.activeSessionFontSize(), 20);

        // Change global again — session 0 stays overridden, session 1 tracks
        settings.setFontSize(26);
        QCOMPARE(view0->fontSize(), 20); // override preserved
        QCOMPARE(view1->fontSize(), 26); // tracking updated
    }

    void testSavePreservesFontSizeZeroSentinel()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        // Create a session — fontSize defaults to 0 (track global)
        mgr.createSession();
        QCOMPARE(mgr.activeSessionFontSize(), 0);

        // Trigger save via renaming (setActiveSessionFontSize(0) is a no-op when already 0)
        mgr.setSessionName(0, "Saved");
        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Read raw INI to verify fontSize persisted as 0
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessionData/session_0");
            QCOMPARE(s.value("fontSize").toInt(), 0);
            s.endGroup();
        }

        // Restore in a fresh SessionManager to verify the sentinel survives round-trip
        SessionManager mgr2(m_settingsPath);
        mgr2.restoreSessions();
        QCOMPARE(mgr2.sessionCount(), 1);
        QCOMPARE(mgr2.activeSessionFontSize(), 0);
    }

    void testSavePreservesExplicitFontSize()
    {
        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();

        mgr.createSession();
        mgr.setActiveSessionFontSize(20, false);
        QCOMPARE(mgr.activeSessionFontSize(), 20);

        // Trigger save via renaming and wait for debounce
        mgr.setSessionName(0, "FontTest");
        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Read raw INI to verify fontSize persisted as 20
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessionData/session_0");
            QCOMPARE(s.value("fontSize").toInt(), 20);
            s.endGroup();
        }

        // Restore in a fresh SessionManager
        SessionManager mgr2(m_settingsPath);
        mgr2.restoreSessions();
        QCOMPARE(mgr2.sessionCount(), 1);
        QCOMPARE(mgr2.activeSessionFontSize(), 20);
    }

    // --- scrollbackDirty pipeline tests (contentChanged / justRestored / titleChanged) ---

    void testContentChangedBlockedByJustRestored()
    {
        // After restoreSessions(), justRestored=true. Emitting contentChanged
        // should be a no-op (no scheduleSave) until titleChanged clears it.
        writeRawSessions({{"A", "/tmp"}});

        bool fileExisted = false;
        {
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();
            QCOMPARE(mgr.sessionCount(), 1);

            // Wait for the initial post-restore save to settle
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            // Remove settings file to detect if saveSessions runs again
            QFile::remove(m_settingsPath);

            // Emit contentChanged while justRestored is still true (no titleChanged yet)
            TerminalView *view = mgr.activeSession();
            QVERIFY(view);
            view->emitContentChanged();

            // Wait for debounce — save should NOT fire because justRestored blocks it
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            // Capture file existence before destructor (destructor calls saveSessions)
            fileExisted = QFile::exists(m_settingsPath);
        }
        // After block: mgr destroyed, destructor calls saveSessions which recreates file
        QVERIFY2(!fileExisted,
                 "saveSessions should not run when contentChanged fires with justRestored=true");
    }

    void testTitleChangedClearsJustRestoredThenContentTriggersSave()
    {
        // titleChanged clears justRestored; subsequent contentChanged should
        // trigger scheduleSave → saveSessions.
        writeRawSessions({{"A", "/tmp"}});

        bool fileExisted = false;
        {
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();
            QCOMPARE(mgr.sessionCount(), 1);

            // Wait for the initial post-restore save to settle
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            // Remove settings file to detect if saveSessions runs again
            QFile::remove(m_settingsPath);

            TerminalView *view = mgr.activeSession();
            QVERIFY(view);

            // titleChanged fires during real PTY data — clears justRestored
            view->setTitle("new title");

            // Now contentChanged should mark dirty and scheduleSave
            view->emitContentChanged();

            // Wait for debounce — save SHOULD fire
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            fileExisted = QFile::exists(m_settingsPath);
        }
        QVERIFY2(fileExisted,
                 "saveSessions should run after titleChanged clears justRestored and contentChanged fires");
    }

    void testPtyDataClearsJustRestoredWithoutTitle()
    {
        // ptyDataReceived clears justRestored; subsequent contentChanged should
        // trigger scheduleSave → saveSessions — even if titleChanged never fires
        // (e.g. sh/dash, bare REPLs).
        writeRawSessions({{"A", "/tmp"}});

        bool fileExisted = false;
        {
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();
            QCOMPARE(mgr.sessionCount(), 1);

            // Wait for the initial post-restore save to settle
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            // Remove settings file to detect if saveSessions runs again
            QFile::remove(m_settingsPath);

            TerminalView *view = mgr.activeSession();
            QVERIFY(view);

            // ptyDataReceived fires during real PTY data — clears justRestored
            // WITHOUT titleChanged (simulates a shell that never sets a title)
            view->emitPtyDataReceived();

            // Now contentChanged should mark dirty and scheduleSave
            view->emitContentChanged();

            // Wait for debounce — save SHOULD fire
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            fileExisted = QFile::exists(m_settingsPath);
        }
        QVERIFY2(fileExisted,
                 "saveSessions should run after ptyDataReceived clears justRestored and contentChanged fires");
    }

    void testContentChangedSpamOnlyTransitionsDirtyOnce()
    {
        // Repeated contentChanged should only flip scrollbackDirty once;
        // verify no crash and session data stays intact after the save.
        writeRawSessions({{"A", "/tmp"}});

        {
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();
            QCOMPARE(mgr.sessionCount(), 1);

            // Wait for initial post-restore save to settle
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            TerminalView *view = mgr.activeSession();
            QVERIFY(view);

            // Clear justRestored via titleChanged (simulates real PTY data)
            view->setTitle("spam test");

            // Emit contentChanged rapidly — should not crash or cause issues
            for (int i = 0; i < 20; i++)
                view->emitContentChanged();

            // Wait for debounce — saveSessions runs once
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            // Verify session data is still intact (no corruption from rapid emits)
            QCOMPARE(mgr.sessionCount(), 1);
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessions");
            QCOMPARE(s.value("count").toInt(), 1);
            s.endGroup();

            s.beginGroup("sessionData/session_0");
            QCOMPARE(s.value("name").toString(), QStringLiteral("A"));
            s.endGroup();
        }
    }

    void testJustRestoredBlocksMultipleSessions()
    {
        // Verify justRestored flag is per-session: contentChanged on session 0
        // is blocked, but titleChanged on session 1 clears only session 1's flag.
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}}, 0);

        bool fileExisted = false;
        {
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();
            QCOMPARE(mgr.sessionCount(), 2);

            // Wait for initial save
            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            QFile::remove(m_settingsPath);

            TerminalView *view0 = mgr.sessionById(mgr.sessionId(0));
            TerminalView *view1 = mgr.sessionById(mgr.sessionId(1));
            QVERIFY(view0);
            QVERIFY(view1);

            // Clear justRestored on session 1 only (via titleChanged)
            view1->setTitle("B");

            // Emit contentChanged on session 0 (still justRestored) — blocked
            view0->emitContentChanged();

            // Emit contentChanged on session 1 (justRestored cleared) — should save
            view1->emitContentChanged();

            QTest::qWait(DEBOUNCE_WAIT_MS + 100);

            fileExisted = QFile::exists(m_settingsPath);
        }
        QVERIFY2(fileExisted,
                 "saveSessions should run when at least one session's justRestored is cleared");
    }

    // --- Non-active session font size restored ---

    void testNonActiveSessionFontSizeRestored()
    {
        // All restored sessions should have their persisted fontSize applied
        // to the view, not just the active one.
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}, {"C", "/var"}}, 1);
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.beginGroup("sessionData/session_0");
            s.setValue("fontSize", 20);
            s.endGroup();
            s.beginGroup("sessionData/session_1");
            s.setValue("fontSize", 24);
            s.endGroup();
            s.beginGroup("sessionData/session_2");
            s.setValue("fontSize", 16);
            s.endGroup();
            s.sync();
        }

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 3);
        QCOMPARE(mgr.activeSessionIndex(), 1); // "B" is active

        // Verify ALL views have correct font sizes — not just the active one
        TerminalView *view0 = mgr.sessionById(mgr.sessionId(0));
        TerminalView *view1 = mgr.sessionById(mgr.sessionId(1));
        TerminalView *view2 = mgr.sessionById(mgr.sessionId(2));
        QVERIFY(view0);
        QVERIFY(view1);
        QVERIFY(view2);

        QCOMPARE(view0->fontSize(), 20); // non-active
        QCOMPARE(view1->fontSize(), 24); // active
        QCOMPARE(view2->fontSize(), 16); // non-active
    }

    void testNonActiveSessionFontSizeZeroUsesGlobalDefault()
    {
        // Sessions with fontSize=0 should get the global default applied
        writeRawSessions({{"A", "/tmp"}, {"B", "/home"}}, 0);
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            // session_0: fontSize=0 (track global)
            s.beginGroup("sessionData/session_0");
            s.setValue("fontSize", 0);
            s.endGroup();
            // session_1: explicit fontSize=22
            s.beginGroup("sessionData/session_1");
            s.setValue("fontSize", 22);
            s.endGroup();
            s.sync();
        }

        Settings settings(m_settingsPath);
        SessionManager mgr(&settings);
        mgr.restoreSessions();
        QCOMPARE(mgr.sessionCount(), 2);

        TerminalView *view0 = mgr.sessionById(mgr.sessionId(0));
        TerminalView *view1 = mgr.sessionById(mgr.sessionId(1));
        QVERIFY(view0);
        QVERIFY(view1);

        // view0 should use the global default (18); view1 has explicit 22
        QCOMPARE(view0->fontSize(), settings.fontSize()); // global default
        QCOMPARE(view1->fontSize(), 22);
    }

    void testNonActiveSessionFontSizePersistsAndRestores()
    {
        // Create sessions with different font sizes, save, restore, verify
        {
            Settings settings(m_settingsPath);
            SessionManager mgr(&settings);
            mgr.restoreSessions();

            mgr.createSession(); // session 0
            mgr.createSession(); // session 1
            mgr.createSession(); // session 2

            mgr.setActiveSessionIndex(0);
            mgr.setActiveSessionFontSize(20, false);
            mgr.setActiveSessionIndex(1);
            mgr.setActiveSessionFontSize(24, false);
            mgr.setActiveSessionIndex(2);
            mgr.setActiveSessionFontSize(16, false);

            QTest::qWait(DEBOUNCE_WAIT_MS);
        }

        // Restore in a fresh SessionManager
        Settings settings2(m_settingsPath);
        SessionManager mgr2(&settings2);
        mgr2.restoreSessions();
        QCOMPARE(mgr2.sessionCount(), 3);

        // ALL sessions should have their persisted font sizes
        TerminalView *v0 = mgr2.sessionById(mgr2.sessionId(0));
        TerminalView *v1 = mgr2.sessionById(mgr2.sessionId(1));
        TerminalView *v2 = mgr2.sessionById(mgr2.sessionId(2));
        QVERIFY(v0);
        QVERIFY(v1);
        QVERIFY(v2);

        QCOMPARE(v0->fontSize(), 20);
        QCOMPARE(v1->fontSize(), 24);
        QCOMPARE(v2->fontSize(), 16);
    }
};

QTEST_MAIN(TestSessionPersistence)
#include "tst_session_persistence.moc"
