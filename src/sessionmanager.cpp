#include "sessionmanager.h"
#include "terminalview.h"
#include "ptymanager.h"
#include "settings.h"
#include "scrollencryptor.h"

#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QLocalSocket>
#include <QWindow>
#include <QGuiApplication>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <algorithm>
#include <unistd.h>

static constexpr int kMaxSessionCount = 100;
static constexpr int kMaxSessionNameLength = 128;

static QString sanitizeSessionName(const QString &name)
{
    QString clean = name;
    clean.truncate(kMaxSessionNameLength);
    clean.remove(QChar('\0'));
    clean.remove(QChar('\n'));
    clean.remove(QChar('\r'));
    clean.remove(QChar(':')); // load-bearing: IPC exec: protocol uses : as delimiter
    return clean;
}

struct IpcMessage {
    enum Type { Raise, Switch, Exec } type;
    QString sessionName;
    QString command;
    QStringList args;

    static IpcMessage parse(const QString &raw) {
        IpcMessage msg;
        if (raw == QStringLiteral("raise")) {
            msg.type = Raise;
        } else if (raw.startsWith(QStringLiteral("switch:"))) {
            msg.type = Switch;
            msg.sessionName = sanitizeSessionName(raw.mid(7));
        } else if (raw.startsWith(QStringLiteral("exec:"))) {
            msg.type = Exec;
            QString rest = raw.mid(5);
            int lastColon = rest.lastIndexOf(':');
            if (lastColon < 0) { msg.type = Raise; return msg; } // malformed
            msg.sessionName = sanitizeSessionName(rest.left(lastColon));
            QByteArray commandBytes = rest.mid(lastColon + 1).toUtf8();
            QList<QByteArray> parts = commandBytes.split('\0');
            if (!parts.isEmpty() && !parts.first().isEmpty()) {
                msg.command = QString::fromUtf8(parts.first());
                for (int i = 1; i < parts.size(); i++)
                    msg.args.append(QString::fromUtf8(parts[i]));
            }
        }
        return msg;
    }

    static QByteArray encode(const QString &execCommand, const QStringList &execArgs, const QString &sessionName) {
        if (!execCommand.isEmpty()) {
            QByteArray cmdBytes = execCommand.toUtf8();
            for (const QString &arg : execArgs)
                cmdBytes.append('\0' + arg.toUtf8());
            return (QStringLiteral("exec:") + sessionName + QStringLiteral(":")).toUtf8() + cmdBytes + '\n';
        } else if (!sessionName.isEmpty()) {
            return (QStringLiteral("switch:") + sessionName + QStringLiteral("\n")).toUtf8();
        }
        return QByteArrayLiteral("raise\n");
    }
};

// Delay before auto-removing an anonymous -e session on error,
// so the user can see "Command not found" or the exit code.
static constexpr int kCommandExitDisplayDelayMs = 800;

SessionManager::SessionManager(QObject *parent)
    : SessionManager(Settings::instance(), parent)
{}

SessionManager::SessionManager(const QString &settingsPath, QObject *parent)
    : SessionManager(new Settings(settingsPath), parent)
{
    m_settings->setParent(this);
}

SessionManager::SessionManager(Settings *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    // Initialize scrollback encryption (may fail gracefully — callers check isAvailable)
    m_encryptor = new ScrollEncryptor(this);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500); // 500ms debounce — matches Settings class
    connect(m_saveTimer, &QTimer::timeout, this, &SessionManager::saveSessions);

    // Save sessions early on app quit — before QML engine destruction kills
    // the terminal views (and their shells), which would make /proc/<pid>/cwd
    // unreadable.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this]() {
        QElapsedTimer timer;
        timer.start();
        saveSessions();
        qint64 sessionMs = timer.elapsed();
        saveScrollback();
        qint64 totalMs = timer.elapsed();
        if (totalMs > 1000)
            qWarning() << "Ghosteel: Quit save took" << totalMs << "ms"
                        << "(sessions:" << sessionMs << "ms, scrollback:"
                        << (totalMs - sessionMs) << "ms)";
        m_savedOnQuit = true;
    });
}

