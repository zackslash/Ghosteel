#include <QtTest>
#include "textutil.h"

class TestUrlDetection : public QObject
{
    Q_OBJECT

private:
    // Helper: build flatText + charMap from a single-line string
    // Each char maps to (col, 0). Newlines get sentinel (cols, 0).
    void buildSingleLine(const QString &text, int cols,
                         QString &flatText,
                         QVector<TextUtil::CellCoord> &charMap)
    {
        flatText = text;
        charMap.resize(text.size());
        for (int i = 0; i < text.size(); ++i) {
            charMap[i] = { static_cast<uint16_t>(i), 0 };
        }
    }

    // Helper: build flatText + charMap from multi-line input.
    // Lines separated by \n. Each char maps to (col, row).
    void buildMultiLine(const QStringList &lines, int cols,
                        QString &flatText,
                        QVector<TextUtil::CellCoord> &charMap)
    {
        flatText.clear();
        charMap.clear();
        for (int row = 0; row < lines.size(); ++row) {
            const QString &line = lines[row];
            for (int col = 0; col < line.size(); ++col) {
                flatText.append(line[col]);
                charMap.append({ static_cast<uint16_t>(col),
                                 static_cast<uint16_t>(row) });
            }
            if (row + 1 < lines.size()) {
                flatText.append(QChar('\n'));
                charMap.append({ static_cast<uint16_t>(cols),
                                 static_cast<uint16_t>(row) });
            }
        }
    }

private slots:
    // --- Regex validity ---

    void regexCompiles()
    {
        QVERIFY(TextUtil::urlRegex().isValid());
    }

    // --- Scheme URLs ---

    void httpsUrl()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("visit https://example.com now", 30, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("https://example.com"));
        QCOMPARE(spans[0].startCol, 6);
        QCOMPARE(spans[0].startRow, 0);
        QCOMPARE(spans[0].endCol, 25); // exclusive: 6 + len("https://example.com")
    }

    void httpUrl()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("http://foo.bar/path?q=1", 23, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("http://foo.bar/path?q=1"));
    }

    void mailtoUrl()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("mailto:user@example.com", 23, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("mailto:user@example.com"));
    }

    void sshUrl()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("ssh://myhost", 12, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("ssh://myhost"));
    }

    void ftpUrl()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("ftp://files.example.com/pub", 27, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
    }

    void multipleUrls()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("see https://a.com and http://b.org", 34, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 2);
        QCOMPARE(spans[0].uri, QStringLiteral("https://a.com"));
        QCOMPARE(spans[1].uri, QStringLiteral("http://b.org"));
    }

    // --- Trailing punctuation ---

    void trailingDot()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("go to https://example.com.", 26, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("https://example.com"));
    }

    void trailingComma()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("see https://example.com, ok", 27, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("https://example.com"));
    }

    // --- File paths ---

    void absolutePath()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("file is /usr/share/doc/readme.txt", 33, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("/usr/share/doc/readme.txt"));
    }

    void dotSlashPath()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("./src/main.cpp", 14, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("./src/main.cpp"));
    }

    void dotDotSlashPath()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("../test/file.txt", 16, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("../test/file.txt"));
    }

    void tildePath()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("~/Documents/notes.md", 20, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("~/Documents/notes.md"));
    }

    void dollarVarPath()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("$HOME/.bashrc", 13, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("$HOME/.bashrc"));
    }

    void bareRelativePath()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("src/config/url.zig", 18, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("src/config/url.zig"));
    }

    void dotConfigPath()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine(".config/nvim/init.lua", 21, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral(".config/nvim/init.lua"));
    }

    // --- Non-matching ---

    void noMatchInPlainText()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("hello world no links here", 25, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 0);
    }

    void noMatchEmptyString()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        flat.clear();
        map.clear();
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 0);
    }

    void barePathWithoutDot()
    {
        // input/output has no dot — should NOT match
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("input/output", 12, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 0);
    }

    void midWordSlash()
    {
        // some/path is mid-word — should NOT match
        QString flat; QVector<TextUtil::CellCoord> map;
        buildSingleLine("some/path is not a link", 23, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 0);
    }

    // --- Multi-line ---

    void urlOnSecondLine()
    {
        QString flat; QVector<TextUtil::CellCoord> map;
        buildMultiLine({"hello", "see https://example.com"}, 24, flat, map);
        auto spans = TextUtil::findUrls(flat, map);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("https://example.com"));
        QCOMPARE(spans[0].startRow, 1);
    }

    void wrappedUrl()
    {
        // Simulate a URL that wraps across two soft-wrapped rows
        // In a real terminal, soft-wrapped rows have no \n between them.
        // Our buildMultiLine inserts \n by default, so we build manually.
        QString flat = "https://very-long-example.";
        flat.append("com/path/to/page");
        QVector<TextUtil::CellCoord> charMap;
        // First part: cols 0-26 on row 0
        for (int i = 0; i < 27; ++i)
            charMap.append({ static_cast<uint16_t>(i), 0 });
        // Second part: cols 0-16 on row 1 (soft-wrapped, no \n)
        for (int i = 0; i < 17; ++i)
            charMap.append({ static_cast<uint16_t>(i), 1 });

        auto spans = TextUtil::findUrls(flat, charMap);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].uri, QStringLiteral("https://very-long-example.com/path/to/page"));
        QCOMPARE(spans[0].startRow, 0);
        QCOMPARE(spans[0].endRow, 1);
    }

    // --- Coordinate mapping ---

    void coordinatesCorrectForNonAscii()
    {
        // CJK char '中' (1 QChar) at col 0, then URL starting at col 1
        QString flat;
        flat.append(QChar(0x4E2D)); // 中
        flat.append("https://example.com");
        QVector<TextUtil::CellCoord> charMap;
        charMap.append({ 0, 0 }); // 中
        for (int i = 0; i < 20; ++i)
            charMap.append({ static_cast<uint16_t>(i + 1), 0 });

        auto spans = TextUtil::findUrls(flat, charMap);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].startCol, 1);
        QCOMPARE(spans[0].uri, QStringLiteral("https://example.com"));
    }

    void supplementaryPlaneMapping()
    {
        // Emoji U+1F600 (😀) = surrogate pair = 2 QChars, both map to same col
        QString flat;
        flat.append(QChar(0xD83D)); // high surrogate
        flat.append(QChar(0xDE00)); // low surrogate
        flat.append("https://test.org");
        QVector<TextUtil::CellCoord> charMap;
        charMap.append({ 0, 0 }); // high surrogate
        charMap.append({ 0, 0 }); // low surrogate — same col
        for (int i = 0; i < 16; ++i)
            charMap.append({ static_cast<uint16_t>(i + 1), 0 });

        auto spans = TextUtil::findUrls(flat, charMap);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans[0].startCol, 1);
        QCOMPARE(spans[0].uri, QStringLiteral("https://test.org"));
    }
};

QTEST_MAIN(TestUrlDetection)
#include "tst_url_detection.moc"
