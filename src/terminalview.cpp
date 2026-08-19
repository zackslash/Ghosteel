#include "terminalview.h"
#include "ptymanager.h"
#include "settings.h"
#include "keymapping.h"
#include "textutil.h"

#include <QDebug>
#include <QClipboard>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethod>
#include <QTimer>
#include <QTouchEvent>
#include <QFileInfo>
#include <QDateTime>
#include <QLineF>
#include <sys/ioctl.h>

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setActiveFocusOnTab(true);

    // Monospace font — "monospace" is a fontconfig alias resolved by Qt
    m_font = QFont(QStringLiteral("monospace"), static_cast<int>(m_fontSize));
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    updateFontMetrics();

    m_vt = new GhosttyVt(this);
    m_pty = new PtyManager(this);

    connect(m_pty, &PtyManager::dataReady, this, &TerminalView::onPtyData);
    connect(m_pty, &PtyManager::shellExited, this, &TerminalView::onShellExited);
    connect(m_pty, &PtyManager::shellFallbackNotice, this, &TerminalView::shellFallbackNotice);
    connect(m_vt, &GhosttyVt::titleChanged, this, [this](const QString &t) {
        m_title = t;
        Q_EMIT titleChanged();
    });
    connect(m_vt, &GhosttyVt::bell, this, &TerminalView::terminalBell);
    connect(m_vt, &GhosttyVt::desktopNotification, this, &TerminalView::desktopNotification);
    connect(m_vt, &GhosttyVt::clipboardWriteRequest, this, [this](const QByteArray &base64Data, const QString &kind) {
        // kind is the OSC 52 selection target; the scanner already filters to
        // "c"/"C" (system clipboard) before emitting, so it is intentionally
        // unused here — writes always target the system clipboard.
        Q_UNUSED(kind);
        if (base64Data.isEmpty()) {
            Q_EMIT clipboardTextReady(QString());
            return;
        }
        QByteArray decoded = QByteArray::fromBase64(base64Data);
        // Filter: strip null bytes and control characters (keep printable + whitespace)
        QByteArray filtered;
        filtered.reserve(decoded.size());
        for (int i = 0; i < decoded.size(); i++) {
            unsigned char c = static_cast<unsigned char>(decoded[i]);
            if (c == 0) continue;
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') continue;
            filtered.append(static_cast<char>(c));
        }
        Q_EMIT clipboardTextReady(QString::fromUtf8(filtered));
    });
    connect(m_vt, &GhosttyVt::clipboardReadRequest, this, [this](const QString &kind) {
        Q_EMIT clipboardReadRequest(kind);
    });

    connect(Settings::instance(), &Settings::colorSchemeChanged, this, [this]() {
        if (m_vt && m_vt->terminal()) {
            applyColorScheme();
        }
    });
    connect(Settings::instance(), &Settings::backgroundOpacityChanged, this, [this]() {
        update();
    });
    connect(Settings::instance(), &Settings::fontFamilyChanged, this, [this]() {
        updateFontMetrics();
        if (width() > 0 && height() > 0) {
            // A family swap can alter cell pixels while keeping the grid
            // counts identical; recalculateDimensions() covers both paths —
            // the cols/rows-changed resize and the pixel-only refresh when
            // the counts happen to coincide.
            recalculateDimensions(true);
        }
        update();
    });
    connect(Settings::instance(), &Settings::urlAutoDetectChanged, this, [this]() {
        m_linkScanDirty = true;
        update();
    });

    // Trailing edge of the search-cache refresh throttle: armed when PTY
    // output arrives inside the throttle window so the final refresh still
    // runs once output pauses (otherwise stale highlights until the next user
    // action). The guard re-checks search state so a timeout after
    // closeSearch()/restartShell() is a no-op.
    m_searchRefreshTimer = new QTimer(this);
    m_searchRefreshTimer->setSingleShot(true);
    connect(m_searchRefreshTimer, &QTimer::timeout, this, [this]() {
        if (m_searchActive && !m_searchPattern.isEmpty()
            && m_vt && m_vt->isSearchTextDirty()) {
            refreshSearchCachePreservingMatch();
        }
    });

    m_blinkEpoch.start();
    armBlinkTimer(BlinkInterval + BlinkGuardMs);
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
    QQuickItem::geometryChanged(newGeometry, oldGeometry);

    // Orientation change mid-swipe would mis-measure the commit fraction
    // (computed from width() at release); cancelling is cheap hardening.
    resetSessionSwipe();

    if (newGeometry.width() <= 0 || newGeometry.height() <= 0)
        return;

    recalculateDimensions();
}