SessionManager::~SessionManager()
{
    // Save final state if aboutToQuit hasn't already done it.
    // In production, aboutToQuit fires first (shells alive, CWD readable).
    // In tests or abnormal paths, this is the fallback (shells may be dead).
    if (m_sessionsLoaded && !m_savedOnQuit)
        saveSessions();

    // Cleanly stop each session's PTY before deleting the view.
    // TerminalView owns PtyManager which owns PtyReaderThread.
    // We must ensure threads are stopped before QObject tree destruction
    // races with signal delivery.
    for (auto &info : m_sessions) {
        if (info.view) {
            info.view->cleanup();
            delete info.view;
            info.view = nullptr;
        }
    }
    m_sessions.clear();
}

void SessionManager::setActiveSessionIndex(int index)
{
    if (index < -1 || index >= m_sessions.size())
        return;

    if (m_activeSessionIndex == index)
        return;

    // Update last-used timestamp and rebuild sort order BEFORE emitting
    // signals, so QML bindings that call displayToActual() see the
    // correct mapping when they re-evaluate.
    if (index >= 0 && index < m_sessions.size()) {
        m_sessions[index].lastUsedAt = QDateTime::currentMSecsSinceEpoch();
        rebuildSortedIndices();
    }

    m_activeSessionIndex = index;
    Q_EMIT activeSessionIndexChanged();
    Q_EMIT sessionSwitched(index);

    scheduleSave();
}

QQmlListProperty<TerminalView> SessionManager::sessions()
{
    return QQmlListProperty<TerminalView>(this, nullptr,
                                          &SessionManager::sessionCountCallback,
                                          &SessionManager::sessionAtCallback);
}

int SessionManager::sessionCountCallback(QQmlListProperty<TerminalView> *prop)
{
    SessionManager *manager = static_cast<SessionManager *>(prop->object);
    if (!manager)
        return 0;
    return manager->m_sessions.size();
}

TerminalView* SessionManager::sessionAtCallback(QQmlListProperty<TerminalView> *prop, int index)
{
    SessionManager *manager = static_cast<SessionManager *>(prop->object);
    if (!manager || index < 0 || index >= manager->m_sessions.size())
        return nullptr;
    return manager->m_sessions.at(index).view;
}

void SessionManager::connectSessionSignals(TerminalView *view, int sessionId)
{
    // Route this view's notifications through the aggregated signal
    connect(view, &TerminalView::desktopNotification, this,
            [this, sessionId](const QString &summary, const QString &body) {
        Q_EMIT desktopNotification(sessionId, summary, body);
    });

    // Route clipboard read requests through the aggregated signal
    connect(view, &TerminalView::clipboardReadRequest, this,
            [this, sessionId](const QString &kind) {
        Q_EMIT clipboardReadRequest(sessionId, kind);
    });

    // Route clipboard write results to QML (Clipboard.text singleton)
    connect(view, &TerminalView::clipboardTextReady, this,
            [this](const QString &text) {
        Q_EMIT clipboardTextReady(text);
    });
}

int SessionManager::findSessionByName(const QString &name) const
{
    for (int i = 0; i < m_sessions.size(); i++) {
        if (m_sessions[i].name == name)
            return i;
    }
    return -1;
}

TerminalView* SessionManager::createSession()
{
    // Create a new TerminalView as a child of this manager
    TerminalView *view = new TerminalView();

    SessionInfo info;
    info.id = m_nextSessionId++;
    info.name = tr("Session %1").arg(m_sessions.size() + 1);
    info.cachedWorkingDirectory = QDir::homePath();
    info.createdAt = QDateTime::currentMSecsSinceEpoch();
    info.lastUsedAt = info.createdAt;
    info.view = view;

    m_sessions.append(info);
    finishSessionCreation(view, info);
    return view;
}

