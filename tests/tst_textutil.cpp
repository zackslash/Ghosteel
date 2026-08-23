#include <QtTest>
#include "textutil.h"

class TestTextUtil : public QObject
{
    Q_OBJECT

private slots:
    // --- trimSelectionText ---

    void testTrimTrailingSpaces()
    {
        QCOMPARE(TextUtil::trimSelectionText("hello  \nworld  "),
                 QStringLiteral("hello\nworld"));
    }

    void testTrimTrailingEmptyLines()
    {
        QCOMPARE(TextUtil::trimSelectionText("hello\n\n\n"),
                 QStringLiteral("hello"));
    }

    void testPreserveLeadingWhitespace()
    {
        QCOMPARE(TextUtil::trimSelectionText("  indented\n  code"),
                 QStringLiteral("  indented\n  code"));
    }

    void testEmptyInput()
    {
        QCOMPARE(TextUtil::trimSelectionText(QString()), QString());
    }

    void testNoTrailingSpace()
    {
        QCOMPARE(TextUtil::trimSelectionText("hello\nworld"),
                 QStringLiteral("hello\nworld"));
    }

    void testLineWithOnlySpaces()
    {
        QCOMPARE(TextUtil::trimSelectionText("hello\n   \nworld"),
                 QStringLiteral("hello\n\nworld"));
    }

    void testComplexTrim()
    {
        QCOMPARE(TextUtil::trimSelectionText("hello  \n  \n\n"),
                 QStringLiteral("hello"));
    }

    // --- makeTerminalFont ---

    void testMakeTerminalFontDefaults()
    {
        QFont font = TextUtil::makeTerminalFont(QStringLiteral("DejaVu Sans Mono"), 12);
        QCOMPARE(font.family(), QStringLiteral("DejaVu Sans Mono"));
        QCOMPARE(font.pointSize(), 12);
        QCOMPARE(font.styleHint(), QFont::Monospace);
        QVERIFY(font.fixedPitch());
    }

    void testMakeTerminalFontEmptyFamilyFallsBack()
    {
        QFont font = TextUtil::makeTerminalFont(QString(), 18);
        QCOMPARE(font.family(), QStringLiteral("monospace"));
        QCOMPARE(font.pointSize(), 18);
        QCOMPARE(font.styleHint(), QFont::Monospace);
        QVERIFY(font.fixedPitch());
    }

    // --- cellFromPixel ---

    void testNormalPixel()
    {
        QPointF result = TextUtil::cellFromPixel(QPointF(45, 55), 10, 10, 80, 24, 5);
        QCOMPARE(result, QPointF(4, 5)); // (45/10, (55-5)/10)
    }

    void testZeroCellDimensions()
    {
        QPointF result = TextUtil::cellFromPixel(QPointF(10, 10), 0, 0, 80, 24, 0);
        QCOMPARE(result, QPointF(-1, -1));
    }

    void testNegativeCoordinates()
    {
        // qFloor(-5) = -5, -5/10 = -1 -> out of bounds
        QPointF result = TextUtil::cellFromPixel(QPointF(-5, 10), 10, 10, 80, 24, 0);
        QCOMPARE(result, QPointF(-1, -1));
    }

    void testBeyondBounds()
    {
        QPointF result = TextUtil::cellFromPixel(QPointF(900, 10), 10, 10, 80, 24, 0);
        QCOMPARE(result, QPointF(-1, -1));
    }

    void testTopPaddingArea()
    {
        // qFloor(3 - 5) = -2, -2/10 = -1 -> out of bounds
        QPointF result = TextUtil::cellFromPixel(QPointF(10, 3), 10, 10, 80, 24, 5);
        QCOMPARE(result, QPointF(-1, -1));
    }

    void testExactCellBoundary()
    {
        // Pixel at x=20 with cellWidth=10 -> col=2 (exactly on boundary)
        QPointF result = TextUtil::cellFromPixel(QPointF(20, 15), 10, 10, 80, 24, 0);
        QCOMPARE(result, QPointF(2, 1));
    }

    // --- accumulateScroll ---

    void testSmallDelta()
    {
        auto result = TextUtil::accumulateScroll(0.0, 1.0);
        QCOMPARE(result.lines, 1);
    }

    void testSubLineDelta()
    {
        auto result = TextUtil::accumulateScroll(0.0, 0.5);
        QCOMPARE(result.lines, 0);
        QVERIFY(qAbs(result.accumulator - 0.5) < 0.001);
    }

    void testSubLineAccumulation()
    {
        auto result = TextUtil::accumulateScroll(0.6, 0.5);
        QCOMPARE(result.lines, 1);
        QVERIFY(qAbs(result.accumulator - 0.1) < 0.001);
    }

    void testDirectionReset()
    {
        auto result = TextUtil::accumulateScroll(0.5, -1.0);
        // Direction changed: accumulator resets to 0, then adds -1.0
        QCOMPARE(result.lines, -1);
        QVERIFY(qAbs(result.accumulator) < 0.001);
    }

    void testLargeDelta()
    {
        auto result = TextUtil::accumulateScroll(0.0, 6.3);
        QCOMPARE(result.lines, 6);
        QVERIFY(qAbs(result.accumulator - 0.3) < 0.001);
    }

    void testZeroDelta()
    {
        auto result = TextUtil::accumulateScroll(0.3, 0.0);
        QCOMPARE(result.lines, 0);
        QVERIFY(qAbs(result.accumulator - 0.3) < 0.001);
    }

