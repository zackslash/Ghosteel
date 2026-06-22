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
#include <algorithm>
#include <cmath>
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
            if (c == 0) continue; // strip null bytes
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') continue; // strip control chars
            filtered.append(static_cast<char>(c));
        }
        Q_EMIT clipboardTextReady(QString::fromUtf8(filtered));
    });
    connect(m_vt, &GhosttyVt::clipboardReadRequest, this, [this](const QString &kind) {
        Q_EMIT clipboardReadRequest(kind);
    });

    // Live-apply settings changes to running terminal
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
        update();
    });
    connect(Settings::instance(), &Settings::urlAutoDetectChanged, this, [this]() {
        m_linkScanDirty = true;
        update();
    });

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
    QQuickItem::geometryChanged(newGeometry, oldGeometry);

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

                // Update mouse encoder geometry
                m_vt->updateMouseEncoderSize(
                    static_cast<uint32_t>(width()),
                    static_cast<uint32_t>(height()),
                    static_cast<uint32_t>(m_cellWidth),
                    static_cast<uint32_t>(m_cellHeight),
                    static_cast<uint32_t>(m_topPadding));
            }
        } else {
            setupTerminal();
        }

        m_linkScanDirty = true; // Viewport geometry changed — re-scan links
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
    m_vt->vtWrite(reinterpret_cast<const uint8_t *>(data.constData()),
                   data.size());
    m_linkScanDirty = true;

    // GL is the only renderer — update immediately. Qt's scene graph
    // coalesces multiple update() calls into a single frame.
    update();
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
    m_pty->stop();
    m_vt->destroy();
    setupTerminal();
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
    }
}

