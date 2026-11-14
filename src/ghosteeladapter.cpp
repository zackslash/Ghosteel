#include "ghosteeladapter.h"
#include "sessionmanager.h"

#include <QDebug>
#ifdef GHOSTEEL_STORE
#include <QTimer>
#endif
#include <QGuiApplication>
#include <QWindow>

GhosteelAdapter::GhosteelAdapter(SessionManager *parent)
    : QDBusAbstractAdaptor(parent)
    , m_manager(parent)
{
}

#ifdef GHOSTEEL_STORE
void GhosteelAdapter::launch(const QString &command, const QStringList &args,
                             const QString &sessionName, bool restart)
{
    // Mirrors the local-socket Exec path: apply the CLI request, process it,
    // then raise. The raise is delayed to let QML process sessionCreated().
    m_manager->setCliArgs(command, args, sessionName, restart);
    m_manager->processCliArgs();
    QTimer::singleShot(100, m_manager, []() {
        const auto windows = QGuiApplication::topLevelWindows();
        if (!windows.isEmpty()) {
            windows.first()->raise();
            windows.first()->requestActivate();
        }
    });
}
#endif // GHOSTEEL_STORE

void GhosteelAdapter::activateSession(int sessionId)
{
    int index = m_manager->sessionIndexById(sessionId);
    if (index < 0) {
        qWarning() << "Ghosteel: session ID" << sessionId << "not found, ignoring";
        return;
    }

    // index is an actual m_sessions index — don't hand it to switchToSession(),
    // which expects a display (sorted) index.
    m_manager->setActiveSessionIndex(index);

    const auto windows = QGuiApplication::topLevelWindows();
    if (!windows.isEmpty())
        windows.first()->requestActivate();
}
