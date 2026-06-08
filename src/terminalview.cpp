#include "terminalview.h"
#include "ptymanager.h"
#include "settings.h"
#include "keymapping.h"
#include "textutil.h"

#include <QPainter>
#include <QDebug>
#include <QClipboard>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethod>
#include <QTimer>
#include <QTouchEvent>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QLineF>
#include <cstring>
#include <algorithm>
#include <sys/ioctl.h>

static const int TopPadding = 12; // px, visual comfort padding at top of terminal

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setActiveFocusOnTab(true);
    setOpaquePainting(false); // Support transparent backgrounds

    // Monospace font — DejaVu Sans Mono is standard on Sailfish OS
    m_font = QFont(QStringLiteral("DejaVu Sans Mono"), static_cast<int>(m_fontSize));
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    updateFontMetrics();

    m_vt = new GhosttyVt(this);
    m_pty = new PtyManager(this);

    connect(m_pty, &PtyManager::dataReady, this, &TerminalView::onPtyData);
    connect(m_pty, &PtyManager::shellExited, this, &TerminalView::onShellExited);
    connect(m_vt, &GhosttyVt::titleChanged, this, [this](const QString &t) {
        m_title = t;
        Q_EMIT titleChanged();
    });
    connect(m_vt, &GhosttyVt::bell, this, &TerminalView::terminalBell);
    connect(m_vt, &GhosttyVt::desktopNotification, this, &TerminalView::desktopNotification);

    // Live-apply settings changes to running terminal
    connect(Settings::instance(), &Settings::colorSchemeChanged, this, [this]() {
        if (m_vt && m_vt->terminal()) {
            applyColorScheme();
        }
    });
    connect(Settings::instance(), &Settings::backgroundOpacityChanged, this, [this]() {
        m_needsRender = true;
        update();
    });
    connect(Settings::instance(), &Settings::fontFamilyChanged, this, [this]() {
        updateFontMetrics();
        m_needsRender = true;
        update();
    });

    // Start cursor blink timer
    m_blinkTimerId = startTimer(BlinkInterval);
}

TerminalView::~TerminalView()
{
    cleanup();
}

void TerminalView::cleanup()
{
    // Disconnect all signals from PtyManager to prevent any pending
    // cross-thread signal deliveries from hitting a destroyed object.
    if (m_pty) {
        disconnect(m_pty, nullptr, this, nullptr);
        m_pty->stop();
    }
    if (m_vt) {
        m_vt->destroy();
    }
}

void TerminalView::geometryChanged(const QRectF &newGeometry,
                                   const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChanged(newGeometry, oldGeometry);

    if (newGeometry.width() <= 0 || newGeometry.height() <= 0)
        return;

    recalculateDimensions();
}

void TerminalView::recalculateDimensions()
{
    // Guard against zero cell dimensions (font not yet initialized)
    if (m_cellWidth <= 0 || m_cellHeight <= 0)
        return;

    if (width() <= 0 || height() <= 0)
        return;

    // Calculate terminal dimensions from available size
    // TopPadding for visual comfort
    auto dim = TextUtil::calculateDimensions(width(), height(), m_cellWidth, m_cellHeight, TopPadding);
    uint16_t newCols = dim.cols;
    uint16_t newRows = dim.rows;

    if (newCols != m_cols || newRows != m_rows) {
        bool wasStarted = (m_cols > 0 && m_rows > 0);
        m_cols = newCols;
        m_rows = newRows;

        if (wasStarted && m_pty->childPid() > 0) {
            // Resize existing terminal
            struct winsize ws = {};
            ws.ws_col = m_cols;
            ws.ws_row = m_rows;
            ioctl(m_pty->ptyFd(), TIOCSWINSZ, &ws);

            // Resize ghostty terminal state
            if (m_vt->terminal()) {
                ghostty_terminal_resize(m_vt->terminal(), m_cols, m_rows,
                                        m_cellWidth, m_cellHeight);

                // Update mouse encoder geometry
                m_vt->updateMouseEncoderSize(
                    static_cast<uint32_t>(width()),
                    static_cast<uint32_t>(height()),
                    static_cast<uint32_t>(m_cellWidth),
                    static_cast<uint32_t>(m_cellHeight),
                    static_cast<uint32_t>(TopPadding));
            }
        } else {
            setupTerminal();
        }

        m_needsRender = true;
        update();
    }
}

void TerminalView::focusInEvent(QFocusEvent *event)
{
    QQuickPaintedItem::focusInEvent(event);

    // Show the software keyboard when terminal gains focus
    QInputMethod *im = QGuiApplication::inputMethod();
    if (im && !m_suppressKeyboardAutoShow)
        im->show();
    m_suppressKeyboardAutoShow = false;

    update();
}

void TerminalView::focusOutEvent(QFocusEvent *event)
{
    QQuickPaintedItem::focusOutEvent(event);

    // Reset input method state when losing focus
    QInputMethod *im = QGuiApplication::inputMethod();
    if (im)
        im->reset();
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    resetBlinkOnInput();

    // Commit text from the input method (what the user actually typed)
    if (!event->commitString().isEmpty()) {
        QByteArray utf8 = event->commitString().toUtf8();

        // If sticky modifiers are active (Ctrl/Alt from keybar toggle),
        // send as a key event with modifiers, then clear them.
        if (m_stickyModifiers != 0) {
            // Map the first character to a GhosttyKey
            QChar ch = event->commitString().at(0).toLower();
            GhosttyKey key = KeyMapping::mapCharToKey(ch);

            if (key != GHOSTTY_KEY_UNIDENTIFIED) {
                sendKeyEvent(key, GHOSTTY_KEY_ACTION_PRESS,
                             static_cast<GhosttyMods>(m_stickyModifiers),
                             event->commitString());
                setStickyModifiers(0);
                m_needsRender = true;
                update();
                event->accept();
                return;
            }
            // If we can't map the character, fall through to raw text
            setStickyModifiers(0);
        }

        m_pty->writeData(utf8.constData(), utf8.size());

        // If scrolled up viewing history, scroll back to bottom
        scrollViewportToBottom();

        m_needsRender = true;
        update();
        event->accept();
        return;
    }

    // Preedit text (composing/IMF preview) — we don't display it in the
    // terminal buffer, but we must accept the event so the IME knows
    // we're handling it. On commit, the final text will arrive as
    // commitString above.
    if (!event->preeditString().isEmpty()) {
        event->accept();
        return;
    }

    // If the event carries a key action (e.g. from virtual keyboard
    // special keys like Backspace via IME), handle it
    if (event->replacementStart() != 0 || event->replacementLength() != 0) {
        // Handle text editing operations from IME
        event->accept();
        return;
    }

    QQuickPaintedItem::inputMethodEvent(event);
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImHints:
        // Terminal accepts all input — no auto-capitalize, no predictive text
        return static_cast<int>(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText
                                | Qt::ImhSensitiveData);
    case Qt::ImCursorRectangle:
        // Tell the IME where the cursor is so it can position the candidate window
    {
        if (m_cols == 0 || m_rows == 0)
            return QRectF();

        // Get cursor position from render state
        GhosttyRenderState state = m_vt ? m_vt->renderState() : nullptr;
        if (!state)
            return QRectF();

        bool cursorInViewport = false;
        ghostty_render_state_get(state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                                 &cursorInViewport);
        if (!cursorInViewport)
            return QRectF();

        uint16_t cx = 0, cy = 0;
        ghostty_render_state_get(state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
        ghostty_render_state_get(state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);

        return QRectF(cx * m_cellWidth, cy * m_cellHeight + TopPadding,
                      m_cellWidth, m_cellHeight);
    }
    default:
        return QQuickPaintedItem::inputMethodQuery(query);
    }
}

