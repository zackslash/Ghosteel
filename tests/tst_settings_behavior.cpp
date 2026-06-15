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
        QCOMPARE(s.keybarKeys(), QStringList({"left","down","up","right","tab","ctrl","alt","keyboard","esc"}));
        QCOMPARE(s.keybarVisible(), true);
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
                                QStringLiteral("alt"), QStringLiteral("keyboard"),
                                QStringLiteral("esc")};
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
        // Default is {"left","down","up","right","tab","ctrl","alt","keyboard","esc"}
        QSignalSpy spy(&s, &Settings::keybarKeysChanged);
        s.setKeybarKeys({QStringLiteral("left"), QStringLiteral("down"),
                         QStringLiteral("up"), QStringLiteral("right"),
                         QStringLiteral("tab"), QStringLiteral("ctrl"),
                         QStringLiteral("alt"), QStringLiteral("keyboard"),
                         QStringLiteral("esc")});
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
};

QTEST_MAIN(TestSettingsBehavior)
#include "tst_settings_behavior.moc"