void TerminalView::update()
{
    QQuickItem::update();
    Q_EMIT contentChanged();
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

    QPointF startCell = cellFromPixel(m_selStart);
    QPointF endCell = cellFromPixel(m_selEnd);
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
        // Non-word character: empty cells select a contiguous run; everything else selects a single char.
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

    // Select entire row from column 0 to last column
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
    if (m_shellExited) {
        restartShell();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_cursorBlinkVisible = true;
        m_lastInputTime.start();

        m_mouseTrackingActive = m_vt->isMouseTracking();

        if (m_mouseTrackingActive) {
            // Safety net: reject touches in the pull-down zone. In practice,
            // touchEvent accepts TUI touches before synthesis, so this is
            // rarely reached.
            if (event->pos().y() < m_pullDownZoneHeight) {
                QQuickItem::mousePressEvent(event);
                return;
            }

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
            m_handlesVisible = false;
            m_magnifierVisible = true;
            m_velocityInitialized = false;
            m_tapCount = 0; // Prevent phantom triple-tap after handle drag
            setKeepMouseGrab(true);
            event->accept();
            return;
        }

        // Check if tapping a link — defer opening to release for clean interaction
        {
            QPointF cell = cellFromPixel(event->pos());
            if (cell.x() >= 0 && cell.y() >= 0) {
                QString uri = findLinkAt(static_cast<int>(cell.x()),
                                         static_cast<int>(cell.y()));
                if (!uri.isEmpty()) {
                    m_pendingLinkTap = true;
                    m_tappedLinkUri = uri;
                    m_linkTapStartPos = event->pos();
                    setKeepMouseGrab(true);
                    event->accept();
                    return;
                }
            }
        }

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
            clearSelection();
            selectWordAt(event->pos());
            event->accept();
            return;
        }
        if (m_tapCount == 3) {
            clearSelection();
            selectLineAt(event->pos());
            event->accept();
            return;
        }

        clearSelection();
        m_selStart = event->pos();
        m_selEnd = event->pos();
        m_longPressTimerId = startTimer(LongPressTimeout);
        event->accept();
        return;
    }
    QQuickItem::mousePressEvent(event);
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggingHandle != 0) {
        if (m_draggingHandle == 1)
            m_selStart = event->pos();
        else
            m_selEnd = event->pos();

        // Magnifier stays visible during handle drags — no velocity-based hiding.
        // It was set visible on mousePress and should remain so until release.

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

        m_magnifierVisible = updateMagnifierVelocity(event->pos());

        // Keep cursor blink paused during active selection to prevent
        // full redraws that cause magnifier flicker
        m_lastInputTime.start();

        update();
        event->accept();
        return;
    }

    if (m_mouseTrackingActive) {
        GhosttyMouseButton btn = m_mouseButtonPressed
            ? GHOSTTY_MOUSE_BUTTON_LEFT : GHOSTTY_MOUSE_BUTTON_UNKNOWN;
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_MOTION, btn,
                       event->pos(), KeyMapping::mapQtModifiers(event->modifiers()));
        event->accept();
        return;
    }

    QQuickItem::mouseMoveEvent(event);
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_longPressTimerId) {
        killTimer(m_longPressTimerId);
        m_longPressTimerId = 0;
    }

    if (m_mouseTrackingActive) {
        sendMouseEvent(GHOSTTY_MOUSE_ACTION_RELEASE, GHOSTTY_MOUSE_BUTTON_LEFT,
                       event->pos(), KeyMapping::mapQtModifiers(event->modifiers()));

        m_mouseButtonPressed = false;
        m_vt->setMouseButtonPressed(false);
        m_mouseTrackingActive = false;
        setKeepMouseGrab(false);

        event->accept();
        return;
    }

    // Open link on clean tap-release (no significant drag)
    if (m_pendingLinkTap) {
        qreal dragDist = QLineF(m_linkTapStartPos, event->pos()).length();
        m_pendingLinkTap = false;
        setKeepMouseGrab(false);
        QString uri = m_tappedLinkUri;
        m_tappedLinkUri.clear();
        if (dragDist < TapDistancePx && !uri.isEmpty()) {
            Q_EMIT linkActivated(uri);
            event->accept();
            return;
        }
        // Fall through to normal release handling if finger moved
    }

    if (m_draggingHandle != 0) {
        if (m_draggingHandle == 1)
            m_selStart = event->pos();
        else
            m_selEnd = event->pos();
        m_draggingHandle = 0;
        m_magnifierVisible = false;
        m_handlesVisible = true;
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
        m_magnifierVisible = false;
        m_handlesVisible = true;
        update();
        event->accept();
        return;
    }
    // Release mouse grab acquired by touchEvent multi-touch/TUI path.
    setKeepMouseGrab(false);
    QQuickItem::mouseReleaseEvent(event);
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    if (!m_vt || !m_vt->terminal()) {
        QQuickItem::wheelEvent(event);
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
        m_linkScanDirty = true;
        update();
    }

    event->accept();
}

