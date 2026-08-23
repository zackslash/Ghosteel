#include <QtTest>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QDir>

#include "settings.h"

class TestSettingsBehavior : public QObject
{
    Q_OBJECT

private:
    static constexpr int DEBOUNCE_WAIT_MS = 600;

private slots:
    void initTestCase()
    {
        // QCoreApplication is created by QTEST_MAIN
    }

    // --- Font size ---

    void testFontSizeStoresExactValue()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setFontSize(3);
        QCOMPARE(s.fontSize(), 3);
        s.setFontSize(100);
        QCOMPARE(s.fontSize(), 100);
    }

    void testFontSizeExactBounds()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setFontSize(6);
        QCOMPARE(s.fontSize(), 6);
        s.setFontSize(32);
        QCOMPARE(s.fontSize(), 32);
    }

    void testFontSizeNoOpSuppressesSignal()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setFontSize(18); // set to default
        QSignalSpy spy(&s, &Settings::fontSizeChanged);
        s.setFontSize(18); // same value — should be no-op
        QCOMPARE(spy.count(), 0);
    }

    void testFontSizeSignalOnChange()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setFontSize(18); // set default
        QSignalSpy spy(&s, &Settings::fontSizeChanged);
        s.setFontSize(20);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.fontSize(), 20);
    }

    // --- QML property wiring ---

    void testSettingsExposedAsProperties()
    {
        // Direct setter tests bypass the property system; QML does not. A
        // setting missing its Q_PROPERTY (or with NOTIFY miswired) silently
        // breaks QML while every C++ test stays green.
        const char *names[] = {
            "fontSize", "fontFamily", "shellCommand", "colorScheme",
            "followAmbience", "backgroundOpacity", "bellMode",
            "scrollbackPersistence", "scrollbackRetentionDays", "keybarKeys",
            "keybarVisible", "keybarRowBreaks", "cursorTrails", "pinchToZoom",
            "autoHideKeyboardLandscape", "urlAutoDetect", "kittyGraphics",
            "clipboardReadPolicy", "customShaderPath", "shaderPipelineAvailable",
        };
        const QMetaObject *mo = &Settings::staticMetaObject;
        for (const char *name : names) {
            int idx = mo->indexOfProperty(name);
            QVERIFY2(idx >= 0, name);
            QMetaProperty p = mo->property(idx);
            QVERIFY2(p.isReadable() && p.hasNotifySignal(), name);
            // shaderPipelineAvailable is intentionally read-only
            if (qstrcmp(name, "shaderPipelineAvailable") != 0)
                QVERIFY2(p.isWritable(), name);
            QVERIFY2(p.notifySignal().name() == QByteArray(name) + "Changed", name);
        }
    }

    void testFontFamilyPropertySystemRoundTrip()
    {
        // setProperty returns false for an undeclared property — the exact
        // failure mode of the once-missing fontFamily Q_PROPERTY.
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::fontFamilyChanged);
        QVERIFY(s.setProperty("fontFamily", QStringLiteral("Fira Code")));
        QCOMPARE(s.property("fontFamily").toString(), QStringLiteral("Fira Code"));
        QCOMPARE(spy.count(), 1);
        QVERIFY(s.setProperty("fontFamily", QStringLiteral("Fira Code")));
        QCOMPARE(spy.count(), 1); // same-value no-op holds through the property path
    }

    // --- Background opacity ---

    void testOpacityClampLow()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setBackgroundOpacity(-0.5f);
        QCOMPARE(s.backgroundOpacity(), 0.0f);
    }

    void testOpacityClampHigh()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setBackgroundOpacity(1.5f);
        QCOMPARE(s.backgroundOpacity(), 1.0f);
    }

    void testOpacityEpsilonDedup()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setBackgroundOpacity(0.6f);
        QSignalSpy spy(&s, &Settings::backgroundOpacityChanged);
        s.setBackgroundOpacity(0.6005f); // within epsilon
        QCOMPARE(spy.count(), 0);
    }

    void testOpacitySignalOnChange()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setBackgroundOpacity(0.6f); // set to default
        QSignalSpy spy(&s, &Settings::backgroundOpacityChanged);
        s.setBackgroundOpacity(0.8f);
        QCOMPARE(spy.count(), 1);
    }

    // --- Bell mode ---

    void testBellModeClampLow()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setBellMode(-1);
        QCOMPARE(s.bellMode(), 0);
    }

    void testBellModeClampHigh()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setBellMode(99);
        QCOMPARE(s.bellMode(), 3);
    }

    void testBellModeNoOp()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setBellMode(1); // default
        QSignalSpy spy(&s, &Settings::bellModeChanged);
        s.setBellMode(1);
        QCOMPARE(spy.count(), 0);
    }

    // --- Corrupt/legacy non-numeric values ---

    void testFontSizeGarbageFallsBackToDefault()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("font/size", QStringLiteral("garbage"));
            qs.sync();
        }
        Settings s(path);
        QCOMPARE(s.fontSize(), 18);
    }

    void testBellModeGarbageFallsBackToDefault()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("terminal/bellMode", QStringLiteral("garbage"));
            qs.sync();
        }
        Settings s(path);
        QCOMPARE(s.bellMode(), 1);
    }

    void testBackgroundOpacityGarbageFallsBackToDefault()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("terminal/backgroundOpacity", QStringLiteral("garbage"));
            qs.sync();
        }
        Settings s(path);
        QCOMPARE(s.backgroundOpacity(), 0.6f);
    }

    void testScrollbackRetentionDaysGarbageFallsBackToDefault()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("scrollback/retentionDays", QStringLiteral("garbage"));
            qs.sync();
        }
        Settings s(path);
        QCOMPARE(s.scrollbackRetentionDays(), 30);
    }

    void testSessionSortModeGarbageFallsBackToDefault()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("sessions/sortMode", QStringLiteral("garbage"));
            qs.sync();
        }
        Settings s(path);
        QCOMPARE(s.sessionSortMode(), 1); // SortLastUsed
    }

    void testClipboardReadPolicyGarbageFallsBackToDefault()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("terminal/clipboardReadPolicy", QStringLiteral("garbage"));
            qs.sync();
        }
        Settings s(path);
        QCOMPARE(s.clipboardReadPolicy(), 0);
    }

    void testFontSizeBoundaryClampOnLoad()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("font/size", 5);
            qs.sync();
        }
        Settings s(path);
        QCOMPARE(s.fontSize(), 6); // clamped to min
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("font/size", 100);
            qs.sync();
        }
        Settings s2(path);
        QCOMPARE(s2.fontSize(), 32); // clamped to max
    }

    void testBellModeBoundaryClampOnLoad()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("terminal/bellMode", -1);
            qs.sync();
        }
        Settings s(path);
        QCOMPARE(s.bellMode(), 0); // clamped to min
        {
            QSettings qs(path, QSettings::IniFormat);
            qs.setValue("terminal/bellMode", 99);
            qs.sync();
        }
        Settings s2(path);
        QCOMPARE(s2.bellMode(), 3); // clamped to max
    }

    // --- Font family ---

    void testFontFamilyNoOp()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setFontFamily(QStringLiteral("DejaVu Sans Mono"));
        QSignalSpy spy(&s, &Settings::fontFamilyChanged);
        s.setFontFamily(QStringLiteral("DejaVu Sans Mono"));
        QCOMPARE(spy.count(), 0);
    }

    void testFontFamilySignal()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::fontFamilyChanged);
        s.setFontFamily(QStringLiteral("Liberation Mono"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.fontFamily(), QStringLiteral("Liberation Mono"));
    }

    // --- Shell command ---

    void testShellCommandNoOp()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::shellCommandChanged);
        s.setShellCommand(QString()); // empty is default
        QCOMPARE(spy.count(), 0);
    }

    void testShellCommandSignal()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::shellCommandChanged);
        s.setShellCommand(QStringLiteral("/bin/bash"));
        QCOMPARE(spy.count(), 1);
    }

    // --- Color scheme ---

    void testColorSchemeNoOp()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setColorScheme(QStringLiteral("dark"));
        QSignalSpy spy(&s, &Settings::colorSchemeChanged);
        s.setColorScheme(QStringLiteral("dark"));
        QCOMPARE(spy.count(), 0);
    }

    // --- Persistence ---

    void testValuesPersist()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            Settings s(path);
            s.setFontSize(24);
            s.setFontFamily(QStringLiteral("Fira Code"));
            s.setBackgroundOpacity(0.8f);
            s.setBellMode(2);
            s.setCursorTrails(false);
            s.setCustomShaderPath(QStringLiteral("/test/path.glsl"));
        }
        // Wait for debounce
        QTest::qWait(DEBOUNCE_WAIT_MS);
        // Load from same file
        {
            Settings s(path);
            QCOMPARE(s.fontSize(), 24);
            QCOMPARE(s.fontFamily(), QStringLiteral("Fira Code"));
            QCOMPARE(s.backgroundOpacity(), 0.8f);
            QCOMPARE(s.bellMode(), 2);
            QCOMPARE(s.cursorTrails(), false);
            QCOMPARE(s.customShaderPath(), QStringLiteral("/test/path.glsl"));
        }
    }

    // --- Debounce ---

    void testDebounceBatching()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");

        // Make 3 rapid changes
        s.setFontSize(20);
        s.setFontFamily(QStringLiteral("Monospace"));
        s.setBackgroundOpacity(0.9f);

        // All should be in memory immediately
        QCOMPARE(s.fontSize(), 20);
        QCOMPARE(s.fontFamily(), QStringLiteral("Monospace"));

        // Wait for debounce
        QTest::qWait(DEBOUNCE_WAIT_MS);

        // Verify persisted
        QSettings qs(dir.path() + "/test.conf", QSettings::IniFormat);
        QCOMPARE(qs.value("font/size").toInt(), 20);
        QCOMPARE(qs.value("font/family").toString(), QStringLiteral("Monospace"));
    }

    // --- Defaults ---

    void testDefaultValues()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QCOMPARE(s.fontSize(), 18);
        QCOMPARE(s.fontFamily(), QStringLiteral("monospace"));
        QCOMPARE(s.shellCommand(), QString());
        QCOMPARE(s.colorScheme(), QStringLiteral("dark"));
        QCOMPARE(s.backgroundOpacity(), 0.6f);
        QCOMPARE(s.bellMode(), 1);
        QCOMPARE(s.scrollbackPersistence(), false);
        QCOMPARE(s.scrollbackRetentionDays(), 30);
        QCOMPARE(s.keybarKeys(), QStringList({"left","down","up","right","tab","ctrl","alt","esc","keyboard"}));
        QCOMPARE(s.keybarVisible(), true);
        QCOMPARE(s.cursorTrails(), true);  // load() default is true
        QCOMPARE(s.customShaderPath(), QString());
        QCOMPARE(s.shaderPipelineAvailable(), false);
    }

    // --- Scrollback persistence ---

    void testScrollbackPersistenceNoOpSignalSuppression()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");

        QSignalSpy spy(&s, &Settings::scrollbackPersistenceChanged);
        s.setScrollbackPersistence(false); // same as default
        QCOMPARE(spy.count(), 0);

        s.setScrollbackPersistence(true);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.scrollbackPersistence(), true);

        s.setScrollbackPersistence(true); // no-op
        QCOMPARE(spy.count(), 1);
    }

    void testScrollbackRetentionDaysClamping()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");

        s.setScrollbackRetentionDays(1);
        QCOMPARE(s.scrollbackRetentionDays(), 7); // clamped to min

        s.setScrollbackRetentionDays(999);
        QCOMPARE(s.scrollbackRetentionDays(), 365); // clamped to max

        s.setScrollbackRetentionDays(30);
        QCOMPARE(s.scrollbackRetentionDays(), 30);
    }

    void testScrollbackRetentionDaysNoOpSignalSuppression()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");

        QSignalSpy spy(&s, &Settings::scrollbackRetentionDaysChanged);
        s.setScrollbackRetentionDays(30); // same as default
        QCOMPARE(spy.count(), 0);

        s.setScrollbackRetentionDays(7);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.scrollbackRetentionDays(), 7);

        s.setScrollbackRetentionDays(7); // no-op
        QCOMPARE(spy.count(), 1);
    }

    void testScrollbackSettingsPersistence()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            Settings s(path);
            s.setScrollbackPersistence(true);
            s.setScrollbackRetentionDays(90);
        }
        QTest::qWait(DEBOUNCE_WAIT_MS);

        Settings s2(path);
        QCOMPARE(s2.scrollbackPersistence(), true);
        QCOMPARE(s2.scrollbackRetentionDays(), 90);
    }

    // --- Keybar keys ---

    void testKeybarKeysDefault()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QStringList expected = {QStringLiteral("left"), QStringLiteral("down"),
                                QStringLiteral("up"), QStringLiteral("right"),
                                QStringLiteral("tab"), QStringLiteral("ctrl"),
                                QStringLiteral("alt"), QStringLiteral("esc"),
                                QStringLiteral("keyboard")};
        QCOMPARE(s.keybarKeys(), expected);
    }

    void testKeybarKeysSetAndGet()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QStringList custom = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        s.setKeybarKeys(custom);
        QCOMPARE(s.keybarKeys(), custom);
    }

    void testKeybarKeysSignalOnChange()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::keybarKeysChanged);
        QStringList custom = {QStringLiteral("x"), QStringLiteral("y")};
        s.setKeybarKeys(custom);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.keybarKeys(), custom);
    }

    void testKeybarKeysNoOpSuppressesSignal()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        // Default is {"left","down","up","right","tab","ctrl","alt","esc","keyboard"}
        QSignalSpy spy(&s, &Settings::keybarKeysChanged);
        s.setKeybarKeys({QStringLiteral("left"), QStringLiteral("down"),
                         QStringLiteral("up"), QStringLiteral("right"),
                         QStringLiteral("tab"), QStringLiteral("ctrl"),
                         QStringLiteral("alt"), QStringLiteral("esc"),
                         QStringLiteral("keyboard")});
        QCOMPARE(spy.count(), 0);
    }

    void testKeybarKeysPersistence()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        QStringList custom = {QStringLiteral("enter"), QStringLiteral("esc"), QStringLiteral("space")};
        {
            Settings s(path);
            s.setKeybarKeys(custom);
        }
        QTest::qWait(DEBOUNCE_WAIT_MS);

        Settings s2(path);
        QCOMPARE(s2.keybarKeys(), custom);
    }

    void testKeybarKeysEmptyList()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::keybarKeysChanged);

        s.setKeybarKeys(QStringList());
        QCOMPARE(s.keybarKeys(), QStringList());
        QCOMPARE(spy.count(), 1);

        // Persists empty list
        QTest::qWait(DEBOUNCE_WAIT_MS);
        Settings s2(dir.path() + "/test.conf");
        QCOMPARE(s2.keybarKeys(), QStringList());
    }

    // --- Keybar row breaks ---

    void testKeybarRowBreaksDefault()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        // Default should be empty (single row)
        QCOMPARE(s.keybarRowBreaks(), QVariantList());
    }

    void testKeybarRowBreaksSetAndGet()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QVariantList breaks = {4, 7};
        s.setKeybarRowBreaks(breaks);
        QCOMPARE(s.keybarRowBreaks(), breaks);
    }

    void testKeybarRowBreaksMaxTwo()
    {
        // Max 2 breaks = 3 rows. Third break should be dropped.
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setKeybarKeys({"a","b","c","d","e","f","g","h","i","j"});
        QVariantList input = {2, 5, 8};
        QVariantList expected = {2, 5};
        s.setKeybarRowBreaks(input);
        QCOMPARE(s.keybarRowBreaks(), expected);
    }

    void testKeybarRowBreaksFilterInvalid()
    {
        // Values <= 0 or >= key count should be filtered
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setKeybarKeys({"a","b","c","d","e"});
        QVariantList input = {0, -1, 3, 5, 10};
        QVariantList expected = {3};
        s.setKeybarRowBreaks(input);
        QCOMPARE(s.keybarRowBreaks(), expected);
    }

    void testKeybarRowBreaksDedup()
    {
        // Duplicate values should be removed (monotonic check rejects equal)
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setKeybarKeys({"a","b","c","d","e","f"});
        QVariantList input = {2, 2, 4};
        QVariantList expected = {2, 4};
        s.setKeybarRowBreaks(input);
        QCOMPARE(s.keybarRowBreaks(), expected);
    }

    void testKeybarRowBreaksEmpty()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setKeybarRowBreaks({});
        QCOMPARE(s.keybarRowBreaks(), QVariantList());
    }

    void testKeybarRowBreaksTrailingTrim()
    {
        // Breaks >= key count should be trimmed
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setKeybarKeys({"a","b","c"});
        QVariantList input = {1, 3};  // 3 >= size(3), should be dropped
        QVariantList expected = {1};
        s.setKeybarRowBreaks(input);
        QCOMPARE(s.keybarRowBreaks(), expected);
    }

    // --- Keybar visible ---

    void testKeybarVisibleNoOpSignalSuppression()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::keybarVisibleChanged);
        s.setKeybarVisible(true); // same as default
        QCOMPARE(spy.count(), 0);

        s.setKeybarVisible(false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.keybarVisible(), false);

        s.setKeybarVisible(false); // no-op
        QCOMPARE(spy.count(), 1);
    }

    void testKeybarVisibleSignalOnChange()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::keybarVisibleChanged);
        s.setKeybarVisible(false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.keybarVisible(), false);
    }

    void testKeybarVisiblePersistence()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            Settings s(path);
            s.setKeybarVisible(false);
        }
        QTest::qWait(DEBOUNCE_WAIT_MS);

        Settings s2(path);
        QCOMPARE(s2.keybarVisible(), false);
    }

    // --- Session sort mode ---

    void testSessionSortModeDefault()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QCOMPARE(s.sessionSortMode(), 1); // SortLastUsed
    }

    void testSessionSortModeClamping()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setSessionSortMode(-1);
        QCOMPARE(s.sessionSortMode(), 0);
        s.setSessionSortMode(99);
        QCOMPARE(s.sessionSortMode(), 3);
        s.setSessionSortMode(2);
        QCOMPARE(s.sessionSortMode(), 2);
    }

    void testSessionSortModeNoOpSignalSuppression()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::sessionSortModeChanged);
        s.setSessionSortMode(1); // same as default (SortLastUsed)
        QCOMPARE(spy.count(), 0);
    }

    void testSessionSortModeSignalOnChange()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::sessionSortModeChanged);
        s.setSessionSortMode(3); // SortAlphabetical
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.sessionSortMode(), 3);
    }

    void testSessionSortModePersistence()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.conf";
        {
            Settings s(path);
            s.setSessionSortMode(2); // SortCreated
        }
        QTest::qWait(DEBOUNCE_WAIT_MS);

        Settings s2(path);
        QCOMPARE(s2.sessionSortMode(), 2);
    }

    void testSessionSortModeInvalidClamp()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setSessionSortMode(-5);
        QCOMPARE(s.sessionSortMode(), 0);
        s.setSessionSortMode(100);
        QCOMPARE(s.sessionSortMode(), 3);
    }

    // --- Cursor trails ---

    void testCursorTrailsNoOp()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        s.setCursorTrails(true);  // load() default is true
        QSignalSpy spy(&s, &Settings::cursorTrailsChanged);
        s.setCursorTrails(true);  // same value — no-op
        QCOMPARE(spy.count(), 0);
    }

    void testCursorTrailsChange()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::cursorTrailsChanged);
        s.setCursorTrails(false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.cursorTrails(), false);
    }

    // --- Custom shader path ---

    void testCustomShaderPathNoOp()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::customShaderPathChanged);
        s.setCustomShaderPath(QString());  // empty is default — no-op
        QCOMPARE(spy.count(), 0);
    }

    void testCustomShaderPathChange()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QSignalSpy spy(&s, &Settings::customShaderPathChanged);
        s.setCustomShaderPath(QStringLiteral("/path/to/shader.glsl"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.customShaderPath(), QStringLiteral("/path/to/shader.glsl"));
    }

    // --- Shader pipeline available (runtime-only) ---

    void testShaderPipelineAvailableChange()
    {
        QTemporaryDir dir;
        Settings s(dir.path() + "/test.conf");
        QCOMPARE(s.shaderPipelineAvailable(), false);
        QSignalSpy spy(&s, &Settings::shaderPipelineAvailableChanged);
        s.setShaderPipelineAvailable(true);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(s.shaderPipelineAvailable(), true);
    }
};

QTEST_MAIN(TestSettingsBehavior)
#include "tst_settings_behavior.moc"
