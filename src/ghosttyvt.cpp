#include "ghosttyvt.h"
#include <QDebug>

#include "terminalwidth.h"

GhosttyVt::GhosttyVt(QObject *parent)
    : QObject(parent)
{
}

GhosttyVt::~GhosttyVt()
{
    destroy();
}

bool GhosttyVt::create(uint16_t cols, uint16_t rows, PtyWriteFn writeFn)
{
    m_ptyWriteFn = writeFn;

    // Options passed by pointer to work around Zig i386 struct-by-value
    // ABI bug. See ghostty/src/terminal/c/terminal.zig new_ptr() comment.
    GhosttyTerminalOptions opts = {};
    opts.cols = cols;
    opts.rows = rows;
    opts.max_scrollback = 3 * 1024 * 1024; // 3MB (~2500 lines)
    GhosttyResult res = ghostty_terminal_new(nullptr, &m_terminal, &opts);
    if (res != GHOSTTY_SUCCESS) {
        qWarning() << "ghostty_terminal_new failed:" << res;
        return false;
    }

    // Enable cursor blinking by default (Ghostty mode 12 defaults to false)
    ghostty_terminal_mode_set(m_terminal, GHOSTTY_MODE_CURSOR_BLINKING, true);

    // Enable grapheme cluster mode (DEC 2027) so VS16 (U+FE0F) makes BMP emoji
    // (☀☁⛈) 2 cells wide. Matches the Ghostty app default.
    // NOTE: a hard reset (`reset` / ESC c) clears this; the C API has no
    // default-modes field, so reset-resilience requires an upstream change.
    ghostty_terminal_mode_set(m_terminal, GHOSTTY_MODE_GRAPHEME_CLUSTER, true);

    // Enable Kitty Graphics Protocol image storage (32 MiB per screen)
    uint64_t kittyLimit = 32 * 1024 * 1024;
    ghostty_terminal_set(m_terminal,
                         GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &kittyLimit);

    // Enable file medium for Kitty Graphics (allows t=f file path loading)
    bool kittyFileMedium = true;
    ghostty_terminal_set(m_terminal,
                         GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &kittyFileMedium);

    // Set callbacks
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         reinterpret_cast<const void *>(writePtyCallback));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         reinterpret_cast<const void *>(titleChangedCallback));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_BELL,
                         reinterpret_cast<const void *>(bellCallback));

    // Create render state
    res = ghostty_render_state_new(nullptr, &m_renderState);
    if (res != GHOSTTY_SUCCESS) {
        qWarning() << "ghostty_render_state_new failed:" << res;
        destroy();
        return false;
    }

    // Create key encoder
    res = ghostty_key_encoder_new(nullptr, &m_keyEncoder);
    if (res != GHOSTTY_SUCCESS) {
        qWarning() << "ghostty_key_encoder_new failed:" << res;
        destroy();
        return false;
    }

    // Sync encoder options from terminal
    ghostty_key_encoder_setopt_from_terminal(m_keyEncoder, m_terminal);

    // Create mouse encoder
    res = ghostty_mouse_encoder_new(nullptr, &m_mouseEncoder);
    if (res != GHOSTTY_SUCCESS) {
        qWarning() << "ghostty_mouse_encoder_new failed:" << res;
        destroy();
        return false;
    }

    // Sync mouse encoder options from terminal (tracking mode, format)
    ghostty_mouse_encoder_setopt_from_terminal(m_mouseEncoder, m_terminal);

    return true;
}

void GhosttyVt::destroy()
{
    // Reset OSC 777 scanner to prevent cross-session data leakage
    m_osc777State = OSC777_IDLE;
    m_osc777NotifyIdx = 0;
    m_osc777Title.clear();
    m_osc777Body.clear();

    // Reset OSC 52 scanner
    m_osc52State = OSC52_IDLE;
    m_osc52Kind.clear();
    m_osc52Data.clear();

    if (m_mouseEncoder) {
        ghostty_mouse_encoder_free(m_mouseEncoder);
        m_mouseEncoder = nullptr;
    }
    if (m_keyEncoder) {
        ghostty_key_encoder_free(m_keyEncoder);
        m_keyEncoder = nullptr;
    }
    if (m_renderState) {
        ghostty_render_state_free(m_renderState);
        m_renderState = nullptr;
    }
    if (m_terminal) {
        ghostty_terminal_free(m_terminal);
        m_terminal = nullptr;
    }
}

