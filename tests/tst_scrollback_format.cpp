#include <QtTest>
#include <QByteArray>
#include "ghosttyvt.h"

// NOTE: These tests only verify restoreScrollback() doesn't crash — the Ghostty
// stubs don't process VT writes, so terminal state can't be asserted.

class TestScrollbackFormat : public QObject
{
    Q_OBJECT

private:
    static QByteArray buildScrollback(uint16_t cols, uint16_t rows, const QStringList &lines)
    {
        QByteArray data;
        data.append(QStringLiteral("GHOSTTY_SCROLLBACK_V1\nCOLS=%1\nROWS=%2\n\n")
                        .arg(cols).arg(rows).toUtf8());
        for (const QString &line : lines) {
            data.append(line.toUtf8());
            data.append("\r\n");
        }
        // Strip trailing \r\n (matches exportScrollback behavior)
        while (data.endsWith("\r\n"))
            data.chop(2);
        return data;
    }

private slots:
    void testEmptyData()
    {
        GhosttyVt vt;
        // Should not crash with empty data
        vt.restoreScrollback(QByteArray(), 80);
        vt.restoreScrollback(buildScrollback(0, 0, {}), 80);
    }

    void testInvalidHeader()
    {
        GhosttyVt vt;
        // Missing magic
        vt.restoreScrollback("NOT_VALID\r\n", 80);
        // Missing COLS
        vt.restoreScrollback("GHOSTTY_SCROLLBACK_V1\nROWS=24\n\nhello\r\n", 80);
        // Missing double newline separator
        vt.restoreScrollback("GHOSTTY_SCROLLBACK_V1\nCOLS=80\nROWS=24\nhello", 80);
    }

    void testSameColumnCount()
    {
        GhosttyVt vt;
        QStringList lines = {"hello world", "second line", "third line"};
        QByteArray data = buildScrollback(80, 24, lines);
        // Should not crash — same column count, no re-wrap needed
        vt.restoreScrollback(data, 80);
    }

    void testColumnMismatchPad()
    {
        GhosttyVt vt;
        // Saved at 40 cols, restoring at 80 — lines should be padded
        QStringList lines = {"short line", "another"};
        QByteArray data = buildScrollback(40, 24, lines);
        vt.restoreScrollback(data, 80);
    }

    void testColumnMismatchRewrap()
    {
        GhosttyVt vt;
        // Saved at 80 cols, restoring at 40 — long lines should be re-wrapped
        QString longLine = QString(80, 'x'); // 80 chars, wider than 40
        QStringList lines = {longLine};
        QByteArray data = buildScrollback(80, 24, lines);
        vt.restoreScrollback(data, 40);
    }

    void testTrailingSpaceTrimming()
    {
        GhosttyVt vt;
        // Lines with trailing spaces should be trimmed
        QStringList lines = {"hello     ", "world     "};
        QByteArray data = buildScrollback(80, 24, lines);
        vt.restoreScrollback(data, 80);
    }

    void testCrLfHandling()
    {
        GhosttyVt vt;
        // Build data with \r\n line endings (as export produces)
        QByteArray data = "GHOSTTY_SCROLLBACK_V1\nCOLS=80\nROWS=24\n\n"
                          "line one\r\nline two\r\nline three";
        vt.restoreScrollback(data, 80);
    }

    void testBareLfHandling()
    {
        GhosttyVt vt;
        // Build data with bare \n (old format) — should still work
        QByteArray data = "GHOSTTY_SCROLLBACK_V1\nCOLS=80\nROWS=24\n\n"
                          "line one\nline two\nline three";
        vt.restoreScrollback(data, 80);
    }

    void testEmptyLines()
    {
        GhosttyVt vt;
        QStringList lines = {"first", "", "third", "", "fifth"};
        QByteArray data = buildScrollback(80, 24, lines);
        vt.restoreScrollback(data, 80);
    }

    void testSingleCharacterLines()
    {
        GhosttyVt vt;
        QStringList lines = {"a", "b", "c"};
        QByteArray data = buildScrollback(80, 24, lines);
        vt.restoreScrollback(data, 80);
    }

    void testRewrapUtf8()
    {
        GhosttyVt vt;
        // CJK characters (3 bytes each in UTF-8) — re-wrap must not corrupt
        // 10 CJK chars = 30 bytes but should split on character boundary
        QString cjkLine = QString(10, QChar(0x4E00)); // 10x CJK '一'
        QStringList lines = {cjkLine};
        QByteArray data = buildScrollback(80, 24, lines);
        // Restore at 5 cols — each CJK char is ~2 cols wide, so 10 chars = 20 cols
        // Should split without corrupting UTF-8
        vt.restoreScrollback(data, 5);
    }
};

QTEST_GUILESS_MAIN(TestScrollbackFormat)
#include "tst_scrollback_format.moc"
