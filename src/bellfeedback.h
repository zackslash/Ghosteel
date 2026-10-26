#ifndef BELLFEEDBACK_H
#define BELLFEEDBACK_H

#include <QElapsedTimer>
#include <QObject>

namespace Ngf { class Client; }

// Terminal bell feedback via ngfd; events defined in rpm/ghosteel-ngfd-events.ini.
// Sound needs the proprietary jolla-ambient-sound-theme (absent on community ports).
class BellFeedback : public QObject
{
    Q_OBJECT
public:
    explicit BellFeedback(QObject *parent = nullptr);

    Q_INVOKABLE void playBell();
    Q_INVOKABLE void playVibrate();

private:
    static constexpr int kBellCooldownMs = 1000; // daemon-side queues cannot be retracted mid-play

    void ensureClient();
    Ngf::Client *m_client = nullptr;
    QElapsedTimer m_lastVibrate;
    QElapsedTimer m_lastBell;
    quint32 m_vibrateEventId = 0; // pending custom plays, matched in the eventFailed fallback
    quint32 m_soundEventId = 0;
};

#endif // BELLFEEDBACK_H