void GhosttyVt::vtWrite(const uint8_t *data, size_t len)
{
    // Scan for OSC 777 desktop notifications: ESC]777;notify;title;body BEL
    // Scan for OSC 52 clipboard: ESC]52;{kind};{base64} BEL/ST
    // Both run alongside the terminal parser to intercept escape sequences.
    static const char notify[] = "notify;";

    // --- OSC 52 clipboard scanner helpers (defined once, used per byte) ---
    auto isBase64Char = [](uint8_t ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=' ||
               ch == '?' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    };
    auto emitOsc52 = [&]() {
        if (m_osc52Kind == "c" || m_osc52Kind == "C") {
            if (m_osc52Data == "?")
                Q_EMIT clipboardReadRequest(QString::fromUtf8(m_osc52Kind));
            else
                Q_EMIT clipboardWriteRequest(m_osc52Data, QString::fromUtf8(m_osc52Kind));
        }
        m_osc52State = OSC52_IDLE;
    };

    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];

        // --- OSC 777 scanner ---
        switch (m_osc777State) {
        case OSC777_IDLE:
            if (c == 0x1b) m_osc777State = OSC777_ESC;
            break;
        case OSC777_ESC:
            m_osc777State = (c == ']') ? OSC777_BRACKET : OSC777_IDLE;
            break;
        case OSC777_BRACKET:
            m_osc777State = (c == '7') ? OSC777_7A : OSC777_IDLE;
            break;
        case OSC777_7A:
            m_osc777State = (c == '7') ? OSC777_7B : OSC777_IDLE;
            break;
        case OSC777_7B:
            m_osc777State = (c == '7') ? OSC777_SEMI1 : OSC777_IDLE;
            break;
        case OSC777_SEMI1:
            if (c == ';') {
                m_osc777NotifyIdx = 0;
                m_osc777State = OSC777_NOTIFY;
            } else {
                m_osc777State = OSC777_IDLE;
            }
            break;
        case OSC777_NOTIFY:
            if (c == notify[m_osc777NotifyIdx]) {
                m_osc777NotifyIdx++;
                if (notify[m_osc777NotifyIdx] == '\0') {
                    m_osc777Title.clear();
                    m_osc777State = OSC777_TITLE;
                }
            } else {
                m_osc777State = OSC777_IDLE;
            }
            break;
        case OSC777_TITLE:
            if (c == ';') {
                m_osc777Body.clear();
                m_osc777State = OSC777_BODY;
            } else if (c == 0x07 || c == 0x1b) {
                // Title only, no body — treat as notification with empty body
                if (!m_osc777Title.isEmpty())
                    Q_EMIT desktopNotification(QString::fromUtf8(m_osc777Title), QString());
                m_osc777State = (c == 0x1b) ? OSC777_ESC : OSC777_IDLE;
            } else {
                if (m_osc777Title.size() < 512)
                    m_osc777Title.append(static_cast<char>(c));
            }
            break;
        case OSC777_BODY:
            if (c == 0x07 || c == 0x1b) {
                if (!m_osc777Title.isEmpty())
                    Q_EMIT desktopNotification(QString::fromUtf8(m_osc777Title),
                                                QString::fromUtf8(m_osc777Body));
                m_osc777State = (c == 0x1b) ? OSC777_ESC : OSC777_IDLE;
            } else {
                if (m_osc777Body.size() < 2048)
                    m_osc777Body.append(static_cast<char>(c));
            }
            break;
        }

        // --- OSC 52 clipboard scanner ---
        switch (m_osc52State) {
        case OSC52_IDLE:
            if (c == 0x1b) m_osc52State = OSC52_ESC;
            break;
        case OSC52_ESC:
            m_osc52State = (c == ']') ? OSC52_BRACKET : OSC52_IDLE;
            break;
        case OSC52_BRACKET:
            m_osc52State = (c == '5') ? OSC52_FIVE : OSC52_IDLE;
            break;
        case OSC52_FIVE:
            m_osc52State = (c == '2') ? OSC52_TWO : OSC52_IDLE;
            break;
        case OSC52_TWO:
            m_osc52State = (c == ';') ? OSC52_SEMI : OSC52_IDLE;
            break;
        case OSC52_SEMI:
            m_osc52Kind.clear();
            m_osc52Data.clear();
            if (c == ';') {
                m_osc52Kind.append('c');
                m_osc52State = OSC52_DATA;
            } else if (c == 0x07 || c == 0x1b) {
                m_osc52State = (c == 0x1b) ? OSC52_ESC : OSC52_IDLE;
            } else {
                m_osc52Kind.append(static_cast<char>(c));
                m_osc52State = OSC52_KIND;
            }
            break;
        case OSC52_KIND:
            if (c == ';') {
                m_osc52State = OSC52_DATA;
            } else if (c == 0x07 || c == 0x1b) {
                m_osc52State = (c == 0x1b) ? OSC52_ESC : OSC52_IDLE;
            } else {
                if (m_osc52Kind.size() < MaxOsc52KindLen)
                    m_osc52Kind.append(static_cast<char>(c));
            }
            break;
        case OSC52_DATA:
            if (c == 0x07) {
                emitOsc52();
            } else if (c == 0x1b) {
                // Could be ST terminator (ESC \)
                m_osc52State = OSC52_ST_ESC;
            } else if (m_osc52Data.size() < MaxOsc52DataLen && isBase64Char(c)) {
                m_osc52Data.append(static_cast<char>(c));
            }
            break;
        case OSC52_ST_ESC:
            if (c == '\\') {
                emitOsc52();
            } else {
                // Not ST — ESC is not valid base64, drop it and resume
                // accumulating the following char if it is valid base64.
                if (m_osc52Data.size() < MaxOsc52DataLen && isBase64Char(c))
                    m_osc52Data.append(static_cast<char>(c));
                m_osc52State = OSC52_DATA;
            }
            break;
        }
    }

    if (m_terminal) {
        ghostty_terminal_vt_write(m_terminal, data, len);
        m_needsEncoderSync = true; // Terminal modes may have changed
        m_searchTextDirty = true;
    }
}