void TerminalView::touchEvent(QTouchEvent *event)
{
    if (!m_vt || !m_vt->terminal()) {
        QQuickItem::touchEvent(event);
        return;
    }

    const auto points = event->touchPoints();

    // ── Multi-touch (2+ fingers) ──────────────────────────────────
    if (points.size() >= 2) {
        setKeepMouseGrab(true);
        // Qt 5.6: the touch grab is a SEPARATE mechanism from the mouse grab.
        // SilicaFlickable (a filtering parent) steals the touch grab via its
        // childMouseEventFilter — setKeepMouseGrab alone does NOT stop this.
        // setKeepTouchGrab(true) denies the steal so two-finger scroll/pinch
        // stays with the terminal instead of triggering the PullDownMenu.
        setKeepTouchGrab(true);

        // ACTIVE grab — passive flags above only prevent future steals.
        // SilicaFlickable ignores them once its drag recogniser has armed.
        // grabTouchPoints()/grabMouse() wrest the grab back immediately.
        {
            QVector<int> ids;
            ids.reserve(points.size());
            for (const auto &p : points)
                ids.append(p.id());
            grabTouchPoints(ids);
            grabMouse();
        }

        switch (event->type()) {
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            handleMultiTouchEnd();
            break;
        default:
            // Start the gesture on the FIRST ≥2-point event of any type.
            // When the second finger lands after the first, Qt delivers a
            // TouchUpdate (not TouchBegin), so keying off the event type alone
            // would skip handleMultiTouchBegin — and the Flickable would never
            // be disabled, re-opening the PullDownMenu bug for staggered taps.
            if (!m_multiTouchActive)
                handleMultiTouchBegin(points);
            else
                handleMultiTouchUpdate(points);
            break;
        }
        event->accept();
        return;
    }

    // ── Drop below 2 points during active multi-touch gesture ────
    // Check m_multiTouchActive too: a brief two-finger tap may never
    // leave Undecided, but must still end (or the Flickable stays disabled).
    if (m_multiTouchActive || m_gestureMode != GestureMode::Undecided) {
        handleMultiTouchEnd();
    }

    // ── Single-finger events ──────────────────────────────────────
    // TUI mode (mouse tracking): accept + grab + forward as synthetic
    // mouse/wheel events.  Normal mode: fall through to QQuickItem —
    // accepting would break the Flickable's press-delay disambiguation
    // (instant drag → pull-down, press-hold → selection).

    if (m_mouseTrackingActive) {
        if (event->type() == QEvent::TouchBegin && points.size() == 1) {
            handleTuiTouchBegin(event, points.first());
            return;
        }
        if (event->type() == QEvent::TouchUpdate && points.size() == 1) {
            handleTuiTouchUpdate(event, points.first());
            return;
        }
        if (event->type() == QEvent::TouchEnd
            || event->type() == QEvent::TouchCancel) {
            handleTuiTouchEnd(event, points);
            return;
        }
    }

    // Normal mode: native fall-through — let Qt synthesise mouse events and
    // the Flickable handle pull-down disambiguation.
    QQuickItem::touchEvent(event);
}

void TerminalView::handleTuiTouchBegin(QTouchEvent *event,
                                       const QTouchEvent::TouchPoint &pt)
{
    event->accept();
    setKeepMouseGrab(true);
    setKeepTouchGrab(true);
    Q_EMIT requestParentInteractive(false);
    grabTouchPoints(QVector<int>{ pt.id() });
    grabMouse();
    m_tuiDragLastY = pt.pos().y();
    m_tuiScrollAccumulator = 0;
    QMouseEvent synthPress(QEvent::MouseButtonPress,
                           pt.pos(), pt.screenPos(),
                           Qt::LeftButton, Qt::LeftButton,
                           event->modifiers());
    mousePressEvent(&synthPress);
}

void TerminalView::handleTuiTouchUpdate(QTouchEvent *event,
                                        const QTouchEvent::TouchPoint &pt)
{
    event->accept();

    // Convert vertical drag delta to wheel events for TUI scroll.
    qreal deltaY = pt.pos().y() - m_tuiDragLastY;
    m_tuiDragLastY = pt.pos().y();
    qreal newDelta = deltaY / m_cellHeight; // positive: down-drag = scroll up (natural scrolling)
    auto scrollResult = TextUtil::accumulateScroll(
        m_tuiScrollAccumulator, newDelta);
    m_tuiScrollAccumulator = scrollResult.accumulator;
    if (scrollResult.lines != 0) {
        GhosttyMods mods = KeyMapping::mapQtModifiers(event->modifiers());
        GhosttyMouseButton btn = (scrollResult.lines > 0)
            ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE;
        for (int i = 0; i < qAbs(scrollResult.lines); ++i) {
            sendMouseEvent(GHOSTTY_MOUSE_ACTION_PRESS, btn, pt.pos(), mods);
            sendMouseEvent(GHOSTTY_MOUSE_ACTION_RELEASE, btn, pt.pos(), mods);
        }
    }

    // Also forward mouse motion for TUI click/drag/selection.
    QMouseEvent synthMove(QEvent::MouseMove,
                          pt.pos(), pt.screenPos(),
                          Qt::LeftButton, Qt::LeftButton,
                          event->modifiers());
    mouseMoveEvent(&synthMove);
}

