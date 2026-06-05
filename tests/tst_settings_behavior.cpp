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
        QCOMPARE(s.fontFamily(), QStringLiteral("DejaVu Sans Mono"));
        QCOMPARE(s.shellCommand(), QString());
        QCOMPARE(s.colorScheme(), QStringLiteral("dark"));
        QCOMPARE(s.backgroundOpacity(), 0.6f);
        QCOMPARE(s.bellMode(), 1);
    }
};

QTEST_MAIN(TestSettingsBehavior)
#include "tst_settings_behavior.moc"
