#include <QtTest>
#include <QSignalSpy>

#include "ghosttyvt.h"

class TestOsc52 : public QObject
{
    Q_OBJECT

private slots:
    // Test 1: Write with BEL terminator
    void testWriteBel()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;c;aGVsbG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 2: Write with ST terminator (ESC backslash)
    void testWriteSt()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;c;aGVsbG8=\x1b\x5c");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 3: Empty data (clear clipboard)
    void testEmptyDataClear()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;c;\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray());
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 4: Buffer overflow — payload > 1MB capped at MaxOsc52DataLen
    void testBufferOverflow()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray payload(1024 * 1024 + 100, 'A');
        QByteArray data = "\x1b]52;c;" + payload + "\x07";
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray().size(), 1024 * 1024);
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 5: Kind overflow — selection parameter > 16 bytes, silently ignored
    void testKindOverflow()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray kind(17, 's');
        QByteArray data = "\x1b]52;" + kind + ";aGVsbG8=\x07";
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 6: Malformed base64 — invalid chars skipped, valid chars accumulated
    void testMalformedBase64()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;c;!!!invalid!!!\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("invalid"));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 7: Fragmented delivery across multiple vtWrite calls
    void testFragmentedDelivery()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray part1("\x1b]52;c;aGVs");
        QByteArray part2("bG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(part1.constData()), part1.size());
        QCOMPARE(spy.count(), 0);
        vt.vtWrite(reinterpret_cast<const uint8_t*>(part2.constData()), part2.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 8: No interference with OSC 777 — both signals emitted independently
    void testNoInterferenceWithOsc777()
    {
        GhosttyVt vt;
        QSignalSpy clipSpy(&vt, &GhosttyVt::clipboardWriteRequest);
        QSignalSpy notifSpy(&vt, &GhosttyVt::desktopNotification);
        QByteArray data("\x1b]52;c;aGVsbG8=\x07\x1b]777;notify;Title;Body\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(clipSpy.count(), 1);
        QCOMPARE(notifSpy.count(), 1);
        QCOMPARE(clipSpy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
        QCOMPARE(notifSpy.at(0).at(0).toString(), QStringLiteral("Title"));
    }

    // Test 9: Non-clipboard target — silently ignored
    void testNonClipboardTarget()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;s;aGVsbG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 10: Uppercase C is accepted
    void testUppercaseC()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;C;aGVsbG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("C"));
    }

    // Test 11: Read query with BEL terminator
    void testReadQuery()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardReadRequest);
        QByteArray data("\x1b]52;c;?\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("c"));
    }

    // Test 12: Read query with non-clipboard target — silently ignored
    void testReadQueryNonClipboard()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardReadRequest);
        QByteArray data("\x1b]52;s;?\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 13: Read query with ST terminator
    void testReadQuerySt()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardReadRequest);
        QByteArray data("\x1b]52;c;?\x1b\x5c");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("c"));
    }

    // Test 14: State reset on destroy()
    void testStateResetOnDestroy()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray part1("\x1b]52;c;aGVs");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(part1.constData()), part1.size());
        QCOMPARE(spy.count(), 0);
        vt.destroy();
        QByteArray part2("bG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(part2.constData()), part2.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 15: Empty kind defaults to "c" (clipboard)
    void testDefaultKind()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;;aGVsbG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 16: Base64 with whitespace — whitespace is tolerated
    void testBase64WithWhitespace()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;c;aGVs\nbG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVs\nbG8="));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 17: Control chars skipped — 0x01 byte is silently dropped
    void testControlCharsSkipped()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;c;aGVs\x01" "bG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 18: Empty data with ST terminator (clear via ST)
    void testEmptyDataClearSt()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        // ESC]52;c; followed by ST (ESC backslash)
        QByteArray data("\x1b]52;c;\x1b\x5c");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray());
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("c"));
    }

    // Test 19: ESC in data is filtered by base64 validation
    void testEscInDataBase64Filtered()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        // "aGVs" + ESC + "bG8=" — ESC should be filtered out (not valid base64)
        QByteArray data("\x1b]52;c;aGVs\x1b" "bG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
    }

    // Test 20: ESC followed by non-backslash resumes DATA state
    void testEscNonBackslashResumesData()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        // "aGVs" + ESC + 'X' + "bG8=" + BEL — ESC filtered, 'X' is valid base64
        QByteArray data("\x1b]52;c;aGVs\x1bX" "bG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsXbG8="));
    }

    // Test 21: BEL immediately after semicolon (malformed)
    void testMalformedBelAfterSemi()
    {
        GhosttyVt vt;
        QSignalSpy writeSpy(&vt, &GhosttyVt::clipboardWriteRequest);
        QSignalSpy readSpy(&vt, &GhosttyVt::clipboardReadRequest);
        QByteArray data("\x1b]52;\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(writeSpy.count(), 0);
        QCOMPARE(readSpy.count(), 0);
    }

    // Test 22: Kind without data separator (BEL terminates kind)
    void testMalformedKindTerminatedByBel()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        // ESC]52;c then BEL — no semicolon before data
        QByteArray data("\x1b]52;c\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0);
    }

    // Test 23: Two write sequences in one vtWrite call
    void testMultipleOsc52InOneBuffer()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;c;aGVsbG8=\x07\x1b]52;c;d29ybGQ=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
        QCOMPARE(spy.at(1).at(0).toByteArray(), QByteArray("d29ybGQ="));
    }

    // Test 24: Two read queries in one vtWrite call
    void testMultipleReadQueriesInOneBuffer()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardReadRequest);
        QByteArray data("\x1b]52;c;?\x07\x1b]52;c;?\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 2);
    }

    // Test 25: Read query followed by write in one buffer
    void testReadQueryThenWriteInOneBuffer()
    {
        GhosttyVt vt;
        QSignalSpy readSpy(&vt, &GhosttyVt::clipboardReadRequest);
        QSignalSpy writeSpy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]52;c;?\x07\x1b]52;c;aGVsbG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(readSpy.count(), 1);
        QCOMPARE(writeSpy.count(), 1);
    }

    // Test 26: ESC after semicolon, scanner recovers for next sequence
    void testMalformedEscAfterSemi()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        // ESC]52; then ESC — malformed, scanner goes to ESC state
        // Then "]52;c;aGVsbG8=" + BEL completes a valid sequence
        QByteArray data("\x1b]52;\x1b]52;c;aGVsbG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArray("aGVsbG8="));
    }

    // Test 27: Wrong OSC number (53 instead of 52)
    void testNearMiss53()
    {
        GhosttyVt vt;
        QSignalSpy spy(&vt, &GhosttyVt::clipboardWriteRequest);
        QByteArray data("\x1b]53;c;aGVsbG8=\x07");
        vt.vtWrite(reinterpret_cast<const uint8_t*>(data.constData()), data.size());
        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(TestOsc52)
#include "tst_osc52.moc"