void TerminalView::applyColorScheme()
{
    if (!m_vt || !m_vt->terminal())
        return;

    struct ColorDef { GhosttyColorRgb fg, bg, cursor; };
    static const QMap<QString, ColorDef> schemes = {
        {"light",          {{51,51,51},       {255,255,255},   {0,0,0}}},
        {"solarized-dark", {{147,161,161},    {0,43,54},       {203,75,22}}},
        {"solarized-light",{{101,123,131},    {253,246,227},   {203,75,22}}},
        {"monokai",        {{248,248,242},    {39,40,34},      {248,248,242}}},
        {"dark",           {{204,204,204},    {30,30,30},      {255,255,255}}},
    };

    QString scheme = Settings::instance()->colorScheme();
    auto it = schemes.constFind(scheme);
    if (it == schemes.constEnd())
        it = schemes.constFind(QStringLiteral("dark"));

    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &it->fg);
    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &it->bg);
    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &it->cursor);

    m_needsRender = true;
    update();
}

void TerminalView::setupTerminal()
{
    if (m_cols == 0 || m_rows == 0)
        return;

    // Create terminal
    if (!m_vt->create(m_cols, m_rows, [this](const char *data, size_t len) {
            m_pty->writeData(data, len);
        })) {
        qWarning() << "Failed to create GhosttyVt";
        return;
    }

    // Apply color scheme from settings
    applyColorScheme();

    // Resize terminal with pixel dimensions
    ghostty_terminal_resize(m_vt->terminal(), m_cols, m_rows,
                            m_cellWidth, m_cellHeight);

    // Restore scrollback if pending (must be before startShell)
    if (!m_pendingScrollback.isEmpty()) {
        m_vt->restoreScrollback(m_pendingScrollback, m_cols);
        m_pendingScrollback.clear();
    }

    // Configure mouse encoder geometry
    m_vt->updateMouseEncoderSize(
        static_cast<uint32_t>(width()),
        static_cast<uint32_t>(height()),
        static_cast<uint32_t>(m_cellWidth),
        static_cast<uint32_t>(m_cellHeight),
        static_cast<uint32_t>(TopPadding));

    // Start shell (use configured command if set)
    m_pty->setShellCommand(Settings::instance()->shellCommand());
    if (!m_pty->startShell(m_cols, m_rows)) {
        qWarning() << "Failed to start shell";
        m_vt->destroy();
        return;
    }

    // Run autorun command after shell initializes
    if (!m_autorunCommand.isEmpty()) {
        QTimer::singleShot(AutorunDelayMs, this, &TerminalView::runAutorunCommand);
    }
}

void TerminalView::onPtyData(const QByteArray &data)
{
    m_vt->vtWrite(reinterpret_cast<const uint8_t *>(data.constData()),
                   data.size());
    m_needsRender = true;
    // Coalesce rapid updates — start timer if not already running.
    // This batches multiple PTY data events into a single repaint at ~60fps.
    // Keypresses call update() directly for immediate response.
    if (m_renderTimerId == 0)
        m_renderTimerId = startTimer(RenderInterval);
}

void TerminalView::onShellExited(int exitCode)
{
    qInfo() << "Shell exited with code" << exitCode;
    m_shellExited = true;
    m_shellExitCode = exitCode;
    m_needsRender = true;
    update();
}

void TerminalView::restartShell()
{
    closeSearch(); // Clear stale search state before destroying terminal
    m_shellExited = false;
    m_shellExitCode = 0;
    m_pty->stop();
    m_vt->destroy();
    setupTerminal();
    m_needsRender = true;
    update();
}

void TerminalView::setActive(bool active)
{
    if (active) {
        if (m_blinkTimerId == 0)
            m_blinkTimerId = startTimer(BlinkInterval);
    } else {
        if (m_blinkTimerId) {
            killTimer(m_blinkTimerId);
            m_blinkTimerId = 0;
        }
        // Free render buffer for hidden sessions to save memory
        m_image = QImage();
        m_needsRender = true;
    }
}

void TerminalView::paint(QPainter *painter)
{
    if (m_cols == 0 || m_rows == 0)
        return;

    if (m_needsRender) {
        renderCells(painter);
        m_needsRender = false;
    } else if (!m_image.isNull()) {
        painter->drawImage(0, 0, m_image);
    }

    // Draw selection highlight on top of cached image (updates every frame during drag)
    if (m_selecting && m_selStart != m_selEnd) {
        drawSelectionHighlight(painter, 0, 0, 1.0);
    }

    // Draw search match highlights (below handles/magnifier)
    if (m_searchActive && !m_searchMatches.isEmpty()) {
        drawSearchHighlights(painter);
    }

    // Draw selection handles when selection is finalized (not during active drag)
    if (m_handlesVisible && m_selecting && m_selStart != m_selEnd && !m_magnifierVisible) {
        drawSelectionHandles(painter);
    }

    // Draw magnifier on top of everything when actively dragging selection
    if (m_magnifierVisible && m_selStart != m_selEnd) {
        renderMagnifier(painter);
    }
}

void TerminalView::renderCells(QPainter *painter)
{
    int w = static_cast<int>(width());
    int h = static_cast<int>(height());
    if (w <= 0 || h <= 0)
        return;

    // Only re-allocate QImage when size changes
    if (m_image.width() != w || m_image.height() != h || m_image.isNull())
        m_image = QImage(w, h, QImage::Format_RGBA8888);

    // Update render state (single-threaded — all calls are from main thread)
    m_vt->updateRenderState();

    GhosttyRenderState state = m_vt->renderState();
    if (!state) {
        m_image.fill(Qt::black);
        painter->drawImage(0, 0, m_image);
        return;
    }

    // Get default colors
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_colors_get(state, &colors);

    QColor bgColor(colors.background.r, colors.background.g, colors.background.b);
    QColor fgColor(colors.foreground.r, colors.foreground.g, colors.foreground.b);

    // Fill background with opacity
    QPainter imgPainter(&m_image);
    float opacity = Settings::instance()->backgroundOpacity();

    if (opacity < 1.0f) {
        m_image.fill(Qt::transparent);
        imgPainter.setOpacity(opacity);
    }
    imgPainter.fillRect(m_image.rect(), bgColor);
    imgPainter.setOpacity(1.0);
    imgPainter.setFont(m_font);

    renderCellGrid(&imgPainter, state, bgColor, fgColor);
    drawCursor(&imgPainter, state, colors, fgColor);
    imgPainter.end();

    drawShellExitOverlay();

    painter->drawImage(0, 0, m_image);
}

bool TerminalView::readCellAt(GhosttyRenderState state, int col, int row,
                              const QColor &defaultBg, CellData &out) const
{
    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    int rowIdx = 0;
    bool found = false;
    while (ghostty_render_state_row_iterator_next(iterator)) {
        if (rowIdx == row) {
            ghostty_render_state_row_get(iterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells);
            if (ghostty_render_state_row_cells_select(cells, col) == GHOSTTY_SUCCESS) {
                // Background color
                GhosttyColorRgb cellBg;
                out.bgColor = defaultBg;
                if (ghostty_render_state_row_cells_get(
                        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                        &cellBg) == GHOSTTY_SUCCESS) {
                    out.bgColor = QColor(cellBg.r, cellBg.g, cellBg.b);
                }

                // Graphemes
                out.graphemesLen = 0;
                ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                    &out.graphemesLen);
                if (out.graphemesLen > 0 && out.graphemesLen <= 128) {
                    ghostty_render_state_row_cells_get(
                        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                        out.graphemes);
                }

                // Style
                out.style = GHOSTTY_INIT_SIZED(GhosttyStyle);
                ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &out.style);

                out.valid = true;
                found = true;
            }
            break;
        }
        rowIdx++;
    }

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);
    return found;
}

void TerminalView::drawBlockCursorText(QPainter *painter, int px, int py,
                                       GhosttyRenderState state, const QColor &bgColor,
                                       const QColor & /* fgColor */)
{
    // Try cached cursor cell first (populated during renderCellGrid)
    if (m_cachedCursor.valid && m_cachedCursor.graphemesLen > 0) {
        QString text = QString::fromUcs4(m_cachedCursor.graphemes, m_cachedCursor.graphemesLen);
        painter->setFont(fontForStyle(m_cachedCursor.style));
        painter->setPen(m_cachedCursor.bgColor); // use cell's actual bg as text color
        painter->drawText(QPointF(px, py + painter->fontMetrics().ascent()), text);
        return;
    }

    // Fallback: O(rows) lookup if cursor wasn't in the rendered viewport
    uint16_t cx = 0, cy = 0;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);

    CellData cell;
    if (readCellAt(state, cx, cy, bgColor, cell) && cell.graphemesLen > 0 && cell.graphemesLen <= 128) {
        QString text = QString::fromUcs4(cell.graphemes, cell.graphemesLen);
        painter->setFont(fontForStyle(cell.style));
        painter->setPen(cell.bgColor);
        painter->drawText(QPointF(px, py + painter->fontMetrics().ascent()), text);
    } else if (cell.valid && cell.graphemesLen > 128) {
        painter->setFont(fontForStyle(cell.style));
        painter->setPen(cell.bgColor);
        painter->drawText(QPointF(px, py + painter->fontMetrics().ascent()),
                          QStringLiteral("\u2468"));
    }
}

