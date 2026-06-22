#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QObject>
#include <QQmlListProperty>
#include <QVector>
#include <QTimer>
#include <QLocalServer>

class TerminalView;
class ScrollEncryptor;
class Settings;

struct SessionInfo {
    int id;
    QString name;
    QString cachedWorkingDirectory; // Persisted CWD for inactive sessions
    QString autorunCommand;  // Command to run when session starts
    bool keybarOpen = true;           // Whether the extra keys panel is open
    bool keyboardVisible = true;      // Whether the software keyboard is visible
    int fontSize = 0;                 // Per-session font size (0 = use global default)
    bool commandSession = false;      // true for -e sessions (affects exit behavior + persistence)
    QString execCommand;              // Command binary name from -e
    qint64 createdAt = 0;             // Epoch ms when session was created
    qint64 lastUsedAt = 0;            // Epoch ms when session was last switched to
    TerminalView *view;

    bool isAnonymous() const { return commandSession && name.isEmpty(); }
};

class SessionManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int activeSessionIndex READ activeSessionIndex WRITE setActiveSessionIndex NOTIFY activeSessionIndexChanged)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionCountChanged)
    Q_PROPERTY(QQmlListProperty<TerminalView> sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(bool dbusRegistered READ dbusRegistered NOTIFY dbusRegisteredChanged)

public:
    explicit SessionManager(QObject *parent = nullptr);
    explicit SessionManager(Settings *settings, QObject *parent = nullptr);
    ~SessionManager();

    int activeSessionIndex() const { return m_activeSessionIndex; }
    void setActiveSessionIndex(int index);

    int sessionCount() const { return m_sessions.size(); }

    bool dbusRegistered() const { return m_dbusRegistered; }
    void setDbusRegistered(bool registered);

    QQmlListProperty<TerminalView> sessions();

    Q_INVOKABLE TerminalView* createSession();
    TerminalView* createSessionWithCommand(const QString &name, const QStringList &commandArgs);
    Q_INVOKABLE void switchToSessionByName(const QString &name);
    Q_INVOKABLE void removeSession(int index);
    // QML convenience: setActiveSessionIndex is a Q_PROPERTY setter, not
    // Q_INVOKABLE, so QML needs this to call it by name.
    Q_INVOKABLE void switchToSession(int displayIndex);
    Q_INVOKABLE TerminalView* activeSession() const;
    Q_INVOKABLE TerminalView* sessionById(int sessionId) const;
    Q_INVOKABLE QString sessionName(int index) const;
    Q_INVOKABLE void setSessionName(int index, const QString &name);
    Q_INVOKABLE int sessionId(int index) const;
    Q_INVOKABLE void removeSessionById(int id);
    Q_INVOKABLE int sessionIndexById(int id) const; // Returns vector index for session ID, or -1
    Q_INVOKABLE bool restoreSessions(); // Returns true if sessions were restored
    Q_INVOKABLE QString sessionWorkingDirectory(int index) const;
    Q_INVOKABLE QString sessionAutorunCommand(int index) const;
    Q_INVOKABLE QString sessionExecCommand(int index) const;
    Q_INVOKABLE void setSessionAutorunCommand(int index, const QString &cmd);
    Q_INVOKABLE bool sessionKeybarOpen(int index) const;
    Q_INVOKABLE void setSessionKeybarOpen(int index, bool open);
    Q_INVOKABLE bool sessionKeyboardVisible(int index) const;
    Q_INVOKABLE void setSessionKeyboardVisible(int index, bool visible);

    Q_INVOKABLE void setActiveSessionFontSize(int size);
    Q_INVOKABLE int activeSessionFontSize() const;

    // Session ordering — maps display index (sorted) to actual m_sessions index
    Q_INVOKABLE int displayToActual(int displayIndex) const;
    Q_INVOKABLE int actualToDisplay(int actualIndex) const;
    Q_INVOKABLE int sortMode() const;
    Q_INVOKABLE void setSortMode(int mode);

    // Single-instance guard: returns true if another instance is already running.
    // Call before creating SessionManager. If true, a "raise" message was sent
    // to the existing instance and the caller should exit.
    static bool checkSingleInstance(const QString &execCommand = QString(),
                                    const QStringList &execArgs = QStringList(),
                                    const QString &sessionName = QString());

    // Start the single-instance socket server. Call after D-Bus registration
    // so that future instances can detect this one.
    void startSingleInstanceServer();

    // Store CLI arguments for deferred processing after QML initialization.
    void setCliArgs(const QString &execCommand, const QStringList &execArgs,
                    const QString &sessionName);

    // Process stored CLI arguments: creates command sessions or switches
    // to named sessions. Called from QML after restoreSessions().
    Q_INVOKABLE void processCliArgs();

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
    void dbusRegisteredChanged();
    // Aggregated notification signal — emitted for any session, not just the active one.
    // QML connects once to this instead of per-view.
    void desktopNotification(int sessionId, const QString &summary, const QString &body);
    void clipboardReadRequest(int sessionId, const QString &kind);
    void clipboardTextReady(const QString &text);
    void sortOrderChanged();

private:
    static int sessionCountCallback(QQmlListProperty<TerminalView> *prop);
    static TerminalView* sessionAtCallback(QQmlListProperty<TerminalView> *prop, int index);
    static QString socketPath();

    void saveSessions();
    void scheduleSave();
    void rebuildSortedIndices();

    // Connect a view's session-routed signals (notifications, clipboard) to
    // this manager's aggregated signals. Called from both create and restore
    // paths to keep the wiring in one place.
    void connectSessionSignals(TerminalView *view, int sessionId);
    int findSessionByName(const QString &name) const;  // Returns m_sessions index, or -1

    // Scrollback persistence
    void saveScrollback();
    void cleanupScrollbackFiles();
    QString scrollbackDir() const;
    QString scrollbackFilePath(int sessionId) const;

private Q_SLOTS:
    void onNewInstanceConnection();

private:
    QVector<SessionInfo> m_sessions;
    QVector<int> m_sortedIndices; // display index → actual m_sessions index
    int m_activeSessionIndex = -1;
    int m_nextSessionId = 1;

    // Session persistence
    Settings *m_settings = nullptr;
    QTimer *m_saveTimer = nullptr;
    bool m_sessionsLoaded = false;
    bool m_savedOnQuit = false;
    bool m_dbusRegistered = false;

    // Single-instance socket server
    QLocalServer *m_localServer = nullptr;

    // Scrollback encryption (Sailfish Secrets + Crypto)
    ScrollEncryptor *m_encryptor = nullptr;

    // CLI arguments (set from main(), processed by processCliArgs() from QML)
    QString m_cliExecCommand;
    QStringList m_cliExecArgs;
    QString m_cliSessionName;
};

#endif // SESSIONMANAGER_H