TerminalView* SessionManager::createSessionWithCommand(const QString &name, const QStringList &commandArgs)
{
    if (m_sessions.size() >= kMaxSessionCount) {
        qWarning() << "Session limit reached (" << kMaxSessionCount << "), ignoring command";
        return nullptr;
    }

    TerminalView *view = new TerminalView();

    SessionInfo info;
    info.id = m_nextSessionId++;
    info.name = name;
    info.cachedWorkingDirectory = QDir::homePath();
    info.execCommand = commandArgs.isEmpty() ? QString() : commandArgs.first();
    info.execArgs = commandArgs;
    info.createdAt = QDateTime::currentMSecsSinceEpoch();
    info.lastUsedAt = info.createdAt;
    info.view = view;

    view->setCommandArgs(commandArgs);

    m_sessions.append(info);

    // Auto-remove on exit; delay for errors so user sees the message.
    // Skipped if user renames during the delay window (session is no longer anonymous).
    connect(view, &TerminalView::commandExited, this, [this, sessionId = info.id](int exitCode) {
        int delay = (exitCode != 0) ? kCommandExitDisplayDelayMs : 0;
        QTimer::singleShot(delay, this, [this, sessionId]() {
            int idx = sessionIndexById(sessionId);
            if (idx >= 0 && m_sessions[idx].isAnonymous()) {
                removeSession(idx);
            }
        });
    });

    finishSessionCreation(view, info);
    return view;
}

void SessionManager::finishSessionCreation(TerminalView *view, SessionInfo &info)
{
    connectSessionSignals(view, info.id);
    rebuildSortedIndices();
    int index = m_sessions.size() - 1;
    Q_EMIT sessionCountChanged();
    Q_EMIT sessionsChanged();
    Q_EMIT sessionCreated(index);
    setActiveSessionIndex(index);
}

void SessionManager::switchToSessionByName(const QString &name)
{
    int idx = findSessionByName(name);
    if (idx >= 0) {
        setActiveSessionIndex(idx);
    } else {
        createSession();
        int newIdx = m_sessions.size() - 1;
        setSessionName(newIdx, name);
    }
}

void SessionManager::removeSession(int index)
{
    if (index < 0 || index >= m_sessions.size())
        return;

    SessionInfo info = m_sessions.takeAt(index);
    bool wasActive = (index == m_activeSessionIndex);
    bool wasBeforeActive = (index < m_activeSessionIndex);

    // Adjust active session index silently first, then rebuild sort order.
    // Signals are deferred so that QML bindings calling displayToActual()
    // see the correct mapping when they re-evaluate.
    if (m_sessions.isEmpty()) {
        m_activeSessionIndex = -1;
    } else if (wasBeforeActive) {
        // Removed session was before active — shift index down
        m_activeSessionIndex--;
    } else if (wasActive) {
        // Removed the active session — clamp to valid range
        if (m_activeSessionIndex >= m_sessions.size())
            m_activeSessionIndex = m_sessions.size() - 1;
    }

    rebuildSortedIndices();

    // Emit signals after sorted indices are ready.
    // activeSessionIndexChanged must precede sessionSwitched.
    // sessionSwitched must precede sessionRemoved so that the view
    // is still alive when handlers react to the switch.
    if (wasActive || wasBeforeActive || m_sessions.isEmpty())
        Q_EMIT activeSessionIndexChanged();

    if (wasActive)
        Q_EMIT sessionSwitched(m_activeSessionIndex);

    Q_EMIT sessionCountChanged();
    Q_EMIT sessionsChanged();
    Q_EMIT sessionRemoved(index, info.id);

    // Clean up and delete the view AFTER all signals have been emitted,
    // so handlers that reference the old view (e.g. to disconnect
    // signals) can still safely access it.
    if (info.view) {
        info.view->cleanup();
        delete info.view;
    }

    // Delete scrollback file for removed session (regardless of persistence
    // toggle — if the session is gone, the file has no reason to exist)
    QFile::remove(scrollbackFilePath(info.id));

    scheduleSave();
}

// QML convenience wrapper — setActiveSessionIndex is a Q_PROPERTY setter,
// not Q_INVOKABLE, so QML cannot call it by name.  C++ code should call
// setActiveSessionIndex() directly.
void SessionManager::switchToSession(int displayIndex)
{
    int actual = displayToActual(displayIndex);
    if (actual >= 0)
        setActiveSessionIndex(actual);
}

TerminalView* SessionManager::activeSession() const
{
    if (m_activeSessionIndex < 0 || m_activeSessionIndex >= m_sessions.size())
        return nullptr;
    return m_sessions.at(m_activeSessionIndex).view;
}

TerminalView* SessionManager::sessionById(int sessionId) const
{
    int idx = sessionIndexById(sessionId);
    if (idx < 0 || idx >= m_sessions.size())
        return nullptr;
    return m_sessions.at(idx).view;
}

