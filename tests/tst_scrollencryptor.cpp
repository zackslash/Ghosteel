#include <QtTest>
#include <QByteArray>
#include "scrollencryptor.h"

class TestScrollEncryptor : public QObject
{
    Q_OBJECT

private slots:
    // --- isEncryptedFormat ---

    void testIsEncryptedFormat_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<bool>("expected");

        QTest::newRow("valid magic") << QByteArray("GSB\x01", 4) << true;
        QTest::newRow("valid with payload") << QByteArray("GSB\x01"
                                     "\x00\x00\x00\x00"
                                     "1234567890123456"
                                     "ciphertext", 28) << true;
        QTest::newRow("empty") << QByteArray() << false;
        QTest::newRow("too short") << QByteArray("GSB", 3) << false;
        QTest::newRow("wrong magic") << QByteArray("GSC\x01", 4) << false;
        QTest::newRow("wrong version") << QByteArray("GSB\x02", 4) << false;
        QTest::newRow("plaintext") << QByteArray("GHOSTTY_SCROLLBACK_V1\n") << false;
        QTest::newRow("single byte") << QByteArray("G", 1) << false;
    }

    void testIsEncryptedFormat()
    {
        QFETCH(QByteArray, data);
        QFETCH(bool, expected);
        QCOMPARE(ScrollEncryptor::isEncryptedFormat(data), expected);
    }

    // --- pkcs7Pad ---

    void testPkcs7PadEmpty()
    {
        QByteArray padded = ScrollEncryptor::pkcs7Pad(QByteArray());
        QCOMPARE(padded.size(), 16);
        QCOMPARE(static_cast<unsigned char>(padded.at(0)), 16u);
        // All 16 bytes should be 0x10
        for (int i = 0; i < 16; i++)
            QCOMPARE(static_cast<unsigned char>(padded.at(i)), 16u);
    }

    void testPkcs7PadExactBlock()
    {
        // 16 bytes input → needs full 16-byte padding block
        QByteArray data(16, 'A');
        QByteArray padded = ScrollEncryptor::pkcs7Pad(data);
        QCOMPARE(padded.size(), 32);
        QCOMPARE(static_cast<unsigned char>(padded.at(16)), 16u);
    }

    void testPkcs7PadPartialBlock()
    {
        // 10 bytes input → needs 6 bytes of padding
        QByteArray data(10, 'B');
        QByteArray padded = ScrollEncryptor::pkcs7Pad(data);
        QCOMPARE(padded.size(), 16);
        QCOMPARE(static_cast<unsigned char>(padded.at(10)), 6u);
        QCOMPARE(static_cast<unsigned char>(padded.at(15)), 6u);
    }

    void testPkcs7PadSingleByte()
    {
        QByteArray data("X", 1);
        QByteArray padded = ScrollEncryptor::pkcs7Pad(data);
        QCOMPARE(padded.size(), 16);
        QCOMPARE(padded.at(0), 'X');
        QCOMPARE(static_cast<unsigned char>(padded.at(1)), 15u);
    }

    void testPkcs7Pad15Bytes()
    {
        QByteArray data(15, 'C');
        QByteArray padded = ScrollEncryptor::pkcs7Pad(data);
        QCOMPARE(padded.size(), 16);
        QCOMPARE(static_cast<unsigned char>(padded.at(15)), 1u);
    }

    // --- pkcs7Unpad ---

    void testPkcs7UnpadEmpty()
    {
        QCOMPARE(ScrollEncryptor::pkcs7Unpad(QByteArray()), QByteArray());
    }

    void testPkcs7UnpadValid()
    {
        // Build a known padded value: "HELLO" + 11 bytes of 0x0B
        QByteArray data("HELLO");
        data.append(QByteArray(11, '\x0B'));
        QByteArray unpadded = ScrollEncryptor::pkcs7Unpad(data);
        QCOMPARE(unpadded, QByteArray("HELLO"));
    }

    void testPkcs7UnpadFullBlock()
    {
        // 16 bytes of 0x10 → empty after unpad
        QByteArray data(16, '\x10');
        QByteArray unpadded = ScrollEncryptor::pkcs7Unpad(data);
        QCOMPARE(unpadded.size(), 0);
    }

    void testPkcs7UnpadInvalidPaddingByte()
    {
        // Padding byte 0 → invalid
        QByteArray data(16, '\x00');
        QCOMPARE(ScrollEncryptor::pkcs7Unpad(data), QByteArray());
    }

    void testPkcs7UnpadInconsistentPadding()
    {
        // Mix of padding values → invalid
        QByteArray data("HELLO");
        data.append('\x0B');
        data.append('\x0C'); // inconsistent
        data.append(QByteArray(9, '\x0B'));
        QCOMPARE(ScrollEncryptor::pkcs7Unpad(data), QByteArray());
    }

    void testPkcs7UnpadPaddingTooLarge()
    {
        // Padding byte 17 → invalid (> 16)
        QByteArray data(16, '\x11');
        QCOMPARE(ScrollEncryptor::pkcs7Unpad(data), QByteArray());
    }

    // --- Round-trip: pad then unpad ---

    void testPkcs7RoundTrip_data()
    {
        QTest::addColumn<int>("size");

        QTest::newRow("0 bytes") << 0;
        QTest::newRow("1 byte") << 1;
        QTest::newRow("15 bytes") << 15;
        QTest::newRow("16 bytes") << 16;
        QTest::newRow("17 bytes") << 17;
        QTest::newRow("100 bytes") << 100;
        QTest::newRow("1024 bytes") << 1024;
        QTest::newRow("3MB") << (3 * 1024 * 1024);
    }

    void testPkcs7RoundTrip()
    {
        QFETCH(int, size);
        QByteArray original(size, 'X');
        QByteArray padded = ScrollEncryptor::pkcs7Pad(original);
        // Padded size must be multiple of 16
        QCOMPARE(padded.size() % 16, 0);
        // Unpad must recover original
        QByteArray recovered = ScrollEncryptor::pkcs7Unpad(padded);
        QCOMPARE(recovered, original);
    }
};

QTEST_GUILESS_MAIN(TestScrollEncryptor)
#include "tst_scrollencryptor.moc"