void TerminalView::drawCursor(QPainter *painter, GhosttyRenderState state,
                              const GhosttyRenderStateColors &colors,
                              const QColor &fgColor)
{
    bool cursorVisible = false;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE,
                             &cursorVisible);

    bool cursorInViewport = false;
    ghostty_render_state_get(state,
                             GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                             &cursorInViewport);

    if (!cursorVisible || !cursorInViewport || !m_cursorBlinkVisible)
        return;

    uint16_t cx = 0, cy = 0;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);

    GhosttyRenderStateCursorVisualStyle cursorStyle;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE,
                             &cursorStyle);

    int px = cx * m_cellWidth;
    int py = cy * m_cellHeight + TopPadding;

    QColor cursorColor = QColor(colors.cursor.r, colors.cursor.g, colors.cursor.b);
    if (!colors.cursor_has_value)
        cursorColor = fgColor;

    QColor bgColor(colors.background.r, colors.background.g, colors.background.b);

    switch (cursorStyle) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK:
        // Draw cursor background, then redraw the cell text on top
        // with inverted colors (background becomes foreground)
        painter->setPen(Qt::NoPen);
        painter->setBrush(cursorColor);
        painter->drawRect(px, py, m_cellWidth, m_cellHeight);
        drawBlockCursorText(painter, px, py, state, bgColor, fgColor);
        break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
        painter->setPen(QPen(cursorColor, 2));
        painter->drawLine(px, py, px, py + m_cellHeight);
        break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
        painter->setPen(QPen(cursorColor, 2));
        painter->drawLine(px, py + m_cellHeight - 1,
                          px + m_cellWidth, py + m_cellHeight - 1);
        break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
        painter->setPen(QPen(cursorColor, 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(px, py, m_cellWidth - 1, m_cellHeight - 1);
        break;
    default:
        break;
    }
}

void TerminalView::renderCellGrid(QPainter *painter, GhosttyRenderState state,
                                  const QColor &bgColor, const QColor &fgColor)
{
    // Iterate rows
    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    // Invalidate cursor cache at start of each render pass
    m_cachedCursor.valid = false;

    // Get cursor position for caching during row iteration
    bool wantCursorCache = false;
    uint16_t cacheCx = 0, cacheCy = 0;
    {
        bool cv = false, civ = false;
        ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cv);
        ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &civ);
        if (cv && civ) {
            ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cacheCx);
            ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cacheCy);
            wantCursorCache = true;
        }
    }

    int y = TopPadding;
    int rowIdx = 0;
    while (ghostty_render_state_row_iterator_next(iterator)) {
        ghostty_render_state_row_get(iterator,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells);

        int x = 0;
        int colIdx = 0;
        while (ghostty_render_state_row_cells_next(cells)) {
            // Cell background color
            GhosttyColorRgb cellBg;
            QColor cellBgColor = bgColor;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                    &cellBg) == GHOSTTY_SUCCESS) {
                cellBgColor = QColor(cellBg.r, cellBg.g, cellBg.b);
            }

            // Draw cell background (only if different from default)
            if (cellBgColor != bgColor) {
                painter->fillRect(x, y, m_cellWidth, m_cellHeight,
                                  cellBgColor);
            }

            // Cell foreground color
            GhosttyColorRgb cellFg;
            QColor cellFgColor = fgColor;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                    &cellFg) == GHOSTTY_SUCCESS) {
                cellFgColor = QColor(cellFg.r, cellFg.g, cellFg.b);
            }

            // Cell text
            uint32_t graphemesLen = 0;
            ghostty_render_state_row_cells_get(
                cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                &graphemesLen);

            if (graphemesLen > 0) {
                uint32_t buf[128]; // Generous buffer for complex grapheme clusters
                if (graphemesLen <= 128) {
                    ghostty_render_state_row_cells_get(
                        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                        buf);

                    // Cache cursor cell data for block cursor rendering
                    if (wantCursorCache && rowIdx == cacheCy && colIdx == cacheCx) {
                        std::memcpy(m_cachedCursor.graphemes, buf, graphemesLen * sizeof(uint32_t));
                        m_cachedCursor.graphemesLen = graphemesLen;
                        m_cachedCursor.style = GHOSTTY_INIT_SIZED(GhosttyStyle);
                        ghostty_render_state_row_cells_get(
                            cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                            &m_cachedCursor.style);
                        m_cachedCursor.bgColor = cellBgColor;
                        m_cachedCursor.valid = true;
                    }
                    QString text = QString::fromUcs4(buf, graphemesLen);

                    // Apply style (bold/italic) from pre-cached variants
                    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
                    ghostty_render_state_row_cells_get(
                        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                        &style);

                    painter->setFont(fontForStyle(style));

                    painter->setPen(cellFgColor);
                    painter->drawText(QPointF(x, y + painter->fontMetrics().ascent()),
                                      text);

                    // Draw underline
                    if (style.underline) {
                        int underlineY = y + m_cellHeight - 2;
                        painter->drawLine(x, underlineY, x + m_cellWidth, underlineY);
                    }
                    // Draw strikethrough
                    if (style.strikethrough) {
                        int strikeY = y + m_cellHeight / 2;
                        painter->drawLine(x, strikeY, x + m_cellWidth, strikeY);
                    }
                } else {
                    // Grapheme cluster too complex for buffer — render placeholder
                    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
                    ghostty_render_state_row_cells_get(
                        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                        &style);
                    painter->setFont(fontForStyle(style));
                    painter->setPen(cellFgColor);
                    painter->drawText(QPointF(x, y + painter->fontMetrics().ascent()),
                                      QStringLiteral("\u2468"));
                }
            }

            x += m_cellWidth;
            colIdx++;
        }

        y += m_cellHeight;
        rowIdx++;
    }

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);
}

void TerminalView::drawShellExitOverlay()
{
    if (!m_shellExited)
        return;

    QPainter overlayPainter(&m_image);
    overlayPainter.setPen(Qt::NoPen);
    overlayPainter.setBrush(QColor(0, 0, 0, 180));
    overlayPainter.drawRect(m_image.rect());

    overlayPainter.setPen(Qt::white);
    QFont overlayFont(QStringLiteral("DejaVu Sans Mono"), 14);
    overlayPainter.setFont(overlayFont);
    QString msg = tr("Shell exited with code %1\n\nTap to restart").arg(m_shellExitCode);
    overlayPainter.drawText(QRectF(0, 0, width(), height()),
                            Qt::AlignCenter | Qt::TextWordWrap, msg);
    overlayPainter.end();
}

