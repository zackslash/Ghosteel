#include <QtTest>
#include "keymapping.h"

class TestKeyMapping : public QObject
{
    Q_OBJECT

private slots:
    // --- mapQtKey: Letters ---
    void testMapLetters_data()
    {
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<int>("expected");

        for (char c = 'a'; c <= 'z'; c++) {
            int qtKey = Qt::Key_A + (c - 'a');
            int ghosttyKey = GHOSTTY_KEY_A + (c - 'a');
            QTest::newRow(qPrintable(QString("Key_%1").arg(QChar(c).toUpper())))
                << qtKey << ghosttyKey;
        }
    }
    void testMapLetters()
    {
        QFETCH(int, qtKey);
        QFETCH(int, expected);
        QCOMPARE(static_cast<int>(KeyMapping::mapQtKey(qtKey)), expected);
    }

    // --- mapQtKey: Digits ---
    void testMapDigits_data()
    {
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<int>("expected");
        for (char c = '0'; c <= '9'; c++) {
            int qtKey = Qt::Key_0 + (c - '0');
            int ghosttyKey = GHOSTTY_KEY_DIGIT_0 + (c - '0');
            QTest::newRow(qPrintable(QString("Key_%1").arg(c)))
                << qtKey << ghosttyKey;
        }
    }
    void testMapDigits()
    {
        QFETCH(int, qtKey);
        QFETCH(int, expected);
        QCOMPARE(static_cast<int>(KeyMapping::mapQtKey(qtKey)), expected);
    }

