#ifndef BELLFEEDBACK_H
#define BELLFEEDBACK_H

#include <QElapsedTimer>
#include <QObject>

#ifndef GHOSTEEL_STORE
namespace Ngf { class Client; }
#endif

// Terminal bell feedback via ngfd. Full build: events defined in
// rpm/ghosteel-ngfd-events.ini (sound needs the proprietary
// jolla-ambient-sound-theme, absent on community ports). Store build: talks
// to the ngfd backend over raw D-Bus instead of linking libngf-qt5 (not an
// allowed Harbour dependency), stock system events only.
class BellFeedback : public QObject
{
    Q_OBJECT
public:
    explicit BellFeedback(QObject *parent = nullptr);

    Q_INVOKABLE void playBell();
    Q_INVOKABLE void playVibrate();

private:
    static constexpr int kBellCooldownMs = 1000; // daemon-side queues cannot be retracted mid-play

#ifndef GHOSTEEL_STORE
    void ensureClient();
    Ngf::Client *m_client = nullptr;
    quint32 m_vibrateEventId = 0; // pending custom plays, matched in the eventFailed fallback
    quint32 m_soundEventId = 0;
#else
    void ngfPlay(const QString &event);
#endif
    QElapsedTimer m_lastVibrate;
    QElapsedTimer m_lastBell;
};

#endif // BELLFEEDBACK_H