void TerminalView::handleTuiTouchEnd(QTouchEvent *event,
                                     const QList<QTouchEvent::TouchPoint> &points)
{
    if (points.size() == 1) {
        const auto &pt = points.first();
        QMouseEvent synthRel(QEvent::MouseButtonRelease,
                             pt.pos(), pt.screenPos(),
                             Qt::LeftButton, Qt::NoButton,
                             event->modifiers());
        mouseReleaseEvent(&synthRel);
    }
    Q_EMIT requestParentInteractive(true);
    m_tuiScrollAccumulator = 0;
    setKeepMouseGrab(false);
    setKeepTouchGrab(false);
    event->accept();
}

void TerminalView::handleMultiTouchBegin(const QList<QTouchEvent::TouchPoint> &points)
{
    // Shell exited — ignore multi-touch, let it fall through to parent
    if (m_shellExited)
        return;

    // Clean up any previous gesture that wasn't properly ended. This can
    // happen if TouchEnd was missed (e.g., window deactivated, touch stolen
    // by another item) and a new gesture starts while the overlay is still
    // visible. Without this, pinchingChanged(false) is never emitted.
    if (m_multiTouchActive || m_gestureMode != GestureMode::Undecided) {
        handleMultiTouchEnd();
    }

    // Disable the parent Flickable immediately — passive grabs aren't
    // enough; SilicaFlickable steals the gesture before scroll-commit.
    Q_EMIT requestParentInteractive(false);
    m_multiTouchActive = true;

    // Cancel any active long-press / selection / handle drag
    if (m_longPressTimerId) {
        killTimer(m_longPressTimerId);
        m_longPressTimerId = 0;
    }
    if (m_draggingHandle != 0) {
        m_draggingHandle = 0;
        m_magnifierVisible = false;
        m_handlesVisible = true;
        setKeepMouseGrab(false);
    }
    if (m_selecting)
        clearSelection();

    // Record initial positions
    qreal avgY = (points[0].pos().y() + points[1].pos().y()) / 2.0;
    m_twoFingerLastY = avgY;

    // TUI apps or pinch-to-zoom disabled: force scroll mode
    if (m_vt->isMouseTracking() || !Settings::instance()->pinchToZoom()) {
        m_gestureMode = GestureMode::Scrolling;
        return;
    }

    // Undecided — record initial geometry for later classification
    QPointF p0 = points[0].pos();
    QPointF p1 = points[1].pos();
    m_pinchInitialDistance = QLineF(p0, p1).length();
    m_gestureInitialCentroid = QPointF((p0.x() + p1.x()) / 2.0,
                                       (p0.y() + p1.y()) / 2.0);
    m_gestureMode = GestureMode::Undecided;
    m_pinchCandidateFrames = 0;
}