void TerminalView::recalculateDimensions(bool cellPixelsChanged)
{
    // Guard against zero cell dimensions (font not yet initialized)
    if (m_cellWidth <= 0 || m_cellHeight <= 0)
        return;

    if (width() <= 0 || height() <= 0)
        return;

    auto dim = TextUtil::calculateDimensions(width(), height(), m_cellWidth, m_cellHeight, m_topPadding);
    uint16_t newCols = dim.cols;
    uint16_t newRows = dim.rows;

    if (newCols != m_cols || newRows != m_rows) {
        bool wasStarted = (m_cols > 0 && m_rows > 0);
        m_cols = newCols;
        m_rows = newRows;

        if (wasStarted && m_pty->childPid() > 0) {
            struct winsize ws = {};
            ws.ws_col = m_cols;
            ws.ws_row = m_rows;
            ioctl(m_pty->ptyFd(), TIOCSWINSZ, &ws);

            if (m_vt->terminal()) {
                ghostty_terminal_resize(m_vt->terminal(), m_cols, m_rows,
                                        m_cellWidth, m_cellHeight);

                m_vt->markSearchTextDirty(); // reflow moves search offsets

                m_vt->updateMouseEncoderSize(
                    static_cast<uint32_t>(width()),
                    static_cast<uint32_t>(height()),
                    static_cast<uint32_t>(m_cellWidth),
                    static_cast<uint32_t>(m_cellHeight),
                    static_cast<uint32_t>(m_topPadding));
            }
        } else if (!m_shellExited) {
            // Shell not running yet (first resize before PTY start) — build the
            // terminal now. When the shell HAS exited (m_shellExited), skip the
            // resize entirely: setupTerminal() would re-run m_commandArgs and
            // silently re-execute the exited ssh/command behind the exit overlay
            // (the GhosttyVt double-create guard independently prevents the
            // handle leak, but not the silent re-execution). This gate
            // only catches the exec-failure/never-started path: after a
            // NORMAL shell exit the PTY reap timer clears childPid within
            // ~100-200ms, so post-reap geometry changes fall through to the
            // no-op branch below and never re-execute the exited command.
            // restartShell() rebuilds everything on tap.
            setupTerminal();
        }

        m_linkScanDirty = true; // Viewport geometry changed — re-scan links
        update();
    } else if (cellPixelsChanged && m_pty && m_pty->childPid() > 0
               && m_vt && m_vt->terminal()) {
        // Cell pixels changed (font size/family) while the grid counts happen
        // to coincide (e.g. floor(h/23) == floor(h/24)). The terminal and the
        // mouse encoder still hold the old cell size — refresh them
        // explicitly. Same cols/rows, so the grid content (and search
        // offsets) is untouched; no markSearchTextDirty needed.
        ghostty_terminal_resize(m_vt->terminal(), m_cols, m_rows,
                                m_cellWidth, m_cellHeight);
        m_vt->updateMouseEncoderSize(
            static_cast<uint32_t>(width()),
            static_cast<uint32_t>(height()),
            static_cast<uint32_t>(m_cellWidth),
            static_cast<uint32_t>(m_cellHeight),
            static_cast<uint32_t>(m_topPadding));
        m_linkScanDirty = true; // Cell geometry changed — re-scan links
        update();
    }
}

