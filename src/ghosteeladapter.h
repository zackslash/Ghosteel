#ifndef GHOSTEELADAPTER_H
#define GHOSTEELADAPTER_H

#include <QDBusAbstractAdaptor>

class SessionManager;

class GhosteelAdapter : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.zackslash.ghosteel")

public:
    explicit GhosteelAdapter(SessionManager *parent);

public slots:
    Q_SCRIPTABLE void activateSession(int sessionId);

private:
    SessionManager *m_manager;
};

#endif // GHOSTEELADAPTER_H