void TerminalView::handleMultiTouchUpdate(const QList<QTouchEvent::TouchPoint> &points)
{
    // If any touch point was released, end the gesture immediately. Qt may
    // deliver a TouchUpdate with a released point before TouchEnd arrives.
    // Without this, the gesture continues processing a stale finger position
    // and the overlay may not hide if the final TouchEnd is also missed.
    for (const auto &p : points) {
        if (p.state() & Qt::TouchPointReleased) {
            handleMultiTouchEnd();
            return;
        }
    }

    QPointF p0 = points[0].pos();
    QPointF p1 = points[1].pos();
    qreal currentDistance = QLineF(p0, p1).length();
    QPointF currentCentroid((p0.x() + p1.x()) / 2.0,
                            (p0.y() + p1.y()) / 2.0);

    switch (m_gestureMode) {
    case GestureMode::Undecided: {
        // --- Check for pinch classification ---
        qreal distanceRatio = (m_pinchInitialDistance > 0)
            ? currentDistance / m_pinchInitialDistance : 1.0;
        bool ratioExceeded = (distanceRatio > PinchRatioThreshold)
                          || (distanceRatio < 1.0 / PinchRatioThreshold);

        if (ratioExceeded) {
            m_pinchCandidateFrames++;
            if (m_pinchCandidateFrames >= PinchRatioFrames) {
                // Commit to pinch mode
                m_gestureMode = GestureMode::Pinching;
                m_pinchBaseFontSize = m_fontSize;
                m_lastAppliedFontSize = m_fontSize;
                // Reset baseline to current distance so the scale starts at 1.0
                // at the moment of commitment. The overlay shows the current
                // font size and no change happens until the user pinches further,
                // giving them a visible starting point to track from.
                m_pinchInitialDistance = currentDistance;
                Q_EMIT pinchingChanged(true);
                return;
            }
        } else {
            m_pinchCandidateFrames = 0;
        }

        // --- Check for scroll classification ---
        QPointF centroidDelta = currentCentroid - m_gestureInitialCentroid;
        if (qAbs(centroidDelta.y()) > ScrollMinDistancePx
            && qAbs(centroidDelta.y()) > qAbs(centroidDelta.x())) {
            // Commit to scroll mode
            m_gestureMode = GestureMode::Scrolling;
            // Reset lastY to current centroid so first scroll delta is clean
            m_twoFingerLastY = currentCentroid.y();
            return;
        }

        return;
    }

    case GestureMode::Pinching: {
        qreal scale = (m_pinchInitialDistance > 0)
            ? currentDistance / m_pinchInitialDistance : 1.0;
        // Power-curve dampening: requires more finger travel for the same font
        // delta. Exponent < 1 softens the response around scale=1.0 so small
        // finger movements no longer produce large font jumps.
        qreal dampedScale = std::pow(scale, PinchScaleExponent);
        int targetSize = qRound(m_pinchBaseFontSize * dampedScale);
        targetSize = qBound(6, targetSize, 32);

        if (targetSize != m_lastAppliedFontSize) {
            setFontSize(targetSize);
            m_lastAppliedFontSize = targetSize;
        }
        return;
    }

    case GestureMode::Scrolling: {
        qreal avgY = (p0.y() + p1.y()) / 2.0;
        qreal deltaY = avgY - m_twoFingerLastY;
        m_twoFingerLastY = avgY;

        qreal newDelta = -deltaY / m_cellHeight;
        auto touchScrollResult = TextUtil::accumulateScroll(m_touchScrollAccumulator, newDelta);
        m_touchScrollAccumulator = touchScrollResult.accumulator;
        int lines = touchScrollResult.lines;

        if (lines != 0) {
            GhosttyTerminalScrollViewport scroll = {};
            scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
            scroll.value.delta = lines;
            ghostty_terminal_scroll_viewport(m_vt->terminal(), scroll);
            m_linkScanDirty = true;
            update();
        }
        return;
    }
    }
}

