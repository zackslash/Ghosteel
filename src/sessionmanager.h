#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QObject>
#include <QQmlListProperty>
#include <QVector>
#include <QTimer>
#include <QLocalServer>
#include <QByteArray>
#include <QStringList>
#include <QPointer>

class TerminalView;
class ScrollEncryptor;
class Settings;

// Session scrollback lifecycle (restore → dirty → save):
//   1. restoreSessions() creates each view, sets justRestored=true.
//      Geometry-update repaints fire contentChanged immediately, but
//      the handler no-ops while justRestored is true (avoids re-encrypting
//      just-restored scrollback on launch).
//   2. First real PTY byte arrives → titleChanged fires synchronously
//      (inside vtWrite, before update() emits contentChanged) → clears
//      justRestored. Subsequent contentChanged marks scrollbackDirty and
//      schedules a debounced save.
//   3. Debounce timer (500ms) or aboutToQuit → saveScrollbackIncremental()
//      encrypts only dirty sessions, active session first.
//   4. If encryption was unavailable at restore time, the file is queued
//      in m_pendingScrollbackRestores and retried once when
//      ScrollEncryptor::availabilityChanged fires.

// Session taxonomy (two orthogonal dimensions):
//
//                    No command (execArgs empty)   Command (execArgs set)
//  No name           Regular shell session         Anonymous command session
//  Named             Named shell session           Named command session
//
// Auto-remove: exit 0 → anonymous only; exit ≠ 0 → all command sessions.
// restartShell() clears execArgs → cancels pending auto-remove.
struct SessionInfo {
    int id;
    QString name;
    QString cachedWorkingDirectory; // Persisted CWD for inactive sessions
    QString autorunCommand;  // Command to run when session starts
    bool keybarOpen = true;           // Whether the extra keys panel is open
    bool keyboardVisible = true;      // Whether the software keyboard is visible
    int fontSize = 0;                 // Per-session font size (0 = use global default)
    QString execCommand;              // Command binary name from -e (display only)
    QStringList execArgs;             // Full command args including binary (for reuse matching)
    qint64 createdAt = 0;             // Epoch ms when session was created
    qint64 lastUsedAt = 0;            // Epoch ms when session was last switched to
    TerminalView *view;

    bool isAnonymous() const { return !execArgs.isEmpty() && name.isEmpty(); }
    bool isCommandSession() const { return !execArgs.isEmpty(); }
    bool scrollbackDirty = false;  // True if scrollback changed since last encrypt+save
    bool justRestored = false;     // True after restoreSessions(); skip dirty-marking until PTY data arrives
};

class SessionManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int activeSessionIndex READ activeSessionIndex WRITE setActiveSessionIndex NOTIFY activeSessionIndexChanged)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY sessionCountChanged)
    Q_PROPERTY(QQmlListProperty<TerminalView> sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(bool dbusRegistered READ dbusRegistered NOTIFY dbusRegisteredChanged)
    Q_PROPERTY(int activeSessionFontSize READ activeSessionFontSize NOTIFY activeSessionFontSizeChanged)

public:
    explicit SessionManager(QObject *parent = nullptr);
    explicit SessionManager(Settings *settings, QObject *parent = nullptr);
    explicit SessionManager(const QString &settingsPath, QObject *parent = nullptr);
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

    Q_INVOKABLE void setActiveSessionFontSize(int size, bool updateGlobal = true);
    int activeSessionFontSize() const;
    Q_INVOKABLE void resetAllSessionFontSizes();
    Q_INVOKABLE QString sessionDisplayName(int index) const;

    // Session ordering — maps display index (sorted) to actual m_sessions index
    Q_INVOKABLE int displayToActual(int displayIndex) const;
    Q_INVOKABLE int actualToDisplay(int actualIndex) const;
    Q_INVOKABLE int sortMode() const;
    Q_INVOKABLE void setSortMode(int mode);

    // Force any pending debounced sort-rebuild to fire now. Call when the
    // session list becomes visible so the user never sees a stale order.
    Q_INVOKABLE void flushSortRebuild();

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

    // Called from QML after restoreSessions() to process deferred CLI args.
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
    void activeSessionFontSizeChanged();
    // Aggregated notification signal — emitted for any session, not just the active one.
    // QML connects once to this instead of per-view.
    void desktopNotification(int sessionId, const QString &summary, const QString &body);
    void clipboardReadRequest(int sessionId, const QString &kind);
    void clipboardTextReady(const QString &text);
    void sortOrderChanged();
    void showTerminal(); // Request QML to navigate back to the terminal page
    void showSessionList(); // Request QML to show session picker

private:
    static int sessionCountCallback(QQmlListProperty<TerminalView> *prop);
    static TerminalView* sessionAtCallback(QQmlListProperty<TerminalView> *prop, int index);
    static QString socketPath();

    void saveSessions();
    void scheduleSave();
    void rebuildSortedIndices();
    void scheduleSortRebuild();   // debounced rebuild — see .cpp

    // Connect a view's session-routed signals (notifications, clipboard) to
    // this manager's aggregated signals. Called from both create and restore
    // paths to keep the wiring in one place.
    void connectSessionSignals(TerminalView *view, int sessionId);
    int findSessionByName(const QString &name) const;  // Returns m_sessions index, or -1
    void finishSessionCreation(TerminalView *view, SessionInfo &info);
    static void raiseWindow();
    void clearCliArgs();
    void restoreScrollbackForSession(TerminalView *view, int savedId);
    int resolveActiveSession(int activeId, int legacyActiveIndex) const;

    // Scrollback persistence
    void saveScrollbackIncremental();
    void saveSessionScrollback(SessionInfo &info);
    void cleanupScrollbackFiles();
    QString scrollbackDir() const;
    QString scrollbackFilePath(int sessionId) const;

    // Queued scrollback restores for when encryption becomes available
    struct PendingScrollbackRestore {
        QPointer<TerminalView> view;
        int sessionId;
    };
    QVector<PendingScrollbackRestore> m_pendingScrollbackRestores;

private Q_SLOTS:
    void onNewInstanceConnection();
    void onSortRebuildTimer();

private:
    QVector<SessionInfo> m_sessions;
    QVector<int> m_sortedIndices; // display index → actual m_sessions index
    int m_activeSessionIndex = -1;
    int m_nextSessionId = 1;

    // Session persistence
    Settings *m_settings = nullptr;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_sortRebuildTimer = nullptr;  // debounces MRU rebuild after navigation
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

// IPC protocol for single-instance communication.
struct IpcMessage {
    enum Type { Raise, Switch, Exec } type = Raise;
    QString sessionName;
    QString command;
    QStringList args;

    static constexpr int kMaxSessionNameLength = 128;

    static QString sanitizeSessionName(const QString &name) {
        QString clean = name;
        clean.truncate(kMaxSessionNameLength);
        clean.remove(QChar('\0'));
        clean.remove(QChar('\n'));
        clean.remove(QChar('\r'));
        clean.remove(QChar(':')); // load-bearing: exec: protocol uses : as delimiter
        return clean;
    }

    static IpcMessage parse(const QByteArray &raw) {
        IpcMessage msg;
        QList<QByteArray> parts = raw.split('\0');
        QByteArray header = parts.isEmpty() ? QByteArray() : parts.first();

        if (header == "raise") {
            msg.type = Raise;
        } else if (header.startsWith("switch:")) {
            msg.type = Switch;
            msg.sessionName = sanitizeSessionName(QString::fromUtf8(header.mid(7)));
        } else if (header.startsWith("exec:")) {
            msg.type = Exec;
            QByteArray afterPrefix = header.mid(5);
            int colonPos = afterPrefix.indexOf(':');
            if (colonPos < 0) { msg.type = Raise; return msg; }
            msg.sessionName = sanitizeSessionName(QString::fromUtf8(afterPrefix.left(colonPos)));
            if (colonPos + 1 < afterPrefix.size())
                msg.command = QString::fromUtf8(afterPrefix.mid(colonPos + 1));
            for (int i = 1; i < parts.size(); i++)
                if (!parts[i].isEmpty())
                    msg.args.append(QString::fromUtf8(parts[i]));
        }
        return msg;
    }

    static QByteArray encode(const QString &execCommand, const QStringList &execArgs, const QString &sessionName) {
        if (!execCommand.isEmpty()) {
            QByteArray cmdBytes = execCommand.toUtf8();
            for (const QString &arg : execArgs) {
                cmdBytes.append('\0');
                cmdBytes.append(arg.toUtf8());
            }
            return (QStringLiteral("exec:") + sessionName + QStringLiteral(":")).toUtf8() + cmdBytes + '\n';
        } else if (!sessionName.isEmpty()) {
            return (QStringLiteral("switch:") + sessionName + QStringLiteral("\n")).toUtf8();
        }
        return QByteArrayLiteral("raise\n");
    }
};

#endif // SESSIONMANAGER_H
