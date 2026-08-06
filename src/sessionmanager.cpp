#include "sessionmanager.h"
#include "sessionstore.h"
#include "terminalview.h"
#include "ptymanager.h"
#include "settings.h"
#include "scrollencryptor.h"

#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <algorithm>
#include <QGuiApplication>
#include <QClipboard>

static constexpr int kMaxSessionCount = 100;

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
    m_store = std::make_unique<SessionStore>(m_settings, m_encryptor);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500); // 500ms debounce — matches Settings class
    connect(m_saveTimer, &QTimer::timeout, this, [this]() {
        // Only rewrite the settings file when session metadata actually
        // changed — a scrollback-only arm (scheduleScrollbackSave) leaves
        // the flag false so continuous output doesn't fsync settings ~2x/sec.
        if (m_sessionsDirty) {
            m_store->saveSessionsMetadata(m_sessions, m_activeSessionIndex, m_nextSessionId);
            m_sessionsDirty = false;
        }
        // Also encrypt scrollback incrementally for sessions whose content
        // changed since the last save. By the time aboutToQuit fires, most
        // sessions are already on disk — only a few remain dirty.
        if (m_store->saveScrollbackIncremental(m_sessions, m_activeSessionIndex))
            scheduleScrollbackSave();
    });

    // Retry pending scrollback restores when encryption becomes available.
    // The handler may run synchronously from initializeNow() below — m_saveTimer
    // is created above so scheduleScrollbackSave() is safe to call from it.
    connect(m_encryptor, &ScrollEncryptor::availabilityChanged, this, [this]() {
        if (!m_encryptor->isAvailable()) {
            // Encryption init failed — these scrollback files can't be
            // restored; drop the pending list to avoid re-queuing.
            if (!m_pendingScrollbackRestores.isEmpty())
                qWarning() << "Ghosteel: Encryption unavailable, skipping"
                           << m_pendingScrollbackRestores.size()
                           << "pending scrollback restores";
            m_pendingScrollbackRestores.clear();
            return;
        }
        const auto pending = m_pendingScrollbackRestores;
        m_pendingScrollbackRestores.clear();
        for (const auto &p : pending) {
            if (p.view)  // QPointer returns null if the object was deleted
                m_store->restoreScrollbackForSession(p.view, p.sessionId, m_pendingScrollbackRestores);
        }
        // A scrollback save may have failed while encryption was unavailable,
        // leaving sessions dirty with no timer armed (the failure path doesn't
        // re-arm). Retry once encryption is ready.
        if (m_settings->scrollbackPersistence()) {
            bool anyDirty = false;
            for (const SessionInfo &info : m_sessions) {
                if (info.view && info.scrollbackDirty) {
                    anyDirty = true;
                    break;
                }
            }
            if (anyDirty)
                scheduleScrollbackSave();
        }
    });

    // Without synchronous init, restoreSessions() runs before the event loop
    // fires the deferred singleShot, so decrypting saved scrollback slips to
    // availabilityChanged — which fires after setupTerminal() has consumed
    // the (empty) pending buffer, silently dropping the restored content.
    if (m_settings->scrollbackPersistence())
        m_encryptor->initializeNow();

    // When the global default font size changes, propagate it to every session
    // that is tracking the default (fontSize == 0). Sessions with an explicit
    // override are left alone.
    connect(m_settings, &Settings::fontSizeChanged, this, [this]() {
        int defaultSize = m_settings->fontSize();
        for (SessionInfo &info : m_sessions) {
            if (info.fontSize == 0 && info.view)
                info.view->setFontSize(defaultSize);
        }
    });

    // Enabling scrollback persistence mid-session must re-arm the save timer:
    // while the setting was off, content left scrollbackDirty true, but the
    // save path returns early without arming the timer. Without this, that
    // content only flushes at aboutToQuit — and is lost if the app is killed first.
    connect(m_settings, &Settings::scrollbackPersistenceChanged, this, [this]() {
        if (!m_settings->scrollbackPersistence()) {
            // Purge all scrollback files immediately. Not gated on m_sessionsLoaded:
            // files exist on disk regardless of session state (privacy expectation).
            m_store->cleanupScrollbackFiles(true);
            return;
        }
        if (!m_sessionsLoaded)
            return;
        // Enabled — re-arm dirty sessions + schedule a save.
        bool any = false;
        for (SessionInfo &info : m_sessions) {
            if (info.view) {
                info.scrollbackDirty = true;
                any = true;
            }
        }
        if (any)
            scheduleScrollbackSave();
    });

    connect(m_settings, &Settings::scrollbackRetentionDaysChanged, this, [this]() {
        // Reducing retention should purge now-overage files immediately.
        // No-op when persistence is off (the disable handler already purged everything).
        if (!m_settings->scrollbackPersistence())
            return;
        m_store->cleanupScrollbackFiles();  // mtime-gated
    });

    // Save sessions early on app quit — before QML engine destruction kills
    // the terminal views (and their shells), which would make /proc/<pid>/cwd
    // unreadable.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this]() {
        QElapsedTimer timer;
        timer.start();
        m_store->saveSessionsMetadata(m_sessions, m_activeSessionIndex, m_nextSessionId);
        qint64 sessionMs = timer.elapsed();
        // Catch sessions the debounce timer hasn't fired for yet; force=true
        // bypasses the 5s throttle so nothing is lost on shutdown.
        m_store->saveScrollbackIncremental(m_sessions, m_activeSessionIndex, true);
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
    if (m_sessionsLoaded && !m_savedOnQuit) {
        m_store->saveSessionsMetadata(m_sessions, m_activeSessionIndex, m_nextSessionId);
        // Views are still alive here (cleaned up below), so we can still flush
        // the scrollback that aboutToQuit never got to save.
        m_store->saveScrollbackIncremental(m_sessions, m_activeSessionIndex, true);
    }

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

