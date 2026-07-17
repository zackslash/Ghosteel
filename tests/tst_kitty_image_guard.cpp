#include <QTest>
#include <QByteArray>
#include "kittyimageguard.h"

class KittyImageGuardTest : public QObject {
    Q_OBJECT
private slots:
    void withinCap();
    void bombExceedsCap();
    void oneDimExceeds();
    void boundaryExactlyAtCap();
    void boundaryJustOverCap();
    void boundaryJustOverCapSingleAxis();
    void maxWidthUint32();
    void emptyDataFallsThrough();
    void nonPngFallsThrough();
    void truncatedFallsThrough();
    void validSigButNotIhdr();
    void nullData();
};

// Build a 24-byte PNG-header prefix with the given width/height (big-endian),
// exactly the bytes kittyPngExceedsDimCap inspects. The rest of a real PNG
// (chunk data, CRC, IDAT…) is irrelevant to the sniff and omitted.
static QByteArray pngHeader(uint32_t width, uint32_t height)
{
    static const uchar sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uchar ihdr[4] = {0x49, 0x48, 0x44, 0x52};  // "IHDR"

    QByteArray b;
    b.reserve(24);
    b.append(reinterpret_cast<const char *>(sig), 8);
    b.append(4, '\0');                                   // IHDR chunk length (not validated)
    b.append(reinterpret_cast<const char *>(ihdr), 4);

    uchar wh[8] = {
        static_cast<uchar>((width >> 24) & 0xFF),
        static_cast<uchar>((width >> 16) & 0xFF),
        static_cast<uchar>((width >> 8) & 0xFF),
        static_cast<uchar>(width & 0xFF),
        static_cast<uchar>((height >> 24) & 0xFF),
        static_cast<uchar>((height >> 16) & 0xFF),
        static_cast<uchar>((height >> 8) & 0xFF),
        static_cast<uchar>(height & 0xFF),
    };
    b.append(reinterpret_cast<const char *>(wh), 8);
    return b;
}

static bool exceeds(const QByteArray &b, int maxDim = 4096)
{
    return kittyPngExceedsDimCap(reinterpret_cast<const uint8_t *>(b.constData()),
                                 static_cast<size_t>(b.size()), maxDim);
}

void KittyImageGuardTest::withinCap()
{
    QCOMPARE(exceeds(pngHeader(1920, 1080)), false);
    QCOMPARE(exceeds(pngHeader(1, 1)), false);
}

void KittyImageGuardTest::bombExceedsCap()
{
    QCOMPARE(exceeds(pngHeader(50000, 50000)), true);
}

void KittyImageGuardTest::oneDimExceeds()
{
    QCOMPARE(exceeds(pngHeader(5000, 100)), true);
    QCOMPARE(exceeds(pngHeader(100, 5000)), true);
}

void KittyImageGuardTest::boundaryExactlyAtCap()
{
    QCOMPARE(exceeds(pngHeader(4096, 4096)), false);
}

void KittyImageGuardTest::boundaryJustOverCap()
{
    QCOMPARE(exceeds(pngHeader(4097, 4097)), true);
}

void KittyImageGuardTest::boundaryJustOverCapSingleAxis()
{
    // The || at the sniff boundary must trip on either axis alone.
    QCOMPARE(exceeds(pngHeader(4097, 1)), true);
    QCOMPARE(exceeds(pngHeader(1, 4097)), true);
    QCOMPARE(exceeds(pngHeader(4096, 4097)), true);
    QCOMPARE(exceeds(pngHeader(4097, 4096)), true);
}

void KittyImageGuardTest::maxWidthUint32()
{
    // Locks in shift-overflow safety: 0xFFFFFFFF big-endian = FF FF FF FF.
    // Correct only if each byte is widened to uint32_t before the shift and
    // the comparison is unsigned.
    QCOMPARE(exceeds(pngHeader(0xFFFFFFFFu, 1)), true);
    QCOMPARE(exceeds(pngHeader(1, 0xFFFFFFFFu)), true);
}

void KittyImageGuardTest::emptyDataFallsThrough()
{
    // Non-null pointer, zero length — distinct from nullData().
    QCOMPARE(kittyPngExceedsDimCap(reinterpret_cast<const uint8_t *>(""), 0, 4096), false);
}

void KittyImageGuardTest::nonPngFallsThrough()
{
    // JPEG SOI — not a PNG signature; sniff returns false so the caller's
    // normal loadFromData path handles it (preserving existing diagnostics).
    QByteArray jpeg = QByteArray::fromHex("ffd8ffe000104a464946000101");
    QCOMPARE(exceeds(jpeg), false);
}

void KittyImageGuardTest::truncatedFallsThrough()
{
    QByteArray trunc = pngHeader(50000, 50000).left(10);
    QCOMPARE(exceeds(trunc), false);
}

void KittyImageGuardTest::validSigButNotIhdr()
{
    static const uchar sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    QByteArray b;
    b.append(reinterpret_cast<const char *>(sig), 8);
    b.append(4, '\0');
    b.append("IDAT", 4);   // wrong first chunk type
    b.append(8, '\0');
    QCOMPARE(exceeds(b), false);
}

void KittyImageGuardTest::nullData()
{
    QCOMPARE(kittyPngExceedsDimCap(nullptr, 100, 4096), false);
}

QTEST_APPLESS_MAIN(KittyImageGuardTest)
#include "tst_kitty_image_guard.moc"
