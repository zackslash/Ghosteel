#include "glrenderer.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kFloatsPerFlatVertex = 6;     // pos(2) + color(4) — matches flat-vertex layout
constexpr uint32_t kMaxGraphemeLen = 128;   // matches ghostty Cell.grapheme cap
constexpr float kArrowLen = 8.0f;
constexpr float kArrowHalfWidth = 6.0f;
constexpr int kLinkR = 100, kLinkG = 180, kLinkB = 255, kLinkA = 200;
}

void GLRenderer::Renderer::createVBO()
{
    // Populated lazily in render() when dirty
    m_vbo = QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
    m_vbo.create();
    m_vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
}

void GLRenderer::Renderer::createFlatVBO()
{
    m_flatVbo = QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
    m_flatVbo.create();
    m_flatVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
}

void GLRenderer::Renderer::appendCircle(float cx, float cy, float radius,
                                         float r, float g, float b, float a, int segments)
{
    const float step = 2.0f * static_cast<float>(M_PI) / segments;
    for (int i = 0; i < segments; ++i) {
        float angle0 = step * i;
        float angle1 = step * (i + 1);
        // Triangle fan: center + angle i + angle i+1
        m_flatVertices << cx << cy << r << g << b << a;
        m_flatVertices << (cx + radius * cosf(angle0))
                       << (cy + radius * sinf(angle0))
                       << r << g << b << a;
        m_flatVertices << (cx + radius * cosf(angle1))
                       << (cy + radius * sinf(angle1))
                       << r << g << b << a;
    }
}

void GLRenderer::Renderer::buildMagnifierVertices(int fboW, int fboH)
{
    m_magVertices.clear();
    m_magVertexCount = 0;

    if (!m_magnifierVisible || !m_selecting || m_selStart == m_selEnd)
        return;

    QPointF fingerPos = m_magnifierFingerPos;

    int srcW = TerminalView::MagnifierWidth / TerminalView::MagnifierZoom;  // 90
    int srcH = TerminalView::MagnifierHeight / TerminalView::MagnifierZoom; // 50
    int srcX = static_cast<int>(fingerPos.x()) - srcW / 2;
    int srcY = static_cast<int>(fingerPos.y()) - srcH / 2;

    if (fboW > srcW && fboH > srcH) {
        srcX = qBound(0, srcX, fboW - srcW);
        srcY = qBound(0, srcY, fboH - srcH);
    } else {
        srcX = qMax(0, srcX);
        srcY = qMax(0, srcY);
    }

    int destX = static_cast<int>(fingerPos.x()) - TerminalView::MagnifierWidth / 2;
    int destY = static_cast<int>(fingerPos.y()) - TerminalView::MagnifierHeight - TerminalView::MagnifierOffset;

    // destY < 0 ⇒ magnifier would clip top -> flip below finger
    if (destY < 0)
        destY = static_cast<int>(fingerPos.y()) + TerminalView::MagnifierOffset;
    destX = qBound(0, destX, fboW - TerminalView::MagnifierWidth);
    destY = qBound(0, destY, fboH - TerminalView::MagnifierHeight);

    float dx0 = static_cast<float>(destX);
    float dy0 = static_cast<float>(destY);
    float dx1 = static_cast<float>(destX + TerminalView::MagnifierWidth);
    float dy1 = static_cast<float>(destY + TerminalView::MagnifierHeight);

    // UV coords: [0,1] maps the full magnifier quad; the shader maps this to
    // the source rectangle within m_pipelineTex via u_srcRect / u_srcTexSize.
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;

    // 2 triangles (6 vertices), each: pos2 + texcoord2 = 4 floats
    m_magVertices << dx0 << dy0 << u0 << v0;
    m_magVertices << dx1 << dy0 << u1 << v0;
    m_magVertices << dx1 << dy1 << u1 << v1;
    m_magVertices << dx0 << dy0 << u0 << v0;
    m_magVertices << dx1 << dy1 << u1 << v1;
    m_magVertices << dx0 << dy1 << u0 << v1;

    m_magVertexCount = 6;
}

