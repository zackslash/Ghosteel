#include <QtTest>
#include <QByteArray>
#include <QStringList>
#include <vector>
#include <string>
#include "ghosttyvt.h"
#include "ghostty_stubs.h"

// NOTE: These tests only verify restoreScrollback() doesn't crash — the Ghostty
// stubs don't process VT writes, so terminal state can't be asserted.

// RAII guard for the opt-in canned grid fixture (see ghostty_stubs.h). Arms the
// fixture for the lifetime of the guard so a failed assertion cannot leak it to
// other tests.
class GridFixture
{
public:
    GridFixture(uint16_t cols, uint16_t rows,
                const QStringList &text, const QList<bool> &cont)
    {
        Q_ASSERT(text.size() == rows);
        Q_ASSERT(cont.size() == rows);
        std::vector<const char*> cstr;
        std::vector<std::string> storage;
        // reserve() before collecting c_str() pointers is load-bearing: it
        // guarantees no reallocation (the Q_ASSERT above is debug-only) so
        // pointers taken from storage.back() stay valid through the loop.
        storage.reserve(rows);
        cstr.reserve(rows);
        for (int r = 0; r < rows; r++) {
            storage.push_back(text[r].toStdString());
            cstr.push_back(storage.back().c_str());
        }
        // std::vector<bool> has no data(); use char 0/1 flags instead.
        std::vector<char> contVec;
        contVec.reserve(rows);
        for (int r = 0; r < rows; r++)
            contVec.push_back(cont[r] ? 1 : 0);
        ghostty_stubs_set_grid(cols, rows, cstr.data(), contVec.data());
    }

