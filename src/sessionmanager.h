#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QObject>
#include <QQmlListProperty>
#include <QVector>

class TerminalView;

struct SessionInfo {
    int id;
    QString name;
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
    ~SessionManager();

    int activeSessionIndex() const { return m_activeSessionIndex; }
    void setActiveSessionIndex(int index);

    int sessionCount() const { return m_sessions.size(); }

    QQmlListProperty<TerminalView> sessions();

    Q_INVOKABLE TerminalView* createSession();
    Q_INVOKABLE void removeSession(int index);
    Q_INVOKABLE void switchToSession(int index);
    Q_INVOKABLE TerminalView* activeSession() const;
    Q_INVOKABLE QString sessionName(int index) const;
    Q_INVOKABLE void setSessionName(int index, const QString &name);
    Q_INVOKABLE int sessionId(int index) const;
    Q_INVOKABLE void removeSessionById(int id);

Q_SIGNALS:
    void activeSessionIndexChanged();
    void sessionCountChanged();
    void sessionsChanged();
    void sessionCreated(int index);
    void sessionRemoved(int index);
    void sessionSwitched(int index);
    void sessionNameChanged(int idx);

private:
    static int sessionCountCallback(QQmlListProperty<TerminalView> *prop);
    static TerminalView* sessionAtCallback(QQmlListProperty<TerminalView> *prop, int index);

    QVector<SessionInfo> m_sessions;
    int m_activeSessionIndex = -1;
    int m_nextSessionId = 1;
};

#endif // SESSIONMANAGER_H
