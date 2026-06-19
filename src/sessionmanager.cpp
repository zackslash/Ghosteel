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
#include <QDateTime>
#include <QElapsedTimer>
#include <algorithm>

#include <unistd.h>

SessionManager::SessionManager(QObject *parent)
    : SessionManager(
          QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
          + QStringLiteral("/" APP_ORG "/" APP_NAME ".conf"),
          parent)
{}

SessionManager::SessionManager(const QString &settingsPath, QObject *parent)
    : QObject(parent)
    , m_settings(settingsPath, QSettings::IniFormat)
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

TerminalView* SessionManager::createSession()
{
    // Create a new TerminalView as a child of this manager
    TerminalView *view = new TerminalView();

    SessionInfo info;
    info.id = m_nextSessionId++;
    info.name = tr("Session %1").arg(m_sessions.size() + 1);
    info.cachedWorkingDirectory = QDir::homePath();
    info.autorunCommand = QString();
    info.keybarOpen = true;
    info.keyboardVisible = true;
    info.createdAt = QDateTime::currentMSecsSinceEpoch();
    info.lastUsedAt = info.createdAt;
    info.view = view;

    int index = m_sessions.size();
    m_sessions.append(info);

    // Route this view's session-routed signals through the aggregated signals
    connectSessionSignals(view, info.id);

    // Rebuild sorted indices BEFORE emitting signals so that QML bindings
    // (Repeater delegates calling displayToActual()) see valid mappings.
    rebuildSortedIndices();

    Q_EMIT sessionCountChanged();
    Q_EMIT sessionsChanged();
    Q_EMIT sessionCreated(index);

    // Auto-switch to the new session
    setActiveSessionIndex(index);

    return view;
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
    return Settings::instance()->sessionSortMode();
}

void SessionManager::setSortMode(int mode)
{
    Settings::instance()->setSessionSortMode(mode);
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

    int mode = Settings::instance()->sessionSortMode();

    // Manual mode: keep insertion order
    if (mode == Settings::SortManual) {
        m_sortedIndices.resize(m_sessions.size());
        for (int i = 0; i < m_sessions.size(); i++)
            m_sortedIndices[i] = i;
        return;
    }

    // Build index list and sort
    m_sortedIndices.resize(m_sessions.size());
    for (int i = 0; i < m_sessions.size(); i++)
        m_sortedIndices[i] = i;

    if (mode == Settings::SortLastUsed) {
        std::stable_sort(m_sortedIndices.begin(), m_sortedIndices.end(),
                  [this](int a, int b) {
            return m_sessions[a].lastUsedAt > m_sessions[b].lastUsedAt; // most recent first
        });
    } else if (mode == Settings::SortCreated) {
        std::stable_sort(m_sortedIndices.begin(), m_sortedIndices.end(),
                  [this](int a, int b) {
            return m_sessions[a].createdAt > m_sessions[b].createdAt; // newest first
        });
    } else if (mode == Settings::SortAlphabetical) {
        std::stable_sort(m_sortedIndices.begin(), m_sortedIndices.end(),
                  [this](int a, int b) {
            return m_sessions[a].name.toLower() < m_sessions[b].name.toLower();
        });
    }
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

bool SessionManager::checkSingleInstance()
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (socket.waitForConnected(500)) {
        socket.write("raise\n");
        socket.waitForBytesWritten(1000);
        socket.disconnectFromServer();
        return true;
    }
    return false;
}

void SessionManager::startSingleInstanceServer()
{
    m_localServer = new QLocalServer(this);
    if (!m_localServer->listen(socketPath())) {
        // AddressInUse — either another instance is running (live socket)
        // or a stale socket file remains from a crash.  Try connecting
        // to distinguish: if the connect succeeds, the other instance is
        // alive (shouldn't happen since checkSingleInstance() already
        // caught it, but handle the race).  If the connect fails, the
        // socket is stale and we can safely remove it.
        if (m_localServer->serverError() == QAbstractSocket::AddressInUseError) {
            QLocalSocket probe;
            probe.connectToServer(socketPath());
            if (probe.waitForConnected(200)) {
                // Live instance exists — this shouldn't happen after
                // checkSingleInstance(), but be safe.
                qWarning() << "Ghosteel: Another instance detected via socket probe";
                delete m_localServer;
                m_localServer = nullptr;
                return;
            }
            // Stale socket — remove and retry
            QLocalServer::removeServer(socketPath());
            if (!m_localServer->listen(socketPath())) {
                qWarning() << "Ghosteel: Single-instance server failed:" << m_localServer->errorString();
                delete m_localServer;
                m_localServer = nullptr;
                return;
            }
        } else {
            qWarning() << "Ghosteel: Single-instance server failed:" << m_localServer->errorString();
            delete m_localServer;
            m_localServer = nullptr;
            return;
        }
    }
    connect(m_localServer, &QLocalServer::newConnection,
            this, &SessionManager::onNewInstanceConnection);
}

void SessionManager::onNewInstanceConnection()
{
    QLocalSocket *socket = m_localServer->nextPendingConnection();
    if (!socket) return;

    // Read data in the disconnected handler — by then all bytes are
    // guaranteed to be in the buffer, avoiding partial-read issues.
    connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
        QByteArray data = socket->readAll();
        if (data.trimmed() == "raise") {
            const auto windows = QGuiApplication::topLevelWindows();
            if (!windows.isEmpty()) {
                if (auto *window = windows.first())
                    window->requestActivate();
            }
        }
        socket->deleteLater();
    });
}

