#include "terminalview.h"
#include "settings.h"
#include "textutil.h"

#include <algorithm>

namespace {
constexpr uint32_t kMaxGraphemesPerCell = 128;  // matches ghostty Cell.grapheme cap
}

void TerminalView::refreshLinks()
{
    // Throttle: limit to ~4Hz to avoid per-frame viewport regex scans
    // under continuous output. The dirty flag stays true until a real
    // scan runs, so links are always eventually refreshed.
    if (m_lastLinkScanTime.isValid() && m_lastLinkScanTime.elapsed() < 250)
        return;
    m_lastLinkScanTime.start();

    if (!m_vt || !m_vt->terminal() || m_cols == 0 || m_rows == 0) {
        m_linkScanDirty = false;
        return;
    }

    // When URL auto-detection is disabled, skip regex scanning.
    // OSC 8 hyperlinks still work — they're resolved independently
    // via getHyperlinkAt() in findLinkAt().
    if (!Settings::instance()->urlAutoDetect()) {
        m_currentLinks.clear();
        m_linkScanDirty = false;
        return;
    }

    GhosttyRenderState state = m_vt->renderState();
    if (!state) {
        m_linkScanDirty = false;
        return;
    }

    m_currentLinks.clear();

    // Serialize visible viewport to flat text + QChar→cell map, mirroring
    // Ghostty's renderer/link.zig: build one string, run regex, map offsets
    // back to cells.
    //
    // Uses GRAPHEMES_BUF (codepoints) rather than GRAPHEMES_UTF8 so QChar
    // indices from QRegularExpression line up with charMap positions.

    QString flatText;
    flatText.reserve(static_cast<int>(m_rows * m_cols));

    // charMap[i] = {col, row} for QChar position i in flatText
    QVector<TextUtil::CellCoord> charMap;
    charMap.reserve(static_cast<int>(m_rows * m_cols));

    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    uint16_t rowIdx = 0;
    while (ghostty_render_state_row_iterator_next(iterator)) {
        ghostty_render_state_row_get(iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells);

        // Soft-wrapped rows get no trailing \n (continuation of the line above)
        GhosttyRow rawRow = 0;
        bool isWrapped = false;
        if (ghostty_render_state_row_get(iterator,
                                         GHOSTTY_RENDER_STATE_ROW_DATA_RAW,
                                         &rawRow) == GHOSTTY_SUCCESS) {
            ghostty_row_get(rawRow, GHOSTTY_ROW_DATA_WRAP, &isWrapped);
        }

        uint16_t colIdx = 0;
        auto appendBlank = [&] {
            flatText.append(QChar(' '));
            charMap.append({colIdx, rowIdx});
        };
        while (ghostty_render_state_row_cells_next(cells)) {
            // Skip wide char spacer via render state (not grid_ref)
            GhosttyCell rawCell = 0;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                    &rawCell) == GHOSTTY_SUCCESS
                    && GhosttyVt::isWideSpacerCell(rawCell)) {
                colIdx++;
                continue;
            }

            uint32_t graphemeLen = 0;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                    &graphemeLen) == GHOSTTY_SUCCESS && graphemeLen > 0
                    && graphemeLen <= kMaxGraphemesPerCell) {
                uint32_t graphemes[kMaxGraphemesPerCell] = {};
                if (ghostty_render_state_row_cells_get(
                        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                        graphemes) == GHOSTTY_SUCCESS) {
                    for (uint32_t g = 0; g < graphemeLen; ++g) {
                        uint32_t cp = graphemes[g];
                        if (cp > 0xFFFF) {
                            // Supplementary plane — surrogate pair (2 QChars)
                            flatText.append(QChar(static_cast<char16_t>(
                                0xD800 + ((cp - 0x10000) >> 10))));
                            charMap.append({colIdx, rowIdx});
                            flatText.append(QChar(static_cast<char16_t>(
                                0xDC00 + ((cp - 0x10000) & 0x3FF))));
                            charMap.append({colIdx, rowIdx});
                        } else {
                            flatText.append(QChar(static_cast<char16_t>(cp)));
                            charMap.append({colIdx, rowIdx});
                        }
                    }
                } else {
                    appendBlank();
                }
            } else {
                appendBlank();
            }
            colIdx++;
        }

        if (!isWrapped && rowIdx + 1 < m_rows) {
            flatText.append(QChar('\n'));
            charMap.append({static_cast<uint16_t>(m_cols), rowIdx}); // sentinel
        }

        rowIdx++;
    }

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);

    // Run regex on the flat text — match.capturedStart()/capturedEnd() are
    // QChar indices that directly index into charMap.
    if (flatText.isEmpty()) {
        m_linkScanDirty = false;
        return;
    }

    m_currentLinks = TextUtil::findUrls(flatText, charMap);

    // Drop regex-detected URLs on the cursor row — typed input shouldn't
    // be clickable. OSC 8 hyperlinks are exempt (explicit app metadata,
    // resolved in findLinkAt()).
    bool cursorInView = false;
    uint16_t cursorViewportY = 0;
    ghostty_render_state_get(state,
                             GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                             &cursorInView);
    if (cursorInView) {
        ghostty_render_state_get(state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y,
                                 &cursorViewportY);
        m_currentLinks.erase(
            std::remove_if(m_currentLinks.begin(), m_currentLinks.end(),
                           [cursorViewportY](const TextUtil::LinkSpan &span) {
                               return cursorViewportY >= span.startRow
                                      && cursorViewportY <= span.endRow;
                           }),
            m_currentLinks.end());
    }

    m_linkScanDirty = false;
}

QString TerminalView::findRegexLinkAt(int col, int row) const
{
    for (const TextUtil::LinkSpan &span : m_currentLinks) {
        if (row < span.startRow || row > span.endRow)
            continue;
        if (row == span.startRow && col < span.startCol)
            continue;
        if (row == span.endRow && col >= span.endCol)
            continue;
        return span.uri;
    }
    return {};
}

QString TerminalView::findLinkAt(int col, int row)
{
    // OSC 8 hyperlinks take priority (explicit application metadata)
    if (m_vt) {
        QString uri = m_vt->getHyperlinkAt(static_cast<uint16_t>(col),
                                            static_cast<uint32_t>(row));
        if (!uri.isEmpty())
            return uri;
    }
    return findRegexLinkAt(col, row);
}
