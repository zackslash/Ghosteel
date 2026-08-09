#include <QtTest>
#include <QString>
#include "terminalwidth.h"

// Unit tests for terminal character-cell width (UAX #11).
// Covers Narrow Latin-1/Cyrillic/Greek, Ambiguous box-drawing,
// Wide CJK/Hangul/fullwidth/astral emoji, and zero-width combining marks.

class TestTerminalWidth : public QObject
{
    Q_OBJECT

private slots:
    // --- Narrow scripts ---
    void asciiIsNarrow() {
        QCOMPARE(terminalCharWidth(uint('a')), 1);
        QCOMPARE(terminalCharWidth(uint(' ')), 1);
        QCOMPARE(terminalCharWidth(uint('0')), 1);
        QCOMPARE(terminalCharWidth(uint('~')), 1);
    }
    void latinIsNarrow() {
        QCOMPARE(terminalCharWidth(0x00E9), 1); // é
        QCOMPARE(terminalCharWidth(0x00F1), 1); // ñ
        QCOMPARE(terminalCharWidth(0x00C6), 1); // Æ
        QCOMPARE(terminalCharWidth(0x00E0), 1); // à
    }
    void cyrillicIsNarrow() {
        QCOMPARE(terminalCharWidth(0x0410), 1); // А
        QCOMPARE(terminalCharWidth(0x044F), 1); // я
        QCOMPARE(terminalCharWidth(0x0401), 1); // Ё
    }
    void greekIsNarrow() {
        QCOMPARE(terminalCharWidth(0x0391), 1); // Α
        QCOMPARE(terminalCharWidth(0x03C9), 1); // ω
    }
    void boxDrawingIsNarrow() {
        // East Asian Ambiguous -> 1 cell in non-CJK locale (SailfishOS default).
        QCOMPARE(terminalCharWidth(0x2500), 1); // ─
        QCOMPARE(terminalCharWidth(0x2551), 1); // ║
        QCOMPARE(terminalCharWidth(0x2569), 1); // ╩
    }
    void softHyphenIsNarrow() {
        // U+00AD is explicitly excluded from the combining table -> width 1.
        QCOMPARE(terminalCharWidth(0x00AD), 1);
    }

    // --- Wide (2-cell) ---
    void cjkIsWide() {
        QCOMPARE(terminalCharWidth(0x4E00), 2); // 一
        QCOMPARE(terminalCharWidth(0x65E5), 2); // 日
        QCOMPARE(terminalCharWidth(0x3042), 2); // あ (Hiragana)
    }
    void hangulIsWide() {
        QCOMPARE(terminalCharWidth(0xAC00), 2); // 가
    }
    void fullwidthIsWide() {
        QCOMPARE(terminalCharWidth(0xFF21), 2); // Ａ
    }
    void astralEmojiIsWide() {
        QCOMPARE(terminalCharWidth(0x1F600), 2); // 😀
        QCOMPARE(terminalCharWidth(0x1F680), 2); // 🚀
    }

    // --- Zero-width ---
    void combiningIsZero() {
        QCOMPARE(terminalCharWidth(0x0301), 0); // combining acute
        QCOMPARE(terminalCharWidth(0x0300), 0); // combining grave
        QCOMPARE(terminalCharWidth(0x200B), 0); // zero-width space
    }
    void controlIsZero() {
        QCOMPARE(terminalCharWidth(0x0000), 0);
        QCOMPARE(terminalCharWidth(0x0009), 0); // TAB
        QCOMPARE(terminalCharWidth(0x001B), 0); // ESC
        QCOMPARE(terminalCharWidth(0x007F), 0); // DEL
    }

    // --- String-level (surrogate pair + combining handling) ---
    void stringWidthEmpty() {
        QCOMPARE(terminalStringWidth(QString()), 0);
        QCOMPARE(terminalStringWidth(QString("")), 0);
    }
    void stringWidthSurrogates() {
        // "a😀b" -> 1 + 2 + 1 = 4. (Concatenate literals so the hex escape
        // doesn't greedily consume the trailing 'b' as a hex digit.)
        QString s = QString::fromUtf8("a\xF0\x9F\x98\x80" "b");
        QCOMPARE(terminalStringWidth(s), 4);
    }
    void stringWidthCombining() {
        // Precomposed é (U+00E9) -> 1
        QCOMPARE(terminalStringWidth(QString::fromUtf8("\xC3\xA9")), 1);
        // Decomposed "e" + combining acute (U+0301) -> 1 + 0 = 1
        QCOMPARE(terminalStringWidth(QString::fromUtf8("e\xCC\x81")), 1);
    }
    void stringWidthMixedLine() {
        // 6 Cyrillic glyphs = 6 cells.
        QCOMPARE(terminalStringWidth(QString::fromUtf8("Привет")), 6);
    }

    // --- Boundary cases on the wide ternary ---
    void halfwidthIsNarrow() {
        // 0xFF61 is just past the fullwidth range 0xFF00-0xFF60 -> narrow.
        QCOMPARE(terminalCharWidth(0xFF61), 1);
    }
    void wideBracketIsWide() {
        // 0x2329 is outside the main CJK range but explicitly wide.
        QCOMPARE(terminalCharWidth(0x2329), 2);
    }
    void powerlineIsNarrow() {
        // U+E0B0 powerline glyph: Private Use Area, not Wide -> 1 cell.
        QCOMPARE(terminalCharWidth(0xE0B0), 1);
    }

    // --- fitToDisplayWidth (prefix that fits N cells) ---
    void fitAsciiExactWidth() {
        QCOMPARE(fitToDisplayWidth(QString("hello"), 5), QString("hello"));
        QCOMPARE(fitToDisplayWidth(QString("hello"), 3), QString("hel"));
        QCOMPARE(fitToDisplayWidth(QString("hello"), 0), QString(""));
    }
    void fitSurrogatePairNotSplit() {
        // "a😀b" capped at 2 cells must return "a", never split the pair.
        QString s = QString::fromUtf8("a\xF0\x9F\x98\x80" "b");
        QCOMPARE(fitToDisplayWidth(s, 2), QString("a"));
        QCOMPARE(fitToDisplayWidth(s, 3), QString("a") + QString::fromUtf8("\xF0\x9F\x98\x80"));
    }
    void fitCyrillicOnePerCell() {
        // "Привет" capped at 3 cells -> "При".
        QCOMPARE(fitToDisplayWidth(QString::fromUtf8("Привет"), 3),
                 QString::fromUtf8("При"));
    }
};

QTEST_GUILESS_MAIN(TestTerminalWidth)
#include "tst_terminal_width.moc"