void GhosttyVt::updateRenderState()
{
    if (m_renderState && m_terminal) {
        ghostty_render_state_update(m_renderState, m_terminal);
        // Only re-sync encoder options when terminal modes have changed
        // (e.g., mouse tracking toggled, keypad mode changed).
        // This avoids redundant syscalls every frame.
        if (m_needsEncoderSync) {
            ghostty_key_encoder_setopt_from_terminal(m_keyEncoder, m_terminal);
            ghostty_mouse_encoder_setopt_from_terminal(m_mouseEncoder, m_terminal);
            m_needsEncoderSync = false;
        }
    }
}

QByteArray GhosttyVt::encodeKeyEvent(GhosttyKey key, GhosttyKeyAction action,
                                     GhosttyMods mods, const char *utf8,
                                     size_t utf8Len)
{
    if (!m_keyEncoder)
        return {};

    GhosttyKeyEvent event;
    if (ghostty_key_event_new(nullptr, &event) != GHOSTTY_SUCCESS)
        return {};

    ghostty_key_event_set_action(event, action);
    ghostty_key_event_set_key(event, key);
    ghostty_key_event_set_mods(event, mods);
    if (utf8 && utf8Len > 0)
        ghostty_key_event_set_utf8(event, utf8, utf8Len);

    // Query required size
    size_t required = 0;
    ghostty_key_encoder_encode(m_keyEncoder, event, nullptr, 0, &required);

    QByteArray result;
    if (required > 0) {
        result.resize(required);
        size_t written = 0;
        ghostty_key_encoder_encode(m_keyEncoder, event, result.data(),
                                   result.size(), &written);
        result.resize(written);
    }

    ghostty_key_event_free(event);
    return result;
}

bool GhosttyVt::isWideSpacerCell(GhosttyCell cell)
{
    if (cell == 0)
        return false;
    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    return wide == GHOSTTY_CELL_WIDE_SPACER_TAIL
        || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD;
}

