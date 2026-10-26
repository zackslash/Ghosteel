#include "bellfeedback.h"

#include <NgfClient>
#include <QDebug>

BellFeedback::BellFeedback(QObject *parent)
    : QObject(parent)
{
}

void BellFeedback::ensureClient()
{
    if (m_client)
        return;
    // Lazy: skip the D-Bus connection until first bell.
    // Ngf::Client auto-reconnects if the daemon appears after the first failure.
    m_client = new Ngf::Client(this);
    if (!m_client->connect()) {
        qWarning() << "BellFeedback: ngfd connection failed; bell will be silent";
    }
    // The custom event only exists where our RPM installed it; fall back to
    // the stock double-pulse event when the daemon rejects ours.
    connect(m_client, &Ngf::Client::eventFailed, this, [this](quint32 id) {
        if (id && id == m_customVibrateId) {
            m_customVibrateId = 0;
            m_client->play(QStringLiteral("vibra"));
        }
    });
}

// Bell feedback events: "ghosteel_bell_vibrate" and "ghosteel_bell_sound"
// are our own ngfd events (rpm/ghosteel-ngfd-events.ini, installed by the
// RPM; ngfd reads it at daemon start). Vibration is the single-pulse
// "strong" haptic with the stock double-pulse "vibra" event as fallback;
// sound is the themed general_warning tone with no fallback — the old
// "default" event had no sound source of its own and played nothing.
void BellFeedback::playBell()
{
    ensureClient();
    m_client->play(QStringLiteral("ghosteel_bell_sound"));
}

void BellFeedback::playVibrate()
{
    // The vibrator queues effects and a daemon-side Stop cannot retract a
    // queued buzz, so a BEL burst must be throttled here or the phone keeps
    // buzzing long after the last bell.
    if (m_lastVibrate.isValid() && m_lastVibrate.elapsed() < kVibrateCooldownMs)
        return;
    m_lastVibrate.start();
    ensureClient();
    m_customVibrateId = m_client->play(QStringLiteral("ghosteel_bell_vibrate"));
}