QString SessionManager::sessionName(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return QString();
    return m_sessions.at(index).name;
}

void SessionManager::setSessionName(int index, const QString &name)
{
    if (index < 0 || index >= m_sessions.size())
        return;

    if (m_sessions[index].name == name)
        return;

    m_sessions[index].name = name;

    // Alphabetical sort depends on name — rebuild before emitting so that
    // QML bindings see the correct displayToActual() mapping.
    rebuildSortedIndices();
    Q_EMIT sessionNameChanged(index);

    scheduleSave();
}

int SessionManager::sessionId(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return -1;
    return m_sessions.at(index).id;
}

QString SessionManager::sessionWorkingDirectory(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return QString();

    // Try live CWD from shell process first
    const SessionInfo &info = m_sessions.at(index);
    if (info.view) {
        QString live = info.view->workingDirectory();
        if (!live.isEmpty())
            return live;
    }

    // Fall back to cached value from last save
    return info.cachedWorkingDirectory;
}

QString SessionManager::sessionAutorunCommand(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return QString();
    return m_sessions.at(index).autorunCommand;
}

QString SessionManager::sessionExecCommand(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return QString();
    return m_sessions.at(index).execCommand;
}

void SessionManager::setSessionAutorunCommand(int index, const QString &cmd)
{
    if (index < 0 || index >= m_sessions.size())
        return;

    if (m_sessions[index].autorunCommand == cmd)
        return;

    m_sessions[index].autorunCommand = cmd;
    Q_EMIT sessionAutorunCommandChanged(index);

    scheduleSave();
}

bool SessionManager::sessionKeybarOpen(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return true;
    return m_sessions[index].keybarOpen;
}

void SessionManager::setSessionKeybarOpen(int index, bool open)
{
    if (index < 0 || index >= m_sessions.size())
        return;
    if (m_sessions[index].keybarOpen == open)
        return;
    m_sessions[index].keybarOpen = open;
    Q_EMIT sessionKeybarOpenChanged(index);
    scheduleSave();
}

bool SessionManager::sessionKeyboardVisible(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return true;
    return m_sessions[index].keyboardVisible;
}

void SessionManager::setSessionKeyboardVisible(int index, bool visible)
{
    if (index < 0 || index >= m_sessions.size())
        return;
    if (m_sessions[index].keyboardVisible == visible)
        return;
    m_sessions[index].keyboardVisible = visible;
    Q_EMIT sessionKeyboardVisibleChanged(index);
    scheduleSave();
}

int SessionManager::displayToActual(int displayIndex) const
{
    if (m_sortedIndices.isEmpty()) {
        // No sorting active — display index == actual index
        if (displayIndex < 0 || displayIndex >= m_sessions.size())
            return -1;
        return displayIndex;
    }
    if (displayIndex < 0 || displayIndex >= m_sortedIndices.size())
        return -1;
    return m_sortedIndices.at(displayIndex);
}

int SessionManager::actualToDisplay(int actualIndex) const
{
    if (m_sortedIndices.isEmpty()) {
        if (actualIndex < 0 || actualIndex >= m_sessions.size())
            return -1;
        return actualIndex;
    }
    for (int i = 0; i < m_sortedIndices.size(); i++) {
        if (m_sortedIndices[i] == actualIndex)
            return i;
    }
    return -1;
}

int SessionManager::sortMode() const
{
    return m_settings->sessionSortMode();
}

void SessionManager::setSortMode(int mode)
{
    m_settings->setSessionSortMode(mode);
    rebuildSortedIndices();
    Q_EMIT sessionsChanged();
    Q_EMIT sortOrderChanged();
}