bool GhosttyVt::isWideCharSpacer(GhosttyTerminal terminal, uint16_t col, uint32_t row)
{
    if (!terminal)
        return false;
    GhosttyPoint point = {};
    point.tag = GHOSTTY_POINT_TAG_SCREEN;
    point.value.coordinate.x = col;
    point.value.coordinate.y = row;
    GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    if (ghostty_terminal_grid_ref(terminal, point, &ref) != GHOSTTY_SUCCESS)
        return false;
    GhosttyCell cell = 0;
    if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS)
        return false;
    return isWideSpacerCell(cell);
}

QString GhosttyVt::getHyperlinkAt(uint16_t col, uint32_t row) const
{
    if (!m_terminal)
        return {};

    GhosttyPoint point = {};
    point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    point.value.coordinate.x = col;
    point.value.coordinate.y = row;

    GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    if (ghostty_terminal_grid_ref(m_terminal, point, &ref) != GHOSTTY_SUCCESS)
        return {};

    // Fast boolean check first
    GhosttyCell cell = 0;
    if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS || cell == 0)
        return {};
    bool hasLink = false;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_HAS_HYPERLINK, &hasLink);
    if (!hasLink)
        return {};

    // Query required buffer size — returns GHOSTTY_OUT_OF_SPACE (not SUCCESS)
    // when a hyperlink exists, because the nullptr/0 call can't fit the URI.
    size_t requiredLen = 0;
    GhosttyResult rc = ghostty_grid_ref_hyperlink_uri(&ref, nullptr, 0, &requiredLen);
    if (requiredLen == 0)
        return {};  // No hyperlink on this cell
    if (rc != GHOSTTY_OUT_OF_SPACE)
        return {};  // Actual error

    // Read URI
    QByteArray buf(static_cast<int>(requiredLen), '\0');
    rc = ghostty_grid_ref_hyperlink_uri(&ref, reinterpret_cast<uint8_t*>(buf.data()),
                                         buf.size(), &requiredLen);
    if (rc != GHOSTTY_SUCCESS)
        return {};

    return QString::fromUtf8(buf.data(), static_cast<int>(requiredLen));
}

bool GhosttyVt::isMouseTracking() const
{
    if (!m_terminal)
        return false;
    bool result = false;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &result);
    return result;
}

QByteArray GhosttyVt::encodeMouseEvent(GhosttyMouseAction action,
                                        GhosttyMouseButton button,
                                        float x, float y, GhosttyMods mods)
{
    if (!m_mouseEncoder)
        return {};

    GhosttyMouseEvent event;
    if (ghostty_mouse_event_new(nullptr, &event) != GHOSTTY_SUCCESS)
        return {};

    ghostty_mouse_event_set_action(event, action);
    if (button == GHOSTTY_MOUSE_BUTTON_UNKNOWN)
        ghostty_mouse_event_clear_button(event);
    else
        ghostty_mouse_event_set_button(event, button);
    ghostty_mouse_event_set_mods(event, mods);
    GhosttyMousePosition pos = {x, y};
    ghostty_mouse_event_set_position(event, pos);

    // Query required size
    size_t required = 0;
    ghostty_mouse_encoder_encode(m_mouseEncoder, event, nullptr, 0, &required);

    QByteArray result;
    if (required > 0) {
        result.resize(required);
        size_t written = 0;
        ghostty_mouse_encoder_encode(m_mouseEncoder, event, result.data(),
                                     result.size(), &written);
        result.resize(written);
    }

    ghostty_mouse_event_free(event);
    return result;
}

