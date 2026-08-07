#include <QTest>
#include <QByteArray>
#include "kittyimageguard.h"

class KittyImageGuardTest : public QObject {
    Q_OBJECT
private slots:
    void parsesDimensions();
    void parsesColorTypeAndInterlace();
    void boundaryExactlyAtCap();
    void maxWidthUint32();
    void lengthFloorRequiresFullIhdr();
    void emptyDataInvalid();
    void nonPngInvalid();
    void truncatedInvalid();
    void validSigButNotIhdrInvalid();
    void nullDataInvalid();
};

// Build a 29-byte PNG IHDR prefix: sig(8) + length(4) + "IHDR"(4) + W(4) + H(4)
// + bit_depth(1) + color_type(1) + compression(1) + filter(1) + interlace(1).
// The rest of a real PNG (CRC, IDAT...) is irrelevant to the sniff and omitted.
static QByteArray pngHeader(uint32_t width, uint32_t height,
                            uint8_t colorType = 2, uint8_t interlace = 0)
{
    static const uchar sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uchar ihdr[4] = {0x49, 0x48, 0x44, 0x52};  // "IHDR"

    QByteArray b;
    b.reserve(29);
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
    b.append(static_cast<char>(8));        // bit depth
    b.append(static_cast<char>(colorType));
    b.append(static_cast<char>(0));        // compression method
    b.append(static_cast<char>(0));        // filter method
    b.append(static_cast<char>(interlace));
    return b;
}

static KittyPngHeader sniff(const QByteArray &b)
{
    return kittyPngSniffHeader(reinterpret_cast<const uint8_t *>(b.constData()),
                               static_cast<size_t>(b.size()));
}

void KittyImageGuardTest::parsesDimensions()
{
    const auto h = sniff(pngHeader(1920, 1080));
    QVERIFY(h.valid);
    QCOMPARE(h.width, 1920u);
    QCOMPARE(h.height, 1080u);
    QCOMPARE(h.colorType, uint8_t(2));
    QCOMPARE(h.interlaceMethod, uint8_t(0));

    const auto tiny = sniff(pngHeader(1, 1));
    QVERIFY(tiny.valid);
    QCOMPARE(tiny.width, 1u);
    QCOMPARE(tiny.height, 1u);
}

void KittyImageGuardTest::parsesColorTypeAndInterlace()
{
    // The sniff must surface color_type and interlace so the decoder can decide
    // whether Qt can stream-downscale (RGB/RGBA, non-interlaced).
    QCOMPARE(sniff(pngHeader(100, 100, 6, 0)).colorType, uint8_t(6));        // RGBA
    QCOMPARE(sniff(pngHeader(100, 100, 0, 0)).colorType, uint8_t(0));        // gray
    QCOMPARE(sniff(pngHeader(100, 100, 3, 0)).colorType, uint8_t(3));        // palette
    QCOMPARE(sniff(pngHeader(100, 100, 2, 1)).interlaceMethod, uint8_t(1));  // Adam7
}

void KittyImageGuardTest::boundaryExactlyAtCap()
{
    const auto h = sniff(pngHeader(4096, 4096));
    QVERIFY(h.valid);
    QCOMPARE(h.width, 4096u);
    QCOMPARE(h.height, 4096u);
}

void KittyImageGuardTest::maxWidthUint32()
{
    // Regression guard: 0xFFFFFFFF big-endian must decode exactly (exercises
    // all four byte-position shifts of the width field).
    const auto h = sniff(pngHeader(0xFFFFFFFFu, 1));
    QVERIFY(h.valid);
    QCOMPARE(h.width, 0xFFFFFFFFu);
}

void KittyImageGuardTest::lengthFloorRequiresFullIhdr()
{
    // color_type lives at byte 25 and interlace at byte 28, so the sniff needs
    // the full 29-byte IHDR. 28 bytes must be rejected as un-sniffable.
    QCOMPARE(sniff(pngHeader(100, 100).left(28)).valid, false);
    QCOMPARE(sniff(pngHeader(100, 100)).valid, true);
}

void KittyImageGuardTest::emptyDataInvalid()
{
    // Non-null pointer, zero length. Distinct from nullData().
    QCOMPARE(kittyPngSniffHeader(reinterpret_cast<const uint8_t *>(""), 0).valid, false);
}

void KittyImageGuardTest::nonPngInvalid()
{
    // JPEG SOI, not a PNG signature; sniff returns invalid so the caller's
    // normal loadFromData path handles it (preserving existing diagnostics).
    QByteArray jpeg = QByteArray::fromHex("ffd8ffe000104a464946000101");
    QCOMPARE(sniff(jpeg).valid, false);
}

void KittyImageGuardTest::truncatedInvalid()
{
    QByteArray trunc = pngHeader(50000, 50000).left(10);
    QCOMPARE(sniff(trunc).valid, false);
}

void KittyImageGuardTest::validSigButNotIhdrInvalid()
{
    static const uchar sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    QByteArray b;
    b.append(reinterpret_cast<const char *>(sig), 8);
    b.append(4, '\0');
    b.append("IDAT", 4);   // wrong first chunk type
    b.append(13, '\0');    // pad to a full 29-byte header so the failure is the chunk type, not length
    QCOMPARE(sniff(b).valid, false);
}

void KittyImageGuardTest::nullDataInvalid()
{
    QCOMPARE(kittyPngSniffHeader(nullptr, 100).valid, false);
}

QTEST_APPLESS_MAIN(KittyImageGuardTest)
#include "tst_kitty_image_guard.moc"
