#include "textutil.h"
#include <QStringList>
#include <QRegularExpression>
#include <QtMath>
#include <cctype>

namespace TextUtil {

QString trimSelectionText(const QString &text)
{
    if (text.isEmpty())
        return text;

    // Trim trailing whitespace from each line, remove trailing empty lines.
    // Preserve leading whitespace (indentation is significant).
    QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); i++) {
        QString &line = lines[i];
        int end = line.size();
        while (end > 0 && line.at(end - 1).isSpace())
            end--;
        line.resize(end);
    }
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    return lines.join(QLatin1Char('\n'));
}

QPointF cellFromPixel(const QPointF &pos, int cellWidth, int cellHeight,
                      int cols, int rows, int topPadding)
{
    if (cellWidth <= 0 || cellHeight <= 0)
        return QPointF(-1, -1);
    qreal adjustedY = pos.y() - topPadding;
    if (pos.x() < 0 || adjustedY < 0)
        return QPointF(-1, -1);
    int col = static_cast<int>(pos.x()) / cellWidth;
    int row = static_cast<int>(adjustedY) / cellHeight;
    if (col >= cols || row >= rows)
        return QPointF(-1, -1);
    return QPointF(col, row);
}

ScrollResult accumulateScroll(qreal accumulator, qreal delta)
{
    // Reset accumulator on direction change (skip when delta is zero)
    if (accumulator != 0 && delta != 0 && (accumulator > 0) != (delta > 0)) {
        accumulator = 0;
    }
    accumulator += delta;
    int lines = static_cast<int>(accumulator);
    accumulator -= lines;
    return { lines, accumulator };
}

Dimensions calculateDimensions(int width, int height, int cellWidth, int cellHeight, int topPadding)
{
    if (cellWidth <= 0 || cellHeight <= 0)
        return { 0, 0 };
    if (height <= topPadding)
        return { 2, 2 };

    uint16_t cols = static_cast<uint16_t>(width / cellWidth);
    uint16_t rows = static_cast<uint16_t>((height - topPadding) / cellHeight);

    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    if (cols > 512) cols = 512;
    if (rows > 512) rows = 512;

    return { cols, rows };
}

bool isWordChar(uint32_t codepoint)
{
    // Standard terminal word characters: alphanumeric, underscore.
    // Non-ASCII codepoints (CJK, accented letters, etc.) are treated as word chars
    // so entire non-ASCII "words" get selected on double-tap.
    // Exclude box-drawing (U+2500–U+257F) and block elements (U+2580–U+259F)
    // which are common in TUI apps (htop, lazygit, midnight commander).
    if (codepoint > 127) {
        if (codepoint >= 0x2500 && codepoint <= 0x259F)
            return false;
        return true;
    }
    unsigned char c = static_cast<unsigned char>(codepoint);
    return std::isalnum(c) || c == '_';
}

const QRegularExpression &urlRegex()
{
    // Thread-safe: initialized once on first call.
    static QRegularExpression re(
        QStringLiteral(
            R"url((?:(?:https?://|mailto:|ftp://|file:|ssh:|git://|ssh://|tel:|magnet:|ipfs://|ipns://|gemini://|gopher://|news:)(?:(?:\[[0-9a-fA-F:]+(?:[0-9a-fA-F:]*)+\](?::[0-9]+)?)|[\w\-.~:/?#@!$&*+,;=%]+(?:[\(\[]\w*[\)\]])?)+(?<![,.])|(?:\.\.\/|\.\/|(?<!\w)~\/|(?:[\w][\w\-.]*\/)*(?<!\w)\$[A-Za-z_]\w*\/|(?<!\w)\.[\w][\w\-.]*\/|(?<![\w~\/])\/(?!\/))(?:(?=[\w\-.~:\/?#@!$&*+;=%]*\.)[\w\-.~:\/?#@!$&*+;=%]+(?:(?<!:) (?!\w+:\/\/)(?!\.{0,2}\/)(?!~\/)[\w\-.~:\/?#@!$&*+;=%]*[\/.])*|(?![\w\-.~:\/?#@!$&*+;=%]*\.)(?:(?<!:) (?!\w+:\/\/)(?!\.{0,2}\/)(?!~\/)[\w\-.~:\/?#@!$&*+;=%]+)*)*(?<!:)(?: +(?= *$))?|(?<!\w)[\w][\w\-.]*\/[\w\-.~:\/?#@!$&*+;=%]+\.\w+(?<!:)(?: +(?= *$))?))url"
        )
    );
    return re;
}

QVector<LinkSpan> findUrls(const QString &flatText,
                           const QVector<CellCoord> &charMap)
{
    QVector<LinkSpan> results;
    if (flatText.isEmpty() || charMap.isEmpty())
        return results;

    const QRegularExpression &re = urlRegex();
    if (!re.isValid())
        return results;

    QRegularExpressionMatchIterator it = re.globalMatch(flatText);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        int startIdx = match.capturedStart();
        int endIdx = match.capturedEnd(); // exclusive

        if (startIdx < 0 || endIdx <= startIdx)
            continue;
        if (startIdx >= charMap.size() || endIdx > charMap.size())
            continue;

        CellCoord start = charMap[startIdx];
        CellCoord end = charMap[endIdx - 1];

        LinkSpan span;
        span.startCol = start.col;
        span.startRow = start.row;
        span.endCol = end.col + 1; // inclusive → exclusive
        span.endRow = end.row;
        span.uri = match.captured();

        results.append(span);
    }

    return results;
}

} // namespace TextUtil