void TerminalView::focusInEvent(QFocusEvent *event)
{
    QQuickItem::focusInEvent(event);

    QInputMethod *im = QGuiApplication::inputMethod();
    if (im && !m_suppressKeyboardAutoShow)
        im->show();
    m_suppressKeyboardAutoShow = false;

    update();
}

void TerminalView::focusOutEvent(QFocusEvent *event)
{
    QQuickItem::focusOutEvent(event);

    // App background / focus loss mid-swipe would otherwise leave the flag set
    // and misclassify the next touch.
    resetSessionSwipe();

    // No release follows a key pressed before focus loss; drop the shortcut
    // match so a later release (e.g. after refocus) isn't wrongly swallowed.
    m_lastConsumedShortcutKey = 0;

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
            QChar ch = event->commitString().at(0).toLower();
            GhosttyKey key = KeyMapping::mapCharToKey(ch);

            if (key != GHOSTTY_KEY_UNIDENTIFIED) {
                sendKeyEvent(key, GHOSTTY_KEY_ACTION_PRESS,
                             static_cast<GhosttyMods>(m_stickyModifiers),
                             event->commitString());
                setStickyModifiers(0);
                update();
                event->accept();
                return;
            }
            // If we can't map the character, fall through to raw text
            setStickyModifiers(0);
        }

        m_pty->writeData(utf8.constData(), utf8.size());

        scrollViewportToBottom();

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

    // IME replacement events (replacementStart/replacementLength) target an
    // editable text region around the cursor. A terminal has no editable
    // region — the terminal emulator manages its own buffer — so we accept
    // and suppress these events to prevent unwanted default handling.
    if (event->replacementStart() != 0 || event->replacementLength() != 0) {
        event->accept();
        return;
    }

    QQuickItem::inputMethodEvent(event);
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

        return QRectF(cx * m_cellWidth, cy * m_cellHeight + m_topPadding,
                      m_cellWidth, m_cellHeight);
    }
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

void TerminalView::applyColorScheme()
{
    if (!m_vt || !m_vt->terminal())
        return;

    struct ColorDef { GhosttyColorRgb fg, bg, cursor; };
    static const QMap<QString, ColorDef> schemes = {
        {"dark",           {{204,204,204},    {30,30,30},      {255,255,255}}},
        {"light",          {{51,51,51},       {255,255,255},   {0,0,0}}},
    };

    QString scheme = Settings::instance()->colorScheme();
    auto it = schemes.constFind(scheme);
    if (it == schemes.constEnd()) {
        qWarning() << "Unknown color scheme:" << scheme << "falling back to dark";
        it = schemes.constFind(QStringLiteral("dark"));
    }

    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &it->fg);
    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &it->bg);
    ghostty_terminal_set(m_vt->terminal(),
                         GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &it->cursor);

    update();
}

void TerminalView::setupTerminal()
{
    if (m_cols == 0 || m_rows == 0)
        return;

    if (!m_vt->create(m_cols, m_rows, [this](const char *data, size_t len) {
            m_pty->writeData(data, len);
        })) {
        qWarning() << "Failed to create GhosttyVt";
        return;
    }

    applyColorScheme();

    ghostty_terminal_resize(m_vt->terminal(), m_cols, m_rows,
                            m_cellWidth, m_cellHeight);

    // Restore scrollback if pending (must be before startShell)
    if (!m_pendingScrollback.isEmpty()) {
        m_vt->restoreScrollback(m_pendingScrollback, m_cols);
        m_pendingScrollback.clear();
    }

    m_vt->updateMouseEncoderSize(
        static_cast<uint32_t>(width()),
        static_cast<uint32_t>(height()),
        static_cast<uint32_t>(m_cellWidth),
        static_cast<uint32_t>(m_cellHeight),
        static_cast<uint32_t>(m_topPadding));

    if (m_commandArgs.isEmpty()) {
        m_pty->setShellCommand(Settings::instance()->shellCommand());
        if (!m_pty->startShell(m_cols, m_rows)) {
            qWarning() << "Failed to start shell";
            m_vt->destroy();
            return;
        }
    } else {
        if (!m_pty->startCommand(m_commandArgs.first(),
                                 m_commandArgs.mid(1), m_cols, m_rows)) {
            qWarning() << "Failed to start command";
            m_vt->destroy();
            return;
        }
    }

    if (!m_autorunCommand.isEmpty()) {
        QTimer::singleShot(AutorunDelayMs, this, &TerminalView::runAutorunCommand);
    }
}