void TerminalView::handleMultiTouchEnd()
{
    if (m_gestureMode == GestureMode::Pinching) {
        Q_EMIT pinchingChanged(false);
    }

    // Restore the parent SilicaFlickable so single-finger pull-down works again.
    Q_EMIT requestParentInteractive(true);

    m_gestureMode = GestureMode::Undecided;
    m_pinchCandidateFrames = 0;
    m_multiTouchActive = false;
    m_twoFingerLastY = 0;
    m_touchScrollAccumulator = 0;
    setKeepMouseGrab(false);
    setKeepTouchGrab(false);
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
        update();
        return;
    }
    if (event->timerId() == m_blinkTimerId) {
        if (m_lastInputTime.isValid() &&
            m_lastInputTime.elapsed() < BlinkPauseMs) {
            m_cursorBlinkVisible = true;
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
            update();
        }
        return;
    }
    QQuickItem::timerEvent(event);
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
        if (key == GHOSTTY_KEY_EQUAL) { Q_EMIT zoomRequested(1);  event->accept(); return; }  // Ctrl+Shift+=
        if (key == GHOSTTY_KEY_MINUS) { Q_EMIT zoomRequested(-1); event->accept(); return; }  // Ctrl+Shift+-
        if (key == GHOSTTY_KEY_F) {
            if (m_searchActive)
                closeSearch();
            else
                openSearch();
            event->accept();
            return;
        }
        if (key == GHOSTTY_KEY_ARROW_LEFT)  { Q_EMIT navigateSession(-1); event->accept(); return; }
        if (key == GHOSTTY_KEY_ARROW_RIGHT) { Q_EMIT navigateSession(1);  event->accept(); return; }
        if (key == GHOSTTY_KEY_K) { Q_EMIT toggleKeybar(); event->accept(); return; }
    }

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
    bool searchDone = false;
    for (int row = 0; row < m_searchCache.size() && !searchDone; row++) {
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
            if (m_searchMatches.size() >= MaxSearchMatches) {
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

// ---------------------------------------------------------------------------
// Link detection — OSC 8 hyperlinks + regex URL scanning
// ---------------------------------------------------------------------------

void TerminalView::refreshLinks()
{
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

    // Serialize visible viewport to flat text + QChar→cell coordinate map.
    // This mirrors Ghostty's renderer/link.zig approach: build a contiguous
    // string from the visible cells, then run the regex on it, and map match
    // offsets back to cell coordinates.
    //
    // Uses codepoint-based approach (GRAPHEMES_BUF) to build a QString directly,
    // avoiding the GRAPHEMES_UTF8 two-call pattern and ensuring QChar indices
    // from QRegularExpression match charMap positions exactly.

    QString flatText;
    flatText.reserve(static_cast<int>(m_rows * m_cols));

    // charMap[i] = {col, row} for QChar position i in flatText
    QVector<TextUtil::CellCoord> charMap;
    charMap.reserve(static_cast<int>(m_rows * m_cols));

    // Iterate visible viewport using the render state row iterator
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

        // Check if this row is soft-wrapped (don't insert \n for wrapped rows)
        GhosttyRow rawRow = 0;
        bool isWrapped = false;
        if (ghostty_render_state_row_get(iterator,
                                         GHOSTTY_RENDER_STATE_ROW_DATA_RAW,
                                         &rawRow) == GHOSTTY_SUCCESS) {
            ghostty_row_get(rawRow, GHOSTTY_ROW_DATA_WRAP, &isWrapped);
        }

        uint16_t colIdx = 0;
        while (ghostty_render_state_row_cells_next(cells)) {
            // Check wide char spacer via render state (not grid_ref)
            GhosttyCell rawCell = 0;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                    &rawCell) == GHOSTTY_SUCCESS && rawCell != 0) {
                GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
                ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &wide);
                if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
                    colIdx++;
                    continue;
                }
            }

            uint32_t graphemeLen = 0;
            if (ghostty_render_state_row_cells_get(
                    cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                    &graphemeLen) == GHOSTTY_SUCCESS && graphemeLen > 0
                    && graphemeLen <= 128) {
                uint32_t graphemes[128] = {};
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
                    flatText.append(QChar(' '));
                    charMap.append({colIdx, rowIdx});
                }
            } else {
                flatText.append(QChar(' '));
                charMap.append({colIdx, rowIdx});
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

    // Filter out URLs on the cursor's viewport row — the cursor row is
    // where user input appears, and typed URLs should not be clickable.
    // Only output (non-input) rows should have interactive links.
    // Note: OSC 8 hyperlinks (application-emitted) on the cursor row are
    // intentionally NOT filtered — they carry explicit metadata, unlike
    // regex-detected URLs which are a heuristic.
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
    for (int i = 0; i < m_currentLinks.size(); ++i) {
        const TextUtil::LinkSpan &span = m_currentLinks[i];
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
    // Fall back to regex-detected URLs
    return findRegexLinkAt(col, row);
}
