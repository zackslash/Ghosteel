#include "bellfeedback.h"

#ifndef GHOSTEEL_STORE
#include <NgfClient>
#else
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#endif
#include <QDebug>

BellFeedback::BellFeedback(QObject *parent)
    : QObject(parent)
{
}

#ifndef GHOSTEEL_STORE

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

#else

// Direct call to the ngfd backend interface, the one feedback interface the
// Sailjail sandbox filter allows. The store build ships no custom events.d
// definitions, so only the stock system events are available.
void BellFeedback::ngfPlay(const QString &event)
{
    QDBusMessage play = QDBusMessage::createMethodCall(
        QStringLiteral("com.nokia.NonGraphicFeedback1.Backend"),
        QStringLiteral("/com/nokia/NonGraphicFeedback1"),
        QStringLiteral("com.nokia.NonGraphicFeedback1"),
        QStringLiteral("Play"));
    play << event << QVariantMap();
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(play), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                if (w->isError())
                    qWarning() << "BellFeedback: ngfd play failed:" << w->error().message();
                w->deleteLater();
            });
}

#endif

void BellFeedback::playBell()
{
    // Burst gate: sound plays report completion early and would overlap.
    if (m_lastBell.isValid() && m_lastBell.elapsed() < kBellCooldownMs)
        return;
    m_lastBell.start();
#ifndef GHOSTEEL_STORE
    ensureClient();
    m_soundEventId = m_client->play(QStringLiteral("ghosteel_bell_sound"));
#else
    ngfPlay(QStringLiteral("general_warning"));
#endif
}

void BellFeedback::playVibrate()
{
    // Burst gate: the vibrator queues effects unretractably.
    if (m_lastVibrate.isValid() && m_lastVibrate.elapsed() < kBellCooldownMs)
        return;
    m_lastVibrate.start();
#ifndef GHOSTEEL_STORE
    ensureClient();
    m_vibrateEventId = m_client->play(QStringLiteral("ghosteel_bell_vibrate"));
#else
    ngfPlay(QStringLiteral("vibra"));
#endif
}
