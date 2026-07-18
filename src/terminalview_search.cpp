#include "terminalview.h"
#include <algorithm>

namespace {
// Upper bound on collected search matches to prevent unbounded memory growth
// on pathological terminal output. 10000 matches × ~12 bytes ≈ 120 KB.
constexpr int kMaxSearchMatches = 10000;
}

void TerminalView::openSearch()
{
    if (m_searchActive)
        return;

    m_searchActive = true;

    if (m_vt) {
        m_searchCache = m_vt->extractSearchText();
        buildCellMapping();
    }

    clearSelection();
}

void TerminalView::closeSearch()
{
    if (!m_searchActive)
        return;

    m_searchActive = false;
    m_searchPattern.clear();
    m_searchCache.clear();
    m_cellMapping.clear();
    m_searchMatches.clear();
    m_currentMatchIndex = -1;
    update();
    Q_EMIT searchMatchCountChanged();
    Q_EMIT currentMatchIndexChanged();
}

void TerminalView::buildCellMapping()
{
    // Wide chars take 2 cells; supplementary-plane codepoints (emoji)
    // expand to 2 QChars (surrogate pair) in the QString. Each cell's
    // mapping must reflect the actual QChar count of its grapheme cluster,
    // or every subsequent cell in the row will be mis-aligned.
    // Mirrors refreshLinks() (terminalview_links.cpp:96-110) which emits
    // supplementary codepoints as two QChars pointing at the same cell.
    m_cellMapping.clear();
    m_cellMapping.reserve(m_searchCache.size());
    size_t totalRows = 0;
    uint16_t cols = 0;
    GhosttyTerminal terminal = m_vt ? m_vt->terminal() : nullptr;
    if (terminal) {
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TOTAL_ROWS, &totalRows);
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols);
    }
    if (!terminal || cols == 0) {
        m_cellMapping.resize(m_searchCache.size());
        return;
    }
    const int colsInt = static_cast<int>(cols);
    uint32_t graphemeBuf[128];
    for (int row = 0; row < m_searchCache.size(); row++) {
        QVector<int> mapping;
        if (row < static_cast<int>(totalRows)) {
            mapping.resize(colsInt);
            int charIdx = 0;
            const QString &line = m_searchCache[row];
            for (int cell = 0; cell < colsInt; cell++) {
                mapping[cell] = charIdx;

                GhosttyPoint point = {};
                point.tag = GHOSTTY_POINT_TAG_SCREEN;
                point.value.coordinate.x = static_cast<uint16_t>(cell);
                point.value.coordinate.y = static_cast<uint32_t>(row);
                GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
                if (ghostty_terminal_grid_ref(terminal, point, &ref) != GHOSTTY_SUCCESS)
                    continue;

                // Spacers carry no grapheme — charIdx stays put for the next cell.
                GhosttyCell cellData = 0;
                if (ghostty_grid_ref_cell(&ref, &cellData) == GHOSTTY_SUCCESS
                        && GhosttyVt::isWideSpacerCell(cellData)) {
                    continue;
                }

                // Count QChars the cell contributes to the QString:
                // BMP codepoint = 1, supplementary (e.g. emoji) = 2 (surrogate pair).
                int advance = 1; // blank cell — extractSearchText appends one space
                size_t graphemeLen = 0;
                if (ghostty_grid_ref_graphemes(&ref, graphemeBuf, 128, &graphemeLen)
                        == GHOSTTY_SUCCESS && graphemeLen > 0) {
                    advance = 0;
                    for (size_t g = 0; g < graphemeLen; ++g)
                        advance += (graphemeBuf[g] > 0xFFFF) ? 2 : 1;
                }
                if (charIdx < line.size())
                    charIdx = std::min(charIdx + advance, line.size());
            }
        }
        m_cellMapping.append(mapping);
    }
}

void TerminalView::setSearchPattern(const QString &pattern)
{
    if (pattern == m_searchPattern)
        return;

    m_searchPattern = pattern;

    if (m_vt && (m_searchCache.isEmpty() || m_vt->isSearchTextDirty())) {
        m_searchCache = m_vt->extractSearchText();
        buildCellMapping();
    }

    performSearch();

    if (m_currentMatchIndex >= 0)
        scrollToMatch(m_currentMatchIndex);

    update();
}

