#include "terminalview.h"
#include "ptymanager.h"
#include "settings.h"

#include <QPainter>
#include <QDebug>
#include <QClipboard>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethod>
#include <QTouchEvent>
#include <cstring>
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
    uint16_t newCols = static_cast<uint16_t>(width() / m_cellWidth);
    uint16_t newRows = static_cast<uint16_t>((height() - TopPadding) / m_cellHeight);

    if (newCols < 2) newCols = 2;
    if (newRows < 2) newRows = 2;
    if (newCols > 512) newCols = 512;
    if (newRows > 512) newRows = 512;

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
    if (im)
        im->show();

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
    // Reset cursor blink on software keyboard input
    m_cursorBlinkVisible = true;
    m_lastInputTime.start();
    clearSelection();

    // Commit text from the input method (what the user actually typed)
    if (!event->commitString().isEmpty()) {
        QByteArray utf8 = event->commitString().toUtf8();

        // If sticky modifiers are active (Ctrl/Alt from keybar toggle),
        // send as a key event with modifiers, then clear them.
        if (m_stickyModifiers != 0) {
            // Map the first character to a GhosttyKey
            QChar ch = event->commitString().at(0).toLower();
            GhosttyKey key = GHOSTTY_KEY_UNIDENTIFIED;
            if (ch >= 'a' && ch <= 'z')
                key = static_cast<GhosttyKey>(GHOSTTY_KEY_A + (ch.unicode() - 'a'));
            else if (ch >= '0' && ch <= '9')
                key = static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + (ch.unicode() - '0'));
            else {
                // L3: Map common punctuation characters for sticky modifiers
                switch (ch.unicode()) {
                case '-': key = GHOSTTY_KEY_MINUS; break;
                case '=': key = GHOSTTY_KEY_EQUAL; break;
                case '[': key = GHOSTTY_KEY_BRACKET_LEFT; break;
                case ']': key = GHOSTTY_KEY_BRACKET_RIGHT; break;
                case '\\': key = GHOSTTY_KEY_BACKSLASH; break;
                case ';': key = GHOSTTY_KEY_SEMICOLON; break;
                case '\'': key = GHOSTTY_KEY_QUOTE; break;
                case ',': key = GHOSTTY_KEY_COMMA; break;
                case '.': key = GHOSTTY_KEY_PERIOD; break;
                case '/': key = GHOSTTY_KEY_SLASH; break;
                case '`': key = GHOSTTY_KEY_BACKQUOTE; break;
                case ' ': key = GHOSTTY_KEY_SPACE; break;
                default: break;
                }
            }

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

    Settings *s = Settings::instance();
    QString scheme = s->colorScheme();
    GhosttyColorRgb fg, bg, cursor;

    if (scheme == QStringLiteral("light")) {
        fg = {51, 51, 51}; bg = {255, 255, 255}; cursor = {0, 0, 0};
    } else if (scheme == QStringLiteral("solarized-dark")) {
        fg = {147, 161, 161}; bg = {0, 43, 54}; cursor = {203, 75, 22};
    } else if (scheme == QStringLiteral("solarized-light")) {
        fg = {101, 123, 131}; bg = {253, 246, 227}; cursor = {203, 75, 22};
    } else if (scheme == QStringLiteral("monokai")) {
        fg = {248, 248, 242}; bg = {39, 40, 34}; cursor = {248, 248, 242};
    } else {
        // "dark" (default)
        fg = {204, 204, 204}; bg = {30, 30, 30}; cursor = {255, 255, 255};
    }

    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cursor);

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

    // Iterate rows
    GhosttyRenderStateRowIterator iterator;
    ghostty_render_state_row_iterator_new(nullptr, &iterator);
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                             &iterator);

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
                imgPainter.fillRect(x, y, m_cellWidth, m_cellHeight,
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
                        m_cachedCursor.graphemesLen = static_cast<int>(graphemesLen);
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

                    imgPainter.setFont(fontForStyle(style));

                    imgPainter.setPen(cellFgColor);
                    imgPainter.drawText(QPointF(x, y + imgPainter.fontMetrics().ascent()),
                                        text);

                    // L2: Draw underline
                    if (style.underline) {
                        int underlineY = y + m_cellHeight - 2;
                        imgPainter.drawLine(x, underlineY, x + m_cellWidth, underlineY);
                    }
                    // L2: Draw strikethrough
                    if (style.strikethrough) {
                        int strikeY = y + m_cellHeight / 2;
                        imgPainter.drawLine(x, strikeY, x + m_cellWidth, strikeY);
                    }
                } else {
                    // Grapheme cluster too complex for buffer — render placeholder
                    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
                    ghostty_render_state_row_cells_get(
                        cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                        &style);
                    imgPainter.setFont(fontForStyle(style));
                    imgPainter.setPen(cellFgColor);
                    imgPainter.drawText(QPointF(x, y + imgPainter.fontMetrics().ascent()),
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

    // Draw cursor
    bool cursorVisible = false;
    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE,
                             &cursorVisible);

    bool cursorInViewport = false;
    ghostty_render_state_get(state,
                             GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                             &cursorInViewport);

    if (cursorVisible && cursorInViewport && m_cursorBlinkVisible) {
        uint16_t cx = 0, cy = 0;
        ghostty_render_state_get(state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X,
                                 &cx);
        ghostty_render_state_get(state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y,
                                 &cy);

        GhosttyRenderStateCursorVisualStyle cursorStyle;
        ghostty_render_state_get(state,
                                 GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE,
                                 &cursorStyle);

        int px = cx * m_cellWidth;
        int py = cy * m_cellHeight + TopPadding;

        QColor cursorColor = QColor(colors.cursor.r, colors.cursor.g,
                                    colors.cursor.b);
        if (!colors.cursor_has_value)
            cursorColor = fgColor;

        switch (cursorStyle) {
        case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK:
            // Draw cursor background, then redraw the cell text on top
            // with inverted colors (background becomes foreground)
            imgPainter.setPen(Qt::NoPen);
            imgPainter.setBrush(cursorColor);
            imgPainter.drawRect(px, py, m_cellWidth, m_cellHeight);
            // Redraw text on top of cursor with background color as pen
            {
                if (m_cachedCursor.valid) {
                    if (m_cachedCursor.graphemesLen > 0) {
                        QString ctext = QString::fromUcs4(m_cachedCursor.graphemes, m_cachedCursor.graphemesLen);
                        imgPainter.setFont(fontForStyle(m_cachedCursor.style));
                        imgPainter.setPen(m_cachedCursor.bgColor); // use cell's actual bg as text color
                        imgPainter.drawText(QPointF(px, py + imgPainter.fontMetrics().ascent()), ctext);
                    }
                } else {
                    // Fallback: O(rows) lookup if cursor wasn't in the rendered viewport
                    GhosttyRenderStateRowIterator ci;
                    ghostty_render_state_row_iterator_new(nullptr, &ci);
                    ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &ci);
                    GhosttyRenderStateRowCells cc;
                    ghostty_render_state_row_cells_new(nullptr, &cc);
                    uint16_t rowIdx = 0;
                    while (ghostty_render_state_row_iterator_next(ci)) {
                        if (rowIdx == cy) {
                            ghostty_render_state_row_get(ci, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cc);
                            if (ghostty_render_state_row_cells_select(cc, cx) == GHOSTTY_SUCCESS) {
                                GhosttyColorRgb cursorCellBg;
                                QColor cursorCellBgColor = bgColor;
                                if (ghostty_render_state_row_cells_get(
                                        cc, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                                        &cursorCellBg) == GHOSTTY_SUCCESS) {
                                    cursorCellBgColor = QColor(cursorCellBg.r, cursorCellBg.g, cursorCellBg.b);
                                }
                                uint32_t clen = 0;
                                ghostty_render_state_row_cells_get(cc, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &clen);
                                if (clen > 0 && clen <= 128) {
                                    uint32_t cbuf[128];
                                    ghostty_render_state_row_cells_get(cc, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cbuf);
                                    QString ctext = QString::fromUcs4(cbuf, clen);
                                    GhosttyStyle cstyle = GHOSTTY_INIT_SIZED(GhosttyStyle);
                                    ghostty_render_state_row_cells_get(cc, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &cstyle);
                                    imgPainter.setFont(fontForStyle(cstyle));
                                    imgPainter.setPen(cursorCellBgColor);
                                    imgPainter.drawText(QPointF(px, py + imgPainter.fontMetrics().ascent()), ctext);
                                } else if (clen > 128) {
                                    GhosttyStyle cstyle = GHOSTTY_INIT_SIZED(GhosttyStyle);
                                    ghostty_render_state_row_cells_get(cc, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &cstyle);
                                    imgPainter.setFont(fontForStyle(cstyle));
                                    imgPainter.setPen(cursorCellBgColor);
                                    imgPainter.drawText(QPointF(px, py + imgPainter.fontMetrics().ascent()),
                                                        QStringLiteral("\u2468"));
                                }
                            }
                            break;
                        }
                        rowIdx++;
                    }
                    ghostty_render_state_row_cells_free(cc);
                    ghostty_render_state_row_iterator_free(ci);
                }
            }
            break;
        case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
            imgPainter.setPen(QPen(cursorColor, 2));
            imgPainter.drawLine(px, py, px, py + m_cellHeight);
            break;
        case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
            imgPainter.setPen(QPen(cursorColor, 2));
            imgPainter.drawLine(px, py + m_cellHeight - 1,
                                px + m_cellWidth, py + m_cellHeight - 1);
            break;
        case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
            imgPainter.setPen(QPen(cursorColor, 1));
            imgPainter.setBrush(Qt::NoBrush);
            imgPainter.drawRect(px, py, m_cellWidth - 1, m_cellHeight - 1);
            break;
        default:
            break;
        }
    }

    // End imgPainter before starting overlay on the same QImage
    imgPainter.end();

    // Draw shell exit overlay onto the image (persists across repaints)
    if (m_shellExited) {
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

    painter->drawImage(0, 0, m_image);
}

