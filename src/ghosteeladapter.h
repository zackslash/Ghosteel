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
#ifdef GHOSTEEL_STORE
    // Cross-sandbox single-instance entry point (store build): a duplicate
    // process that lost the D-Bus name race forwards its CLI args here.
    Q_SCRIPTABLE void launch(const QString &command, const QStringList &args,
                             const QString &sessionName, bool restart);
#endif

private:
    SessionManager *m_manager;
};

#endif // GHOSTEELADAPTER_H
