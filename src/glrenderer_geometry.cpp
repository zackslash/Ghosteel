#include "glrenderer.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {
constexpr int kFloatsPerFlatVertex = 6;     // pos(2) + color(4) — matches flat-vertex layout
constexpr uint32_t kMaxGraphemeLen = 128;   // buffer must hold >= 1 + ghostty's grapheme_max_len (64); BUF is fetched unconditionally and ghostty truncates clusters at 64
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

void GLRenderer::Renderer::bindCellVertexFormat()
{
    // Bind the cell VBO and (re-)establish the interleaved CellVertex layout
    // for the cell program. ES2 has no VAOs, so attrib enable/pointer state is
    // global and other passes (kitty) may have repointed the same attribute
    // indices at their own buffers — re-assert everything before every draw of
    // the split cell pass.
    m_vbo.bind();
    const int stride = 13 * sizeof(float);
    if (m_positionAttr >= 0) {
        glEnableVertexAttribArray(m_positionAttr);
        glVertexAttribPointer(m_positionAttr, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    }
    if (m_texcoordAttr >= 0) {
        glEnableVertexAttribArray(m_texcoordAttr);
        glVertexAttribPointer(m_texcoordAttr, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(2 * sizeof(float)));
    }
    if (m_fgColorAttr >= 0) {
        glEnableVertexAttribArray(m_fgColorAttr);
        glVertexAttribPointer(m_fgColorAttr, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(4 * sizeof(float)));
    }
    if (m_bgColorAttr >= 0) {
        glEnableVertexAttribArray(m_bgColorAttr);
        glVertexAttribPointer(m_bgColorAttr, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(8 * sizeof(float)));
    }
    if (m_decoAttr >= 0) {
        glEnableVertexAttribArray(m_decoAttr);
        glVertexAttribPointer(m_decoAttr, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(12 * sizeof(float)));
    }
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
            [](const SearchMatchSegment &m, int row) { return m.row < row; });
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

    if (m_handlesVisible && m_selecting && !m_magnifierVisible && m_selStart != m_selEnd) {
        int sr = qBound(0, static_cast<int>(m_selStart.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int sc = qBound(0, static_cast<int>(m_selStart.x()) / m_cellWidth, m_cols - 1);
        int er = qBound(0, static_cast<int>(m_selEnd.y() - m_topPadding) / m_cellHeight, m_rows - 1);
        int ec = qBound(0, static_cast<int>(m_selEnd.x()) / m_cellWidth, m_cols - 1);

        if (sr > er || (sr == er && sc > ec)) {
            qSwap(sr, er);
            qSwap(sc, ec);
        }

        float ba = m_selectionHandleBorderColor.alphaF();
        float br = m_selectionHandleBorderColor.redF() * ba;
        float bg = m_selectionHandleBorderColor.greenF() * ba;
        float bb = m_selectionHandleBorderColor.blueF() * ba;

        float fa = m_selectionHandleColor.alphaF();
        float fr = m_selectionHandleColor.redF() * fa;
        float fg = m_selectionHandleColor.greenF() * fa;
        float fb = m_selectionHandleColor.blueF() * fa;

        float sx = sc * m_cellWidth;
        float sy = (sr + 1) * m_cellHeight + m_topPadding;
        appendCircle(sx, sy, TerminalView::HandleRadius + 2, br, bg, bb, ba);
        appendCircle(sx, sy, TerminalView::HandleRadius, fr, fg, fb, fa);

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

void GLRenderer::Renderer::emitRowVertices(QVector<CellVertex> &out, GhosttyRenderStateRowCells cells, float y, int *outVertexCount)
{
    const int start = out.size();

    const float bgAlpha = m_bgOpacity;
    const float bgR = m_postBgR, bgG = m_postBgG, bgB = m_postBgB;
    const float fgR = m_postFgR, fgG = m_postFgG, fgB = m_postFgB;

    int x = 0;
    while (ghostty_render_state_row_cells_next(cells)) {
        GhosttyCell rawCell = 0;
        GhosttyStyle cellStyle = GHOSTTY_INIT_SIZED(GhosttyStyle);
        uint32_t graphemesLen = 0;
        uint32_t graphemesBuf[kMaxGraphemeLen];
        GhosttyColorRgb cellFg, cellBg;

        // The four infallible keys are fetched in one batch. FG/BG are fetched
        // separately because each returns GHOSTTY_INVALID_VALUE when the cell
        // has no explicit color of that type, and get_multi stops at the first
        // failing key — batching them would drop the other color entirely.
        GhosttyRenderStateRowCellsData keys[] = {
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
        };
        void *values[] = { &rawCell, &cellStyle, &graphemesLen, graphemesBuf };
        ghostty_render_state_row_cells_get_multi(cells, 4, keys, values, nullptr);

        const bool haveFg = ghostty_render_state_row_cells_get(
            cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &cellFg) == GHOSTTY_SUCCESS;
        const bool haveBg = ghostty_render_state_row_cells_get(
            cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &cellBg) == GHOSTTY_SUCCESS;

        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        if (rawCell != 0) {
            ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &wide);
        }

        // Spacer-tail skip:
        // The preceding WIDE_WIDE head already emitted a 2-cell quad covering
        // this spacer's screen position (head advanced x by 2*cellWidth).
        // Do NOT advance x — bare continue.
        if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
            continue;
        }

        float cBgR = bgR, cBgG = bgG, cBgB = bgB;
        if (haveBg) {
            cBgR = cellBg.r / 255.0f;
            cBgG = cellBg.g / 255.0f;
            cBgB = cellBg.b / 255.0f;
        }

        float cFgR = fgR, cFgG = fgG, cFgB = fgB;
        if (haveFg) {
            cFgR = cellFg.r / 255.0f;
            cFgG = cellFg.g / 255.0f;
            cFgB = cellFg.b / 255.0f;
        }

        float deco = 0.0f;
        if (cellStyle.underline > 0) deco = 1.0f;
        else if (cellStyle.strikethrough) deco = 2.0f;

        float pFgR = cFgR, pFgG = cFgG, pFgB = cFgB, pFgA = 1.0f;
        float pBgR = cBgR * bgAlpha, pBgG = cBgG * bgAlpha, pBgB = cBgB * bgAlpha, pBgA = bgAlpha;

        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        if (graphemesLen > 0 && graphemesLen <= kMaxGraphemeLen) {
            const GlyphInfo &gi = (graphemesLen == 1)
                ? m_atlas.glyph(graphemesBuf[0], cellStyle.bold, cellStyle.italic)
                : m_atlas.glyphCluster(graphemesBuf, graphemesLen, cellStyle.bold, cellStyle.italic);
            u0 = gi.u0; v0 = gi.v0; u1 = gi.u1; v1 = gi.v1;
        }

        int cellSpan = (wide == GHOSTTY_CELL_WIDE_WIDE) ? 2 : 1;
        float x0 = static_cast<float>(x);
        float y0 = y;
        float x1 = static_cast<float>(x + cellSpan * m_cellWidth);
        float y1 = y + m_cellHeight;

        out.append({x0, y0, u0, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
        out.append({x1, y0, u1, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
        out.append({x1, y1, u1, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
        out.append({x0, y0, u0, v0, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
        out.append({x1, y1, u1, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});
        out.append({x0, y1, u0, v1, pFgR, pFgG, pFgB, pFgA, pBgR, pBgG, pBgB, pBgA, deco});

        x += cellSpan * m_cellWidth;
    }

    if (outVertexCount)
        *outVertexCount = out.size() - start;
}

void GLRenderer::Renderer::appendCellVertices(GhosttyRenderState state)
{
    m_cellVertices.reserve(m_cols * m_rows * 6);

    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    m_rowVertexStart.clear();
    m_rowVertexCount.clear();
    m_topPaddingAtBuild = m_topPadding;
    m_viewportWidthAtBuild = m_viewportWidth;
    m_viewportHeightAtBuild = m_viewportHeight;

    int y = m_topPadding;
    while (ghostty_render_state_row_iterator_next(iterator)) {
        ghostty_render_state_row_get(iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells);

        int count = 0;
        emitRowVertices(m_cellVertices, cells, static_cast<float>(y), &count);
        m_rowVertexStart.append(m_cellVertices.size() - count);
        m_rowVertexCount.append(count);

        y += m_cellHeight;
    }

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);

    m_stripVertexStart = m_cellVertices.size();

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

void GLRenderer::Renderer::updateCellVertices(GhosttyRenderState state)
{
    // The per-row segment bookkeeping must match the grid; if it does not
    // (e.g. a full build was skipped), fall back to a full rebuild.
    // buildCellVertices re-records the segment bookkeeping; the caller still
    // runs ghostty_render_state_clean() after this returns.
    if (m_rowVertexCount.size() != m_rows) {
        buildCellVertices(state);
        return;
    }

    const int startEpoch = m_atlas.epoch();

    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    QVector<CellVertex> newVerts;
    newVerts.reserve(m_cellVertices.size());
    QVector<int> newRowStart;
    QVector<int> newRowCount;
    newRowStart.reserve(m_rows);
    newRowCount.reserve(m_rows);

    // Merge the dirty-row stream with the grid rows: dirty rows are emitted
    // fresh, clean rows are spliced verbatim from the old segments. The
    // iterator stays positioned on the current dirty row for row_get.
    uint16_t nextDirtyY = 0;
    bool haveDirty = ghostty_render_state_row_iterator_next_dirty(iterator, &nextDirtyY);
    // DIRTY_PARTIAL with no dirty rows: the grid is unchanged, so the current
    // vertex buffer is already correct. Skip the full copy/splice/swap.
    if (!haveDirty) {
        ghostty_render_state_row_cells_free(cells);
        ghostty_render_state_row_iterator_free(iterator);
        return;
    }
    for (int r = 0; r < m_rows; ++r) {
        int count = 0;
        if (haveDirty && nextDirtyY == static_cast<uint16_t>(r)) {
            ghostty_render_state_row_get(iterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells);
            emitRowVertices(newVerts, cells, static_cast<float>(m_topPadding + r * m_cellHeight), &count);
            haveDirty = ghostty_render_state_row_iterator_next_dirty(iterator, &nextDirtyY);
        } else {
            count = m_rowVertexCount[r];
            // Single copy straight into newVerts (reserved to the old size),
            // avoiding the temporary QVector that mid() + += would create.
            std::copy(m_cellVertices.constData() + m_rowVertexStart[r],
                      m_cellVertices.constData() + m_rowVertexStart[r] + m_rowVertexCount[r],
                      std::back_inserter(newVerts));
        }
        newRowStart.append(newVerts.size() - count);
        newRowCount.append(count);
    }

    // Strips depend only on metrics/viewport, which force a full rebuild
    // when they change; splice them verbatim.
    const int stripCount = m_cellVertices.size() - m_stripVertexStart;
    newVerts += m_cellVertices.mid(m_stripVertexStart, stripCount);

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);

    // Glyph rasterization during the walk can wipe the atlas, invalidating
    // UVs baked into both fresh and copied segments; fall back to a full
    // rebuild (its own epoch-retry logic then applies).
    if (m_atlas.epoch() != startEpoch) {
        buildCellVertices(state);
        return;
    }

    m_cellVertices = newVerts;
    m_rowVertexStart = newRowStart;
    m_rowVertexCount = newRowCount;
    m_stripVertexStart = newVerts.size() - stripCount;
}

void GLRenderer::Renderer::buildCellVertices(GhosttyRenderState state)
{
    // Rasterizing a glyph can fill the atlas mid-walk: clearAtlas() wipes the
    // texture and restarts shelf packing, so every quad emitted before the
    // wipe holds UVs into the now-repacked region (visible corruption until
    // the next rebuild). Capture the epoch; if it changed by the end of the
    // walk, rebuild once. The single retry handles the common single-wipe case
    // (the glyph caches are warm after the first pass). If wipes cascade
    // (working set exceeds the atlas) the retry itself can hold stale UVs —
    // accepted rather than looping.
    const int startEpoch = m_atlas.epoch();

    m_cellVertices.clear();
    appendCellVertices(state);

    if (m_atlas.epoch() != startEpoch) {
        m_cellVertices.clear();
        appendCellVertices(state);
    }
}