void SessionManager::rebuildSortedIndices()
{
    if (m_sessions.isEmpty()) {
        m_sortedIndices.clear();
        return;
    }

    m_sortedIndices.resize(m_sessions.size());
    for (int i = 0; i < m_sessions.size(); i++)
        m_sortedIndices[i] = i;

    int mode = m_settings->sessionSortMode();
    if (mode == Settings::SortLastUsed) {
        std::stable_sort(m_sortedIndices.begin(), m_sortedIndices.end(),
                  [this](int a, int b) {
            return m_sessions[a].lastUsedAt > m_sessions[b].lastUsedAt;
        });
    } else if (mode == Settings::SortCreated) {
        std::stable_sort(m_sortedIndices.begin(), m_sortedIndices.end(),
                  [this](int a, int b) {
            return m_sessions[a].createdAt > m_sessions[b].createdAt;
        });
    } else if (mode == Settings::SortAlphabetical) {
        std::stable_sort(m_sortedIndices.begin(), m_sortedIndices.end(),
                  [this](int a, int b) {
            return m_sessions[a].name.toLower() < m_sessions[b].name.toLower();
        });
    }
    // SortManual: identity order from the initialization loop above
}

void SessionManager::removeSessionById(int id)
{
    for (int i = 0; i < m_sessions.size(); i++) {
        if (m_sessions[i].id == id) {
            removeSession(i);
            return;
        }
    }
}

int SessionManager::sessionIndexById(int id) const
{
    for (int i = 0; i < m_sessions.size(); i++) {
        if (m_sessions[i].id == id)
            return i;
    }
    return -1;
}

void SessionManager::setDbusRegistered(bool registered)
{
    if (m_dbusRegistered == registered)
        return;
    m_dbusRegistered = registered;
    Q_EMIT dbusRegisteredChanged();
}

QString SessionManager::socketPath()
{
    // Use XDG_RUNTIME_DIR directly — safe to call before QGuiApplication exists
    const QString runtime = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    if (!runtime.isEmpty())
        return runtime + QStringLiteral("/ghosteel-singleton");
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
           + QStringLiteral("/ghosteel-singleton");
}

bool SessionManager::checkSingleInstance(const QString &execCommand,
                                         const QStringList &execArgs,
                                         const QString &sessionName)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (socket.waitForConnected(500)) {
        QByteArray msg = IpcMessage::encode(execCommand, execArgs, sessionName);
        socket.write(msg);
        socket.waitForBytesWritten(1000);
        socket.disconnectFromServer();
        return true;
    }
    return false;
}

void SessionManager::startSingleInstanceServer()
{
    m_localServer = new QLocalServer(this);

    auto failServer = [this]() {
        delete m_localServer;
        m_localServer = nullptr;
    };

    if (!m_localServer->listen(socketPath())) {
        if (m_localServer->serverError() == QAbstractSocket::AddressInUseError) {
            QLocalSocket probe;
            probe.connectToServer(socketPath());
            if (probe.waitForConnected(200)) {
                qWarning() << "Ghosteel: Another instance detected via socket probe";
                failServer();
                return;
            }
            QLocalServer::removeServer(socketPath());
            if (!m_localServer->listen(socketPath())) {
                qWarning() << "Ghosteel: Single-instance server failed:" << m_localServer->errorString();
                failServer();
                return;
            }
        } else {
            qWarning() << "Ghosteel: Single-instance server failed:" << m_localServer->errorString();
            failServer();
            return;
        }
    }
    connect(m_localServer, &QLocalServer::newConnection,
            this, &SessionManager::onNewInstanceConnection);
}

void SessionManager::setCliArgs(const QString &execCommand,
                                const QStringList &execArgs,
                                const QString &sessionName)
{
    m_cliExecCommand = execCommand;
    m_cliExecArgs = execArgs;
    m_cliSessionName = sanitizeSessionName(sessionName);
}

void SessionManager::clearCliArgs()
{
    m_cliExecCommand.clear();
    m_cliExecArgs.clear();
    m_cliSessionName.clear();
}

void SessionManager::processCliArgs()
{
    if (m_cliExecCommand.isEmpty() && m_cliSessionName.isEmpty())
        return;

    if (!m_cliExecCommand.isEmpty()) {
        QStringList fullArgs;
        fullArgs << m_cliExecCommand << m_cliExecArgs;

        // Named session reuse by name
        if (!m_cliSessionName.isEmpty()) {
            int named = findSessionByName(m_cliSessionName);
            if (named >= 0) {
                setActiveSessionIndex(named);
                clearCliArgs();
                return;
            }
        }

        // Anonymous reuse by full command — skips named sessions
        for (int i = 0; i < m_sessions.size(); i++) {
            if (m_sessions[i].name.isEmpty() && m_sessions[i].execArgs == fullArgs) {
                setActiveSessionIndex(i);
                clearCliArgs();
                return;
            }
        }

        createSessionWithCommand(m_cliSessionName, fullArgs);
    } else if (!m_cliSessionName.isEmpty()) {
        switchToSessionByName(m_cliSessionName);
    }

    clearCliArgs();
}

