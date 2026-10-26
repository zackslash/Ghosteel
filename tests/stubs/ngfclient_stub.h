#ifndef NGFCLIENT_STUB_H
#define NGFCLIENT_STUB_H

#include <QObject>
#include <QString>
#include <QStringList>

namespace Ngf {

class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject *parent = nullptr) : QObject(parent) {}
    bool connect() { return true; }
    quint32 play(const QString &event)
    {
        played << event;
        return ++nextId;
    }

    QStringList played;
    quint32 nextId = 0;

signals:
    void eventFailed(quint32 id);
};

} // namespace Ngf

#endif // NGFCLIENT_STUB_H