void TerminalView::onPtyData(const QByteArray &data)
{
    Q_EMIT ptyDataReceived();

    // The selection is anchored to viewport pixels, so only output that
    // moves the viewport invalidates it; in-place rewrites keep it
    // (v1.0.0 behavior; copying from live output in less or tmux still
    // works). If the scrollbar read fails, fall back to clearing.
    GhosttyTerminalScrollbar scrollbarBefore = {};
    bool scrollbarOk = false;
    if (m_selecting && m_vt->terminal()) {
        scrollbarOk = ghostty_terminal_get(m_vt->terminal(),
                                           GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                           &scrollbarBefore) == GHOSTTY_SUCCESS;
    }

    m_vt->vtWrite(reinterpret_cast<const uint8_t *>(data.constData()),
                   data.size());
    m_linkScanDirty = true;

    bool viewportMoved = false;
    if (m_selecting && scrollbarOk) {
        GhosttyTerminalScrollbar scrollbarAfter = {};
        if (ghostty_terminal_get(m_vt->terminal(),
                                 GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                                 &scrollbarAfter) == GHOSTTY_SUCCESS) {
            viewportMoved = scrollbarAfter.offset != scrollbarBefore.offset
                || scrollbarAfter.total != scrollbarBefore.total
                || scrollbarAfter.len != scrollbarBefore.len;
        } else {
            viewportMoved = true;
        }
    }

    if (m_selecting && (!scrollbarOk || viewportMoved))
        clearSelection();

    // Live output shifts rows while the search panel is open; refresh the
    // match cache on a throttle so highlights don't drift stale until the
    // user navigates. Skipped when no search is active or the pattern is
    // empty (nothing to keep in sync).
    if (m_searchActive && !m_searchPattern.isEmpty()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsed = now - m_lastSearchRefreshMs;
        if (elapsed >= SearchRefreshIntervalMs) {
            m_lastSearchRefreshMs = now;
            if (m_searchRefreshTimer->isActive())
                m_searchRefreshTimer->stop();
            refreshSearchCachePreservingMatch();
        } else if (!m_searchRefreshTimer->isActive()) {
            // Output paused inside the throttle window — arm the trailing
            // edge for the remaining time so the final refresh still runs.
            m_searchRefreshTimer->start(
                SearchRefreshIntervalMs - static_cast<int>(elapsed));
        }
    }

    // GL is the only renderer — update immediately. Qt's scene graph
    // coalesces multiple update() calls into a single frame.
    update();
    Q_EMIT contentChanged(); // PTY data is the real content-change signal
}

void TerminalView::update()
{
    QQuickItem::update();
    Q_EMIT repaintRequested(); // Trigger GL repaint (selection, blink, scroll, etc.)
}

void TerminalView::onShellExited(int exitCode)
{
    qInfo() << "Shell exited with code" << exitCode;
    m_shellExited = true;
    m_shellExitCode = exitCode;
    if (!m_commandArgs.isEmpty()) {
        Q_EMIT commandExited(exitCode);
    }
    update();
}

void TerminalView::restartShell()
{
    closeSearch(); // Clear stale search state before destroying terminal
    m_shellExited = false;
    m_shellExitCode = 0;
    m_commandArgs.clear();
    Q_EMIT shellRestarted();
    m_pty->stop(false); // async reap — avoids 500ms GUI freeze on restart
    m_vt->destroy();
    setupTerminal();
    update();
}