void SessionManager::raiseWindow()
{
    const auto windows = QGuiApplication::topLevelWindows();
    if (!windows.isEmpty()) {
        if (auto *window = windows.first()) {
            window->raise();
            window->requestActivate();
        }
    }
}

void SessionManager::onNewInstanceConnection()
{
    QLocalSocket *socket = m_localServer->nextPendingConnection();
    if (!socket) return;

    // Race: sender may have written and disconnected before we get here.
    auto processMessage = [this, socket]() {
        QByteArray data = socket->readAll();
        socket->deleteLater();
        QString msg = QString::fromUtf8(data.trimmed());

        IpcMessage parsed = IpcMessage::parse(msg);
        if (parsed.type == IpcMessage::Raise) {
            raiseWindow();
        } else if (parsed.type == IpcMessage::Switch) {
            switchToSessionByName(parsed.sessionName);
            raiseWindow();
        } else if (parsed.type == IpcMessage::Exec) {
            if (parsed.command.isEmpty()) return;
            setCliArgs(parsed.command, parsed.args, parsed.sessionName);
            processCliArgs();
            // Delay raise to let QML process sessionCreated() signal
            QTimer::singleShot(100, this, raiseWindow);
        }
    };

    if (socket->bytesAvailable() > 0 || socket->state() == QLocalSocket::UnconnectedState) {
        // Data already in buffer or socket already closed — process now
        processMessage();
    } else {
        // Wait for data to arrive
        connect(socket, &QLocalSocket::readyRead, this, processMessage);
    }
}

void SessionManager::saveSessions()
{
    QSettings &s = m_settings->raw();

    // Clear old session entries
    s.remove(QStringLiteral("sessionData"));

    // Skip anonymous command sessions during save
    int saveIndex = 0;
    for (int i = 0; i < m_sessions.size(); i++) {
        SessionInfo &info = m_sessions[i];

        if (info.isAnonymous())
            continue;

        QString group = QStringLiteral("sessionData/session_%1").arg(saveIndex);
        s.beginGroup(group);
        s.setValue(QStringLiteral("id"), info.id);
        s.setValue(QStringLiteral("name"), info.name);
        // Use live CWD from /proc if shell is running, otherwise use cached value
        if (info.view) {
            QString liveCwd = info.view->workingDirectory();
            if (!liveCwd.isEmpty())
                info.cachedWorkingDirectory = liveCwd;
        }
        QString cwd = info.cachedWorkingDirectory;
        if (cwd.isEmpty())
            cwd = QDir::homePath();
        s.setValue(QStringLiteral("workingDirectory"), cwd);
        s.setValue(QStringLiteral("autorunCommand"), info.autorunCommand);
        // Read live font size from view before persisting
        if (info.view)
            info.fontSize = info.view->fontSize();
        s.setValue(QStringLiteral("fontSize"), info.fontSize);
        s.setValue(QStringLiteral("keybarOpen"), info.keybarOpen);
        s.setValue(QStringLiteral("keyboardVisible"), info.keyboardVisible);
        s.setValue(QStringLiteral("createdAt"), info.createdAt);
        s.setValue(QStringLiteral("lastUsedAt"), info.lastUsedAt);
        s.endGroup();
        saveIndex++;
    }

    s.beginGroup(QStringLiteral("sessions"));
    s.setValue(QStringLiteral("count"), saveIndex);
    s.setValue(QStringLiteral("nextId"), m_nextSessionId);
    
    int activeSessionId = (m_activeSessionIndex >= 0
                           && m_activeSessionIndex < m_sessions.size())
                          ? m_sessions[m_activeSessionIndex].id : -1;
    s.setValue(QStringLiteral("activeId"), activeSessionId);
    s.endGroup();

    m_settings->save();
}

void SessionManager::scheduleSave()
{
    if (m_sessionsLoaded)
        m_saveTimer->start();
}

QString SessionManager::scrollbackDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/scrollback");
}