void GhosttyVt::updateMouseEncoderSize(uint32_t screenW, uint32_t screenH,
                                        uint32_t cellW, uint32_t cellH,
                                        uint32_t paddingTop)
{
    if (!m_mouseEncoder)
        return;
    GhosttyMouseEncoderSize size = {};
    size.size = sizeof(GhosttyMouseEncoderSize);
    size.screen_width = screenW;
    size.screen_height = screenH;
    size.cell_width = cellW;
    size.cell_height = cellH;
    size.padding_top = paddingTop;
    size.padding_bottom = 0;
    size.padding_left = 0;
    size.padding_right = 0;
    ghostty_mouse_encoder_setopt(m_mouseEncoder,
                                 GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
}

void GhosttyVt::setMouseButtonPressed(bool pressed)
{
    if (!m_mouseEncoder)
        return;
    ghostty_mouse_encoder_setopt(m_mouseEncoder,
                                 GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                                 &pressed);
}

void GhosttyVt::writePtyCallback(GhosttyTerminal, void *ud,
                                 const uint8_t *data, size_t len)
{
    auto *self = static_cast<GhosttyVt *>(ud);
    if (self->m_ptyWriteFn)
        self->m_ptyWriteFn(reinterpret_cast<const char *>(data), len);
}

void GhosttyVt::titleChangedCallback(GhosttyTerminal t, void *ud)
{
    auto *self = static_cast<GhosttyVt *>(ud);
    GhosttyString title;
    if (ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_TITLE, &title) == GHOSTTY_SUCCESS) {
        Q_EMIT self->titleChanged(
            QString::fromUtf8(reinterpret_cast<const char *>(title.ptr), title.len));
    }
}

void GhosttyVt::bellCallback(GhosttyTerminal, void *ud)
{
    auto *self = static_cast<GhosttyVt *>(ud);
    Q_EMIT self->bell();
}

QStringList GhosttyVt::extractSearchText()
{
    if (!m_terminal)
        return {};

    size_t totalRows = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_TOTAL_ROWS, &totalRows);
    uint16_t cols = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols);

    if (totalRows == 0 || cols == 0)
        return {};

    QStringList result;
    result.reserve(static_cast<int>(totalRows));
    m_wideSpacerCache.clear();
    m_wideSpacerCache.reserve(static_cast<int>(totalRows));
    uint32_t graphemeBuf[128];

    for (size_t row = 0; row < totalRows; row++) {
        QString line;
        line.reserve(cols);
        QVector<bool> rowSpacers;
        rowSpacers.resize(static_cast<int>(cols));
        rowSpacers.fill(false);

        for (uint16_t col = 0; col < cols; col++) {
            GhosttyPoint point = {};
            point.tag = GHOSTTY_POINT_TAG_SCREEN;
            point.value.coordinate.x = col;
            point.value.coordinate.y = static_cast<uint32_t>(row);

            GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
            if (ghostty_terminal_grid_ref(m_terminal, point, &ref) != GHOSTTY_SUCCESS) {
                line += QLatin1Char(' ');
                continue;
            }

            // Reuse the grid ref above instead of isWideCharSpacer()'s second grid_ref.
            GhosttyCell cell = 0;
            if (ghostty_grid_ref_cell(&ref, &cell) == GHOSTTY_SUCCESS
                    && isWideSpacerCell(cell)) {
                rowSpacers[static_cast<int>(col)] = true;
                continue;
            }

            size_t graphemeLen = 0;
            if (ghostty_grid_ref_graphemes(&ref, graphemeBuf, 128, &graphemeLen)
                    == GHOSTTY_SUCCESS && graphemeLen > 0) {
                line += QString::fromUcs4(graphemeBuf, graphemeLen);
            } else {
                line += QLatin1Char(' ');
            }
        }

        m_wideSpacerCache.append(rowSpacers);
        result.append(line);
    }

    m_searchTextDirty = false;
    return result;
}

