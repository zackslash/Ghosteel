#include <QtTest>
#include <QSignalSpy>

#include "ghosttyvt.h"

class TestOsc777 : public QObject
{
    Q_OBJECT

private slots:
    // Test 1: Full notification with title and body, terminated by BEL
    void testFullNotification()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]777;notify;Hello;World\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Hello"));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("World"));
    }

    // Test 2: Title-only notification terminated by BEL
    void testTitleOnlyBel()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]777;notify;Hello\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Hello"));
        QCOMPARE(spy.at(0).at(1).toString(), QString());
    }

    // Test 3: Title-only notification terminated by ESC (ST terminator)
    void testTitleOnlyEsc()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        // ESC instead of BEL terminates the title, transitions to ESC state
        QByteArray data("\x1b]777;notify;Hello\x1b");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Hello"));
        QCOMPARE(spy.at(0).at(1).toString(), QString());
    }

    // Test 4: Empty title is skipped (no signal)
    void testEmptyTitleSkipped()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]777;notify;;Body\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0); // title.isEmpty() guard
    }

    // Test 5: Partial sequence split across two vtWrite calls
    void testPartialAcrossBuffers()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray part1("\x1b]777;no");
        QByteArray part2("tify;Title;Body\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(part1.constData()), part1.size());
        QCOMPARE(spy.count(), 0); // no signal yet
        vt.vtWrite(reinterpret_cast<const uint8_t*>(part2.constData()), part2.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Title"));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("Body"));
    }

    // Test 6: Near-miss — prefix 776 (not 777)
    void testNearMiss776()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]776;notify;T;B\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 7: Near-miss — prefix 778
    void testNearMiss778()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]778;notify;T;B\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 8: Wrong keyword (not "notify")
    void testWrongKeyword()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]777;alert;T;B\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 9: Title size cap (512 bytes)
    void testTitleSizeCap()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        // 600-byte title, capped at 512
        QByteArray title(600, 'A');
        QByteArray data = "\x1b]777;notify;" + title + ";Body\x07";
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString().size(), 512);
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("Body"));
    }

    // Test 10: Body size cap (2048 bytes)
    void testBodySizeCap()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray body(3000, 'B');
        QByteArray data = "\x1b]777;notify;Title;" + body + "\x07";
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Title"));
        QCOMPARE(spy.at(0).at(1).toString().size(), 2048);
    }

    // Test 11: Multiple notifications in a single vtWrite
    void testMultipleInOneBuffer()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]777;notify;A;B\x07\x1b]777;notify;C;D\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("A"));
        QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("C"));
    }

    // Test 12: State reset after destroy()
    void testStateResetOnDestroy()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        // Feed partial sequence
        QByteArray part1("\x1b]777;notify");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(part1.constData()), part1.size());
        QCOMPARE(spy.count(), 0);
        // Destroy resets scanner state
        vt.destroy();
        // Feed continuation — should NOT produce a notification
        QByteArray part2(";Title;Body\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(part2.constData()), part2.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 13: Interleaved normal data doesn't affect scanner
    void testInterleavedNormalData()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("abc\x1b]777;notify;T;B" "\x07" "def");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("T"));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("B"));
    }

    // Test 14: BEL in body terminates early (but BEL itself ends the sequence)
    void testBelInBody()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        // Only first BEL terminates. Characters after BEL are normal data.
        QByteArray data("\x1b]777;notify;T;Body\x07rest");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("Body"));
    }

    // Test 15: BEL during title terminates as title-only
    void testBelDuringTitle()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]777;notify;QuickTitle\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("QuickTitle"));
        QCOMPARE(spy.at(0).at(1).toString(), QString());
    }

    // Test 16: bell() signal is NOT emitted by OSC 777 (it only emits desktopNotification)
    void testNoBellFromOsc777()
    {
        GhosttyVt vt;
        QSignalSpy bellSpy(&vt, &GhosttyVt::bell);
        QSignalSpy notifSpy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]777;notify;T;B\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(bellSpy.count(), 0);
        QCOMPARE(notifSpy.count(), 1);
    }

    // Test 17: Unicode characters in title and body
    void testUnicodeNotification()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        // UTF-8 encoded: "\xf0\x9f\x94\x94" = 4 bytes, "\xe4\xb8\x96\xe7\x95\x8c" = 6 bytes
        QByteArray data("\x1b]777;notify;\xf0\x9f\x94\x94;\xe4\xb8\x96\xe7\x95\x8c\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QString::fromUtf8("\xf0\x9f\x94\x94"));
        QCOMPARE(spy.at(0).at(1).toString(), QString::fromUtf8("\xe4\xb8\x96\xe7\x95\x8c"));
    }

    // Test 18: ESC inside body resets to ESC state (title-only from body perspective)
    void testEscInBody()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        // ESC in body terminates the body section with what we have so far
        QByteArray data("\x1b]777;notify;T;Partial\x1b");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("T"));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("Partial"));
    }

    // Test 19: Proper ST terminator (ESC backslash)
    void testEscBackslashTerminator()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]777;notify;Title;Body\x1b\x5c");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Title"));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("Body"));
    }
};

QTEST_MAIN(TestOsc777)
#include "tst_osc777.moc"
