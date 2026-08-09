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
};

QTEST_MAIN(TestTextUtil)
#include "tst_textutil.moc"