void TerminalView::renderMagnifier(QPainter *painter)
{
    // SailfishOS-style magnifier: zoomed view of terminal around the finger
    // Shows a 2x zoomed bubble above the touch point for precise text selection

    if (m_image.isNull())
        return;

    QPointF fingerPos = (m_draggingHandle == 1) ? m_selStart : m_selEnd;

    // Source region: the area around the finger to zoom into
    int srcW = MagnifierWidth / MagnifierZoom;
    int srcH = MagnifierHeight / MagnifierZoom;
    int srcX = static_cast<int>(fingerPos.x()) - srcW / 2;
    int srcY = static_cast<int>(fingerPos.y()) - srcH / 2;

    // Clamp source to image bounds
    srcX = qBound(0, srcX, m_image.width() - srcW);
    srcY = qBound(0, srcY, m_image.height() - srcH);

    // Destination: above the finger, centered horizontally
    int destX = static_cast<int>(fingerPos.x()) - MagnifierWidth / 2;
    int destY = static_cast<int>(fingerPos.y()) - MagnifierHeight - MagnifierOffset;

    // Clamp destination to stay within viewport
    if (destY < 0)
        destY = static_cast<int>(fingerPos.y()) + MagnifierOffset; // Below finger if no room above
    int viewW = static_cast<int>(width());
    int viewH = static_cast<int>(height());
    destX = qBound(0, destX, viewW - MagnifierWidth);
    destY = qBound(0, destY, viewH - MagnifierHeight);

    QRectF srcRect(srcX, srcY, srcW, srcH);
    QRectF destRect(destX, destY, MagnifierWidth, MagnifierHeight);

    painter->save();

    // Clip to rounded rectangle for the bubble shape
    QPainterPath clipPath;
    clipPath.addRoundedRect(destRect, 8, 8);
    painter->setClipPath(clipPath);

    // Draw zoomed content
    painter->drawImage(destRect, m_image, srcRect);

    // Draw selection highlight inside the magnifier at zoomed coordinates
    // offsetX/offsetY adjust so that cell coordinates map to magnifier dest space:
    //   destX + (cellX - srcX) * zoom = offsetX + cellX * zoom
    //   => offsetX = destX - srcX * zoom
    if (m_selecting && m_selStart != m_selEnd) {
        qreal offX = destRect.x() - srcX * MagnifierZoom;
        qreal offY = destRect.y() - srcY * MagnifierZoom;
        drawSelectionHighlight(painter, offX, offY, MagnifierZoom);
    }

    // Draw border (SailfishOS highlight color style)
    painter->setClipping(false);
    QPen borderPen(QColor(255, 255, 255, 120), 2.0);
    painter->setPen(borderPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(destRect.adjusted(1, 1, -1, -1), 8, 8);

    // Draw a small triangle/arrow pointing down to the finger
    QPointF arrowCenter(destRect.center().x(), destRect.bottom());
    QPolygonF arrow;
    arrow << QPointF(arrowCenter.x() - 6, arrowCenter.y())
          << QPointF(arrowCenter.x(), arrowCenter.y() + 8)
          << QPointF(arrowCenter.x() + 6, arrowCenter.y());
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(255, 255, 255, 120));
    painter->drawPolygon(arrow);

    painter->restore();
}

const QFont &TerminalView::fontForStyle(const GhosttyStyle &style) const
{
    if (style.bold && style.italic)
        return m_fontBoldItalic;
    if (style.bold)
        return m_fontBold;
    if (style.italic)
        return m_fontItalic;
    return m_font;
}

void TerminalView::paste()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QString text = clipboard->text(QClipboard::Clipboard);
    if (text.isEmpty())
        return;

    QByteArray utf8 = text.toUtf8();

    // ghostty_paste_encode modifies data in place. Use a copy for the
    // sizing call, then a fresh copy for the actual encode to avoid
    // double-processing of already-mutated data.
    QByteArray sizingCopy = utf8;

    // First call: query required size (pass nullptr buffer)
    size_t written = 0;
    GhosttyResult res = ghostty_paste_encode(sizingCopy.data(), sizingCopy.size(), true,
                                             nullptr, 0, &written);
    if (res == GHOSTTY_OUT_OF_SPACE && written > 0) {
        // Second call with correctly sized buffer — use fresh copy
        QByteArray encodeCopy = utf8;
        QByteArray buf(written, '\0');
        res = ghostty_paste_encode(encodeCopy.data(), encodeCopy.size(), true,
                                   buf.data(), buf.size(), &written);
        if (res == GHOSTTY_SUCCESS && written > 0) {
            m_pty->writeData(buf.constData(), written);
            return;
        }
    } else if (res == GHOSTTY_SUCCESS && written > 0) {
        // Data was small enough to encode in-place (sizingCopy already mutated)
        m_pty->writeData(sizingCopy.constData(), written);
        return;
    }

    // Fallback: send raw UTF-8 only if encoding completely fails
    m_pty->writeData(utf8.constData(), utf8.size());
}

void TerminalView::copySelection()
{
    if (!m_vt || !m_vt->renderState())
        return;

    QPointF startCell = cellFromPixel(m_selStart);
    QPointF endCell = cellFromPixel(m_selEnd);
    if (startCell.x() < 0 || endCell.x() < 0)
        return;

    // Normalize: ensure start <= end
    int startCol = static_cast<int>(startCell.x());
    int startRow = static_cast<int>(startCell.y());
    int endCol = static_cast<int>(endCell.x());
    int endRow = static_cast<int>(endCell.y());
    if (startRow > endRow || (startRow == endRow && startCol > endCol)) {
        qSwap(startCol, endCol);
        qSwap(startRow, endRow);
    }

    GhosttyRenderState state = m_vt->renderState();
    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    QString text;
    int rowIdx = 0;
    while (ghostty_render_state_row_iterator_next(iterator)) {
        if (rowIdx < startRow) { rowIdx++; continue; }
        if (rowIdx > endRow) break;

        ghostty_render_state_row_get(iterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells);

        int colIdx = 0;
        while (ghostty_render_state_row_cells_next(cells)) {
            if (rowIdx == startRow && colIdx < startCol) { colIdx++; continue; }
            if (rowIdx == endRow && colIdx > endCol) break;

            uint32_t graphemesLen = 0;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &graphemesLen);
            if (graphemesLen > 0 && graphemesLen <= 128) {
                uint32_t buf[128];
                ghostty_render_state_row_cells_get(cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, buf);
                text += QString::fromUcs4(buf, graphemesLen);
            } else if (graphemesLen == 0) {
                text += QLatin1Char(' ');
            }

            colIdx++;
        }

        if (rowIdx < endRow)
            text += QLatin1Char('\n');

        rowIdx++;
    }

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);

    // Trim trailing whitespace from each line, remove trailing empty lines.
    // Preserve leading whitespace (indentation is significant).
    QString result = TextUtil::trimSelectionText(text);
    if (!result.isEmpty()) {
        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(result, QClipboard::Clipboard);
    }

    // Update selectedText property for QML share action
    if (m_selectedText != result) {
        m_selectedText = result;
        Q_EMIT selectedTextChanged();
    }
}

void TerminalView::setFontSize(int size)
{
    if (size < 6) size = 6;
    if (size > 32) size = 32;
    if (m_fontSize == size)
        return;

    m_fontSize = size;
    updateFontMetrics();
    Q_EMIT fontSizeChanged();

    // Trigger geometry recalculation — cell dimensions changed so
    // cols/rows will differ, causing a terminal resize
    if (width() > 0 && height() > 0) {
        recalculateDimensions();
    }
}

void TerminalView::setStickyModifiers(int mods)
{
    if (m_stickyModifiers == mods)
        return;
    m_stickyModifiers = mods;
    Q_EMIT stickyModifiersChanged();
}

void TerminalView::updateFontMetrics()
{
    QString family = Settings::instance()->fontFamily();
    if (family.isEmpty())
        family = QStringLiteral("DejaVu Sans Mono");
    m_font = QFont(family, static_cast<int>(m_fontSize));
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);

    m_fontBold = m_font;
    m_fontBold.setBold(true);
    m_fontItalic = m_font;
    m_fontItalic.setItalic(true);
    m_fontBoldItalic = m_font;
    m_fontBoldItalic.setBold(true);
    m_fontBoldItalic.setItalic(true);

    QFontMetrics fm(m_font);
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    m_cellWidth = fm.horizontalAdvance(QLatin1Char('M'));
#else
    m_cellWidth = fm.width(QLatin1Char('M'));
#endif
    m_cellHeight = fm.height();
}

void TerminalView::sendKey(int qtKey, int modifiers)
{
    resetBlinkOnInput();

    GhosttyKey key = KeyMapping::mapQtKey(qtKey);
    // Accept GhosttyMods directly (not Qt modifier values)
    sendKeyEvent(key, GHOSTTY_KEY_ACTION_PRESS, static_cast<GhosttyMods>(modifiers), QString());
    m_needsRender = true;
    update();
}

QPointF TerminalView::cellFromPixel(const QPointF &pos) const
{
    return TextUtil::cellFromPixel(pos, m_cellWidth, m_cellHeight, m_cols, m_rows, TopPadding);
}

