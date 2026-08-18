#include "terminalview.h"
#include <QTimer>
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
    }

    clearSelection();
}

void TerminalView::closeSearch()
{
    if (!m_searchActive)
        return;

    m_searchActive = false;
    m_searchPattern.clear();
    m_searchCache = VtSearchText();
    m_searchMatches.clear();
    m_currentMatchIndex = -1;
    if (m_searchRefreshTimer)
        m_searchRefreshTimer->stop();
    update();
    Q_EMIT searchMatchCountChanged();
    Q_EMIT currentMatchIndexChanged();
}

void TerminalView::setSearchPattern(const QString &pattern)
{
    QString trimmed = pattern.trimmed();
    if (trimmed == m_searchPattern)
        return;

    m_searchPattern = trimmed;

    if (m_vt && (m_searchCache.lines.isEmpty() || m_vt->isSearchTextDirty())) {
        m_searchCache = m_vt->extractSearchText();
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

    if (m_searchPattern.isEmpty() || m_searchCache.lines.isEmpty()
        || m_searchCache.logicalLines.isEmpty()) {
        Q_EMIT searchMatchCountChanged();
        Q_EMIT currentMatchIndexChanged();
        return;
    }

    const int patternLen = m_searchPattern.size();
    int logicalMatches = 0;
    bool searchDone = false;

    // Match against the logical lines (autowrap continuations joined, see
    // extractSearchText), so a phrase spanning a wrap boundary is found.
    // Each match is split into one SearchMatchSegment per spanned physical
    // row, keeping highlighting, scrollbar geometry and scrollToMatch (all
    // per physical row) unchanged.
    for (int li = 0; li < m_searchCache.logicalLines.size() && !searchDone; li++) {
        const QString &joined = m_searchCache.logicalLines[li];
        int col = 0;
        while (col < joined.size()) {
            int idx = joined.indexOf(m_searchPattern, col, Qt::CaseInsensitive);
            if (idx < 0)
                break;

            const QVector<SearchMatchSegment> segments =
                GhosttyVt::splitSearchMatch(m_searchCache, li, idx, patternLen);
            m_searchMatches += segments;

            // The kMaxSearchMatches cap counts logical matches, not the
            // per-row segments appended above. The QML "n / total" counter
            // and m_currentMatchIndex index the segment vector (the
            // renderer's contract); the stable_sort below restores row order
            // across overlapping matches.
            logicalMatches++;
            if (logicalMatches >= kMaxSearchMatches) {
                searchDone = true;
                break;
            }
            col = idx + 1;
        }
    }

    // Renderer contract: glrenderer_geometry.cpp binary-searches this vector
    // by row (std::lower_bound) and early-breaks past the viewport, so the
    // segments must be sorted by (row, cellCol). The walk above emits them in
    // match-start order, and an overlapping match can begin on an earlier row
    // than the previous match's last segment (rows "aaa"/"aaa", pattern
    // "aaaa" → [0,0] [1,0] [0,1] ...). stable_sort restores the contract.
    // Side effect on navigation: a logical match's segments can end up
    // interleaved with an overlapping neighbor's, so findNext/findPrevious
    // step per segment in visual (row) order — every segment is still visited
    // exactly once, in ascending highlight order.
    std::stable_sort(m_searchMatches.begin(), m_searchMatches.end(),
                     [](const SearchMatchSegment &a, const SearchMatchSegment &b) {
                         return a.row < b.row
                             || (a.row == b.row && a.cellCol < b.cellCol);
                     });

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
        // performSearch() already emitted with the reset value; only notify
        // QML again when the preserved-match reassignment actually moved it
        // (otherwise the "n / total" indicator stays in sync).
        if (bestIdx != m_currentMatchIndex) {
            m_currentMatchIndex = bestIdx;
            Q_EMIT currentMatchIndexChanged();
        }
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