void TerminalView::performSearch()
{
    m_searchMatches.clear();
    m_currentMatchIndex = -1;

    if (m_searchPattern.isEmpty() || m_searchCache.isEmpty()) {
        Q_EMIT searchMatchCountChanged();
        Q_EMIT currentMatchIndexChanged();
        return;
    }

    bool searchDone = false;
    for (int row = 0; row < m_searchCache.size() && !searchDone; row++) {
        int col = 0;
        const QString &line = m_searchCache[row];
        while (col < line.size()) {
            int idx = line.indexOf(m_searchPattern, col, Qt::CaseInsensitive);
            if (idx < 0)
                break;

            int cellCol = idx; // ASCII default; mapping below adjusts for wide chars.
            int cellWidth = m_searchPattern.size();
            if (row < m_cellMapping.size() && !m_cellMapping[row].isEmpty()) {
                const QVector<int> &mapping = m_cellMapping[row];
                for (int cell = 0; cell < mapping.size(); cell++) {
                    if (mapping[cell] == idx) {
                        cellCol = cell;
                        break;
                    }
                }
                int matchEnd = idx + m_searchPattern.size();
                cellWidth = 0;
                for (int cell = cellCol; cell < mapping.size(); cell++) {
                    if (mapping[cell] >= matchEnd)
                        break;
                    cellWidth++;
                }
                // Extend cellWidth past a trailing wide-char spacer tail: the spacer
                // shares the next cell's charIdx in the mapping (spacers don't advance it).
                // Note: assumes TAIL spacers (CJK/emoji wide chars); HEAD spacers (rare RTL)
                // would also satisfy mapping[tail]==mapping[tail+1] and may over-extend.
                int tail = cellCol + cellWidth;
                if (tail < mapping.size() && tail + 1 < mapping.size()
                    && mapping[tail] == mapping[tail + 1]) {
                    cellWidth++;
                }
                if (cellWidth == 0)
                    cellWidth = 1;
            }

            m_searchMatches.append({row, cellCol, cellWidth});
            if (m_searchMatches.size() >= kMaxSearchMatches) {
                searchDone = true;
                break;
            }
            col = idx + 1;
        }
    }

    if (!m_searchMatches.isEmpty())
        m_currentMatchIndex = 0;

    Q_EMIT searchMatchCountChanged();
    Q_EMIT currentMatchIndexChanged();
}

void TerminalView::setSearchPanelHeight(int height)
{
    if (m_searchPanelHeight == height)
        return;
    m_searchPanelHeight = height;
    Q_EMIT searchPanelHeightChanged();

    // Panel height changed mid-search (e.g. navButtons appeared after the
    // first match resolved). Re-run the scroll so the current match lands
    // below the now-current panel size instead of the stale value used at
    // setSearchPattern time.
    if (m_searchActive && m_currentMatchIndex >= 0)
        scrollToMatch(m_currentMatchIndex);
}

void TerminalView::scrollToMatch(int index)
{
    if (index < 0 || index >= m_searchMatches.size() || !m_vt || !m_vt->terminal())
        return;

    const auto &match = m_searchMatches[index];

    GhosttyTerminalScrollbar scrollbar = {};
    ghostty_terminal_get(m_vt->terminal(), GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);

    const int matchRow = match.row;
    const int viewTop = static_cast<int>(scrollbar.offset);
    const int viewLen = static_cast<int>(scrollbar.len);

    // The top-docked search panel overlays the terminal in QML, hiding the
    // topmost rows even though ghostty still reports them as on-screen —
    // exclude them from the visible range below.
    const int obscuredRows = (m_searchPanelHeight > 0 && m_cellHeight > 0)
        ? (m_searchPanelHeight + m_cellHeight - 1) / m_cellHeight
        : 0;
    const int visibleTop = viewTop + obscuredRows;

    if (viewLen > 0 && matchRow >= visibleTop && matchRow < viewTop + viewLen)
        return;

    // Center the match in the viewport; when the panel covers more than half
    // the viewport, place the match just below the panel so the very overlay
    // we're compensating for doesn't re-hide it.
    const int halfView = viewLen / 2;
    const int desiredOffset = std::max(halfView, obscuredRows);
    const int targetTop = std::max(0, matchRow - desiredOffset);
    const int delta = targetTop - viewTop;

    if (delta != 0) {
        GhosttyTerminalScrollViewport scroll = {};
        scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
        scroll.value.delta = delta;
        ghostty_terminal_scroll_viewport(m_vt->terminal(), scroll);
        update();
    }
}

void TerminalView::refreshSearchCachePreservingMatch()
{
    int prevRow = (m_currentMatchIndex >= 0 && m_currentMatchIndex < m_searchMatches.size())
        ? m_searchMatches[m_currentMatchIndex].row : -1;

    m_searchCache = m_vt->extractSearchText();
    buildCellMapping();
    performSearch();

    if (prevRow >= 0) {
        int bestIdx = -1;
        int bestDist = -1;
        for (int i = 0; i < m_searchMatches.size(); ++i) {
            int d = m_searchMatches[i].row - prevRow;
            if (d < 0) d = -d;
            if (bestDist < 0 || d < bestDist) {
                bestDist = d;
                bestIdx = i;
            }
        }
        m_currentMatchIndex = bestIdx;
    }
}

void TerminalView::findNext()
{
    // Live PTY output or a resize may have invalidated the cache; refresh
    // before navigating so matches reflect the current terminal state.
    if (m_vt && m_vt->isSearchTextDirty())
        refreshSearchCachePreservingMatch();

    if (m_searchMatches.isEmpty())
        return;

    m_currentMatchIndex = (m_currentMatchIndex + 1) % m_searchMatches.size();
    scrollToMatch(m_currentMatchIndex);
    Q_EMIT currentMatchIndexChanged();
    update();
}

void TerminalView::findPrevious()
{
    if (m_vt && m_vt->isSearchTextDirty())
        refreshSearchCachePreservingMatch();

    if (m_searchMatches.isEmpty())
        return;

    m_currentMatchIndex = (m_currentMatchIndex - 1 + m_searchMatches.size()) % m_searchMatches.size();
    scrollToMatch(m_currentMatchIndex);
    Q_EMIT currentMatchIndexChanged();
    update();
}