void TerminalView::paste()
{
    resetBlinkOnInput();
    scrollViewportToBottom();

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
    }

    // Fallback: send raw UTF-8 only if encoding completely fails
    m_pty->writeData(utf8.constData(), utf8.size());
}

void TerminalView::sendClipboardText(const QString &text, const QString &kind)
{
    if (!m_pty || m_pty->childPid() <= 0)
        return;

    // Defense-in-depth: kind is interpolated into the OSC 52 response, so
    // reject anything that isn't a single alphabetic char to prevent
    // injecting ';', BEL, or ESC into the escape sequence.
    QByteArray safeKind = (kind.size() == 1 && kind.at(0).isLetter())
                          ? kind.toUtf8() : QByteArray("c");

    QByteArray utf8 = text.toUtf8();
    // Check decoded size before encoding to avoid unnecessary ~1MB allocation
    if (utf8.size() > 768 * 1024) // ~1MB when base64-encoded
        return;

    QByteArray base64 = utf8.toBase64();

    QByteArray response;
    response.append("\x1b]52;");
    response.append(safeKind);
    response.append(';');
    response.append(base64);
    response.append('\x07'); // BEL terminator
    m_pty->writeData(response.constData(), response.size());
}

void TerminalView::copySelection()
{
    if (!m_vt || !m_vt->renderState())
        return;

    QPointF startCell = cellFromPixelClamped(m_selStart);
    QPointF endCell = cellFromPixelClamped(m_selEnd);
    if (startCell.x() < 0 || endCell.x() < 0)
        return;

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

        // Track last-cell content for the readline wrap heuristic.
        bool lastCellHadContent = false;

        int colIdx = 0;
        while (ghostty_render_state_row_cells_next(cells)) {
            if (rowIdx == startRow && colIdx < startCol) { colIdx++; continue; }
            if (rowIdx == endRow && colIdx > endCol) break;

            // Skip wide-char spacer cells (tail of CJK/emoji, head of RTL)
            // to avoid injecting phantom spaces.
            GhosttyCell rawCell = 0;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                    &rawCell) == GHOSTTY_SUCCESS
                    && GhosttyVt::isWideSpacerCell(rawCell)) {
                // A wide-char spacer at the last column means the head cell
                // (col-1) fills the row. Treat that as content for the heuristic.
                if (rowIdx < endRow && colIdx == m_cols - 1)
                    lastCellHadContent = true;
                colIdx++;
                continue;
            }

            uint32_t graphemesLen = 0;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &graphemesLen);

            if (rowIdx < endRow && colIdx == m_cols - 1)
                lastCellHadContent = (graphemesLen > 0);

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
        if (rowIdx < endRow) {
            // Suppress newline when this row soft-wraps into the next.
            // Primary: ghostty WRAP flag (terminal autowrap). Fallback:
            // full-width heuristic: if WRAP is false but the row fills the
            // entire width (last cell is non-empty), treat it as a soft wrap.
            // Handles readline/busybox wrapping which positions the cursor
            // manually instead of triggering terminal autowrap.
            // May false-positive on an exact-width line with a hard newline.
            // Alternative: OSC 133 shell integration would scope the heuristic
            // to input rows only, but requires per-shell scripts and maintenance.
            // See also refreshLinks (terminalview_links.cpp) — heuristic not
            // ported there; link extraction still uses WRAP flag only.
            GhosttyRow rawRow = 0;
            bool isWrapped = false;
            if (ghostty_render_state_row_get(iterator,
                                             GHOSTTY_RENDER_STATE_ROW_DATA_RAW,
                                             &rawRow) == GHOSTTY_SUCCESS) {
                ghostty_row_get(rawRow, GHOSTTY_ROW_DATA_WRAP, &isWrapped);
            }
            isWrapped = TextUtil::isSoftWrapped(isWrapped, lastCellHadContent);

            if (!isWrapped)
                text += QLatin1Char('\n');
        }

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

    // Trigger geometry recalculation — cell dimensions changed, which usually
    // also changes cols/rows; recalculateDimensions() falls back to a
    // pixel-only refresh when the counts happen to coincide.
    if (width() > 0 && height() > 0) {
        recalculateDimensions(true);
    }
}

