#include <QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "settings.h"

// Settings::migrateLegacySettings() one-time copy from the legacy flat
// config path to the AppConfigLocation path. Runs as a side effect of the
// QSettings member initializer, and only for the default path; the four
// cases below pin exactly that contract.
class TestSettingsMigration : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();

    void testCopiesLegacyWhenNewAbsent();
    void testKeepsNewerNewFile();
    void testOverwritesWhenLegacyNewer();
    void testInjectedPathNeverMigrates();

private:
    QString defaultPath() const;
    QString legacyPath() const;
    void writeLegacy(const QByteArray &ini);
    void setMtime(const QString &path, const QDateTime &when);

    // Fresh dir per test case: QSettings caches parsed content per file
    // path within a process, so reusing one default path across cases
    // would serve stale content no matter what the migration did.
    QTemporaryDir *m_dir = nullptr;
};

void TestSettingsMigration::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral(APP_ORG));
    QCoreApplication::setApplicationName(QStringLiteral(APP_XDG_NAME));
}

void TestSettingsMigration::init()
{
    m_dir = new QTemporaryDir();
    QVERIFY(m_dir->isValid());
    // QStandardPaths re-reads the XDG env per call on Linux, so this
    // redirects every path the migration computes into the temp dir.
    qputenv("XDG_CONFIG_HOME", m_dir->path().toUtf8());
}

void TestSettingsMigration::cleanup()
{
    delete m_dir;
    m_dir = nullptr;
}

void TestSettingsMigration::cleanupTestCase()
{
    qunsetenv("XDG_CONFIG_HOME");
}

QString TestSettingsMigration::defaultPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
           + QStringLiteral("/" APP_XDG_NAME ".conf");
}

QString TestSettingsMigration::legacyPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
           + QStringLiteral("/" APP_ORG "/" APP_XDG_NAME ".conf");
}

void TestSettingsMigration::writeLegacy(const QByteArray &ini)
{
    QDir().mkpath(QFileInfo(legacyPath()).absolutePath());
    QFile f(legacyPath());
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(legacyPath()));
    QVERIFY(f.write(ini) > 0);
}

void TestSettingsMigration::setMtime(const QString &path, const QDateTime &when)
{
    QFile f(path);
    QVERIFY2(f.open(QIODevice::ReadWrite), qPrintable(path));
    QVERIFY(f.setFileTime(when, QFileDevice::FileModificationTime));
}

void TestSettingsMigration::testCopiesLegacyWhenNewAbsent()
{
    writeLegacy("[font]\nsize=24\n[custom]\nunknownKey=42\n");
    setMtime(legacyPath(), QDateTime::currentDateTime());

    {
        Settings s(defaultPath());
        QCOMPARE(s.fontSize(), 24);
    }

    QVERIFY2(QFile::exists(defaultPath()), "migration must create the new file");
    QVERIFY(QFile::exists(legacyPath())); // never deleted

    QSettings migrated(defaultPath(), QSettings::IniFormat);
    QCOMPARE(migrated.value("font/size").toInt(), 24);
    // Unknown keys must survive: the migration is a file copy, not a rewrite
    QCOMPARE(migrated.value("custom/unknownKey").toInt(), 42);
}

void TestSettingsMigration::testKeepsNewerNewFile()
{
    writeLegacy("[font]\nsize=24\n");

    QDir().mkpath(QFileInfo(defaultPath()).absolutePath());
    QFile nf(defaultPath());
    QVERIFY(nf.open(QIODevice::WriteOnly));
    nf.write("[font]\nsize=11\n");
    nf.close();

    const QDateTime now = QDateTime::currentDateTime();
    setMtime(defaultPath(), now);
    setMtime(legacyPath(), now.addSecs(-100)); // legacy older

    {
        Settings s(defaultPath());
        QCOMPARE(s.fontSize(), 11);
    }

    QSettings kept(defaultPath(), QSettings::IniFormat);
    QCOMPARE(kept.value("font/size").toInt(), 11);
}

void TestSettingsMigration::testOverwritesWhenLegacyNewer()
{
    writeLegacy("[font]\nsize=24\n");

    QDir().mkpath(QFileInfo(defaultPath()).absolutePath());
    QFile nf(defaultPath());
    QVERIFY(nf.open(QIODevice::WriteOnly));
    nf.write("[font]\nsize=11\n");
    nf.close();

    // Downgrade round-trip: the new file exists but is older than the
    // legacy file (user downgraded, changed settings, came back).
    const QDateTime now = QDateTime::currentDateTime();
    setMtime(legacyPath(), now);
    setMtime(defaultPath(), now.addSecs(-100));

    {
        Settings s(defaultPath());
        QCOMPARE(s.fontSize(), 24);
    }

    QSettings migrated(defaultPath(), QSettings::IniFormat);
    QCOMPARE(migrated.value("font/size").toInt(), 24);
}

void TestSettingsMigration::testInjectedPathNeverMigrates()
{
    writeLegacy("[font]\nsize=24\n");

    const QString injected = m_dir->path() + QStringLiteral("/injected.conf");
    {
        Settings s(injected);
        QCOMPARE(s.fontSize(), 18); // defaults; nothing was copied in
    }

    QVERIFY(!QFile::exists(injected)); // no save happened, no migration copy
    QVERIFY(QFile::exists(legacyPath()));
}

QTEST_MAIN(TestSettingsMigration)
#include "tst_settings_migration.moc"