void TerminalView::clearSelection()
{
    if (m_selecting) {
        m_selecting = false;
        m_magnifierVisible = false;
        m_handlesVisible = false;
        m_draggingHandle = 0;
        setKeepMouseGrab(false);
        if (m_longPressTimerId) {
            killTimer(m_longPressTimerId);
            m_longPressTimerId = 0;
        }
        m_velocityInitialized = false;
        // Clear selected text and notify QML
        if (!m_selectedText.isEmpty()) {
            m_selectedText.clear();
            Q_EMIT selectedTextChanged();
        }
        m_needsRender = true;
        update();
        m_tapCount = 0; // Only reset when clearing an active selection
    }
}

void TerminalView::selectWordAt(const QPointF &pos)
{
    QPointF cell = cellFromPixel(pos);
    if (cell.x() < 0)
        return;

    int col = static_cast<int>(cell.x());
    int row = static_cast<int>(cell.y());

    GhosttyRenderState state = m_vt ? m_vt->renderState() : nullptr;
    if (!state)
        return;

    // Get the row's cells to scan for word boundaries
    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iterator);

    GhosttyRenderStateRowCells cells;
    ghostty_render_state_row_cells_new(nullptr, &cells);

    int rowIdx = 0;
    while (ghostty_render_state_row_iterator_next(iterator)) {
        if (rowIdx == row) {
            ghostty_render_state_row_get(iterator, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells);
            break;
        }
        rowIdx++;
    }

    if (rowIdx != row) {
        // Row not found (shouldn't happen)
        ghostty_render_state_row_cells_free(cells);
        ghostty_render_state_row_iterator_free(iterator);
        return;
    }

    // Check if the tapped cell is a word character
    // First, get the grapheme at the tapped column
    auto getGraphemeAt = [&](int c) -> uint32_t {
        if (ghostty_render_state_row_cells_select(cells, static_cast<uint16_t>(c)) != GHOSTTY_SUCCESS)
            return 0;
        uint32_t len = 0;
        ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &len);
        if (len == 0)
            return 0;
        uint32_t buf[128];
        if (len > 128) return 0;
        ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, buf);
        return buf[0]; // base codepoint
    };

    uint32_t tappedChar = getGraphemeAt(col);
    bool tappedIsWord = TextUtil::isWordChar(tappedChar);

    int startCol = col;
    int endCol = col;

    if (tappedIsWord) {
        // Expand left to find word start
        while (startCol > 0) {
            uint32_t ch = getGraphemeAt(startCol - 1);
            if (!TextUtil::isWordChar(ch))
                break;
            startCol--;
        }
        // Expand right to find word end
        while (endCol < m_cols - 1) {
            uint32_t ch = getGraphemeAt(endCol + 1);
            if (!TextUtil::isWordChar(ch))
                break;
            endCol++;
        }
    } else {
        // Non-word character: select just this character (whitespace/punctuation)
        // Expand left/right over contiguous non-word, non-space characters
        // Actually, for non-word chars like punctuation, select just the single char.
        // For spaces, select the run of spaces.
        if (tappedChar == 0) {
            // Empty cell — select a run of empty cells
            while (startCol > 0) {
                uint32_t ch = getGraphemeAt(startCol - 1);
                if (ch != 0) break;
                startCol--;
            }
            while (endCol < m_cols - 1) {
                uint32_t ch = getGraphemeAt(endCol + 1);
                if (ch != 0) break;
                endCol++;
            }
        }
        // Otherwise: single punctuation char selected (startCol == endCol == col)
    }

    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(iterator);

    // Convert cell boundaries to pixel coordinates
    m_selStart = QPointF(startCol * m_cellWidth, row * m_cellHeight + TopPadding);
    m_selEnd = QPointF((endCol + 1) * m_cellWidth - 1, row * m_cellHeight + TopPadding);
    m_selecting = true;
    m_magnifierVisible = false; // No magnifier for double-tap — instant selection
    m_handlesVisible = true;    // Show drag handles for precise adjustment

    copySelection();
    m_needsRender = true;
    update();
}

void TerminalView::selectLineAt(const QPointF &pos)
{
    QPointF cell = cellFromPixel(pos);
    if (cell.x() < 0)
        return;

    int row = static_cast<int>(cell.y());

    // Select entire row from column 0 to last column
    m_selStart = QPointF(0, row * m_cellHeight + TopPadding);
    m_selEnd = QPointF(m_cols * m_cellWidth - 1, row * m_cellHeight + TopPadding);
    m_selecting = true;
    m_magnifierVisible = false; // No magnifier for triple-tap — instant selection
    m_handlesVisible = true;    // Show drag handles for precise adjustment

    copySelection();
    m_needsRender = true;
    update();
}

void TerminalView::drawSelectionHighlight(QPainter *painter, qreal offsetX, qreal offsetY, qreal scale)
{
    QPointF startCell = cellFromPixel(m_selStart);
    QPointF endCell = cellFromPixel(m_selEnd);
    if (startCell.x() < 0 || endCell.x() < 0)
        return;

    int sc = static_cast<int>(startCell.x());
    int sr = static_cast<int>(startCell.y());
    int ec = static_cast<int>(endCell.x());
    int er = static_cast<int>(endCell.y());
    if (sr > er || (sr == er && sc > ec)) {
        qSwap(sc, ec);
        qSwap(sr, er);
    }

    QColor highlightColor(255, 255, 255, 76);
    painter->setPen(Qt::NoPen);
    for (int row = sr; row <= er; row++) {
        int cellStartX = (row == sr) ? sc * m_cellWidth : 0;
        int cellEndX = (row == er) ? (ec + 1) * m_cellWidth : m_cols * m_cellWidth;
        int cellY = row * m_cellHeight + TopPadding;
        qreal rx = offsetX + cellStartX * scale;
        qreal ry = offsetY + cellY * scale;
        qreal rw = (cellEndX - cellStartX) * scale;
        qreal rh = m_cellHeight * scale;
        painter->fillRect(QRectF(rx, ry, rw, rh), highlightColor);
    }
}

void TerminalView::drawSelectionHandles(QPainter *painter)
{
    QPointF startCell = cellFromPixel(m_selStart);
    QPointF endCell = cellFromPixel(m_selEnd);
    if (startCell.x() < 0 || endCell.x() < 0)
        return;

    int sc = static_cast<int>(startCell.x());
    int sr = static_cast<int>(startCell.y());
    int ec = static_cast<int>(endCell.x());
    int er = static_cast<int>(endCell.y());

    // No normalization — handle positions match m_selStart/m_selEnd directly
    // so handle 1 always corresponds to m_selStart and handle 2 to m_selEnd
    QPointF startPos(sc * m_cellWidth, (sr + 1) * m_cellHeight + TopPadding);
    QPointF endPos((ec + 1) * m_cellWidth, (er + 1) * m_cellHeight + TopPadding);

    painter->setPen(Qt::NoPen);

    // Draw handles as filled circles with highlight color
    QColor handleColor(255, 255, 255, 200);
    QColor borderColor(255, 255, 255, 120);

    for (const QPointF &pos : {startPos, endPos}) {
        QRectF circle(pos.x() - HandleRadius, pos.y() - HandleRadius,
                      HandleRadius * 2, HandleRadius * 2);

        // Shadow/border
        painter->setBrush(borderColor);
        painter->drawEllipse(circle.adjusted(-2, -2, 2, 2));

        // Fill
        painter->setBrush(handleColor);
        painter->drawEllipse(circle);
    }
}

int TerminalView::handleHitTest(const QPointF &pos) const
{
    if (!m_handlesVisible || !m_selecting || m_selStart == m_selEnd)
        return 0;

    QPointF startCell = cellFromPixel(m_selStart);
    QPointF endCell = cellFromPixel(m_selEnd);
    if (startCell.x() < 0 || endCell.x() < 0)
        return 0;

    int sc = static_cast<int>(startCell.x());
    int sr = static_cast<int>(startCell.y());
    int ec = static_cast<int>(endCell.x());
    int er = static_cast<int>(endCell.y());

    // No normalization swap — handle 1 always maps to m_selStart,
    // handle 2 always maps to m_selEnd, regardless of selection direction
    QPointF startPos(sc * m_cellWidth, (sr + 1) * m_cellHeight + TopPadding);
    QPointF endPos((ec + 1) * m_cellWidth, (er + 1) * m_cellHeight + TopPadding);

    // Use a generous hit area (HandleRadius + some padding for finger imprecision)
    qreal hitRadius = HandleRadius * 1.5;

    if (QLineF(pos, startPos).length() <= hitRadius)
        return 1; // Start handle (m_selStart)
    if (QLineF(pos, endPos).length() <= hitRadius)
        return 2; // End handle (m_selEnd)

    return 0; // No hit
}

