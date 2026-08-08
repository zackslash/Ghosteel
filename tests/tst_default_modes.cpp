#include <QtTest>

#include "ghosttyvt.h"
#include "ghostty_stubs.h"

// Regression guard: GhosttyVt::create() must enable the terminal modes
// Ghosteel depends on (runtime mode-sets via the C API). Verifies the
// call is made, not Ghostty's engine behavior — that is upstream's job.

class TestDefaultModes : public QObject
{
    Q_OBJECT

private slots:
    // DEC 2027 (grapheme cluster mode) must be on so VS16 (U+FE0F) makes BMP
    // emoji such as sun/cloud/thundercloud occupy 2 cells. Without this call,
    // emoji-heavy output (e.g. `curl wttr.in/v2`) misaligns columns.
    void testGraphemeClusterEnabled()
    {
        ghostty_stubs_reset_modes();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        bool value = false;
        QVERIFY2(ghostty_stubs_mode_set_called(GHOSTTY_MODE_GRAPHEME_CLUSTER, &value),
                 "create() did not enable grapheme cluster mode (DEC 2027)");
        QVERIFY(value);
    }

    // DEC mode 12 defaults to false; Ghosteel enables cursor blinking.
    void testCursorBlinkingEnabled()
    {
        ghostty_stubs_reset_modes();
        GhosttyVt vt;
        QVERIFY(vt.create(80, 24, [](const char *, size_t) {}));

        bool value = false;
        QVERIFY2(ghostty_stubs_mode_set_called(GHOSTTY_MODE_CURSOR_BLINKING, &value),
                 "create() did not enable cursor blinking mode");
        QVERIFY(value);
    }
};

QTEST_MAIN(TestDefaultModes)
#include "tst_default_modes.moc"