QByteArray GhosttyVt::exportScrollback(uint16_t &outCols, uint16_t &outRows) const
{
    if (!m_terminal)
        return {};

    // Skip alternate screen — TUI apps (vim, htop) use alternate screen
    // with no meaningful scrollback to persist.
    GhosttyTerminalScreen screen;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
    if (screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE)
        return {};

    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLS, &outCols);
    size_t totalRows = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_TOTAL_ROWS, &totalRows);
    outRows = static_cast<uint16_t>(totalRows);

    if (outCols == 0 || totalRows == 0)
        return {};

    // Using grid_ref API — the formatter API crashes on Zig null-unwrap with empty page lists.
    QByteArray result;
    // Reserve ~4 bytes per cell to avoid reallocations for UTF-8 content (CJK, emoji)
    result.reserve(static_cast<int>(totalRows * outCols * 4));
    uint32_t graphemeBuf[128];

    // Accumulate into a logical line buffer. Soft-wrapped continuation rows
    // are merged into the current line without a newline. Only when we reach
    // a non-continuation row do we flush the previous line with \r\n.
    QByteArray lineBuf;

    for (size_t row = 0; row < totalRows; row++) {
        QByteArray line;
        line.reserve(outCols);

        for (uint16_t col = 0; col < outCols; col++) {
            GhosttyPoint point = {};
            point.tag = GHOSTTY_POINT_TAG_SCREEN;
            point.value.coordinate.x = col;
            point.value.coordinate.y = static_cast<uint32_t>(row);

            GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
            if (ghostty_terminal_grid_ref(m_terminal, point, &ref) != GHOSTTY_SUCCESS) {
                line.append(' ');
                continue;
            }

            // Skip wide character spacer cells — they have no text content
            // but would otherwise be exported as phantom spaces.
            // Reuse the grid ref above instead of isWideCharSpacer()'s second grid_ref.
            GhosttyCell cell = 0;
            if (ghostty_grid_ref_cell(&ref, &cell) == GHOSTTY_SUCCESS
                    && isWideSpacerCell(cell))
                continue;

            size_t graphemeLen = 0;
            if (ghostty_grid_ref_graphemes(&ref, graphemeBuf, 128, &graphemeLen)
                    == GHOSTTY_SUCCESS && graphemeLen > 0) {
                line.append(QString::fromUcs4(graphemeBuf, graphemeLen).toUtf8());
            } else {
                line.append(' ');
            }
        }

        // Trim trailing spaces to avoid pending-wrap + newline double-skip on replay.
        while (!line.isEmpty() && line.endsWith(' '))
            line.chop(1);

        // Check if this row is a soft-wrap continuation of the previous row.
        bool isContinuation = false;
        if (row > 0) {
            GhosttyPoint firstCol = {};
            firstCol.tag = GHOSTTY_POINT_TAG_SCREEN;
            firstCol.value.coordinate.x = 0;
            firstCol.value.coordinate.y = static_cast<uint32_t>(row);
            GhosttyGridRef rowRef = GHOSTTY_INIT_SIZED(GhosttyGridRef);
            if (ghostty_terminal_grid_ref(m_terminal, firstCol, &rowRef) == GHOSTTY_SUCCESS) {
                GhosttyRow rowHandle = 0;
                if (ghostty_grid_ref_row(&rowRef, &rowHandle) == GHOSTTY_SUCCESS && rowHandle != 0) {
                    bool wc = false;
                    if (ghostty_row_get(rowHandle, GHOSTTY_ROW_DATA_WRAP_CONTINUATION, &wc) == GHOSTTY_SUCCESS)
                        isContinuation = wc;
                }
            }
        }

        if (isContinuation) {
            // Soft-wrapped continuation — merge into current line buffer.
            // Don't trim: the original line content needs to stay intact
            // so it re-wraps at the same point on replay.
            lineBuf.append(line);
        } else {
            // New logical line — flush the previous line buffer with \r\n,
            // then start accumulating this row.
            if (!lineBuf.isEmpty()) {
                result.append(lineBuf);
                result.append("\r\n");
            }
            lineBuf = line;
        }
    }

    // Flush the last line
    if (!lineBuf.isEmpty()) {
        result.append(lineBuf);
        result.append("\r\n");
    }

    // Strip trailing empty lines — these are blank viewport rows below the
    // last real content. Without this, restoring creates a screen-height
    // gap of whitespace before the shell prompt.
    while (result.endsWith("\r\n"))
        result.chop(2);

    // Strip the active prompt line — when the shell is waiting for input,
    // the last line is just the prompt (e.g. "[user@host ~]$"). On restore,
    // the shell prints a fresh prompt, so exporting the old one causes
    // duplicate prompts to accumulate across restarts.
    // Detection: if the cursor is at the end of the last line with nothing
    // typed after it, the line is just a prompt — safe to strip.
    if (!result.isEmpty()) {
        uint16_t cursorX = 0;
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &cursorX);

        int lastNl = result.lastIndexOf('\n');
        QByteArray lastLine = (lastNl >= 0) ? result.mid(lastNl + 1) : result;
        // Trim trailing \r for length comparison
        QByteArray trimmed = lastLine;
        while (trimmed.endsWith('\r'))
            trimmed.chop(1);

        // Compare cursor column (cells) against the prompt's display width,
        // not its UTF-8 byte count — multibyte prompts otherwise defeat the
        // duplicate-prompt strip.
        if (cursorX > 0 && cursorX >= terminalStringWidth(QString::fromUtf8(trimmed))) {
            // Cursor is at or past end of last line — it's an un-typed prompt.
            // Strip the last line entirely.
            if (lastNl >= 0)
                result.resize(lastNl);
            else
                result.clear();

            // Re-strip any blank lines exposed by the removal
            while (result.endsWith("\r\n"))
                result.chop(2);
            // Also strip lone \r left when prompt was on a line after blank rows
            while (result.endsWith('\r'))
                result.chop(1);
        }
    }

    QByteArray header = QStringLiteral("GHOSTTY_SCROLLBACK_V1\nCOLS=%1\nROWS=%2\n\n")
                            .arg(outCols).arg(totalRows).toUtf8();
    return header + result;
}