    // --- mapQtKey: Special keys ---
    void testSpecialKeys()
    {
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Return), GHOSTTY_KEY_ENTER);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Enter), GHOSTTY_KEY_ENTER);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Backspace), GHOSTTY_KEY_BACKSPACE);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Tab), GHOSTTY_KEY_TAB);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Escape), GHOSTTY_KEY_ESCAPE);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Space), GHOSTTY_KEY_SPACE);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Delete), GHOSTTY_KEY_DELETE);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Insert), GHOSTTY_KEY_INSERT);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Home), GHOSTTY_KEY_HOME);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_End), GHOSTTY_KEY_END);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_PageUp), GHOSTTY_KEY_PAGE_UP);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_PageDown), GHOSTTY_KEY_PAGE_DOWN);
    }

    // --- mapQtKey: Arrow keys ---
    void testArrowKeys()
    {
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Up), GHOSTTY_KEY_ARROW_UP);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Down), GHOSTTY_KEY_ARROW_DOWN);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Left), GHOSTTY_KEY_ARROW_LEFT);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Right), GHOSTTY_KEY_ARROW_RIGHT);
    }

    // --- mapQtKey: Function keys ---
    void testFunctionKeys_data()
    {
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<int>("expected");
        for (int i = 1; i <= 12; i++) {
            QTest::newRow(qPrintable(QString("F%1").arg(i)))
                << (Qt::Key_F1 + i - 1) << (int)(GHOSTTY_KEY_F1 + i - 1);
        }
    }
    void testFunctionKeys()
    {
        QFETCH(int, qtKey);
        QFETCH(int, expected);
        QCOMPARE(static_cast<int>(KeyMapping::mapQtKey(qtKey)), expected);
    }

    // --- mapQtKey: Punctuation ---
    void testPunctuationKeys()
    {
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Minus), GHOSTTY_KEY_MINUS);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Equal), GHOSTTY_KEY_EQUAL);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_BracketLeft), GHOSTTY_KEY_BRACKET_LEFT);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_BracketRight), GHOSTTY_KEY_BRACKET_RIGHT);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Backslash), GHOSTTY_KEY_BACKSLASH);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Semicolon), GHOSTTY_KEY_SEMICOLON);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Apostrophe), GHOSTTY_KEY_QUOTE);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Comma), GHOSTTY_KEY_COMMA);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Period), GHOSTTY_KEY_PERIOD);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_Slash), GHOSTTY_KEY_SLASH);
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_QuoteLeft), GHOSTTY_KEY_BACKQUOTE);
    }

    // --- mapQtKey: Unknown key ---
    void testUnknownKey()
    {
        QCOMPARE(KeyMapping::mapQtKey(Qt::Key_F35), GHOSTTY_KEY_UNIDENTIFIED);
        QCOMPARE(KeyMapping::mapQtKey(0), GHOSTTY_KEY_UNIDENTIFIED);
        QCOMPARE(KeyMapping::mapQtKey(99999), GHOSTTY_KEY_UNIDENTIFIED);
    }

    // --- mapQtModifiers ---
    void testNoModifiers()
    {
        QCOMPARE(KeyMapping::mapQtModifiers(Qt::NoModifier), static_cast<GhosttyMods>(0));
    }

    void testShiftModifier()
    {
        GhosttyMods mods = KeyMapping::mapQtModifiers(Qt::ShiftModifier);
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_SHIFT), static_cast<int>(GHOSTTY_MODS_SHIFT));
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_CTRL), 0);
    }

    void testCtrlModifier()
    {
        GhosttyMods mods = KeyMapping::mapQtModifiers(Qt::ControlModifier);
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_CTRL), static_cast<int>(GHOSTTY_MODS_CTRL));
    }

    void testAltModifier()
    {
        GhosttyMods mods = KeyMapping::mapQtModifiers(Qt::AltModifier);
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_ALT), static_cast<int>(GHOSTTY_MODS_ALT));
    }

    void testMetaModifier()
    {
        GhosttyMods mods = KeyMapping::mapQtModifiers(Qt::MetaModifier);
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_SUPER), static_cast<int>(GHOSTTY_MODS_SUPER));
    }

    void testCombinedModifiers()
    {
        GhosttyMods mods = KeyMapping::mapQtModifiers(Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_CTRL), static_cast<int>(GHOSTTY_MODS_CTRL));
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_SHIFT), static_cast<int>(GHOSTTY_MODS_SHIFT));
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_ALT), 0);
    }

    void testAllModifiers()
    {
        GhosttyMods mods = KeyMapping::mapQtModifiers(
            Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_SHIFT), static_cast<int>(GHOSTTY_MODS_SHIFT));
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_CTRL), static_cast<int>(GHOSTTY_MODS_CTRL));
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_ALT), static_cast<int>(GHOSTTY_MODS_ALT));
        QCOMPARE(static_cast<int>(mods & GHOSTTY_MODS_SUPER), static_cast<int>(GHOSTTY_MODS_SUPER));
    }

    // --- mapCharToKey: Lowercase letters ---
    void testCharLetters_data()
    {
        QTest::addColumn<QChar>("ch");
        QTest::addColumn<int>("expected");
        for (char c = 'a'; c <= 'z'; c++) {
            QTest::newRow(qPrintable(QString(c)))
                << QChar(c) << static_cast<int>(GHOSTTY_KEY_A + (c - 'a'));
        }
    }
    void testCharLetters()
    {
        QFETCH(QChar, ch);
        QFETCH(int, expected);
        QCOMPARE(static_cast<int>(KeyMapping::mapCharToKey(ch)), expected);
    }

    // --- mapCharToKey: Uppercase letters (should lowercase) ---
    void testCharUppercaseLetters()
    {
        QCOMPARE(KeyMapping::mapCharToKey(QChar('A')), GHOSTTY_KEY_A);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('Z')), GHOSTTY_KEY_Z);
    }

    // --- mapCharToKey: Digits ---
    void testCharDigits_data()
    {
        QTest::addColumn<QChar>("ch");
        QTest::addColumn<int>("expected");
        for (char c = '0'; c <= '9'; c++) {
            QTest::newRow(qPrintable(QString(c)))
                << QChar(c) << static_cast<int>(GHOSTTY_KEY_DIGIT_0 + (c - '0'));
        }
    }
    void testCharDigits()
    {
        QFETCH(QChar, ch);
        QFETCH(int, expected);
        QCOMPARE(static_cast<int>(KeyMapping::mapCharToKey(ch)), expected);
    }

    // --- mapCharToKey: Punctuation ---
    void testCharPunctuation()
    {
        QCOMPARE(KeyMapping::mapCharToKey(QChar('-')), GHOSTTY_KEY_MINUS);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('=')), GHOSTTY_KEY_EQUAL);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('[')), GHOSTTY_KEY_BRACKET_LEFT);
        QCOMPARE(KeyMapping::mapCharToKey(QChar(']')), GHOSTTY_KEY_BRACKET_RIGHT);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('\\')), GHOSTTY_KEY_BACKSLASH);
        QCOMPARE(KeyMapping::mapCharToKey(QChar(';')), GHOSTTY_KEY_SEMICOLON);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('\'')), GHOSTTY_KEY_QUOTE);
        QCOMPARE(KeyMapping::mapCharToKey(QChar(',')), GHOSTTY_KEY_COMMA);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('.')), GHOSTTY_KEY_PERIOD);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('/')), GHOSTTY_KEY_SLASH);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('`')), GHOSTTY_KEY_BACKQUOTE);
        QCOMPARE(KeyMapping::mapCharToKey(QChar(' ')), GHOSTTY_KEY_SPACE);
    }

    // --- mapCharToKey: Unknown characters ---
    void testCharUnknown()
    {
        QCOMPARE(KeyMapping::mapCharToKey(QChar('@')), GHOSTTY_KEY_UNIDENTIFIED);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('#')), GHOSTTY_KEY_UNIDENTIFIED);
        QCOMPARE(KeyMapping::mapCharToKey(QChar('!')), GHOSTTY_KEY_UNIDENTIFIED);
    }

    // --- mapCharToKey: Non-ASCII Unicode ---
    void testCharNonAscii()
    {
        QCOMPARE(KeyMapping::mapCharToKey(QChar(0x00E4)), GHOSTTY_KEY_UNIDENTIFIED); // ä
        QCOMPARE(KeyMapping::mapCharToKey(QChar(0x4E16)), GHOSTTY_KEY_UNIDENTIFIED); // 世
        QCOMPARE(KeyMapping::mapCharToKey(QChar(0xD83D)), GHOSTTY_KEY_UNIDENTIFIED); // surrogate half
    }
};

QTEST_MAIN(TestKeyMapping)
#include "tst_key_mapping.moc"
