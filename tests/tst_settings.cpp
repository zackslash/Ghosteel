#include <QtTest>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QSignalSpy>

#include "settings.h"

class TestSettings : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;
    QString m_settingsPath;

private slots:
    void initTestCase()
    {
        QVERIFY2(m_tempDir.isValid(), qUtf8Printable(m_tempDir.errorString()));
        m_settingsPath = m_tempDir.path() + "/test.conf";

        // Settings is a singleton that reads from QStandardPaths.
        // For testing, we write directly to verify the INI format.
    }

    void testDefaultValues()
    {
        // Write empty settings, read back with defaults
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.clear();
            s.sync();
        }

        QSettings s(m_settingsPath, QSettings::IniFormat);

        QCOMPARE(s.value("font/size", 18).toInt(), 18);
        QCOMPARE(s.value("font/family", "monospace").toString(),
                 QStringLiteral("monospace"));
        QCOMPARE(s.value("terminal/shell", "").toString(), QString());
        QCOMPARE(s.value("terminal/colorScheme", "dark").toString(),
                 QStringLiteral("dark"));
        QCOMPARE(s.value("terminal/backgroundOpacity", 0.6).toFloat(),
                 0.6f);
        QCOMPARE(s.value("terminal/bellMode", 1).toInt(), 1);
        QCOMPARE(s.value("terminal/cursorTrails", true).toBool(), true);
        QCOMPARE(s.value("terminal/urlAutoDetect", true).toBool(), true);
        QCOMPARE(s.value("terminal/customShaderPath").toString(), QString());
    }

    void testWriteAndReadBack()
    {
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.setValue("font/size", 24);
            s.setValue("font/family", "Liberation Mono");
            s.setValue("terminal/shell", "/bin/bash");
            s.setValue("terminal/colorScheme", "solarized-dark");
            s.setValue("terminal/backgroundOpacity", 0.8);
            s.setValue("terminal/bellMode", 2);
            s.sync();
        }

        QSettings s(m_settingsPath, QSettings::IniFormat);
        QCOMPARE(s.value("font/size").toInt(), 24);
        QCOMPARE(s.value("font/family").toString(), QStringLiteral("Liberation Mono"));
        QCOMPARE(s.value("terminal/shell").toString(), QStringLiteral("/bin/bash"));
        QCOMPARE(s.value("terminal/colorScheme").toString(),
                 QStringLiteral("solarized-dark"));
        QCOMPARE(s.value("terminal/backgroundOpacity").toFloat(), 0.8f);
        QCOMPARE(s.value("terminal/bellMode").toInt(), 2);
    }

    void testSettingsFileFormat()
    {
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.setValue("font/size", 16);
            s.sync();
        }

        // Verify the file is valid INI
        QFile f(m_settingsPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        QByteArray content = f.readAll();
        QVERIFY(content.contains("[font]"));
        QVERIFY(content.contains("size=16"));
    }

    void testSessionSettingsCoexist()
    {
        // Verify that session data doesn't conflict with font/terminal settings
        {
            QSettings s(m_settingsPath, QSettings::IniFormat);
            s.setValue("font/size", 20);
            s.setValue("terminal/colorScheme", "light");

            s.beginGroup("sessions");
            s.setValue("count", 2);
            s.endGroup();

            s.beginGroup("sessionData/session_0");
            s.setValue("name", "Test");
            s.setValue("workingDirectory", "/tmp");
            s.endGroup();
            s.sync();
        }

        QSettings s(m_settingsPath, QSettings::IniFormat);
        QCOMPARE(s.value("font/size").toInt(), 20);
        QCOMPARE(s.value("terminal/colorScheme").toString(), QStringLiteral("light"));

        s.beginGroup("sessions");
        QCOMPARE(s.value("count").toInt(), 2);
        s.endGroup();

        s.beginGroup("sessionData/session_0");
        QCOMPARE(s.value("name").toString(), QStringLiteral("Test"));
        QCOMPARE(s.value("workingDirectory").toString(), QStringLiteral("/tmp"));
        s.endGroup();
    }
};

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