    ~GridFixture()
    {
        ghostty_stubs_clear_grid();
    }
};

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

    // --- extractSearchText / cross-wrap search-match splitting ---
    // The Ghostty stubs report an empty grid (zero rows/cols) and ignore VT
    // writes, so extractSearchText can never produce content here. The
    // cross-wrap match-splitting behavior is therefore pinned on the pure
    // text-math splitter (GhosttyVt::splitSearchMatch), using the exact data
    // shapes extractSearchText produces (per-row physical lines/mappings plus
    // the joined logical lines).

    void testExtractSearchTextEmpty()
    {
        GhosttyVt vt;
        // Uncreated terminal — must return a well-formed empty result and not
        // crash.
        VtSearchText st = vt.extractSearchText();
        QVERIFY(st.lines.isEmpty());
        QVERIFY(st.mapping.isEmpty());
        QVERIFY(st.logicalLines.isEmpty());
        QVERIFY(st.logicalLineStartRow.isEmpty());
        QVERIFY(st.logicalLineRowCount.isEmpty());

        // Created-but-empty terminal (stub grid reports no rows): same shape.
        vt.create(40, 10, [](const char *, size_t) {});
        st = vt.extractSearchText();
        QVERIFY(st.lines.isEmpty());
        QVERIFY(st.logicalLines.isEmpty());
        vt.destroy();
    }

    // --- exportScrollback wrap-boundary space preservation ---
    // These pin the wrap-boundary flush behavior: a row that wraps (WRAP_CONTINUATION
    // set on the row AFTER the wrap) is full-width by construction, so a space
    // in its final cell is a real inter-word space that must survive the join.
    // Only the logical-line END is trimmed at flush time.

    // Returns the payload after the "GHOSTTY_SCROLLBACK_V1\nCOLS=..\nROWS=..\n\n"
    // header, with any trailing \r\n stripped (defensive — exportScrollback
    // already strips them itself).
    // Returns empty if no header is present (caller should treat as failure).
    static QByteArray exportPayload(GhosttyVt &vt)
    {
        uint16_t cols = 0, rows = 0;
        QByteArray data = vt.exportScrollback(cols, rows);
        int headerEnd = data.indexOf("\n\n");
        if (headerEnd < 0)
            return QByteArray();
        QByteArray payload = data.mid(headerEnd + 2);
        while (payload.endsWith("\r\n"))
            payload.chop(2);
        return payload;
    }

    void testExportScrollbackPreservesWrapBoundarySpaces()
    {
        // cols=6, 3 rows: row0 "ab cde" (no cont), row1 "f ghi " (cont, trailing
        // space in last cell), row2 "jk" (cont). The row1 trailing space is a
        // real inter-word space between "ghi" and "jk" (the row1->row2 wrap
        // join) and must survive; the row0->row1 join ("cdef") correctly has
        // none — the fixture's rows abut there without a space.
        QStringList text = {"ab cde", "f ghi ", "jk"};
        QList<bool> cont = {false, true, true};
        GridFixture fixture(6, 3, text, cont);

        GhosttyVt vt;
        vt.create(6, 3, [](const char *, size_t) {});
        QCOMPARE(exportPayload(vt), QByteArray("ab cdef ghi jk"));
        vt.destroy();
    }

    void testExportScrollbackFirstRowTrailingSpace()
    {
        // cols=6, 2 rows: row0 "ab cd " (no cont, trailing space — the FIRST row
        // of a logical line that wraps), row1 "ef gh" (cont). The row0 trailing
        // space is a real inter-word space; the flush-time trim removes only the
        // line END, so it survives.
        QStringList text = {"ab cd ", "ef gh"};
        QList<bool> cont = {false, true};
        GridFixture fixture(6, 2, text, cont);

        GhosttyVt vt;
        vt.create(6, 2, [](const char *, size_t) {});
        QCOMPARE(exportPayload(vt), QByteArray("ab cd ef gh"));
        vt.destroy();
    }

    void testExportScrollbackAllSpacesLineDropped()
    {
        // cols=6, 1 row of all spaces (no cont). The flush-time trim removes
        // the trailing spaces, leaving an empty line, which is dropped — the
        // same as the pre-existing empty-flush behavior.
        QStringList text = {"      "};
        QList<bool> cont = {false};
        GridFixture fixture(6, 1, text, cont);

        GhosttyVt vt;
        vt.create(6, 1, [](const char *, size_t) {});
        QCOMPARE(exportPayload(vt), QByteArray());
        vt.destroy();
    }

    void testSplitSearchMatchSingleRow()
    {
        // No continuation: one logical line == one physical row.
        VtSearchText st;
        st.lines << "hello world";
        st.mapping.append(QVector<int>{0,1,2,3,4,5,6,7,8,9,10});
        st.logicalLines << "hello world";
        st.logicalLineStartRow = {0};
        st.logicalLineRowCount = {1};

        auto segs = GhosttyVt::splitSearchMatch(st, 0, 6, 5);
        QCOMPARE(segs.size(), 1);
        QCOMPARE(segs[0].row, 0);
        QCOMPARE(segs[0].cellCol, 6);
        QCOMPARE(segs[0].cellWidth, 5);
    }

    void testSplitSearchMatchAcrossWrap()
    {
        // Autowrap continuation: rows "hello wor" / "ld" join into one
        // logical line "hello world". The pattern "world" spans the boundary
        // and must be split into one segment per physical row.
        VtSearchText st;
        st.lines << "hello wor" << "ld";
        st.mapping.append(QVector<int>{0,1,2,3,4,5,6,7,8});
        st.mapping.append(QVector<int>{0,1});
        st.logicalLines << "hello world";
        st.logicalLineStartRow = {0};
        st.logicalLineRowCount = {2};

        // "world" starts at joined index 6, spans row 0 (cells 6-8 = "wor")
        // and row 1 (cells 0-1 = "ld").
        auto segs = GhosttyVt::splitSearchMatch(st, 0, 6, 5);
        QCOMPARE(segs.size(), 2);
        QCOMPARE(segs[0].row, 0);
        QCOMPARE(segs[0].cellCol, 6);
        QCOMPARE(segs[0].cellWidth, 3);
        QCOMPARE(segs[1].row, 1);
        QCOMPARE(segs[1].cellCol, 0);
        QCOMPARE(segs[1].cellWidth, 2);
    }

    void testSplitSearchMatchBoundaryStart()
    {
        // Match starts exactly at a join boundary — must land on the next row.
        VtSearchText st;
        st.lines << "abc" << "def";
        st.mapping.append(QVector<int>{0,1,2});
        st.mapping.append(QVector<int>{0,1,2});
        st.logicalLines << "abcdef";
        st.logicalLineStartRow = {0};
        st.logicalLineRowCount = {2};

        auto segs = GhosttyVt::splitSearchMatch(st, 0, 3, 2);
        QCOMPARE(segs.size(), 1);
        QCOMPARE(segs[0].row, 1);
        QCOMPARE(segs[0].cellCol, 0);
        QCOMPARE(segs[0].cellWidth, 2);
    }

    void testSplitSearchMatchWideCharSpacer()
    {
        // Row "ab" + CJK 中 (2 cells: head at col 2, spacer at col 3 with -1)
        // + "c" at col 4. The segment covering the wide char must include the
        // trailing spacer cell, and the spacer (-1) can never be picked as a
        // segment start.
        QString line0 = QStringLiteral("ab");
        line0.append(QChar(0x4E00)); // 中 — 2 cells wide
        line0.append(QLatin1Char('c'));
        VtSearchText st;
        st.lines << line0;
        st.mapping.append(QVector<int>{0, 1, 2, -1, 3});
        st.logicalLines << line0;
        st.logicalLineStartRow = {0};
        st.logicalLineRowCount = {1};

        // Pattern "b中" (QChar offsets 1..3) -> cells 1,2,3 (b, head, spacer).
        auto segs = GhosttyVt::splitSearchMatch(st, 0, 1, 2);
        QCOMPARE(segs.size(), 1);
        QCOMPARE(segs[0].row, 0);
        QCOMPARE(segs[0].cellCol, 1);
        QCOMPARE(segs[0].cellWidth, 3);

        // Pattern at the wide char's head (offset 2) -> cell 2, width 2 (the
        // spacer is counted so the highlight covers both cells).
        auto segs2 = GhosttyVt::splitSearchMatch(st, 0, 2, 1);
        QCOMPARE(segs2.size(), 1);
        QCOMPARE(segs2[0].cellCol, 2);
        QCOMPARE(segs2[0].cellWidth, 2);
    }

    void testSplitSearchMatchKeepsTrailingSpaces()
    {
        // No-trim pin: a row containing a wide char is genuinely shorter than
        // the grid width (2 cells, 1 QChar), but its full text — including
        // genuine trailing spaces — still contributes to the logical join.
        // cols = 7: row 0 = "ab" + 中 (cells 2-3) + "x  " (cells 4-6), text
        // "ab中x  " (6 QChars); row 1 = "y". Logical join = "ab中x  y".
        VtSearchText st;
        QString line0 = QStringLiteral("ab");
        line0.append(QChar(0x4E00)); // 中 — 2 cells wide
        line0.append(QLatin1Char('x'));
        line0.append(QLatin1Char(' '));
        line0.append(QLatin1Char(' '));
        st.lines << line0 << "y";
        st.mapping.append(QVector<int>{0, 1, 2, -1, 3, 4, 5});
        st.mapping.append(QVector<int>{0});
        st.logicalLines << line0 + QLatin1Char('y');
        st.logicalLineStartRow = {0};
        st.logicalLineRowCount = {2};

        // "x  y" starts at joined index 3; the two spaces are genuine row-0
        // content that must survive in the join. Spans row 0 (QChars 3-5 =
        // "x  ", cells 4-6) and row 1 ("y", cell 0).
        auto segs = GhosttyVt::splitSearchMatch(st, 0, 3, 4);
        QCOMPARE(segs.size(), 2);
        QCOMPARE(segs[0].row, 0);
        QCOMPARE(segs[0].cellCol, 4);
        QCOMPARE(segs[0].cellWidth, 3);
        QCOMPARE(segs[1].row, 1);
        QCOMPARE(segs[1].cellCol, 0);
        QCOMPARE(segs[1].cellWidth, 1);
    }

    void testSplitSearchMatchInvalidInput()
    {
        VtSearchText st;
        st.lines << "abc";
        st.mapping.append(QVector<int>{0,1,2});
        st.logicalLines << "abc";
        st.logicalLineStartRow = {0};
        st.logicalLineRowCount = {1};

        // Out-of-range logical line, match past the joined text end, and zero
        // pattern length all yield no segments (and must not crash).
        QVERIFY(GhosttyVt::splitSearchMatch(st, 5, 0, 1).isEmpty());
        QVERIFY(GhosttyVt::splitSearchMatch(st, 0, 2, 2).isEmpty());
        QVERIFY(GhosttyVt::splitSearchMatch(st, 0, 0, 0).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestScrollbackFormat)
#include "tst_scrollback_format.moc"