void GLRenderer::Renderer::buildOverlayVertices(int fboW, int fboH)
{
    m_flatVertices.clear();
    m_flatVertexCount = 0;

    // Selection highlights
    if (m_selecting && m_selStart != m_selEnd) {
        float a = m_selectionHighlightColor.alphaF();
        float r = m_selectionHighlightColor.redF() * a;
        float g = m_selectionHighlightColor.greenF() * a;
        float b = m_selectionHighlightColor.blueF() * a;

        int sr = qBound(0, static_cast<int>(m_selStart.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int sc = qBound(0, static_cast<int>(m_selStart.x()) / m_cellWidth, m_cols - 1);
        int er = qBound(0, static_cast<int>(m_selEnd.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int ec = qBound(0, static_cast<int>(m_selEnd.x()) / m_cellWidth, m_cols - 1);

        if (sr > er || (sr == er && sc > ec)) {
            qSwap(sr, er);
            qSwap(sc, ec);
        }

        for (int row = sr; row <= er; ++row) {
            float y = row * m_cellHeight + m_topPadding;
            float x0 = (row == sr) ? sc * m_cellWidth : 0;
            float x1 = (row == er) ? (ec + 1) * m_cellWidth : m_cols * m_cellWidth;
            float y1 = y + m_cellHeight;

            // 2 triangles = 6 vertices, each: pos(2) + color(4) = 6 floats
            m_flatVertices << x0 << y  << r << g << b << a;
            m_flatVertices << x1 << y  << r << g << b << a;
            m_flatVertices << x1 << y1 << r << g << b << a;
            m_flatVertices << x0 << y  << r << g << b << a;
            m_flatVertices << x1 << y1 << r << g << b << a;
            m_flatVertices << x0 << y1 << r << g << b << a;
        }
    }

    // Search highlights
    if (m_searchActive && !m_searchMatches.isEmpty()) {
        int scrollOffset = m_scrollOffset;
        int visibleStartRow = scrollOffset;
        int visibleEndRow = scrollOffset + m_rows;

        // m_searchMatches is sorted by row — binary search for the first
        // match at/after the visible range instead of a linear scan of up to
        // 10k matches every frame. The draw loop below breaks as soon as a
        // match passes visibleEndRow, so no further pre-filtering is needed.
        const auto firstVisible = std::lower_bound(
            m_searchMatches.begin(), m_searchMatches.end(), visibleStartRow,
            [](const TerminalView::SearchMatch &m, int row) { return m.row < row; });
        int startIdx = static_cast<int>(firstVisible - m_searchMatches.begin());

        for (int i = startIdx; i < m_searchMatches.size(); ++i) {
            const auto &match = m_searchMatches[i];
            if (match.row > visibleEndRow) break;

            int viewportRow = match.row - scrollOffset;
            if (viewportRow < 0 || viewportRow >= m_rows) continue;

            float y = viewportRow * m_cellHeight + m_topPadding;
            float x = match.cellCol * m_cellWidth;
            float w = match.cellWidth * m_cellWidth;

            if (x + w > m_cols * m_cellWidth)
                w = m_cols * m_cellWidth - x;

            float x1 = x + w;
            float y1 = y + m_cellHeight;

            QColor color = (i == m_currentMatchIndex) ? m_searchCurrentColor : m_searchHighlightColor;
            float a = color.alphaF();
            float cr = color.redF() * a;
            float cg = color.greenF() * a;
            float cb = color.blueF() * a;

            m_flatVertices << x  << y  << cr << cg << cb << a;
            m_flatVertices << x1 << y  << cr << cg << cb << a;
            m_flatVertices << x1 << y1 << cr << cg << cb << a;
            m_flatVertices << x  << y  << cr << cg << cb << a;
            m_flatVertices << x1 << y1 << cr << cg << cb << a;
            m_flatVertices << x  << y1 << cr << cg << cb << a;
        }
    }

    // Link underlines
    if (!m_linkSpans.isEmpty()) {
        float la = kLinkA / 255.0f;
        float lr = (kLinkR / 255.0f) * la;
        float lg = (kLinkG / 255.0f) * la;
        float lb = (kLinkB / 255.0f) * la;

        for (const auto &span : m_linkSpans) {
            for (int r = span.startRow; r <= span.endRow; ++r) {
                if (r < 0 || r >= m_rows) continue;

                int colStart = (r == span.startRow) ? span.startCol : 0;
                int colEnd = (r == span.endRow) ? span.endCol : m_cols;

                // 2px-tall rect at bottom of cell
                float y = r * m_cellHeight + m_topPadding + m_cellHeight - 2;
                float x0 = colStart * m_cellWidth;
                float x1 = colEnd * m_cellWidth;
                float y1 = y + 2;

                m_flatVertices << x0 << y  << lr << lg << lb << la;
                m_flatVertices << x1 << y  << lr << lg << lb << la;
                m_flatVertices << x1 << y1 << lr << lg << lb << la;
                m_flatVertices << x0 << y  << lr << lg << lb << la;
                m_flatVertices << x1 << y1 << lr << lg << lb << la;
                m_flatVertices << x0 << y1 << lr << lg << lb << la;
            }
        }
    }

    // Shell exit overlay (full-screen semi-transparent rect)
    if (m_shellExited) {
        float a = m_shellExitOverlayColor.alphaF();
        float r = m_shellExitOverlayColor.redF() * a;
        float g = m_shellExitOverlayColor.greenF() * a;
        float b = m_shellExitOverlayColor.blueF() * a;

        float x0 = 0.0f, y0 = 0.0f;
        float x1 = static_cast<float>(fboW);
        float y1 = static_cast<float>(fboH);

        m_flatVertices << x0 << y0 << r << g << b << a;
        m_flatVertices << x1 << y0 << r << g << b << a;
        m_flatVertices << x1 << y1 << r << g << b << a;
        m_flatVertices << x0 << y0 << r << g << b << a;
        m_flatVertices << x1 << y1 << r << g << b << a;
        m_flatVertices << x0 << y1 << r << g << b << a;
    }

    // Selection handles (tessellated circles)
    if (m_handlesVisible && m_selecting && !m_magnifierVisible && m_selStart != m_selEnd) {
        int sr = qBound(0, static_cast<int>(m_selStart.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int sc = qBound(0, static_cast<int>(m_selStart.x()) / m_cellWidth, m_cols - 1);
        int er = qBound(0, static_cast<int>(m_selEnd.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int ec = qBound(0, static_cast<int>(m_selEnd.x()) / m_cellWidth, m_cols - 1);

        if (sr > er || (sr == er && sc > ec)) {
            qSwap(sr, er);
            qSwap(sc, ec);
        }

        // Border
        float ba = m_selectionHandleBorderColor.alphaF();
        float br = m_selectionHandleBorderColor.redF() * ba;
        float bg = m_selectionHandleBorderColor.greenF() * ba;
        float bb = m_selectionHandleBorderColor.blueF() * ba;

        // Fill
        float fa = m_selectionHandleColor.alphaF();
        float fr = m_selectionHandleColor.redF() * fa;
        float fg = m_selectionHandleColor.greenF() * fa;
        float fb = m_selectionHandleColor.blueF() * fa;

        // Start handle — border then fill
        float sx = sc * m_cellWidth;
        float sy = (sr + 1) * m_cellHeight + m_topPadding;
        appendCircle(sx, sy, TerminalView::HandleRadius + 2, br, bg, bb, ba);
        appendCircle(sx, sy, TerminalView::HandleRadius, fr, fg, fb, fa);

        // End handle — border then fill
        float ex = (ec + 1) * m_cellWidth;
        float ey = (er + 1) * m_cellHeight + m_topPadding;
        appendCircle(ex, ey, TerminalView::HandleRadius + 2, br, bg, bb, ba);
        appendCircle(ex, ey, TerminalView::HandleRadius, fr, fg, fb, fa);
    }

    // Magnifier arrow (drawn after magnifier quad via separate draw call)
    if (m_magnifierVisible && m_selecting && m_selStart != m_selEnd) {
        // Compute dest rect to find arrow position (same logic as buildMagnifierVertices)
        QPointF fingerPos = m_magnifierFingerPos;
        int destX = static_cast<int>(fingerPos.x()) - TerminalView::MagnifierWidth / 2;
        int unclampedDestY = static_cast<int>(fingerPos.y()) - TerminalView::MagnifierHeight - TerminalView::MagnifierOffset;
        int destY = unclampedDestY;
        if (destY < 0)
            destY = static_cast<int>(fingerPos.y()) + TerminalView::MagnifierOffset;
        destX = qBound(0, destX, fboW - TerminalView::MagnifierWidth);
        destY = qBound(0, destY, fboH - TerminalView::MagnifierHeight);

        float arrowCenterX = destX + TerminalView::MagnifierWidth / 2.0f;
        // Arrow at bottom edge of magnifier on screen, pointing DOWN toward finger.
        // Ortho is Y-up; Qt scene graph flips the FBO so low Y = top on screen.
        // Bottom of magnifier on screen = high FBO Y = destY + MagnifierHeight.
        float arrowBase = static_cast<float>(destY + TerminalView::MagnifierHeight);
        float arrowTip = arrowBase + kArrowLen;

        // If magnifier was flipped below finger, arrow at top edge pointing UP
        if (unclampedDestY < 0) {
            arrowBase = static_cast<float>(destY);
            arrowTip = arrowBase - kArrowLen;
        }

        float a = m_magnifierBorderColor.alphaF();
        float r = m_magnifierBorderColor.redF() * a;
        float g = m_magnifierBorderColor.greenF() * a;
        float b = m_magnifierBorderColor.blueF() * a;

        m_flatVertices << (arrowCenterX - kArrowHalfWidth) << arrowBase << r << g << b << a;
        m_flatVertices << arrowCenterX << arrowTip << r << g << b << a;
        m_flatVertices << (arrowCenterX + kArrowHalfWidth) << arrowBase << r << g << b << a;
    }

    m_flatVertexCount = m_flatVertices.size() / kFloatsPerFlatVertex;
    if (m_flatVertexCount > 0) {
        m_flatVbo.bind();
        m_flatVbo.allocate(m_flatVertices.constData(),
                           m_flatVertices.size() * sizeof(float));
        m_flatVbo.release();
    }
}

void GLRenderer::Renderer::buildCellVertices(GhosttyRenderState state)
{
    m_cellVertices.clear();
    m_cellVertices.reserve(m_cols * m_rows * 6);

    float bgAlpha = m_bgOpacity;
    float bgR = m_postBgR, bgG = m_postBgG, bgB = m_postBgB;
    float fgR = m_postFgR, fgG = m_postFgG, fgB = m_postFgB;

    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    int y = m_topPadding;
    int rowIdx = 0;
    while (ghostty_render_state_row_iterator_next(iterator)) {
        ghostty_render_state_row_get(iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells);

        int x = 0;
        int colIdx = 0;
        while (ghostty_render_state_row_cells_next(cells)) {
            // Wide flag
            GhosttyCell rawCell = 0;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            if (ghostty_render_state_row_cells_get(cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &rawCell) == GHOSTTY_SUCCESS
                    && rawCell != 0) {
                ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &wide);
            }

            // Spacer-tail skip:
            // The preceding WIDE_WIDE head already emitted a 2-cell quad covering
            // this spacer's screen position (head advanced x by 2*cellWidth).
            // Do NOT advance x — bare continue.
            if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
                continue;
            }

            GhosttyColorRgb cellBg;
            float cBgR = bgR, cBgG = bgG, cBgB = bgB;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                    &cellBg) == GHOSTTY_SUCCESS) {
                cBgR = cellBg.r / 255.0f;
                cBgG = cellBg.g / 255.0f;
                cBgB = cellBg.b / 255.0f;
            }

            GhosttyColorRgb cellFg;
            float cFgR = fgR, cFgG = fgG, cFgB = fgB;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                    &cellFg) == GHOSTTY_SUCCESS) {
                cFgR = cellFg.r / 255.0f;
                cFgG = cellFg.g / 255.0f;
                cFgB = cellFg.b / 255.0f;
            }

            GhosttyStyle cellStyle = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(
                cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                &cellStyle);
            float deco = 0.0f;
            if (cellStyle.underline > 0) deco = 1.0f;
            else if (cellStyle.strikethrough) deco = 2.0f;

            float pFgR = cFgR, pFgG = cFgG, pFgB = cFgB, pFgA = 1.0f;
            float pBgR = cBgR * bgAlpha, pBgG = cBgG * bgAlpha, pBgB = cBgB * bgAlpha, pBgA = bgAlpha;

            // Glyph lookup
            uint32_t graphemesLen = 0;
            ghostty_render_state_row_cells_get(
                cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                &graphemesLen);

            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            if (graphemesLen > 0 && graphemesLen <= kMaxGraphemeLen) {
                uint32_t buf[kMaxGraphemeLen];
                ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                    buf);

                const GlyphInfo &gi = (graphemesLen == 1)
                    ? m_atlas.glyph(buf[0], cellStyle.bold, cellStyle.italic)
                    : m_atlas.glyphCluster(buf, graphemesLen, cellStyle.bold, cellStyle.italic);
                u0 = gi.u0; v0 = gi.v0; u1 = gi.u1; v1 = gi.v1;
            }

            // Quad emission — width from grid wide flag
            int cellSpan = (wide == GHOSTTY_CELL_WIDE_WIDE) ? 2 : 1;
            float x0 = static_cast<float>(x);
            float y0 = static_cast<float>(y);
            float x1 = static_cast<float>(x + cellSpan * m_cellWidth);
            float y1 = static_cast<float>(y + m_cellHeight);

            m_cellVertices.append({x0, y0, u0, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x1, y0, u1, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x1, y1, u1, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x0, y0, u0, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x1, y1, u1, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
            m_cellVertices.append({x0, y1, u0, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});

            x += cellSpan * m_cellWidth;
            colIdx += cellSpan;
        }

        y += m_cellHeight;
        rowIdx++;
    }

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);

    // Fill the cell-grid leftover (width % cellWidth, often ~1px) so the FBO
    // is bg-filled edge-to-edge — without this the transparent clear-color
    // strip shows as a gap when content slides during a session swipe.
    // Texcoord (0,0) -> atlas reserved transparent pixel -> bg-only output
    // (same as empty cells). Strips split on the grid boundary to avoid
    // double-applying the premultiplied bg at the corner.
    float sAlpha = m_bgOpacity;
    float spBgR = m_postBgR * sAlpha, spBgG = m_postBgG * sAlpha, spBgB = m_postBgB * sAlpha, spBgA = sAlpha;
    float spFgR = m_postFgR, spFgG = m_postFgG, spFgB = m_postFgB, spFgA = 1.0f;

    int gridW = m_cols * m_cellWidth;
    int gridH = m_topPadding + m_rows * m_cellHeight;

    // Right strip: columns beyond the grid, full viewport height.
    if (m_viewportWidth > gridW && m_cellWidth > 0) {
        float x0 = static_cast<float>(gridW), x1 = static_cast<float>(m_viewportWidth);
        float y0 = 0.0f, y1 = static_cast<float>(m_viewportHeight);
        m_cellVertices.append({x0, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x0, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x0, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
    }

    // Top strip: padding band above the grid (y in [0, m_topPadding]), grid width
    // only — the right strip above already covers this band for x in [gridW, viewportWidth].
    if (m_topPadding > 0 && m_cellWidth > 0) {
        float x0 = 0.0f, x1 = static_cast<float>(gridW);
        float y0 = 0.0f, y1 = static_cast<float>(m_topPadding);
        m_cellVertices.append({x0, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x0, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x0, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
    }

    // Bottom strip: rows beyond the grid, grid width only (the right strip covers the rest).
    if (m_viewportHeight > gridH && m_cellHeight > 0) {
        float x0 = 0.0f, x1 = static_cast<float>(gridW);
        float y0 = static_cast<float>(gridH), y1 = static_cast<float>(m_viewportHeight);
        m_cellVertices.append({x0, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x0, y0, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x1, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
        m_cellVertices.append({x0, y1, 0, 0, spFgR, spFgG, spFgB, spFgA, spBgR, spBgG, spBgB, spBgA, 0});
    }
}