void SessionManager::saveSessions()
{
    m_settings.beginGroup(QStringLiteral("sessions"));
    m_settings.setValue(QStringLiteral("count"), m_sessions.size());
    m_settings.setValue(QStringLiteral("nextId"), m_nextSessionId);
    m_settings.setValue(QStringLiteral("activeIndex"), m_activeSessionIndex);
    m_settings.endGroup();

    // Clear old session entries
    m_settings.remove(QStringLiteral("sessionData"));

    // Save each session by index
    for (int i = 0; i < m_sessions.size(); i++) {
        SessionInfo &info = m_sessions[i];
        QString group = QStringLiteral("sessionData/session_%1").arg(i);
        m_settings.beginGroup(group);
        m_settings.setValue(QStringLiteral("id"), info.id);
        m_settings.setValue(QStringLiteral("name"), info.name);
        // Use live CWD from /proc if shell is running, otherwise use cached value
        if (info.view) {
            QString liveCwd = info.view->workingDirectory();
            if (!liveCwd.isEmpty())
                info.cachedWorkingDirectory = liveCwd;
        }
        QString cwd = info.cachedWorkingDirectory;
        if (cwd.isEmpty())
            cwd = QDir::homePath();
        m_settings.setValue(QStringLiteral("workingDirectory"), cwd);
        m_settings.setValue(QStringLiteral("autorunCommand"), info.autorunCommand);
        m_settings.setValue(QStringLiteral("keybarOpen"), info.keybarOpen);
        m_settings.setValue(QStringLiteral("keyboardVisible"), info.keyboardVisible);
        m_settings.setValue(QStringLiteral("createdAt"), info.createdAt);
        m_settings.setValue(QStringLiteral("lastUsedAt"), info.lastUsedAt);
        m_settings.endGroup();
    }

    m_settings.sync();
}

void SessionManager::scheduleSave()
{
    if (m_sessionsLoaded)
        m_saveTimer->start(); // restarts timer on each call (debounce)
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
    if (!Settings::instance()->scrollbackPersistence())
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
    int retentionDays = Settings::instance()->scrollbackRetentionDays();
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

bool SessionManager::restoreSessions()
{
    m_settings.beginGroup(QStringLiteral("sessions"));
    int count = m_settings.value(QStringLiteral("count"), 0).toInt();
    int nextId = m_settings.value(QStringLiteral("nextId"), 1).toInt();
    int activeIndex = m_settings.value(QStringLiteral("activeIndex"), 0).toInt();
    m_settings.endGroup();

    if (count <= 0) {
        m_sessionsLoaded = true;
        return false;
    }

    // Sanity cap to protect against corrupted settings
    if (count > 50)
        count = 50;

    m_nextSessionId = nextId;

    cleanupScrollbackFiles();

    for (int i = 0; i < count; i++) {
        QString group = QStringLiteral("sessionData/session_%1").arg(i);
        m_settings.beginGroup(group);
        int savedId = m_settings.value(QStringLiteral("id"), m_nextSessionId).toInt();
        QString name = m_settings.value(QStringLiteral("name"),
                                        tr("Session %1").arg(i + 1)).toString();
        QString workingDir = m_settings.value(QStringLiteral("workingDirectory"),
                                              QDir::homePath()).toString();
        QString autorun = m_settings.value(QStringLiteral("autorunCommand"), QString()).toString();
        bool keybarOpen = m_settings.value(QStringLiteral("keybarOpen"), true).toBool();
        bool keyboardVisible = m_settings.value(QStringLiteral("keyboardVisible"), true).toBool();
        qint64 createdAt = m_settings.value(QStringLiteral("createdAt"), 0).toLongLong();
        qint64 lastUsedAt = m_settings.value(QStringLiteral("lastUsedAt"), 0).toLongLong();
        m_settings.endGroup();

        // Validate working directory exists, fallback to home
        if (!QDir(workingDir).exists())
            workingDir = QDir::homePath();

        // Create session with restored settings
        TerminalView *view = new TerminalView();
        view->setWorkingDirectory(workingDir);
        if (!autorun.isEmpty())
            view->setAutorunCommand(autorun);

        if (Settings::instance()->scrollbackPersistence()) {
            QString sbPath = scrollbackFilePath(savedId);
            QFile sbFile(sbPath);
            if (sbFile.exists() && sbFile.open(QIODevice::ReadOnly)) {
                if (sbFile.size() > 4 * 1024 * 1024) {
                    qWarning() << "Scrollback file too large, skipping:" << sbPath;
                } else {
                    QByteArray sbData = sbFile.readAll();
                    if (!sbData.isEmpty()) {
                        // Only restore if encrypted and decryption succeeds.
                        // Plaintext files are not accepted — encryption is mandatory.
                        if (ScrollEncryptor::isEncryptedFormat(sbData)
                                && m_encryptor && m_encryptor->isAvailable()) {
                            QByteArray restored = m_encryptor->decrypt(sbData);
                            if (!restored.isEmpty())
                                view->setPendingScrollback(restored);
                        } else if (ScrollEncryptor::isEncryptedFormat(sbData)) {
                            qWarning() << "Encrypted scrollback found but secrets "
                                          "daemon unavailable, skipping:" << sbPath;
                        }
                    }
                }
            }
        }

        SessionInfo info;
        info.id = savedId;
        info.name = name;
        info.cachedWorkingDirectory = workingDir;
        info.autorunCommand = autorun;
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

    // Restore active session index
    if (activeIndex >= 0 && activeIndex < m_sessions.size())
        setActiveSessionIndex(activeIndex);
    else if (!m_sessions.isEmpty())
        setActiveSessionIndex(0);

    m_sessionsLoaded = true;
    rebuildSortedIndices();
    Q_EMIT sessionsRestored();
    return true;
}