bool TerminalView::updateMagnifierVelocity(const QPointF &pos)
{
    // Minimum interval between velocity samples — skip sub-frame deltas
    // that produce noisy velocity spikes from tiny position jitter
    static const qint64 MinVelocityDtMs = 16;

    if (m_velocityInitialized) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64 dt = now - m_lastMoveTime;
        if (dt >= MinVelocityDtMs) {
            qreal dist = QLineF(pos, m_lastMovePos).length();
            qreal velocity = dist * 1000.0 / dt; // pixels per second
            m_lastMoveTime = now;
            m_lastMovePos = pos;
            // Hysteresis: use different thresholds to prevent flicker
            // when velocity oscillates around the boundary
            if (m_magnifierVisible)
                return velocity <= MagnifierVelocityHide;  // hide only above 600
            else
                return velocity < MagnifierVelocityShow;   // show only below 400
        }
        if (dt > 0) {
            m_lastMoveTime = now;
            m_lastMovePos = pos;
        }
        return m_magnifierVisible; // no change when dt is too small
    }
    // First move after activation — initialize velocity tracking
    m_lastMoveTime = QDateTime::currentMSecsSinceEpoch();
    m_lastMovePos = pos;
    m_velocityInitialized = true;
    return true; // show magnifier on first move
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    // If shell has exited, tap to restart
    if (m_shellExited) {
        restartShell();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_cursorBlinkVisible = true;
        m_lastInputTime.start();

        // Check if the terminal has mouse tracking enabled (TUI app mode)
        m_mouseTrackingActive = m_vt->isMouseTracking();
        m_touchStartPos = event->pos();

        if (m_mouseTrackingActive) {
            // Forward mouse press to the terminal as an escape sequence
            sendMouseEvent(GHOSTTY_MOUSE_ACTION_PRESS, GHOSTTY_MOUSE_BUTTON_LEFT,
                           event->pos(), KeyMapping::mapQtModifiers(event->modifiers()));

            // Tell encoder a button is pressed (enables motion events)
            m_mouseButtonPressed = true;
            m_vt->setMouseButtonPressed(true);

            // Prevent SilicaFlickable from stealing drag gestures
            setKeepMouseGrab(true);
            event->accept();
            return;
        }

        // Check if tapping a selection handle (before tap detection or clearing)
        int handle = handleHitTest(event->pos());
        if (handle != 0) {
            m_draggingHandle = handle;
            m_handlesVisible = false; // Hide handles while dragging
            m_magnifierVisible = true;
            m_velocityInitialized = false;
            m_tapCount = 0; // Prevent phantom triple-tap after handle drag
            setKeepMouseGrab(true);
            event->accept();
            return;
        }

        // No mouse tracking — detect double/triple tap for word/line selection
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qreal dist = QLineF(event->pos(), m_lastTapPos).length();
        bool withinWindow = (m_tapCount > 0)
            && (now - m_lastTapTime) <= TapTimeoutMs
            && dist <= TapDistancePx;

        if (withinWindow) {
            m_tapCount = qMin(m_tapCount + 1, 3);
        } else {
            m_tapCount = 1;
        }
        m_lastTapTime = now;
        m_lastTapPos = event->pos();

        if (m_tapCount == 2) {
            // Double-tap → select word
            clearSelection();
            selectWordAt(event->pos());
            event->accept();
            return;
        }
        if (m_tapCount == 3) {
            // Triple-tap → select line
            clearSelection();
            selectLineAt(event->pos());
            event->accept();
            return;
        }

        // Single tap — start long-press timer for manual selection
        clearSelection();
        m_selStart = event->pos();
        m_selEnd = event->pos();
        m_longPressTimerId = startTimer(LongPressTimeout);
        event->accept();
        return;
    }
    QQuickPaintedItem::mousePressEvent(event);
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    // Handle dragging a selection handle
    if (m_draggingHandle != 0) {
        if (m_draggingHandle == 1)
            m_selStart = event->pos();
        else
            m_selEnd = event->pos();

        // Velocity-based magnifier hiding (same logic as normal selection)
        m_magnifierVisible = updateMagnifierVelocity(event->pos());

        // Keep cursor blink paused during handle drag
        m_lastInputTime.start();

        update();
        event->accept();
        return;
    }

    if (m_selecting) {
        // Only track movement for long-press drags (handles not yet visible).
        // Word/line selections have finalized endpoints — use handles to adjust.
        if (!m_handlesVisible) {
            m_selEnd = event->pos();
        }

        // Velocity-based magnifier hiding: hide during fast swipes
        m_magnifierVisible = updateMagnifierVelocity(event->pos());

        // Keep cursor blink paused during active selection to prevent
        // full renderCells() redraws that cause magnifier flicker
        m_lastInputTime.start();

        // Don't set m_needsRender — the selection highlight and magnifier
        // are drawn on top of the cached terminal image. Just trigger a
        // repaint so the magnifier follows the finger.
        update();
        event->accept();
        return;
    }

    if (m_mouseTrackingActive) {
        // Forward mouse motion to the terminal (including hover without button)
        GhosttyMouseButton btn = m_mouseButtonPressed
            ? GHOSTTY_MOUSE_BUTTON_LEFT : GHOSTTY_MOUSE_BUTTON_UNKNOWN;
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_MOTION, btn,
                       event->pos(), KeyMapping::mapQtModifiers(event->modifiers()));
        event->accept();
        return;
    }

    QQuickPaintedItem::mouseMoveEvent(event);
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_longPressTimerId) {
        killTimer(m_longPressTimerId);
        m_longPressTimerId = 0;
    }

    if (m_mouseTrackingActive) {
        // Forward mouse release to the terminal
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_RELEASE, GHOSTTY_MOUSE_BUTTON_LEFT,
                       event->pos(), KeyMapping::mapQtModifiers(event->modifiers()));

        // No more buttons pressed
        m_mouseButtonPressed = false;
        m_vt->setMouseButtonPressed(false);
        m_mouseTrackingActive = false;
        setKeepMouseGrab(false);

        event->accept();
        return;
    }

    // Finalize handle drag
    if (m_draggingHandle != 0) {
        if (m_draggingHandle == 1)
            m_selStart = event->pos();
        else
            m_selEnd = event->pos();
        m_draggingHandle = 0;
        m_magnifierVisible = false;
        m_handlesVisible = true; // Show handles again after adjustment
        setKeepMouseGrab(false);
        copySelection();
        update();
        event->accept();
        return;
    }

    if (m_selecting) {
        // Only update endpoint for long-press drags (handles not yet visible).
        // Word/line selections already finalized their endpoints.
        if (!m_handlesVisible) {
            m_selEnd = event->pos();
            // Cancel if finger didn't move enough — long-press without drag
            // would create a phantom single-character selection
            if (QLineF(m_selStart, m_selEnd).length() < TapDistancePx) {
                clearSelection();
                event->accept();
                return;
            }
            copySelection();
        }
        // Hide magnifier on finger lift, but keep highlight and show handles
        m_magnifierVisible = false;
        m_handlesVisible = true;
        update();
        event->accept();
        return;
    }
    QQuickPaintedItem::mouseReleaseEvent(event);
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    if (!m_vt || !m_vt->terminal()) {
        QQuickPaintedItem::wheelEvent(event);
        return;
    }

    // When mouse tracking is active, forward scroll as mouse buttons 4/5
    if (m_vt->isMouseTracking()) {
        int delta = event->angleDelta().y();
        GhosttyMods mods = KeyMapping::mapQtModifiers(event->modifiers());
        GhosttyMouseButton button = (delta > 0) ? GHOSTTY_MOUSE_BUTTON_FOUR
                                                 : GHOSTTY_MOUSE_BUTTON_FIVE;
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_PRESS, button, event->pos(), mods);
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_RELEASE, button, event->pos(), mods);
        event->accept();
        return;
    }

    // Qt wheel events give delta in 1/8 degree units.
    // A typical mouse wheel click is 120 units = 15 degrees = 3 lines.
    int delta = event->angleDelta().y(); // positive = up, negative = down

    // Accumulate fractional scroll lines so sub-line deltas aren't lost
    qreal newDelta = -static_cast<qreal>(delta) / 40.0;
    auto scrollResult = TextUtil::accumulateScroll(m_scrollAccumulator, newDelta);
    m_scrollAccumulator = scrollResult.accumulator;
    int lines = scrollResult.lines;

    if (lines != 0) {
        // Ghostty scroll: negative delta = scroll up (toward scrollback)
        GhosttyTerminalScrollViewport scroll = {};
        scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
        scroll.value.delta = -lines;
        ghostty_terminal_scroll_viewport(m_vt->terminal(), scroll);
        m_needsRender = true;
        update();
    }

    event->accept();
}