void TerminalView::setStickyModifiers(int mods)
{
    if (m_stickyModifiers == mods)
        return;
    m_stickyModifiers = mods;
    Q_EMIT stickyModifiersChanged();
}

void TerminalView::setSelectionHighlightColor(const QColor &color)
{
    if (m_selectionHighlightColor == color)
        return;
    m_selectionHighlightColor = color;
    Q_EMIT selectionHighlightColorChanged();
    update();
}

void TerminalView::setSelectionHandleColor(const QColor &color)
{
    if (m_selectionHandleColor == color)
        return;
    m_selectionHandleColor = color;
    Q_EMIT selectionHandleColorChanged();
    update();
}

void TerminalView::setSelectionHandleBorderColor(const QColor &color)
{
    if (m_selectionHandleBorderColor == color)
        return;
    m_selectionHandleBorderColor = color;
    Q_EMIT selectionHandleBorderColorChanged();
    update();
}

void TerminalView::setSearchHighlightColor(const QColor &color)
{
    if (m_searchHighlightColor == color)
        return;
    m_searchHighlightColor = color;
    Q_EMIT searchHighlightColorChanged();
    update();
}

void TerminalView::setSearchCurrentColor(const QColor &color)
{
    if (m_searchCurrentColor == color)
        return;
    m_searchCurrentColor = color;
    Q_EMIT searchCurrentColorChanged();
    update();
}

void TerminalView::setShellExitOverlayColor(const QColor &color)
{
    if (m_shellExitOverlayColor == color)
        return;
    m_shellExitOverlayColor = color;
    Q_EMIT shellExitOverlayColorChanged();
    update();
}

void TerminalView::setShellExitTextColor(const QColor &color)
{
    if (m_shellExitTextColor == color)
        return;
    m_shellExitTextColor = color;
    Q_EMIT shellExitTextColorChanged();
    update();
}

void TerminalView::setMagnifierBorderColor(const QColor &color)
{
    if (m_magnifierBorderColor == color)
        return;
    m_magnifierBorderColor = color;
    Q_EMIT magnifierBorderColorChanged();
    update();
}

void TerminalView::setTopPadding(int padding)
{
    if (m_topPadding == padding)
        return;
    m_topPadding = padding;
    Q_EMIT topPaddingChanged();
    update();
    // Recalculate dimensions since padding affects available terminal rows
    if (width() > 0 && height() > 0)
        recalculateDimensions();
}

void TerminalView::setPullDownZoneHeight(int height)
{
    if (m_pullDownZoneHeight == height)
        return;
    m_pullDownZoneHeight = height;
    Q_EMIT pullDownZoneHeightChanged();
}

void TerminalView::updateFontMetrics()
{
    QString family = Settings::instance()->fontFamily();
    if (family.isEmpty())
        family = QStringLiteral("monospace");
    m_font = QFont(family, static_cast<int>(m_fontSize));
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);

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

    // If scrolled up viewing history, scroll back to bottom so the user
    // can see what they're typing (matches keyPressEvent behavior).
    scrollViewportToBottom();

    GhosttyKey key = KeyMapping::mapQtKey(qtKey);
    // Accept GhosttyMods directly (not Qt modifier values)
    sendKeyEvent(key, GHOSTTY_KEY_ACTION_PRESS, static_cast<GhosttyMods>(modifiers), QString());
    update();
}

QPointF TerminalView::cellFromPixel(const QPointF &pos) const
{
    return TextUtil::cellFromPixel(pos, m_cellWidth, m_cellHeight, m_cols, m_rows, m_topPadding);
}

