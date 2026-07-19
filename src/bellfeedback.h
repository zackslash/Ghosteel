#ifndef BELLFEEDBACK_H
#define BELLFEEDBACK_H

#include <QObject>

namespace Ngf { class Client; }

// Terminal bell via ngfd (SailfishOS feedback daemon). Chosen over a bundled
// WAV because community ports lack the proprietary jolla-ambient-sound-theme.
class BellFeedback : public QObject
{
    Q_OBJECT
public:
    explicit BellFeedback(QObject *parent = nullptr);

    Q_INVOKABLE void playBell();

private:
    void ensureClient();
    Ngf::Client *m_client = nullptr;
};

#endif // BELLFEEDBACK_H