void TerminalView::touchEvent(QTouchEvent *event)
{
    if (!m_vt || !m_vt->terminal()) {
        QQuickPaintedItem::touchEvent(event);
        return;
    }

    const auto points = event->touchPoints();

    // Two-finger vertical swipe = scroll terminal viewport
    if (points.size() >= 2) {
        if (event->type() == QEvent::TouchBegin) {
            m_twoFingerScrolling = true;
            if (m_longPressTimerId) {
                killTimer(m_longPressTimerId);
                m_longPressTimerId = 0;
            }
            // Cancel any active handle drag
            if (m_draggingHandle != 0) {
                m_draggingHandle = 0;
                m_magnifierVisible = false;
                m_handlesVisible = true;
                setKeepMouseGrab(false);
            }
            // Clear any active selection — pixel coordinates become stale after scroll
            if (m_selecting)
                clearSelection();
            // Average Y of both fingers as starting point
            m_twoFingerLastY = (points[0].pos().y() + points[1].pos().y()) / 2.0;
            event->accept();
            return;
        }

        if (event->type() == QEvent::TouchUpdate && m_twoFingerScrolling) {
            qreal avgY = (points[0].pos().y() + points[1].pos().y()) / 2.0;
            qreal deltaY = avgY - m_twoFingerLastY;
            m_twoFingerLastY = avgY;

            // Accumulate fractional scroll lines so sub-line deltas aren't lost
            qreal newDelta = -deltaY / m_cellHeight;
            auto touchScrollResult = TextUtil::accumulateScroll(m_touchScrollAccumulator, newDelta);
            m_touchScrollAccumulator = touchScrollResult.accumulator;
            int lines = touchScrollResult.lines;

            if (lines != 0) {
                GhosttyTerminalScrollViewport scroll = {};
                scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
                scroll.value.delta = lines;
                ghostty_terminal_scroll_viewport(m_vt->terminal(), scroll);
                m_needsRender = true;
                update();
            }
            event->accept();
            return;
        }

        if (event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
            m_twoFingerScrolling = false;
            m_touchScrollAccumulator = 0;
            event->accept();
            return;
        }
    }

    // Single finger — not scrolling, pass through
    if (event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
        m_twoFingerScrolling = false;
        m_touchScrollAccumulator = 0;
    }

    QQuickPaintedItem::touchEvent(event);
}

void TerminalView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_longPressTimerId) {
        killTimer(m_longPressTimerId);
        m_longPressTimerId = 0;
        m_selecting = true;
        m_magnifierVisible = true;
        // Initialize velocity tracking so first move doesn't use stale data
        m_velocityInitialized = false;
        // Prevent parent SilicaFlickable from stealing the drag
        setKeepMouseGrab(true);
        m_needsRender = true;
        update();
        return;
    }
    if (event->timerId() == m_renderTimerId) {
        killTimer(m_renderTimerId);
        m_renderTimerId = 0;
        if (m_needsRender)
            update();
        return;
    }
    if (event->timerId() == m_blinkTimerId) {
        // Pause blinking for BlinkPauseMs after any input activity
        if (m_lastInputTime.isValid() &&
            m_lastInputTime.elapsed() < BlinkPauseMs) {
            m_cursorBlinkVisible = true;
            m_needsRender = true;
            update();
            return;
        }

        // Blink cursor by default. Only stop when terminal explicitly requests
        // a steady cursor (DECSCUSR mode 2, 4, or 6).
        GhosttyRenderState state = m_vt ? m_vt->renderState() : nullptr;
        bool cursorBlinking = true;
        if (state) {
            ghostty_render_state_get(state,
                                     GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING,
                                     &cursorBlinking);
        }
        if (cursorBlinking) {
            m_cursorBlinkVisible = !m_cursorBlinkVisible;
            m_needsRender = true;
            update();
        }
        return;
    }
    QQuickPaintedItem::timerEvent(event);
}

void TerminalView::sendKeyEvent(GhosttyKey key, GhosttyKeyAction action,
                                GhosttyMods mods, const QString &text)
{
    QByteArray utf8 = text.toUtf8();
    QByteArray encoded = m_vt->encodeKeyEvent(
        key, action, mods,
        utf8.isEmpty() ? nullptr : utf8.constData(), utf8.size());

    if (!encoded.isEmpty())
        m_pty->writeData(encoded.constData(), encoded.size());
}

void TerminalView::sendMouseEvent(GhosttyMouseAction action, GhosttyMouseButton button,
                                  const QPointF &pos, GhosttyMods mods)
{
    QByteArray encoded = m_vt->encodeMouseEvent(
        action, button,
        static_cast<float>(pos.x()), static_cast<float>(pos.y()), mods);
    if (!encoded.isEmpty())
        m_pty->writeData(encoded.constData(), encoded.size());
}

void TerminalView::resetBlinkOnInput()
{
    m_cursorBlinkVisible = true;
    m_lastInputTime.start();
    clearSelection();
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    resetBlinkOnInput();

    GhosttyKey key = KeyMapping::mapQtKey(event->key());
    GhosttyMods mods = KeyMapping::mapQtModifiers(event->modifiers());

    if ((mods & GHOSTTY_MODS_CTRL) && (mods & GHOSTTY_MODS_SHIFT)) {
        if (key == GHOSTTY_KEY_C) { copySelection(); event->accept(); return; }
        if (key == GHOSTTY_KEY_V) { paste(); event->accept(); return; }
        if (key == GHOSTTY_KEY_F) {
            if (m_searchActive)
                closeSearch();
            else
                openSearch();
            event->accept();
            return;
        }
    }

    // Auto-repeat maps to REPEAT action (enables Kitty protocol repeat)
    GhosttyKeyAction action = event->isAutoRepeat()
        ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS;

    // If scrolled up viewing history, scroll back to bottom so the user
    // can see what they're typing.
    scrollViewportToBottom();

    sendKeyEvent(key, action, mods, event->text());
    m_needsRender = true;
    update();
    event->accept();
}

void TerminalView::keyReleaseEvent(QKeyEvent *event)
{
    // Ignore auto-repeat release events — they fire between PRESS/REPEAT
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    GhosttyKey key = KeyMapping::mapQtKey(event->key());
    GhosttyMods mods = KeyMapping::mapQtModifiers(event->modifiers());
    sendKeyEvent(key, GHOSTTY_KEY_ACTION_RELEASE, mods, event->text());
    event->accept();
}

QString TerminalView::workingDirectory() const
{
    if (!m_pty || m_pty->childPid() <= 0)
        return QString(); // Shell not running — return empty so caller can use cached value

    QString procPath = QStringLiteral("/proc/%1/cwd").arg(m_pty->childPid());
    QString target = QFileInfo(procPath).symLinkTarget();
    return target; // Empty if /proc unavailable or process exited
}

void TerminalView::setWorkingDirectory(const QString &dir)
{
    if (m_pty)
        m_pty->setWorkingDirectory(dir);
}

void TerminalView::setAutorunCommand(const QString &cmd)
{
    m_autorunCommand = cmd;
}