QPointF TerminalView::cellFromPixelClamped(const QPointF &pos) const
{
    return TextUtil::cellFromPixelClamped(pos, m_cellWidth, m_cellHeight, m_cols, m_rows, m_topPadding);
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
        if (!m_selectedText.isEmpty()) {
            m_selectedText.clear();
            Q_EMIT selectedTextChanged();
        }
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
        if (c < 0 || c >= m_cols)
            return 0;
        if (ghostty_render_state_row_cells_select(cells, static_cast<uint16_t>(c)) != GHOSTTY_SUCCESS)
            return 0;
        uint32_t len = 0;
        ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &len);
        // Spacers carry no grapheme — walk LEFT to the head cell of the wide
        // char and return its codepoint. SPACER_TAIL only; SPACER_HEAD's head
        // is on the next row, so a left-walk would land on the wrong cell.
        // Mirrors the spacer-handling pattern in refreshLinks
        // (terminalview_links.cpp:78-85).
        while (len == 0 && c > 0) {
            GhosttyCell rawCell = 0;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                    &rawCell) != GHOSTTY_SUCCESS
                    || !GhosttyVt::isWideSpacerTailCell(rawCell)) {
                return 0; // genuinely empty cell, or SPACER_HEAD (head is on the next row)
            }
            c--;
            if (ghostty_render_state_row_cells_select(cells, static_cast<uint16_t>(c)) != GHOSTTY_SUCCESS)
                return 0;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &len);
        }
        if (len == 0)
            return 0;
        uint32_t buf[128];
        if (len > 128) return 0;
        ghostty_render_state_row_cells_get(cells,
            GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, buf);
        return buf[0]; // base codepoint
    };

    uint32_t tappedChar = getGraphemeAt(col);
    bool tappedIsWord = TextUtil::isWordChar(tappedChar);

    int startCol = col;
    int endCol = col;

    if (tappedIsWord) {
        while (startCol > 0) {
            uint32_t ch = getGraphemeAt(startCol - 1);
            if (!TextUtil::isWordChar(ch))
                break;
            startCol--;
        }
        while (endCol < m_cols - 1) {
            uint32_t ch = getGraphemeAt(endCol + 1);
            if (!TextUtil::isWordChar(ch))
                break;
            endCol++;
        }
    } else {
        // Non-word character: empty cells select a contiguous run; everything else selects a single char.
        if (tappedChar == 0) {
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

    m_selStart = QPointF(startCol * m_cellWidth, row * m_cellHeight + m_topPadding);
    m_selEnd = QPointF((endCol + 1) * m_cellWidth - 1, row * m_cellHeight + m_topPadding);
    m_selecting = true;
    m_magnifierVisible = false;
    m_handlesVisible = true;

    copySelection();
    update();
}

void TerminalView::selectLineAt(const QPointF &pos)
{
    QPointF cell = cellFromPixel(pos);
    if (cell.x() < 0)
        return;

    int row = static_cast<int>(cell.y());

    m_selStart = QPointF(0, row * m_cellHeight + m_topPadding);
    m_selEnd = QPointF(m_cols * m_cellWidth - 1, row * m_cellHeight + m_topPadding);
    m_selecting = true;
    m_magnifierVisible = false;
    m_handlesVisible = true;

    copySelection();
    update();
}

int TerminalView::handleHitTest(const QPointF &pos) const
{
    if (!m_handlesVisible || !m_selecting || m_selStart == m_selEnd)
        return 0;

    QPointF startCell = cellFromPixelClamped(m_selStart);
    QPointF endCell = cellFromPixelClamped(m_selEnd);
    if (startCell.x() < 0 || endCell.x() < 0)
        return 0;

    int sc = static_cast<int>(startCell.x());
    int sr = static_cast<int>(startCell.y());
    int ec = static_cast<int>(endCell.x());
    int er = static_cast<int>(endCell.y());

    // No normalization swap — handle 1 always maps to m_selStart,
    // handle 2 always maps to m_selEnd, regardless of selection direction
    QPointF startPos(sc * m_cellWidth, (sr + 1) * m_cellHeight + m_topPadding);
    QPointF endPos((ec + 1) * m_cellWidth, (er + 1) * m_cellHeight + m_topPadding);

    // Use a generous hit area (HandleRadius + some padding for finger imprecision)
    qreal hitRadius = HandleRadius * 1.5;

    if (QLineF(pos, startPos).length() <= hitRadius)
        return 1; // Start handle (m_selStart)
    if (QLineF(pos, endPos).length() <= hitRadius)
        return 2; // End handle (m_selEnd)

    return 0; // No hit
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
    holdBlinkSolid();
    clearSelection();
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    resetBlinkOnInput();

    GhosttyKey key = KeyMapping::mapQtKey(event->key());
    GhosttyMods mods = KeyMapping::mapQtModifiers(event->modifiers());

    if ((mods & GHOSTTY_MODS_CTRL) && (mods & GHOSTTY_MODS_SHIFT)) {
        const bool isShortcutKey = key == GHOSTTY_KEY_C
            || key == GHOSTTY_KEY_V
            || key == GHOSTTY_KEY_EQUAL
            || key == GHOSTTY_KEY_MINUS
            || key == GHOSTTY_KEY_F
            || key == GHOSTTY_KEY_ARROW_LEFT
            || key == GHOSTTY_KEY_ARROW_RIGHT
            || key == GHOSTTY_KEY_K;
        if (isShortcutKey) {
            // Autorepeat of a consumed shortcut must not fall through (hold-to-toggle spam).
            if (event->isAutoRepeat()) {
                event->accept();
                return;
            }

            // No PRESS is sent for consumed shortcuts; remember the key so the release is swallowed.
            if (key == GHOSTTY_KEY_C) { copySelection(); m_lastConsumedShortcutKey = event->key(); event->accept(); return; }
            if (key == GHOSTTY_KEY_V) { paste(); m_lastConsumedShortcutKey = event->key(); event->accept(); return; }
            if (key == GHOSTTY_KEY_EQUAL) { Q_EMIT zoomRequested(1);  m_lastConsumedShortcutKey = event->key(); event->accept(); return; }  // Ctrl+Shift+=
            if (key == GHOSTTY_KEY_MINUS) { Q_EMIT zoomRequested(-1); m_lastConsumedShortcutKey = event->key(); event->accept(); return; }  // Ctrl+Shift+-
            if (key == GHOSTTY_KEY_F) {
                if (m_searchActive)
                    closeSearch();
                else
                    openSearch();
                // Emit AFTER the state flip but BEFORE recording the consumed
                // key: the handler synchronously focuses the search field,
                // whose focusOutEvent zeroes m_lastConsumedShortcutKey — the
                // assignment below must run last to win.
                Q_EMIT searchToggled();
                m_lastConsumedShortcutKey = event->key();
                event->accept();
                return;
            }
            if (key == GHOSTTY_KEY_ARROW_LEFT)  { Q_EMIT navigateSession(-1); m_lastConsumedShortcutKey = event->key(); event->accept(); return; }
            if (key == GHOSTTY_KEY_ARROW_RIGHT) { Q_EMIT navigateSession(1);  m_lastConsumedShortcutKey = event->key(); event->accept(); return; }
            if (key == GHOSTTY_KEY_K) { Q_EMIT toggleKeybar(); m_lastConsumedShortcutKey = event->key(); event->accept(); return; }
        }
    }

    // Any other key press invalidates the shortcut-release match.
    m_lastConsumedShortcutKey = 0;

    // Auto-repeat maps to REPEAT action (enables Kitty protocol repeat)
    GhosttyKeyAction action = event->isAutoRepeat()
        ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS;

    // If scrolled up viewing history, scroll back to bottom so the user
    // can see what they're typing.
    scrollViewportToBottom();

    sendKeyEvent(key, action, mods, event->text());
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

    // Swallow the release of a shortcut-consumed press (see m_lastConsumedShortcutKey doc).
    if (event->key() == m_lastConsumedShortcutKey) {
        m_lastConsumedShortcutKey = 0;
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

void TerminalView::setCommandArgs(const QStringList &args)
{
    m_commandArgs = args;
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

