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
    // Lazy: skip the D-Bus connection until first bell; the client
    // auto-reconnects if the daemon appears later.
    m_client = new Ngf::Client(this);
    if (!m_client->connect()) {
        qWarning() << "BellFeedback: ngfd connection failed; bell will be silent";
    }
    // Fall back to the stock events where our RPM did not install our own.
    connect(m_client, &Ngf::Client::eventFailed, this, [this](quint32 id) {
        if (id && id == m_vibrateEventId) {
            m_vibrateEventId = 0;
            m_client->play(QStringLiteral("vibra"));
        } else if (id && id == m_soundEventId) {
            m_soundEventId = 0;
            m_client->play(QStringLiteral("general_warning"));
        }
    });
}

void BellFeedback::playBell()
{
    // Burst gate: canberra reports completion early, plays would overlap.
    if (m_lastBell.isValid() && m_lastBell.elapsed() < kBellCooldownMs)
        return;
    m_lastBell.start();
    ensureClient();
    m_soundEventId = m_client->play(QStringLiteral("ghosteel_bell_sound"));
}

void BellFeedback::playVibrate()
{
    // Burst gate: the vibrator queues effects unretractably.
    if (m_lastVibrate.isValid() && m_lastVibrate.elapsed() < kBellCooldownMs)
        return;
    m_lastVibrate.start();
    ensureClient();
    m_vibrateEventId = m_client->play(QStringLiteral("ghosteel_bell_vibrate"));
}
