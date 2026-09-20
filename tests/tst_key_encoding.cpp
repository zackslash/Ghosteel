#include <QtTest>

#include "ghosttyvt.h"
#include "ghostty_stubs.h"

// Contract tests for GhosttyVt::encodeKeyEvent against the stub recorder.
// These pin the key-event fields the real ghostty encoder depends on:
//   1. text keys held with Ctrl/Alt/Super must set the unshifted codepoint,
//      or kitty-protocol apps (fish 4, neovim) receive the bare utf8 text
//      with the modifier dropped;
//   2. C0/DEL bytes must never be passed as utf8 (the C API forbids them);
//      under Ctrl the key's base character is synthesized instead. Kernel
//      ctrl tables encode the key's shifted glyph, so the byte cannot be
//      trusted to identify the key; key identity wins, matching the
//      encoder's own ctrlSeq fallback to the logical key.

class TestKeyEncoding : public QObject
{
    Q_OBJECT

private slots:
    // Sticky-Ctrl + VKB 'c' path: printable text plus ctrl must both carry
    // the text and set the codepoint so kitty encoding produces CSI 99;5u.
    void testCtrlLetterSetsUnshiftedCodepoint()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_C, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "c", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.key, GHOSTTY_KEY_C);
        QCOMPARE(ev.mods, static_cast<GhosttyMods>(GHOSTTY_MODS_CTRL));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('c'));
        QCOMPARE(ev.utf8_len, static_cast<size_t>(1));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("c"));
    }

    // Hardware Ctrl+C path: Qt delivers the C0 byte as text. It must be
    // mapped back to the printable letter and never passed through as
    // the control byte.
    void testCtrlLetterControlTextSynthesizesLetter()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_C, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "\x03", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('c'));
        QCOMPARE(ev.utf8_len, static_cast<size_t>(1));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("c"));
    }

    // Hardware Ctrl+Shift+X ("\x18"): without the synthesized letter the
    // legacy encoder writes nothing for this combo; with it bash-classic
    // 0x18 reaches the app.
    void testCtrlShiftLetterControlTextSynthesizesLetter()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_X, GHOSTTY_KEY_ACTION_PRESS,
                          static_cast<GhosttyMods>(GHOSTTY_MODS_CTRL
                                                   | GHOSTTY_MODS_SHIFT),
                          "\x18", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('x'));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("x"));
    }

    void testPlainLetterKeepsTextPath()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_C, GHOSTTY_KEY_ACTION_PRESS,
                          0, "c", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>(0));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("c"));
    }

    // Shift-only typing also stays on the raw-text path; setting the
    // codepoint here would CSI-u-encode uppercase letters in kitty apps.
    void testShiftLetterKeepsTextPath()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_C, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_SHIFT, "C", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>(0));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("C"));
    }

    void testCtrlFunctionKeySetsNoCodepoint()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_ARROW_LEFT, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, nullptr, 0);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.key, GHOSTTY_KEY_ARROW_LEFT);
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>(0));
        QCOMPARE(ev.utf8_len, static_cast<size_t>(0));
    }

    // Verifies the gate covers ALT, not just CTRL (kitty CSI 120;3u).
    void testAltLetterSetsUnshiftedCodepoint()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_X, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_ALT, "x", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('x'));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("x"));
    }

    // Hardware Enter arrives with text "\r"; the C0 byte must not reach
    // utf8. The actual \r encoding is table-driven in the engine.
    void testEnterControlTextDropsUtf8()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_ENTER, GHOSTTY_KEY_ACTION_PRESS,
                          0, "\r", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.key, GHOSTTY_KEY_ENTER);
        QCOMPARE(ev.utf8_len, static_cast<size_t>(0));
    }

    // DEL is excluded alongside C0 by the API contract; under Ctrl it
    // synthesizes the key's base character like any control byte.
    void testCtrlDelTextSynthesizesBaseKey()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_C, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "\x7f", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('c'));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("c"));
    }

    // The dead-key case this guards against: kernel ctrl tables map
    // minus/underscore to 0x1F (the shifted glyph's code), and '-' has no
    // C0 encoding, so without synthesis legacy apps receive nothing for
    // Ctrl+Minus. The encoder emits the fixterms CSI 45;5u from the base
    // character instead.
    void testMinusControlTextSynthesizesBaseKey()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_MINUS, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "\x1f", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('-'));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("-"));
    }

    // Two-byte ESC-prefixed control text (Ctrl+Alt+letter) also
    // synthesizes the base character; the encoder re-derives the Alt ESC
    // prefix from the mods, which evdev reports reliably.
    void testCtrlAltTwoByteControlTextSynthesizesBaseKey()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_C, GHOSTTY_KEY_ACTION_PRESS,
                          static_cast<GhosttyMods>(GHOSTTY_MODS_CTRL
                                                   | GHOSTTY_MODS_ALT),
                          "\x1b\x03", 2);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('c'));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("c"));
    }

    // Multi-byte UTF-8 (first byte >= 0xC0) passes the filter unchanged;
    // pins the unsigned-char comparison.
    void testMultiByteTextKept()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_C, GHOSTTY_KEY_ACTION_PRESS,
                          0, "\xc3\xa9", 2);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.utf8_len, static_cast<size_t>(2));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("\xc3\xa9"));
    }

    // The pass filter's lower boundary: 0x20 (space) must pass, a strict >
    // comparison here would kill the space key in every app.
    void testSpaceTextKept()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_SPACE, GHOSTTY_KEY_ACTION_PRESS,
                          0, " ", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.utf8_len, static_cast<size_t>(1));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray(" "));
    }

    // SUPER also arms the codepoint gate (Meta+letter via Qt::Meta).
    void testSuperLetterSetsUnshiftedCodepoint()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_X, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_SUPER, "x", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('x'));
    }

    // Ctrl+Enter: the control byte is in range but the key is functional,
    // so nothing may be synthesized into utf8.
    void testCtrlEnterControlTextNotSynthesized()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_ENTER, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "\r", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>(0));
        QCOMPARE(ev.utf8_len, static_cast<size_t>(0));
    }

    // Synthesis range boundaries: Ctrl+A and Ctrl+Z (0x01, 0x1A).
    void testSynthesisRangeBoundaries()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_A, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "\x01", 1);
        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("a"));

        vt.encodeKeyEvent(GHOSTTY_KEY_Z, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "\x1a", 1);
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("z"));
    }

    // A control byte inconsistent with the key's base character is still
    // synthesized from the key: kernel ctrl tables encode the shifted
    // glyph, so mismatches are mapping artifacts, not a different key.
    void testMismatchedControlByteSynthesizesBaseKey()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_C, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "\x1b", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("c"));
    }

    // Ctrl+[ (text "\x1b", key BRACKET_LEFT): the mask check recovers the
    // bracket, which the letters-only range could not.
    void testBracketControlTextSynthesizes()
    {
        ghostty_stubs_reset_key_event();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        vt.encodeKeyEvent(GHOSTTY_KEY_BRACKET_LEFT, GHOSTTY_KEY_ACTION_PRESS,
                          GHOSTTY_MODS_CTRL, "\x1b", 1);

        GhosttyStubKeyEvent ev;
        QVERIFY(ghostty_stubs_last_key_event(&ev));
        QCOMPARE(ev.unshifted_codepoint, static_cast<uint32_t>('['));
        QCOMPARE(QByteArray(ev.utf8, ev.utf8_len), QByteArray("["));
    }
};

QTEST_MAIN(TestKeyEncoding)
#include "tst_key_encoding.moc"
