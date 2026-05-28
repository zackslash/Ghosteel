#include "sessionmanager.h"
#include "terminalview.h"
#include "ptymanager.h"

SessionManager::SessionManager(QObject *parent)
    : QObject(parent)
{
}

SessionManager::~SessionManager()
{
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

    m_activeSessionIndex = index;
    Q_EMIT activeSessionIndexChanged();
    Q_EMIT sessionSwitched(index);
}

QQmlListProperty<TerminalView> SessionManager::sessions()
{
    return QQmlListProperty<TerminalView>(this, nullptr,
                                          &SessionManager::sessionCountCallback,
                                          &SessionManager::sessionAtCallback);
}

int SessionManager::sessionCountCallback(QQmlListProperty<TerminalView> *prop)
{
    SessionManager *manager = qobject_cast<SessionManager *>(prop->object);
    if (!manager)
        return 0;
    return manager->m_sessions.size();
}

TerminalView* SessionManager::sessionAtCallback(QQmlListProperty<TerminalView> *prop, int index)
{
    SessionManager *manager = qobject_cast<SessionManager *>(prop->object);
    if (!manager || index < 0 || index >= manager->m_sessions.size())
        return nullptr;
    return manager->m_sessions.at(index).view;
}

TerminalView* SessionManager::createSession()
{
    // Create a new TerminalView as a child of this manager
    TerminalView *view = new TerminalView();

    SessionInfo info;
    info.id = m_nextSessionId++;
    info.name = QStringLiteral("Session %1").arg(info.id);
    info.view = view;

    int index = m_sessions.size();
    m_sessions.append(info);

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

    // Adjust active session index BEFORE emitting signals, so that
    // sessionSwitched handlers see a valid active index.
    if (m_sessions.isEmpty()) {
        m_activeSessionIndex = -1;
        Q_EMIT activeSessionIndexChanged();
    } else if (index < m_activeSessionIndex) {
        // Removed session was before active — shift index down
        m_activeSessionIndex--;
        Q_EMIT activeSessionIndexChanged();
    } else if (wasActive) {
        // Removed the active session — clamp to valid range
        if (m_activeSessionIndex >= m_sessions.size())
            m_activeSessionIndex = m_sessions.size() - 1;
        Q_EMIT activeSessionIndexChanged();
        // Notify FirstPage to switch to the new active terminal.
        // This must happen BEFORE deleting the old view, because the
        // sessionSwitched handler disconnects signals from the old
        // terminal — accessing a deleted object would be UB.
        Q_EMIT sessionSwitched(m_activeSessionIndex);
    }

    Q_EMIT sessionCountChanged();
    Q_EMIT sessionsChanged();
    Q_EMIT sessionRemoved(index);

    // Clean up and delete the view AFTER all signals have been emitted,
    // so handlers that reference the old view (e.g. to disconnect
    // signals) can still safely access it.
    if (info.view) {
        info.view->cleanup();
        delete info.view;
    }
}

void SessionManager::switchToSession(int index)
{
    setActiveSessionIndex(index);
}

TerminalView* SessionManager::activeSession() const
{
    if (m_activeSessionIndex < 0 || m_activeSessionIndex >= m_sessions.size())
        return nullptr;
    return m_sessions.at(m_activeSessionIndex).view;
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
    Q_EMIT sessionNameChanged(index);
}

int SessionManager::sessionId(int index) const
{
    if (index < 0 || index >= m_sessions.size())
        return -1;
    return m_sessions[index].id;
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
