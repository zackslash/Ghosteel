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
}

void BellFeedback::playBell()
{
    ensureClient();
    m_client->play(QStringLiteral("default"));
}