void TerminalView::setPendingScrollback(const QByteArray &data)
{
    m_pendingScrollback = data;
}

QByteArray TerminalView::exportScrollback(uint16_t &outCols, uint16_t &outRows) const
{
    if (!m_vt)
        return {};
    return m_vt->exportScrollback(outCols, outRows);
}

void TerminalView::suppressNextKeyboardAutoShow()
{
    m_suppressKeyboardAutoShow = true;
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

    // Clear any existing selection to avoid visual confusion
    clearSelection();

    Q_EMIT searchActiveChanged();
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
    m_needsRender = true;
    update();
    Q_EMIT searchActiveChanged();
    Q_EMIT searchMatchCountChanged();
    Q_EMIT currentMatchIndexChanged();
}

void TerminalView::buildCellMapping()
{
    // Build cell-to-character index mapping for CJK/emoji support.
    // Wide characters (CJK, some emoji) occupy 2 terminal cells but
    // produce 1 character in the QString. Without this mapping, search
    // highlight positions would be wrong for non-ASCII text.
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
        // No terminal or zero columns — fill with empty mappings
        for (int row = 0; row < m_searchCache.size(); row++)
            m_cellMapping.append(QVector<int>());
        return;
    }
    for (int row = 0; row < m_searchCache.size(); row++) {
        QVector<int> mapping;
        if (row < static_cast<int>(totalRows)) {
            mapping.resize(static_cast<int>(cols));
            int charIdx = 0;
            const QString &line = m_searchCache[row];
            for (int cell = 0; cell < static_cast<int>(cols); cell++) {
                mapping[cell] = charIdx;
                if (GhosttyVt::isWideCharSpacer(terminal, static_cast<uint16_t>(cell), static_cast<uint32_t>(row))) {
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

    // Re-extract if cache is empty or terminal received new data since last extract
    if (m_vt && (m_searchCache.isEmpty() || m_vt->isSearchTextDirty())) {
        m_searchCache = m_vt->extractSearchText();
        buildCellMapping();
    }

    performSearch();

    if (m_currentMatchIndex >= 0)
        scrollToMatch(m_currentMatchIndex);

    m_needsRender = true;
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

    // Case-insensitive search across all rows (capped to prevent OOM)
    static const int MaxSearchMatches = 10000;
    for (int row = 0; row < m_searchCache.size(); row++) {
        int col = 0;
        const QString &line = m_searchCache[row];
        while (col < line.size()) {
            int idx = line.indexOf(m_searchPattern, col, Qt::CaseInsensitive);
            if (idx < 0)
                break;

            // Map character index to cell column using the cell mapping.
            // For pure ASCII, cellCol == idx. For CJK/emoji, the mapping
            // accounts for wide characters occupying 2 cells.
            int cellCol = idx;
            int cellWidth = m_searchPattern.size();
            if (row < m_cellMapping.size() && !m_cellMapping[row].isEmpty()) {
                const QVector<int> &mapping = m_cellMapping[row];
                // Find cell column for the start of the match
                for (int cell = 0; cell < mapping.size(); cell++) {
                    if (mapping[cell] == idx) {
                        cellCol = cell;
                        break;
                    }
                }
                // Find cell width: count cells from cellCol that span the match characters
                int matchEnd = idx + m_searchPattern.size();
                cellWidth = 0;
                for (int cell = cellCol; cell < mapping.size(); cell++) {
                    if (mapping[cell] >= matchEnd)
                        break;
                    cellWidth++;
                }
                if (cellWidth == 0)
                    cellWidth = 1; // safety
            }

            m_searchMatches.append({row, cellCol, cellWidth});
            if (m_searchMatches.size() >= MaxSearchMatches)
                goto searchDone;
            col = idx + 1;
        }
    }
searchDone: // exit point for nested row/col search loop (goto breaks both levels)

    if (!m_searchMatches.isEmpty())
        m_currentMatchIndex = 0;

    Q_EMIT searchMatchCountChanged();
    Q_EMIT currentMatchIndexChanged();
}

void TerminalView::scrollToMatch(int index)
{
    if (index < 0 || index >= m_searchMatches.size() || !m_vt || !m_vt->terminal())
        return;

    const auto &match = m_searchMatches[index];

    GhosttyTerminalScrollbar scrollbar = {};
    ghostty_terminal_get(m_vt->terminal(), GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);

    int matchRow = match.row;
    int viewTop = static_cast<int>(scrollbar.offset);
    int viewLen = static_cast<int>(scrollbar.len);

    if (viewLen > 0 && matchRow >= viewTop && matchRow < viewTop + viewLen)
        return;

    int targetTop = matchRow - viewLen / 2;
    if (targetTop < 0)
        targetTop = 0;
    int delta = targetTop - viewTop;

    if (delta != 0) {
        GhosttyTerminalScrollViewport scroll = {};
        scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
        scroll.value.delta = delta;
        ghostty_terminal_scroll_viewport(m_vt->terminal(), scroll);
        m_needsRender = true;
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
    m_needsRender = true;
    update();
}

void TerminalView::findPrevious()
{
    if (m_searchMatches.isEmpty())
        return;

    m_currentMatchIndex = (m_currentMatchIndex - 1 + m_searchMatches.size()) % m_searchMatches.size();
    scrollToMatch(m_currentMatchIndex);
    Q_EMIT currentMatchIndexChanged();
    m_needsRender = true;
    update();
}

void TerminalView::drawSearchHighlights(QPainter *painter)
{
    if (m_searchMatches.isEmpty() || !m_vt || !m_vt->terminal())
        return;

    // Query scrollbar state to map absolute rows to viewport rows
    GhosttyTerminalScrollbar scrollbar = {};
    ghostty_terminal_get(m_vt->terminal(), GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);

    int viewTop = static_cast<int>(scrollbar.offset);
    int viewLen = static_cast<int>(scrollbar.len);
    if (viewLen <= 0)
        return;

    // Amber/yellow for all matches, brighter orange for current match
    QColor highlightColor(255, 200, 0, 100);
    QColor currentColor(255, 100, 0, 140);

    painter->setPen(Qt::NoPen);

    // Binary search: find first match at or after viewTop.
    // Matches are sorted by row (search iterates row-major), so we can skip
    // all matches before the viewport in O(log n) instead of O(n).
    SearchMatch lowerBound = {viewTop, 0, 0};
    SearchMatch upperBound = {viewTop + viewLen, 0, 0};
    auto begin = std::lower_bound(m_searchMatches.begin(), m_searchMatches.end(), lowerBound,
        [](const SearchMatch &a, const SearchMatch &b) { return a.row < b.row; });
    auto end = std::upper_bound(begin, m_searchMatches.end(), upperBound,
        [](const SearchMatch &b, const SearchMatch &a) { return b.row < a.row; });

    for (auto it = begin; it != end; ++it) {
        int i = static_cast<int>(std::distance(m_searchMatches.begin(), it));
        const auto &match = *it;

        int vpRow = match.row - viewTop;
        int x = match.cellCol * m_cellWidth;
        int y = vpRow * m_cellHeight + TopPadding;
        int w = match.cellWidth * m_cellWidth;
        int h = m_cellHeight;

        if (x + w > m_cols * m_cellWidth)
            w = m_cols * m_cellWidth - x;
        if (w <= 0)
            continue;

        QColor color = (i == m_currentMatchIndex) ? currentColor : highlightColor;
        painter->fillRect(x, y, w, h, color);
    }
}

void TerminalView::runAutorunCommand()
{
    if (m_autorunCommand.isEmpty() || !m_pty
        || m_pty->childPid() <= 0 || m_shellExited)
        return;

    QByteArray cmd = m_autorunCommand.toUtf8();
    m_pty->writeData(cmd.constData(), cmd.size());
    m_pty->writeData("\r", 1);
}

void TerminalView::scrollViewportToBottom()
{
    if (!m_vt || !m_vt->terminal())
        return;
    bool viewportActive = true;
    ghostty_terminal_get(m_vt->terminal(),
                         GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &viewportActive);
    if (!viewportActive) {
        GhosttyTerminalScrollViewport scroll = {};
        scroll.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
        ghostty_terminal_scroll_viewport(m_vt->terminal(), scroll);
    }
}