    // --- calculateDimensions ---

    void testNormalDimensions()
    {
        auto d = TextUtil::calculateDimensions(800, 480, 10, 20, 0);
        QCOMPARE(d.cols, static_cast<uint16_t>(80));
        QCOMPARE(d.rows, static_cast<uint16_t>(24));
    }

    void testSmallWidgetClamped()
    {
        auto d = TextUtil::calculateDimensions(5, 5, 10, 20, 0);
        QCOMPARE(d.cols, static_cast<uint16_t>(2));
        QCOMPARE(d.rows, static_cast<uint16_t>(2));
    }

    void testLargeWidgetClamped()
    {
        auto d = TextUtil::calculateDimensions(10000, 10000, 10, 10, 0);
        QCOMPARE(d.cols, static_cast<uint16_t>(512));
        QCOMPARE(d.rows, static_cast<uint16_t>(512));
    }

    void testZeroCellDimensionsReturnZero()
    {
        auto d = TextUtil::calculateDimensions(800, 480, 0, 0, 0);
        QCOMPARE(d.cols, static_cast<uint16_t>(0));
        QCOMPARE(d.rows, static_cast<uint16_t>(0));
    }

    void testTopPadding()
    {
        auto d = TextUtil::calculateDimensions(800, 480, 10, 20, 10);
        QCOMPARE(d.cols, static_cast<uint16_t>(80));
        QCOMPARE(d.rows, static_cast<uint16_t>(23)); // (480 - 10) / 20 = 23.5 -> 23
    }

    void testTopPaddingExceedsHeight()
    {
        // height(10) <= topPadding(50) -> early return with minimum dimensions
        auto d = TextUtil::calculateDimensions(800, 10, 10, 20, 50);
        QCOMPARE(d.cols, static_cast<uint16_t>(2));
        QCOMPARE(d.rows, static_cast<uint16_t>(2));
    }

    void testNegativeAccumulation()
    {
        auto result = TextUtil::accumulateScroll(0.0, -0.5);
        QCOMPARE(result.lines, 0);
        QVERIFY(qAbs(result.accumulator - (-0.5)) < 0.001);

        auto result2 = TextUtil::accumulateScroll(-0.5, -0.6);
        QCOMPARE(result2.lines, -1);
        QVERIFY(qAbs(result2.accumulator - (-0.1)) < 0.001);
    }

    void testIsSoftWrappedWrapFlag()
    {
        // WRAP flag set: always soft-wrapped regardless of content
        QVERIFY(TextUtil::isSoftWrapped(true, false));
        QVERIFY(TextUtil::isSoftWrapped(true, true));
    }

    void testIsSoftWrappedHeuristic()
    {
        // WRAP flag false, last cell has content: heuristic fires
        QVERIFY(TextUtil::isSoftWrapped(false, true));
    }

    void testIsSoftWrappedNoWrap()
    {
        // WRAP flag false, last cell empty: hard newline
        QVERIFY(!TextUtil::isSoftWrapped(false, false));
    }

    // --- blink phase math ---

    void testBlinkPhaseVisibleWindows()
    {
        const int interval = 500;
        QVERIFY(TextUtil::blinkPhaseVisible(0, interval));      // window 0 ON
        QVERIFY(TextUtil::blinkPhaseVisible(499, interval));
        QVERIFY(!TextUtil::blinkPhaseVisible(500, interval));   // window 1 OFF
        QVERIFY(!TextUtil::blinkPhaseVisible(999, interval));
        QVERIFY(TextUtil::blinkPhaseVisible(1000, interval));   // window 2 ON
        QVERIFY(TextUtil::blinkPhaseVisible(1050, interval));
        QVERIFY(!TextUtil::blinkPhaseVisible(2500, interval));  // window 5 OFF
    }

    void testBlinkPhaseVisibleLongElapsed()
    {
        // An hour idle: no parity drift or overflow.
        QVERIFY(TextUtil::blinkPhaseVisible(3600000LL, 500));   // window 7200
        QVERIFY(!TextUtil::blinkPhaseVisible(3600500LL, 500)); // window 7201
    }

    void testNextBlinkTickDelayBoundaries()
    {
        const int interval = 500, guard = 50;
        QCOMPARE(TextUtil::nextBlinkTickDelay(0, interval, guard), 550);
        QCOMPARE(TextUtil::nextBlinkTickDelay(499, interval, guard), 51);
        QCOMPARE(TextUtil::nextBlinkTickDelay(500, interval, guard), 550);
        // Late tick 70ms into a window self-corrects to the next boundary.
        QCOMPARE(TextUtil::nextBlinkTickDelay(570, interval, guard), 480);
        // Tick just before a boundary lands just after it.
        QCOMPARE(TextUtil::nextBlinkTickDelay(1499, interval, guard), 51);
    }

    void testNextBlinkTickDelayNeverSpins()
    {
        // For any elapsed >= 0 the delay exceeds the guard: no 1ms re-arm loop.
        const int interval = 500, guard = 50;
        for (qint64 elapsed = 0; elapsed < 5000; ++elapsed)
            QVERIFY(TextUtil::nextBlinkTickDelay(elapsed, interval, guard) > guard);
    }
};

QTEST_MAIN(TestTextUtil)
#include "tst_textutil.moc"
