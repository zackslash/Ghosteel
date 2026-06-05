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

    m_manager->switchToSession(index);

    // Raise the application window to the foreground
    const auto windows = QGuiApplication::topLevelWindows();
    if (!windows.isEmpty())
        windows.first()->requestActivate();
}
