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
    // Build cell-to-character index mapping for CJK/emoji support.
    // Wide chars (CJK/emoji) take 2 cells but produce 1 QChar;
    // without this mapping, search highlights would be wrong.
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
    for (int row = 0; row < m_searchCache.size(); row++) {
        QVector<int> mapping;
        if (row < static_cast<int>(totalRows)) {
            mapping.resize(colsInt);
            int charIdx = 0;
            const QString &line = m_searchCache[row];
            // Use the wide-spacer cache from extractSearchText() when available,
            // avoiding redundant ghostty_terminal_grid_ref calls per cell.
            const QVector<QVector<bool>> &spacers = m_vt->wideSpacerCache();
            bool hasSpacerCache = (row < spacers.size()
                                   && spacers[row].size() == colsInt);
            for (int cell = 0; cell < colsInt; cell++) {
                mapping[cell] = charIdx;
                bool isSpacer = hasSpacerCache
                    ? spacers[row][cell]
                    : GhosttyVt::isWideCharSpacer(terminal, static_cast<uint16_t>(cell), static_cast<uint32_t>(row));
                if (isSpacer) {
                    continue;
                }
                if (charIdx < line.size())
                    charIdx++;
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

void TerminalView::findNext()
{
    if (m_searchMatches.isEmpty())
        return;

    m_currentMatchIndex = (m_currentMatchIndex + 1) % m_searchMatches.size();
    scrollToMatch(m_currentMatchIndex);
    Q_EMIT currentMatchIndexChanged();
    update();
}

void TerminalView::findPrevious()
{
    if (m_searchMatches.isEmpty())
        return;

    m_currentMatchIndex = (m_currentMatchIndex - 1 + m_searchMatches.size()) % m_searchMatches.size();
    scrollToMatch(m_currentMatchIndex);
    Q_EMIT currentMatchIndexChanged();
    update();
}
