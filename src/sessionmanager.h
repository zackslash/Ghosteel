#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QObject>
#include <QQmlListProperty>
#include <QVector>
#include <QSettings>
#include <QTimer>

class TerminalView;

struct SessionInfo {
    int id;
    QString name;
    QString cachedWorkingDirectory; // Persisted CWD for inactive sessions
    QString autorunCommand;  // Command to run when session starts
    bool keybarOpen = true;           // Whether the extra keys panel is open
    bool keyboardVisible = true;      // Whether the software keyboard is visible
    TerminalView *view;
};

class SessionManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int activeSessionIndex READ activeSessionIndex WRITE setActiveSessionIndex NOTIFY activeSessionIndexChanged)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionCountChanged)
    Q_PROPERTY(QQmlListProperty<TerminalView> sessions READ sessions NOTIFY sessionsChanged)

public:
    explicit SessionManager(QObject *parent = nullptr);
    explicit SessionManager(const QString &settingsPath, QObject *parent = nullptr);
    ~SessionManager();

    int activeSessionIndex() const { return m_activeSessionIndex; }
    void setActiveSessionIndex(int index);

    int sessionCount() const { return m_sessions.size(); }

    QQmlListProperty<TerminalView> sessions();

    Q_INVOKABLE TerminalView* createSession();
    Q_INVOKABLE void removeSession(int index);
    // QML convenience: setActiveSessionIndex is a Q_PROPERTY setter, not
    // Q_INVOKABLE, so QML needs this to call it by name.
    Q_INVOKABLE void switchToSession(int index);
    Q_INVOKABLE TerminalView* activeSession() const;
    Q_INVOKABLE QString sessionName(int index) const;
    Q_INVOKABLE void setSessionName(int index, const QString &name);
    Q_INVOKABLE int sessionId(int index) const;
    Q_INVOKABLE void removeSessionById(int id);
    Q_INVOKABLE bool restoreSessions(); // Returns true if sessions were restored
    Q_INVOKABLE QString sessionWorkingDirectory(int index) const;
    Q_INVOKABLE QString sessionAutorunCommand(int index) const;
    Q_INVOKABLE void setSessionAutorunCommand(int index, const QString &cmd);
    Q_INVOKABLE bool sessionKeybarOpen(int index) const;
    Q_INVOKABLE void setSessionKeybarOpen(int index, bool open);
    Q_INVOKABLE bool sessionKeyboardVisible(int index) const;
    Q_INVOKABLE void setSessionKeyboardVisible(int index, bool visible);

Q_SIGNALS:
    void activeSessionIndexChanged();
    void sessionCountChanged();
    void sessionsChanged();
    void sessionCreated(int index);
    void sessionRemoved(int index, int sessionId);
    void sessionSwitched(int index);
    void sessionNameChanged(int idx);
    void sessionAutorunCommandChanged(int idx);
    void sessionKeybarOpenChanged(int idx);
    void sessionKeyboardVisibleChanged(int idx);
    void sessionsRestored(); // Emitted once after restoreSessions() completes

private:
    static int sessionCountCallback(QQmlListProperty<TerminalView> *prop);
    static TerminalView* sessionAtCallback(QQmlListProperty<TerminalView> *prop, int index);

    void saveSessions();
    void scheduleSave();

    QVector<SessionInfo> m_sessions;
    int m_activeSessionIndex = -1;
    int m_nextSessionId = 1;

    // Session persistence
    QSettings m_settings;
    QTimer *m_saveTimer = nullptr;
    bool m_sessionsLoaded = false;
    bool m_savedOnQuit = false;
};

#endif // SESSIONMANAGER_H