QString SessionManager::clipboardText() const
{
    return QGuiApplication::clipboard()->text(QClipboard::Clipboard);
}

void SessionManager::setClipboardText(const QString &text)
{
    QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
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

    // Route clipboard write results to QML (SessionManager.setClipboardText)
    connect(view, &TerminalView::clipboardTextReady, this,
            [this](const QString &text) {
        Q_EMIT clipboardTextReady(text);
    });

    // When a command session's shell is restarted (user taps after exit),
    // clear execArgs so isCommandSession() returns false and auto-remove skips it.
    connect(view, &TerminalView::shellRestarted, this,
        [this, sessionId]() {
        int idx = sessionIndexById(sessionId);
        if (idx >= 0) {
            m_sessions[idx].execArgs.clear();
            m_sessions[idx].execCommand.clear();
        }
    });

    // Track content changes for incremental scrollback encryption.
    // Mark dirty on first content change; scheduleScrollbackSave restarts
    // the 500ms debounce timer, which will call saveScrollbackIncremental().
    connect(view, &TerminalView::contentChanged, this,
        [this, sessionId]() {
        int idx = sessionIndexById(sessionId);
        if (idx < 0)
            return;
        // Skip geometry-update repaints that fire immediately after
        // restoreSessions(). Cleared by titleChanged or ptyDataReceived
        // (whichever fires first).
        if (m_sessions[idx].justRestored)
            return;
        if (!m_sessions[idx].scrollbackDirty) {
            m_sessions[idx].scrollbackDirty = true;
            scheduleScrollbackSave();
        }
    });

    // justRestored must be cleared before the first data-driven contentChanged
    // runs, or that handler will keep skipping saves. titleChanged (shells that
    // set a title) fires synchronously from onPtyData → vtWrite, before the
    // update() that emits contentChanged; ptyDataReceived fires earlier still
    // (before vtWrite) and also covers title-less shells like sh/dash. Hence
    // two connections — whichever fires first wins.
    connect(view, &TerminalView::titleChanged, this,
        [this, sessionId]() {
        int idx = sessionIndexById(sessionId);
        if (idx >= 0)
            m_sessions[idx].justRestored = false;
    });
    connect(view, &TerminalView::ptyDataReceived, this,
        [this, sessionId]() {
        int idx = sessionIndexById(sessionId);
        if (idx >= 0)
            m_sessions[idx].justRestored = false;
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
    if (m_sessions.size() >= kMaxSessionCount) {
        qWarning() << "Session limit reached (" << kMaxSessionCount << "), ignoring new session";
        return nullptr;
    }

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
    // Success: only remove anonymous sessions (command finished normally).
    // Error: remove all command sessions — the command couldn't run.
    // Skipped if user taps terminal during delay (restartShell clears execArgs).
    connect(view, &TerminalView::commandExited, this, [this, sessionId = info.id](int exitCode) {
        int delay = (exitCode != 0) ? kCommandExitDisplayDelayMs : 0;
        QTimer::singleShot(delay, this, [this, sessionId, exitCode]() {
            int idx = sessionIndexById(sessionId);
            if (idx < 0) return;
            bool shouldRemove = (exitCode == 0) ? m_sessions[idx].isAnonymous()
                                                : m_sessions[idx].isCommandSession();
            if (shouldRemove) {
                bool wasActive = (idx == m_activeSessionIndex);
                removeSession(idx);
                if (wasActive && !m_sessions.isEmpty())
                    Q_EMIT showSessionList();
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
        // createSession() returns nullptr at the session cap; only rename when
        // a session was actually created, otherwise we'd rename an existing one.
        if (createSession()) {
            int newIdx = m_sessions.size() - 1;
            setSessionName(newIdx, name);
        }
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
        // Active session removed — refined after rebuildSortedIndices() below.
        m_activeSessionIndex = qBound(0, m_activeSessionIndex, m_sessions.size() - 1);
    }

    rebuildSortedIndices();

    // When the active session was removed, pick the first session in
    // the current sort order rather than blindly clamping the raw index.
    // For "last used" sort, this selects the most recently used session.
    if (wasActive && !m_sortedIndices.isEmpty())
        m_activeSessionIndex = m_sortedIndices[0];

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
    QFile::remove(m_store->scrollbackFilePath(info.id));

    scheduleSave();

    // If we just removed the last session, create a fresh shell so the
    // user isn't staring at a blank screen with no way to interact.
    if (m_sessions.isEmpty()) {
        createSession();
    }
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

void SessionManager::processCliArgs()
{
    if (m_cliExecCommand.isEmpty() && m_cliSessionName.isEmpty())
        return;

    bool didSomething = false;

    if (!m_cliExecCommand.isEmpty()) {
        QStringList fullArgs;
        fullArgs << m_cliExecCommand << m_cliExecArgs;

        if (!m_cliSessionName.isEmpty()) {
            int named = findSessionByName(m_cliSessionName);
            if (named >= 0) {
                if (m_sessions[named].isCommandSession() && !m_sessions[named].view->shellExited()) {
                    // Command still running — switch to it
                    setActiveSessionIndex(named);
                } else {
                    // Command exited or session is plain shell — replace with new session.
                    // Create first so removeSession never hits the empty-list fallback.
                    // Only remove the old one if the replacement was created: at the session
                    // cap createSessionWithCommand returns null.
                    if (createSessionWithCommand(m_cliSessionName, fullArgs))
                        removeSession(named);
                }
                didSomething = true;
            }
        }

        if (!didSomething) {
            for (int i = 0; i < m_sessions.size(); i++) {
                if (m_sessions[i].name.isEmpty() && m_sessions[i].execArgs == fullArgs) {
                    setActiveSessionIndex(i);
                    didSomething = true;
                    break;
                }
            }
        }

        if (!didSomething) {
            // At the session cap this returns null and the command won't run; leave
            // didSomething false rather than claiming success.
            if (createSessionWithCommand(m_cliSessionName, fullArgs))
                didSomething = true;
        }
    } else if (!m_cliSessionName.isEmpty()) {
        switchToSessionByName(m_cliSessionName);
        didSomething = true;
    }

    clearCliArgs();
    if (didSomething)
        Q_EMIT showTerminal();
}

void SessionManager::scheduleSave()
{
    // Session metadata changed — ensure the timer fires to flush via m_store->saveSessionsMetadata().
    m_sessionsDirty = true;
    if (m_sessionsLoaded)
        m_saveTimer->start();
}

void SessionManager::scheduleScrollbackSave()
{
    // Scrollback-only change — arm the timer for saveScrollbackIncremental()
    // without forcing a settings rewrite (avoids fsync thrash under output).
    if (m_sessionsLoaded)
        m_saveTimer->start();
}

void SessionManager::setActiveSessionFontSize(int size, bool updateGlobal)
{
    if (m_activeSessionIndex < 0 || m_activeSessionIndex >= m_sessions.size())
        return;
    SessionInfo &info = m_sessions[m_activeSessionIndex];

    if (size == 0) {
        // Reset to track global default — updateGlobal is N/A here:
        // resetting to "track default" must never modify the global itself.
        if (info.fontSize == 0)
            return;
        info.fontSize = 0;
        if (info.view)
            info.view->setFontSize(m_settings->fontSize());
        Q_EMIT activeSessionFontSizeChanged();
        scheduleSave();
        return;
    }

    size = qBound(Settings::kMinFontSize, size, Settings::kMaxFontSize);
    if (info.fontSize == size) {
        // Value unchanged, but still sync global if requested
        if (updateGlobal)
            m_settings->setFontSize(size);
        return;
    }
    info.fontSize = size;
    if (info.view)
        info.view->setFontSize(size);
    if (updateGlobal)
        m_settings->setFontSize(size);
    Q_EMIT activeSessionFontSizeChanged();
    scheduleSave();
}

int SessionManager::activeSessionFontSize() const
{
    if (m_activeSessionIndex < 0 || m_activeSessionIndex >= m_sessions.size())
        return 0;
    return m_sessions[m_activeSessionIndex].fontSize;
}

void SessionManager::resetAllSessionFontSizes()
{
    int defaultSize = m_settings->fontSize();
    bool changed = false;
    for (SessionInfo &info : m_sessions) {
        if (info.fontSize != 0) {
            info.fontSize = 0;
            if (info.view)
                info.view->setFontSize(defaultSize);
            changed = true;
        }
    }
    if (changed) {
        Q_EMIT activeSessionFontSizeChanged();
        scheduleSave();
    }
}

QString SessionManager::sessionDisplayName(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return QString();
    const SessionInfo &info = m_sessions.at(index);
    if (!info.name.isEmpty())
        return info.name;
    if (!info.execArgs.isEmpty())
        return info.execArgs.join(QLatin1Char(' '));
    if (!info.execCommand.isEmpty())
        return info.execCommand;
    return tr("Session %1").arg(index + 1);
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

    m_store->cleanupScrollbackFiles();

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
        // Apply persisted font size immediately so the save path reads back the correct value
        // the correct value for non-active sessions (not the stale default 18).
        if (fontSize > 0)
            view->setFontSize(fontSize);
        else
            view->setFontSize(m_settings->fontSize());
        if (!autorun.isEmpty())
            view->setAutorunCommand(autorun);

        m_store->restoreScrollbackForSession(view, savedId, m_pendingScrollbackRestores);

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

        // Mark as just-restored so the contentChanged handler skips
        // geometry-update repaints; cleared by titleChanged (real PTY data).
        m_sessions.last().justRestored = true;

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

    // Reset the metadata-dirty flag: restore's trailing setActiveSessionIndex
    // sets it via scheduleSave, but the just-loaded state is already on disk,
    // so the next scrollback-only arm must not trigger a redundant metadata save.
    m_sessionsDirty = false;
    m_sessionsLoaded = true;
    rebuildSortedIndices();
    Q_EMIT sessionsRestored();
    return true;
}