QString SessionManager::scrollbackFilePath(int sessionId) const
{
    return scrollbackDir() + QStringLiteral("/session_%1.vt").arg(sessionId);
}

void SessionManager::saveScrollback()
{
    if (!m_settings->scrollbackPersistence())
        return;

    QString dir = scrollbackDir();
    QDir().mkpath(dir);

    for (const SessionInfo &info : m_sessions) {
        if (!info.view)
            continue;

        uint16_t cols = 0, rows = 0;
        QByteArray data = info.view->exportScrollback(cols, rows);
        if (data.isEmpty())
            continue;

        QByteArray output;
        if (m_encryptor && m_encryptor->isAvailable())
            output = m_encryptor->encrypt(data);
        if (output.isEmpty()) {
            qWarning() << "Ghosteel: Scrollback encryption failed for session"
                       << info.id << ", skipping";
            continue; // Don't write plaintext to disk
        }

        // Atomic write via QSaveFile — commit() is a POSIX rename(),
        // which is atomic on the same filesystem. No window where both
        // old and new files are gone.
        QSaveFile saveFile(scrollbackFilePath(info.id));
        if (saveFile.open(QIODevice::WriteOnly)) {
            if (saveFile.write(output) != -1) {
                // fsync before rename to ensure data is on disk —
                // protects against power loss between write and rename
                ::fsync(saveFile.handle());
                if (!saveFile.commit()) {
                    qWarning() << "Failed to commit scrollback:" << saveFile.errorString();
                }
            } else {
                qWarning() << "Failed to write scrollback:" << saveFile.errorString();
            }
        }
    }
}

void SessionManager::cleanupScrollbackFiles()
{
    int retentionDays = m_settings->scrollbackRetentionDays();
    QDir dir(scrollbackDir());
    if (!dir.exists())
        return;

    QDateTime cutoff = QDateTime::currentDateTime().addDays(-retentionDays);
    const QFileInfoList files = dir.entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files) {
        if (fi.fileName().startsWith(QStringLiteral("session_"))) {
            if (fi.fileName().endsWith(QStringLiteral(".vt"))) {
                if (fi.lastModified() < cutoff)
                    QFile::remove(fi.absoluteFilePath());
            } else {
                // Orphaned QSaveFile temp file from a crash during write.
                // These have random suffixes (e.g. session_1.vt.aBcDeF).
                // Safe to remove — they're never read on restore.
                QFile::remove(fi.absoluteFilePath());
            }
        }
    }
}

void SessionManager::setActiveSessionFontSize(int size)
{
    if (m_activeSessionIndex < 0 || m_activeSessionIndex >= m_sessions.size())
        return;
    size = qBound(6, size, 32);  // Clamp before storing — keeps INI clean
    SessionInfo &info = m_sessions[m_activeSessionIndex];
    info.fontSize = size;
    if (info.view)
        info.view->setFontSize(size);
    scheduleSave();
}

int SessionManager::activeSessionFontSize() const
{
    if (m_activeSessionIndex < 0 || m_activeSessionIndex >= m_sessions.size())
        return 0;
    return m_sessions[m_activeSessionIndex].fontSize;
}

QString SessionManager::sessionDisplayName(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return QString();
    const SessionInfo &info = m_sessions.at(index);
    if (!info.name.isEmpty())
        return info.name;
    if (!info.execCommand.isEmpty())
        return info.execCommand;
    return tr("Session %1").arg(index + 1);
}

void SessionManager::restoreScrollbackForSession(TerminalView *view, int savedId)
{
    if (!m_settings->scrollbackPersistence())
        return;

    QString sbPath = scrollbackFilePath(savedId);
    QFile sbFile(sbPath);
    if (!sbFile.exists() || !sbFile.open(QIODevice::ReadOnly))
        return;

    if (sbFile.size() > 4 * 1024 * 1024) {
        qWarning() << "Scrollback file too large, skipping:" << sbPath;
        return;
    }

    QByteArray sbData = sbFile.readAll();
    if (sbData.isEmpty())
        return;

    if (ScrollEncryptor::isEncryptedFormat(sbData) && m_encryptor && m_encryptor->isAvailable()) {
        QByteArray restored = m_encryptor->decrypt(sbData);
        if (!restored.isEmpty())
            view->setPendingScrollback(restored);
    } else if (ScrollEncryptor::isEncryptedFormat(sbData)) {
        qWarning() << "Encrypted scrollback found but secrets daemon unavailable, skipping:" << sbPath;
    }
}

