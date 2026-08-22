#include <QtTest>

#include "fontcatalog.h"

class TestFontCatalog : public QObject
{
    Q_OBJECT

private slots:
    void sentinelAlwaysFirst();
    void caseInsensitiveSort();
    void sentinelDedupe();
    void caseInsensitiveDedupe();
    void currentAppendedWhenMissing();
    void currentNotDuplicated();
    void emptyCurrentNoAppend();
};

void TestFontCatalog::sentinelAlwaysFirst()
{
    QStringList input;
    input << "Arial" << "Zebra Mono" << "Courier New";
    QStringList result = assembleFamilyList(input, QString());
    QCOMPARE(result.first(), QStringLiteral("monospace"));
}

void TestFontCatalog::caseInsensitiveSort()
{
    QStringList input;
    input << "Zebra Mono" << "alpha Mono" << "Bravo Mono";
    QStringList result = assembleFamilyList(input, QString());
    QCOMPARE(result.at(1), QStringLiteral("alpha Mono"));
    QCOMPARE(result.at(2), QStringLiteral("Bravo Mono"));
    QCOMPARE(result.at(3), QStringLiteral("Zebra Mono"));
}

void TestFontCatalog::sentinelDedupe()
{
    QStringList input;
    input << "Monospace" << "MONOSPACE" << "Courier New";
    QStringList result = assembleFamilyList(input, QString());
    QCOMPARE(result.count(QStringLiteral("monospace")), 1);
    QCOMPARE(result.count(QStringLiteral("Monospace")), 0);
    QCOMPARE(result.count(QStringLiteral("MONOSPACE")), 0);
    QCOMPARE(result.size(), 2);
}

void TestFontCatalog::caseInsensitiveDedupe()
{
    QStringList input;
    input << "DejaVu Sans Mono" << "dejavu sans mono" << "DEJAVU SANS MONO" << "Courier New";
    QStringList result = assembleFamilyList(input, QString());
    QCOMPARE(result.count(QStringLiteral("DejaVu Sans Mono")), 1);
    QCOMPARE(result.size(), 3);
}

void TestFontCatalog::currentAppendedWhenMissing()
{
    QStringList input;
    input << "Courier New";
    QStringList result = assembleFamilyList(input, QStringLiteral("Some Uninstalled Font"));
    QCOMPARE(result.last(), QStringLiteral("Some Uninstalled Font"));
    QCOMPARE(result.size(), 3);
}

void TestFontCatalog::currentNotDuplicated()
{
    QStringList input;
    input << "Courier New";
    QStringList result = assembleFamilyList(input, QStringLiteral("Courier New"));
    QCOMPARE(result.count(QStringLiteral("Courier New")), 1);
    QCOMPARE(result.size(), 2);

    QStringList resultVariant = assembleFamilyList(input, QStringLiteral("courier new"));
    QCOMPARE(resultVariant.count(QStringLiteral("Courier New")), 1);
    QCOMPARE(resultVariant.size(), 2);
}

void TestFontCatalog::emptyCurrentNoAppend()
{
    QStringList input;
    input << "Courier New";
    QStringList result = assembleFamilyList(input, QString());
    QCOMPARE(result.size(), 2);
    QCOMPARE(result.last(), QStringLiteral("Courier New"));

    QCOMPARE(assembleFamilyList(QStringList(), QStringLiteral("X")),
             (QStringList{QStringLiteral("monospace"), QStringLiteral("X")}));
}

QTEST_MAIN(TestFontCatalog)
#include "tst_fontcatalog.moc"