#include "ghosteeladapter.h"
#include "sessionmanager.h"

#include <QDebug>
#include <QGuiApplication>
#include <QWindow>

GhosteelAdapter::GhosteelAdapter(SessionManager *parent)
    : QDBusAbstractAdaptor(parent)
    , m_manager(parent)
{
}

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
