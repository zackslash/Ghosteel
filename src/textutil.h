#ifndef TEXTUTIL_H
#define TEXTUTIL_H

#include <QString>
#include <QPointF>
#include <QRegularExpression>
#include <QVector>

namespace TextUtil {

// Trim trailing whitespace from each line and remove trailing empty lines.
// Used for cleaning copy-to-clipboard text from terminal selections.
// Preserves leading whitespace (indentation).
QString trimSelectionText(const QString &text);

// Convert a pixel position to terminal cell coordinates.
// Returns QPointF(-1, -1) for out-of-bounds positions.
// topPadding is the pixel padding above the terminal grid.
QPointF cellFromPixel(const QPointF &pos, int cellWidth, int cellHeight,
                      int cols, int rows, int topPadding);

// Like cellFromPixel, but saturates out-of-bounds positions to the nearest
// edge cell instead of returning (-1, -1). Used for selection endpoints,
// which can legitimately land exactly on the grid edge (col == cols).
// Returns (-1, -1) only when the grid itself is empty/invalid.
QPointF cellFromPixelClamped(const QPointF &pos, int cellWidth, int cellHeight,
                             int cols, int rows, int topPadding);

// Accumulate fractional scroll lines from wheel/touch delta.
// Resets accumulator on direction change to prevent drifting.
// Returns the accumulated scroll state (lines to scroll + remaining accumulator).
struct ScrollResult {
    int lines;        // Integer lines to scroll (can be negative)
    qreal accumulator; // Remaining fractional accumulation
};
ScrollResult accumulateScroll(qreal accumulator, qreal delta);

// Calculate terminal grid dimensions from pixel size and cell metrics.
// Clamps cols/rows to [2, 512].
struct Dimensions {
    uint16_t cols;
    uint16_t rows;
};
Dimensions calculateDimensions(int width, int height, int cellWidth, int cellHeight, int topPadding);

// Check whether a Unicode codepoint is a "word" character for selection purposes.
// Word characters are alphanumeric, underscore, or non-ASCII (CJK, etc.).
// This is intentionally broad so that CJK and other scripts are treated as words.
bool isWordChar(uint32_t codepoint);

// URL detection — matches scheme-prefixed URLs (http(s)/ftp with host,
// file/git/ipfs/ipns/gemini/gopher, and ssh/mailto/tel/magnet/news).
// No bare or relative paths: every alternative requires a scheme.
// Ported from Ghostty's config/url.zig (Oniguruma regex).
struct LinkSpan {
    int startCol;
    int startRow;
    int endCol;   // exclusive
    int endRow;
    QString uri;
};

// Returns the compiled URL regex. Thread-safe (constructed once via static local).
const QRegularExpression &urlRegex();

// Find all URL matches in a flat text string, mapping match offsets back to
// cell coordinates via charMap (indexed by QChar position).
// flatText + charMap are produced by the caller (e.g. TerminalView::refreshLinks).
// charMap[i] = {col, row} for QChar position i in flatText.
struct CellCoord { uint16_t col; uint16_t row; };
QVector<LinkSpan> findUrls(const QString &flatText,
                           const QVector<CellCoord> &charMap);

// Determines whether a terminal row is soft-wrapped (content continues
// on the next row without a hard newline). Primary: the terminal WRAP
// flag (set by autowrap on program output). Fallback: full-width
// heuristic for shell/readline wrapping which positions the cursor
// manually instead of triggering autowrap. May false-positive on an
// exact-width input line followed by Enter.
bool isSoftWrapped(bool wrapFlag, bool lastCellHadContent);

} // namespace TextUtil

#endif // TEXTUTIL_H
