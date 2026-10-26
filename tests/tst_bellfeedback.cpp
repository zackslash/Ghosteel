#include <QtTest>

#include "bellfeedback.h"
#include "ngfclient_stub.h"

#include <QSettings>

class BellFeedbackTest : public QObject
{
    Q_OBJECT

private slots:
    void vibratePlaysCustomEvent();
    void vibrateBurstCoalesces();
    void failedVibrateEventFallsBackToStock();
    void unrelatedFailureDoesNotFallBack();
    void bellPlaysCustomSoundEvent();
    void bellBurstCoalesces();
    void failedSoundEventFallsBackToStock();
    void iniDefinesTheEventNames();
};

Ngf::Client *clientAfter(BellFeedback &bell, void (BellFeedback::*trigger)())
{
    (bell.*trigger)();
    return bell.findChild<Ngf::Client *>();
}

void BellFeedbackTest::vibratePlaysCustomEvent()
{
    BellFeedback bell;
    Ngf::Client *client = clientAfter(bell, &BellFeedback::playVibrate);
    QVERIFY(client);
    QCOMPARE(client->played, QStringList{QStringLiteral("ghosteel_bell_vibrate")});
}

void BellFeedbackTest::vibrateBurstCoalesces()
{
    BellFeedback bell;
    Ngf::Client *client = clientAfter(bell, &BellFeedback::playVibrate);
    bell.playVibrate();
    bell.playVibrate();
    QCOMPARE(client->played.size(), 1);
}

void BellFeedbackTest::failedVibrateEventFallsBackToStock()
{
    BellFeedback bell;
    Ngf::Client *client = clientAfter(bell, &BellFeedback::playVibrate);
    emit client->eventFailed(client->nextId);
    QCOMPARE(client->played.last(), QStringLiteral("vibra"));
}

void BellFeedbackTest::unrelatedFailureDoesNotFallBack()
{
    BellFeedback bell;
    Ngf::Client *client = clientAfter(bell, &BellFeedback::playVibrate);
    const int before = client->played.size();
    emit client->eventFailed(999);
    emit client->eventFailed(0);
    QCOMPARE(client->played.size(), before);
}

void BellFeedbackTest::bellPlaysCustomSoundEvent()
{
    BellFeedback bell;
    Ngf::Client *client = clientAfter(bell, &BellFeedback::playBell);
    QVERIFY(client);
    QCOMPARE(client->played, QStringList{QStringLiteral("ghosteel_bell_sound")});
}

void BellFeedbackTest::bellBurstCoalesces()
{
    BellFeedback bell;
    Ngf::Client *client = clientAfter(bell, &BellFeedback::playBell);
    bell.playBell();
    QCOMPARE(client->played.size(), 1);
}

void BellFeedbackTest::failedSoundEventFallsBackToStock()
{
    BellFeedback bell;
    Ngf::Client *client = clientAfter(bell, &BellFeedback::playBell);
    emit client->eventFailed(client->nextId);
    QCOMPARE(client->played.last(), QStringLiteral("general_warning"));
}

void BellFeedbackTest::iniDefinesTheEventNames()
{
    // The C++ event names and the shipped ini must agree; a name that
    // resolves to no event leaves the bell dead with no error.
    const QString path = QStringLiteral(GHOSTEEL_SOURCE_DIR)
                         + QStringLiteral("/rpm/ghosteel-ngfd-events.ini");
    const QStringList groups = QSettings(path, QSettings::IniFormat).childGroups();
    QVERIFY(groups.contains(QStringLiteral("ghosteel_bell_vibrate")));
    QVERIFY(groups.contains(QStringLiteral("ghosteel_bell_sound")));
}

QTEST_MAIN(BellFeedbackTest)
#include "tst_bellfeedback.moc"