void TerminalView::renderMagnifier(QPainter *painter)
{
    // SailfishOS-style magnifier: zoomed view of terminal around the finger
    // Shows a 2x zoomed bubble above the touch point for precise text selection

    if (m_image.isNull())
        return;

    QPointF fingerPos = m_selEnd;

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

    // H2: ghostty_paste_encode modifies data in place. Use a copy for the
    // sizing call, then a fresh copy for the actual encode to avoid
    // double-processing of already-mutated data.
    QByteArray input = utf8;

    // First call: query required size (pass nullptr buffer)
    size_t written = 0;
    GhosttyResult res = ghostty_paste_encode(input.data(), input.size(), true,
                                             nullptr, 0, &written);
    if (res == GHOSTTY_OUT_OF_SPACE && written > 0) {
        // Second call with correctly sized buffer — use FRESH copy
        QByteArray safeInput = utf8;
        QByteArray buf(written, '\0');
        res = ghostty_paste_encode(safeInput.data(), safeInput.size(), true,
                                   buf.data(), buf.size(), &written);
        if (res == GHOSTTY_SUCCESS && written > 0) {
            m_pty->writeData(buf.constData(), written);
            return;
        }
    } else if (res == GHOSTTY_SUCCESS && written > 0) {
        // Data was small enough to encode in-place (input already mutated)
        m_pty->writeData(input.constData(), written);
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

    QString result = lines.join(QLatin1Char('\n'));
    if (!result.isEmpty()) {
        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(result, QClipboard::Clipboard);
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
    // Reset cursor blink on keybar button press
    m_cursorBlinkVisible = true;
    m_lastInputTime.start();
    clearSelection();

    GhosttyKey key = mapQtKey(qtKey);
    // Accept GhosttyMods directly (not Qt modifier values)
    sendKeyEvent(key, GHOSTTY_KEY_ACTION_PRESS, static_cast<GhosttyMods>(modifiers), QString());
    m_needsRender = true;
    update();
}

QPointF TerminalView::cellFromPixel(const QPointF &pos) const
{
    if (m_cellWidth <= 0 || m_cellHeight <= 0)
        return QPointF(-1, -1);
    int col = static_cast<int>(pos.x()) / m_cellWidth;
    int row = static_cast<int>(pos.y() - TopPadding) / m_cellHeight;
    if (col < 0 || col >= m_cols || row < 0 || row >= m_rows)
        return QPointF(-1, -1);
    return QPointF(col, row);
}

void TerminalView::clearSelection()
{
    if (m_selecting) {
        m_selecting = false;
        m_magnifierVisible = false;
        setKeepMouseGrab(false);
        if (m_longPressTimerId) {
            killTimer(m_longPressTimerId);
            m_longPressTimerId = 0;
        }
        m_needsRender = true;
        update();
    }
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
            QByteArray encoded = m_vt->encodeMouseEvent(
                GHOSTTY_MOUSE_ACTION_PRESS,
                GHOSTTY_MOUSE_BUTTON_LEFT,
                static_cast<float>(event->pos().x()),
                static_cast<float>(event->pos().y()),
                mapQtModifiers(event->modifiers()));
            if (!encoded.isEmpty())
                m_pty->writeData(encoded.constData(), encoded.size());

            // Tell encoder a button is pressed (enables motion events)
            m_mouseButtonPressed = true;
            m_vt->setMouseButtonPressed(true);

            // Prevent SilicaFlickable from stealing drag gestures
            setKeepMouseGrab(true);
            event->accept();
            return;
        }

        // No mouse tracking — start long-press for selection
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
    if (m_selecting) {
        m_selEnd = event->pos();
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
        QByteArray encoded = m_vt->encodeMouseEvent(
            GHOSTTY_MOUSE_ACTION_MOTION,
            btn,
            static_cast<float>(event->pos().x()),
            static_cast<float>(event->pos().y()),
            mapQtModifiers(event->modifiers()));
        if (!encoded.isEmpty())
            m_pty->writeData(encoded.constData(), encoded.size());
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
        QByteArray encoded = m_vt->encodeMouseEvent(
            GHOSTTY_MOUSE_ACTION_RELEASE,
            GHOSTTY_MOUSE_BUTTON_LEFT,
            static_cast<float>(event->pos().x()),
            static_cast<float>(event->pos().y()),
            mapQtModifiers(event->modifiers()));
        if (!encoded.isEmpty())
            m_pty->writeData(encoded.constData(), encoded.size());

        // No more buttons pressed
        m_mouseButtonPressed = false;
        m_vt->setMouseButtonPressed(false);
        m_mouseTrackingActive = false;
        setKeepMouseGrab(false);

        event->accept();
        return;
    }

    if (m_selecting) {
        m_selEnd = event->pos();
        // Auto-copy selection to clipboard
        copySelection();
        // Hide magnifier on finger lift, but keep highlight visible
        m_magnifierVisible = false;
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
        GhosttyMouseButton button = (delta > 0) ? GHOSTTY_MOUSE_BUTTON_FOUR
                                                 : GHOSTTY_MOUSE_BUTTON_FIVE;
        QByteArray encoded = m_vt->encodeMouseEvent(
            GHOSTTY_MOUSE_ACTION_PRESS, button,
            static_cast<float>(event->pos().x()),
            static_cast<float>(event->pos().y()),
            mapQtModifiers(event->modifiers()));
        if (!encoded.isEmpty())
            m_pty->writeData(encoded.constData(), encoded.size());
        // Also send release
        encoded = m_vt->encodeMouseEvent(
            GHOSTTY_MOUSE_ACTION_RELEASE, button,
            static_cast<float>(event->pos().x()),
            static_cast<float>(event->pos().y()),
            mapQtModifiers(event->modifiers()));
        if (!encoded.isEmpty())
            m_pty->writeData(encoded.constData(), encoded.size());
        event->accept();
        return;
    }

    // Qt wheel events give delta in 1/8 degree units.
    // A typical mouse wheel click is 120 units = 15 degrees = 3 lines.
    int delta = event->angleDelta().y(); // positive = up, negative = down

    // Accumulate fractional scroll lines so sub-line deltas aren't lost
    qreal newDelta = -static_cast<qreal>(delta) / 40.0;
    // Reset accumulator on direction change
    if (m_scrollAccumulator != 0 &&
        (m_scrollAccumulator > 0) != (newDelta > 0)) {
        m_scrollAccumulator = 0;
    }
    m_scrollAccumulator += newDelta;
    int lines = static_cast<int>(m_scrollAccumulator);
    m_scrollAccumulator -= lines;

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
            // Reset accumulator on direction change
            if (m_touchScrollAccumulator != 0 &&
                (m_touchScrollAccumulator > 0) != (newDelta > 0)) {
                m_touchScrollAccumulator = 0;
            }
            m_touchScrollAccumulator += newDelta;
            int lines = static_cast<int>(m_touchScrollAccumulator);
            m_touchScrollAccumulator -= lines;

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
        // a steady cursor (DECSCUSR mode 2 or 6).
        GhosttyRenderState state = m_vt ? m_vt->renderState() : nullptr;
        bool steadyCursor = false;
        if (state) {
            ghostty_render_state_get(state,
                                     GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING,
                                     &steadyCursor);
        }
        if (!steadyCursor) {
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

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    // Reset cursor blink on key input — cursor stays visible while typing
    m_cursorBlinkVisible = true;
    m_lastInputTime.start();
    clearSelection();

    GhosttyKey key = mapQtKey(event->key());
    GhosttyMods mods = mapQtModifiers(event->modifiers());

    // Handle Ctrl+Shift+C = copy, Ctrl+Shift+V = paste
    if (mods & GHOSTTY_MODS_CTRL && mods & GHOSTTY_MODS_SHIFT) {
        if (key == GHOSTTY_KEY_C) { copySelection(); event->accept(); return; }
        if (key == GHOSTTY_KEY_V) { paste(); event->accept(); return; }
    }

    // Auto-repeat maps to REPEAT action (enables Kitty protocol repeat)
    GhosttyKeyAction action = event->isAutoRepeat()
        ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS;

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

    GhosttyKey key = mapQtKey(event->key());
    GhosttyMods mods = mapQtModifiers(event->modifiers());
    sendKeyEvent(key, GHOSTTY_KEY_ACTION_RELEASE, mods, event->text());
    event->accept();
}

GhosttyKey TerminalView::mapQtKey(int qtKey) const
{
    switch (qtKey) {
    // Letters
    case Qt::Key_A: return GHOSTTY_KEY_A;
    case Qt::Key_B: return GHOSTTY_KEY_B;
    case Qt::Key_C: return GHOSTTY_KEY_C;
    case Qt::Key_D: return GHOSTTY_KEY_D;
    case Qt::Key_E: return GHOSTTY_KEY_E;
    case Qt::Key_F: return GHOSTTY_KEY_F;
    case Qt::Key_G: return GHOSTTY_KEY_G;
    case Qt::Key_H: return GHOSTTY_KEY_H;
    case Qt::Key_I: return GHOSTTY_KEY_I;
    case Qt::Key_J: return GHOSTTY_KEY_J;
    case Qt::Key_K: return GHOSTTY_KEY_K;
    case Qt::Key_L: return GHOSTTY_KEY_L;
    case Qt::Key_M: return GHOSTTY_KEY_M;
    case Qt::Key_N: return GHOSTTY_KEY_N;
    case Qt::Key_O: return GHOSTTY_KEY_O;
    case Qt::Key_P: return GHOSTTY_KEY_P;
    case Qt::Key_Q: return GHOSTTY_KEY_Q;
    case Qt::Key_R: return GHOSTTY_KEY_R;
    case Qt::Key_S: return GHOSTTY_KEY_S;
    case Qt::Key_T: return GHOSTTY_KEY_T;
    case Qt::Key_U: return GHOSTTY_KEY_U;
    case Qt::Key_V: return GHOSTTY_KEY_V;
    case Qt::Key_W: return GHOSTTY_KEY_W;
    case Qt::Key_X: return GHOSTTY_KEY_X;
    case Qt::Key_Y: return GHOSTTY_KEY_Y;
    case Qt::Key_Z: return GHOSTTY_KEY_Z;

    // Digits
    case Qt::Key_0: return GHOSTTY_KEY_DIGIT_0;
    case Qt::Key_1: return GHOSTTY_KEY_DIGIT_1;
    case Qt::Key_2: return GHOSTTY_KEY_DIGIT_2;
    case Qt::Key_3: return GHOSTTY_KEY_DIGIT_3;
    case Qt::Key_4: return GHOSTTY_KEY_DIGIT_4;
    case Qt::Key_5: return GHOSTTY_KEY_DIGIT_5;
    case Qt::Key_6: return GHOSTTY_KEY_DIGIT_6;
    case Qt::Key_7: return GHOSTTY_KEY_DIGIT_7;
    case Qt::Key_8: return GHOSTTY_KEY_DIGIT_8;
    case Qt::Key_9: return GHOSTTY_KEY_DIGIT_9;

    // Special keys
    case Qt::Key_Return:
    case Qt::Key_Enter:    return GHOSTTY_KEY_ENTER;
    case Qt::Key_Backspace: return GHOSTTY_KEY_BACKSPACE;
    case Qt::Key_Tab:      return GHOSTTY_KEY_TAB;
    case Qt::Key_Escape:   return GHOSTTY_KEY_ESCAPE;
    case Qt::Key_Space:    return GHOSTTY_KEY_SPACE;
    case Qt::Key_Delete:   return GHOSTTY_KEY_DELETE;
    case Qt::Key_Insert:   return GHOSTTY_KEY_INSERT;
    case Qt::Key_Home:     return GHOSTTY_KEY_HOME;
    case Qt::Key_End:      return GHOSTTY_KEY_END;
    case Qt::Key_PageUp:   return GHOSTTY_KEY_PAGE_UP;
    case Qt::Key_PageDown: return GHOSTTY_KEY_PAGE_DOWN;

    // Arrow keys
    case Qt::Key_Up:    return GHOSTTY_KEY_ARROW_UP;
    case Qt::Key_Down:  return GHOSTTY_KEY_ARROW_DOWN;
    case Qt::Key_Left:  return GHOSTTY_KEY_ARROW_LEFT;
    case Qt::Key_Right: return GHOSTTY_KEY_ARROW_RIGHT;

    // Function keys
    case Qt::Key_F1:  return GHOSTTY_KEY_F1;
    case Qt::Key_F2:  return GHOSTTY_KEY_F2;
    case Qt::Key_F3:  return GHOSTTY_KEY_F3;
    case Qt::Key_F4:  return GHOSTTY_KEY_F4;
    case Qt::Key_F5:  return GHOSTTY_KEY_F5;
    case Qt::Key_F6:  return GHOSTTY_KEY_F6;
    case Qt::Key_F7:  return GHOSTTY_KEY_F7;
    case Qt::Key_F8:  return GHOSTTY_KEY_F8;
    case Qt::Key_F9:  return GHOSTTY_KEY_F9;
    case Qt::Key_F10: return GHOSTTY_KEY_F10;
    case Qt::Key_F11: return GHOSTTY_KEY_F11;
    case Qt::Key_F12: return GHOSTTY_KEY_F12;

    // Punctuation
    case Qt::Key_Minus:       return GHOSTTY_KEY_MINUS;
    case Qt::Key_Equal:       return GHOSTTY_KEY_EQUAL;
    case Qt::Key_BracketLeft: return GHOSTTY_KEY_BRACKET_LEFT;
    case Qt::Key_BracketRight: return GHOSTTY_KEY_BRACKET_RIGHT;
    case Qt::Key_Backslash:   return GHOSTTY_KEY_BACKSLASH;
    case Qt::Key_Semicolon:   return GHOSTTY_KEY_SEMICOLON;
    case Qt::Key_Apostrophe:  return GHOSTTY_KEY_QUOTE;
    case Qt::Key_Comma:       return GHOSTTY_KEY_COMMA;
    case Qt::Key_Period:      return GHOSTTY_KEY_PERIOD;
    case Qt::Key_Slash:       return GHOSTTY_KEY_SLASH;
    case Qt::Key_QuoteLeft:   return GHOSTTY_KEY_BACKQUOTE;

    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

GhosttyMods TerminalView::mapQtModifiers(Qt::KeyboardModifiers mods) const
{
    GhosttyMods result = 0;
    if (mods & Qt::ShiftModifier)   result |= GHOSTTY_MODS_SHIFT;
    if (mods & Qt::ControlModifier) result |= GHOSTTY_MODS_CTRL;
    if (mods & Qt::AltModifier)     result |= GHOSTTY_MODS_ALT;
    if (mods & Qt::MetaModifier)    result |= GHOSTTY_MODS_SUPER;
    return result;
}
