#include <QtTest>
#include "glyphatlas.h"

class TestGlyphCacheKey : public QObject
{
    Q_OBJECT

private slots:
    // Regression guard: existing GlyphKey still works
    void testSingleCpKeyUnchanged()
    {
        GlyphKey k{0x41, false, false};
        QCOMPARE(k.codepoint, static_cast<uint>(0x41));
        QVERIFY(!k.bold);
        QVERIFY(!k.italic);

        GlyphKey k2{0x41, true, true};
        QVERIFY(k2.bold);
        QVERIFY(k2.italic);
    }

    // ClusterKey equality for a ZWJ family sequence
    void testClusterKeyEquality()
    {
        uint32_t cps[] = {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467};
        ClusterKey a(cps, 5, false, false);
        ClusterKey b(cps, 5, false, false);
        QCOMPARE(a, b);
    }

    // Bold/italic variants must not be equal
    void testClusterKeyBoldItalicVariants()
    {
        uint32_t cps[] = {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467};
        ClusterKey base(cps, 5, false, false);
        ClusterKey bold(cps, 5, true, false);
        ClusterKey italic(cps, 5, false, true);
        QVERIFY(!(base == bold));
        QVERIFY(!(base == italic));
    }

    // Order matters: [a,b,c] != [c,b,a]
    void testClusterKeyOrderSignificant()
    {
        uint32_t abc[] = {0x61, 0x62, 0x63};
        uint32_t cba[] = {0x63, 0x62, 0x61};
        ClusterKey a(abc, 3, false, false);
        ClusterKey b(cba, 3, false, false);
        QVERIFY(!(a == b));
    }

    // INLINE_MAX boundary: len==16 cacheable, len==17 falls back
    void testClusterKeyInlineMaxBoundary()
    {
        uint32_t buf16[16] = {};
        ClusterKey a(buf16, 16, false, false);
        QVERIFY(a.cacheable());
        QCOMPARE(static_cast<int>(a.len), 16);

        uint32_t buf17[17] = {};
        ClusterKey b(buf17, 17, false, false);
        QVERIFY(!b.cacheable());
        QCOMPARE(static_cast<int>(b.len), 0); // uncachable sentinel
    }

    // Hash is deterministic
    void testClusterKeyHashDeterministic()
    {
        uint32_t cps[] = {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467};
        ClusterKey a(cps, 5, false, false);
        ClusterKey b(cps, 5, false, false);
        QCOMPARE(qHash(a), qHash(b));
    }

    // Different inputs produce different hashes (best-effort — collision is possible
    // but vanishingly unlikely for these specific inputs)
    void testClusterKeyHashDifferentFromDifferentInput()
    {
        uint32_t cps1[] = {0x1F468, 0x200D, 0x1F469};
        uint32_t cps2[] = {0x1F468, 0x200D, 0x1F467};
        ClusterKey a1(cps1, 3, false, false);
        ClusterKey a2(cps2, 3, false, false);
        ClusterKey bold(cps1, 3, true, false);
        ClusterKey italic(cps1, 3, false, true);

        // All four should have distinct hashes
        QVERIFY(qHash(a1) != qHash(a2));
        QVERIFY(qHash(a1) != qHash(bold));
        QVERIFY(qHash(a1) != qHash(italic));
        QVERIFY(qHash(a2) != qHash(bold));
    }
};

QTEST_MAIN(TestGlyphCacheKey)
#include "tst_glyph_cache_key.moc"
