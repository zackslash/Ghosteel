#ifndef TEXTUTIL_H
#define TEXTUTIL_H

#include <QString>
#include <QPointF>

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

} // namespace TextUtil

#endif // TEXTUTIL_H
