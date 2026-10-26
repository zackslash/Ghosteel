#ifndef BELLFEEDBACK_H
#define BELLFEEDBACK_H

#include <QElapsedTimer>
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
    Q_INVOKABLE void playVibrate();

private:
    static constexpr int kVibrateCooldownMs = 1000;
    void ensureClient();
    Ngf::Client *m_client = nullptr;
    QElapsedTimer m_lastVibrate; // gates the vibra effect rate (queued kernel-side, unretractable)
    quint32 m_customVibrateId = 0; // pending ghosteel_bell_vibrate play, for the stock-event fallback
};

#endif // BELLFEEDBACK_H