int SessionManager::resolveActiveSession(int activeId, int legacyActiveIndex) const
{
    if (activeId >= 0) {
        for (int i = 0; i < m_sessions.size(); i++) {
            if (m_sessions[i].id == activeId)
                return i;
        }
    } else if (legacyActiveIndex >= 0) {
        if (legacyActiveIndex < m_sessions.size())
            return legacyActiveIndex;
    }
    return -1;
}

bool SessionManager::restoreSessions()
{
    QSettings &s = m_settings->raw();
    s.beginGroup(QStringLiteral("sessions"));
    int count = s.value(QStringLiteral("count"), 0).toInt();
    int nextId = s.value(QStringLiteral("nextId"), 1).toInt();
    // activeId (new) with legacy activeIndex fallback
    int activeId = s.value(QStringLiteral("activeId"), -1).toInt();
    int legacyActiveIndex = -1;
    if (activeId < 0) {
        legacyActiveIndex = s.value(QStringLiteral("activeIndex"), 0).toInt();
    }
    s.endGroup();

    if (count <= 0) {
        m_sessionsLoaded = true;
        return false;
    }

    // Sanity cap to protect against corrupted settings
    if (count > kMaxSessionCount)
        count = kMaxSessionCount;

    m_nextSessionId = nextId;

    cleanupScrollbackFiles();

    for (int i = 0; i < count; i++) {
        QString group = QStringLiteral("sessionData/session_%1").arg(i);
        s.beginGroup(group);
        int savedId = s.value(QStringLiteral("id"), m_nextSessionId).toInt();
        QString name = s.value(QStringLiteral("name"),
                                        tr("Session %1").arg(i + 1)).toString();
        QString workingDir = s.value(QStringLiteral("workingDirectory"),
                                              QDir::homePath()).toString();
        QString autorun = s.value(QStringLiteral("autorunCommand"), QString()).toString();
        int fontSize = s.value(QStringLiteral("fontSize"), 0).toInt();
        bool keybarOpen = s.value(QStringLiteral("keybarOpen"), true).toBool();
        bool keyboardVisible = s.value(QStringLiteral("keyboardVisible"), true).toBool();
        qint64 createdAt = s.value(QStringLiteral("createdAt"), 0).toLongLong();
        qint64 lastUsedAt = s.value(QStringLiteral("lastUsedAt"), 0).toLongLong();
        s.endGroup();

        // Validate working directory exists, fallback to home
        if (!QDir(workingDir).exists())
            workingDir = QDir::homePath();

        // Create session with restored settings
        TerminalView *view = new TerminalView();
        view->setWorkingDirectory(workingDir);
        if (!autorun.isEmpty())
            view->setAutorunCommand(autorun);

        restoreScrollbackForSession(view, savedId);

        SessionInfo info;
        info.id = savedId;
        info.name = name;
        info.cachedWorkingDirectory = workingDir;
        info.autorunCommand = autorun;
        info.fontSize = fontSize;
        info.keybarOpen = keybarOpen;
        info.keyboardVisible = keyboardVisible;
        info.createdAt = createdAt;
        info.lastUsedAt = lastUsedAt;
        info.view = view;

        m_sessions.append(info);

        // Route this view's session-routed signals through the aggregated signals
        connectSessionSignals(view, info.id);

        // Ensure nextSessionId stays ahead of any restored ID
        if (savedId >= m_nextSessionId)
            m_nextSessionId = savedId + 1;

        Q_EMIT sessionCountChanged();
        Q_EMIT sessionsChanged();
    }

    // Restore active session by ID (or by legacy index)
    int resolvedActive = resolveActiveSession(activeId, legacyActiveIndex);
    if (resolvedActive >= 0)
        setActiveSessionIndex(resolvedActive);
    else if (!m_sessions.isEmpty())
        setActiveSessionIndex(0);

    m_sessionsLoaded = true;
    rebuildSortedIndices();
    Q_EMIT sessionsRestored();
    return true;
}