void GhosttyVt::restoreScrollback(const QByteArray &data, uint16_t actualCols)
{
    if (!m_terminal || data.isEmpty())
        return;

    // Parse header: GHOSTTY_SCROLLBACK_V1\nCOLS=X\nROWS=Y\n\n<text data>
    int headerEnd = data.indexOf("\n\n");
    if (headerEnd < 0)
        return;

    QByteArray header = data.left(headerEnd);
    QByteArray textData = data.mid(headerEnd + 2);

    if (textData.isEmpty())
        return;

    uint16_t savedCols = 0;
    for (const QByteArray &line : header.split('\n')) {
        if (line.startsWith("COLS="))
            savedCols = line.mid(5).toUShort();
    }

    if (savedCols == 0)
        return;

    // Replay text as VT input. Trim trailing spaces to avoid pending-wrap
    // double-skip: when a line fills the full terminal width, the cursor
    // reaches the right margin and sets a pending-wrap flag. A subsequent
    // \r\n then resolves the wrap (moving down) AND the \n moves down again,
    // producing an extra blank line. Trimming prevents the line from reaching
    // the full width.
    //
    // For lines that are STILL full-width after trimming (rare), use \r
    // instead of \r\n — the pending wrap resolves on \r, giving one line break.
    QByteArray replayData;
    int targetCols = static_cast<int>(actualCols);
    replayData.reserve(textData.size());
    int lineStart = 0;
    for (int i = 0; i <= textData.size(); i++) {
        if (i == textData.size() || textData[i] == '\n') {
            QByteArray line = textData.mid(lineStart, i - lineStart);
            if (!line.isEmpty() && line.endsWith('\r'))
                line.chop(1);
            while (!line.isEmpty() && line.endsWith(' '))
                line.chop(1);

            if (savedCols != actualCols && line.size() > targetCols) {
                // Line is wider than the terminal — re-wrap by splitting at
                // column boundaries. Convert to QString for safe character
                // splitting (byte boundary splits would corrupt UTF-8).
                QString str = QString::fromUtf8(line);
                while (!str.isEmpty()) {
                    QString segment = fitToDisplayWidth(str, targetCols);
                    // Ensure forward progress even if targetCols is zero
                    if (segment.isEmpty()) {
                        segment = str.left(1);
                    }
                    int width = terminalStringWidth(segment);
                    replayData.append(segment.toUtf8());
                    // If segment fills full column width, pending wrap handles
                    // the line break — just send \r to position at col 0.
                    // Use display width (not byte count) for CJK correctness.
                    if (width >= targetCols)
                        replayData.append("\r");
                    else
                        replayData.append("\r\n");
                    str = str.mid(segment.size());
                }
            } else {
                replayData.append(line);
                // If line fills full column width, pending wrap is set — use \r only.
                // Compute display width (not byte count) for CJK correctness.
                int lineDisplayW = terminalStringWidth(QString::fromUtf8(line));
                if (lineDisplayW > 0 && lineDisplayW % targetCols == 0)
                    replayData.append("\r");
                else
                    replayData.append("\r\n");
            }
            lineStart = i + 1;
        }
    }

    ghostty_terminal_vt_write(m_terminal,
                              reinterpret_cast<const uint8_t *>(replayData.constData()),
                              replayData.size());
}
