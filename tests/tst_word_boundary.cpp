#include <QtTest>
#include "textutil.h"

class TestWordBoundary : public QObject
{
    Q_OBJECT

private slots:
    // --- isWordChar ---

    void testAsciiLetters()
    {
        QVERIFY(TextUtil::isWordChar('a'));
        QVERIFY(TextUtil::isWordChar('Z'));
        QVERIFY(TextUtil::isWordChar('m'));
    }

    void testDigits()
    {
        QVERIFY(TextUtil::isWordChar('0'));
        QVERIFY(TextUtil::isWordChar('9'));
        QVERIFY(TextUtil::isWordChar('5'));
    }

    void testUnderscore()
    {
        QVERIFY(TextUtil::isWordChar('_'));
    }

    void testSpace()
    {
        QVERIFY(!TextUtil::isWordChar(' '));
    }

    void testTab()
    {
        QVERIFY(!TextUtil::isWordChar('\t'));
    }

    void testPunctuation()
    {
        QVERIFY(!TextUtil::isWordChar('.'));
        QVERIFY(!TextUtil::isWordChar(','));
        QVERIFY(!TextUtil::isWordChar(';'));
        QVERIFY(!TextUtil::isWordChar(':'));
        QVERIFY(!TextUtil::isWordChar('!'));
        QVERIFY(!TextUtil::isWordChar('?'));
        QVERIFY(!TextUtil::isWordChar('('));
        QVERIFY(!TextUtil::isWordChar(')'));
        QVERIFY(!TextUtil::isWordChar('{'));
        QVERIFY(!TextUtil::isWordChar('}'));
        QVERIFY(!TextUtil::isWordChar('['));
        QVERIFY(!TextUtil::isWordChar(']'));
    }

    void testOperators()
    {
        QVERIFY(!TextUtil::isWordChar('+'));
        QVERIFY(!TextUtil::isWordChar('-'));
        QVERIFY(!TextUtil::isWordChar('='));
        QVERIFY(!TextUtil::isWordChar('<'));
        QVERIFY(!TextUtil::isWordChar('>'));
        QVERIFY(!TextUtil::isWordChar('*'));
        QVERIFY(!TextUtil::isWordChar('/'));
        QVERIFY(!TextUtil::isWordChar('&'));
        QVERIFY(!TextUtil::isWordChar('|'));
        QVERIFY(!TextUtil::isWordChar('^'));
        QVERIFY(!TextUtil::isWordChar('~'));
        QVERIFY(!TextUtil::isWordChar('%'));
    }

    void testPathCharacters()
    {
        QVERIFY(!TextUtil::isWordChar('/'));
        QVERIFY(!TextUtil::isWordChar('\\'));
        QVERIFY(!TextUtil::isWordChar('~'));
    }

    void testQuoteCharacters()
    {
        QVERIFY(!TextUtil::isWordChar('"'));
        QVERIFY(!TextUtil::isWordChar('\''));
        QVERIFY(!TextUtil::isWordChar('`'));
    }

    void testNonAsciiTreatedAsWord()
    {
        // CJK characters (Chinese/Japanese/Korean)
        QVERIFY(TextUtil::isWordChar(0x4E16));  // 世
        QVERIFY(TextUtil::isWordChar(0x754C));  // 界

        // Accented Latin
        QVERIFY(TextUtil::isWordChar(0xE9));    // é
        QVERIFY(TextUtil::isWordChar(0xF1));    // ñ

        // Cyrillic
        QVERIFY(TextUtil::isWordChar(0x0414));  // Д

        // Arabic
        QVERIFY(TextUtil::isWordChar(0x0627));  // ا

        // Emoji
        QVERIFY(TextUtil::isWordChar(0x1F600)); // 😀
    }

    void testBoxDrawingExcluded()
    {
        // Box Drawing (U+2500–U+257F) — common in TUI apps
        QVERIFY(!TextUtil::isWordChar(0x2500)); // ─
        QVERIFY(!TextUtil::isWordChar(0x2502)); // │
        QVERIFY(!TextUtil::isWordChar(0x250C)); // ┌
        QVERIFY(!TextUtil::isWordChar(0x2514)); // └
        QVERIFY(!TextUtil::isWordChar(0x257F)); // last in range
    }

    void testBlockElementsExcluded()
    {
        // Block Elements (U+2580–U+259F) — common in TUI apps
        QVERIFY(!TextUtil::isWordChar(0x2580)); // ▀
        QVERIFY(!TextUtil::isWordChar(0x2588)); // █
        QVERIFY(!TextUtil::isWordChar(0x2590)); // ▐
        QVERIFY(!TextUtil::isWordChar(0x259F)); // last in range
    }

    void testNonWordSymbolRanges()
    {
        // Just outside excluded ranges should be word chars
        QVERIFY(TextUtil::isWordChar(0x25A0));  // ■ (Geometric Shapes)
        QVERIFY(TextUtil::isWordChar(0x24FF));  // ⓿ (just below Box Drawing)
    }

    void testNullCharacter()
    {
        QVERIFY(!TextUtil::isWordChar(0));
    }

    void testAsciiBoundary()
    {
        // Last ASCII char (127 = DEL) — not alphanumeric
        QVERIFY(!TextUtil::isWordChar(127));
        // First non-ASCII char (128) — treated as word
        QVERIFY(TextUtil::isWordChar(128));
    }
};

QTEST_MAIN(TestWordBoundary)
#include "tst_word_boundary.moc"
