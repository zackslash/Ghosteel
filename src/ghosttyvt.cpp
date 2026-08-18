#include "ghosttyvt.h"
#include <QDebug>

#include <algorithm>

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
    // Double-create guard: if this object already owns handles (e.g. a resize
    // path re-ran create() after the shell exited), tear them down first so the
    // old GhosttyTerminal (3MB scrollback + render state) is not leaked by
    // overwriting the pointers below.
    if (m_terminal || m_renderState || m_keyEncoder || m_mouseEncoder)
        destroy();

    m_ptyWriteFn = writeFn;

    GhosttyResult res = ghostty_terminal_new(nullptr, &m_terminal, cols, rows);
    if (res != GHOSTTY_SUCCESS) {
        qWarning() << "ghostty_terminal_new failed:" << res;
        return false;
    }

    // Set scrollback limit (3MB, ~2500 lines)
    size_t scrollbackBytes = 3 * 1024 * 1024;
    ghostty_terminal_set(m_terminal,
                         GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &scrollbackBytes);

    // Enable cursor blinking (DEC mode 12 defaults to false).
    // Set current mode only; upstream marks cursor_blinking as
    // non-default-configurable, so it cannot survive a RIS reset.
    GhosttyTerminalModeConfig cursorBlink = {};
    cursorBlink.mode = GHOSTTY_MODE_CURSOR_BLINKING;
    cursorBlink.value = true;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_MODE, &cursorBlink);

    // Enable grapheme cluster mode (DEC 2027) so VS16 (U+FE0F) makes BMP emoji
    // (☀☁⛈) 2 cells wide. Matches the Ghostty app default. Using
    // MODE_DEFAULT so it survives a hard reset (RIS).
    GhosttyTerminalModeConfig graphemeCluster = {};
    graphemeCluster.mode = GHOSTTY_MODE_GRAPHEME_CLUSTER;
    graphemeCluster.value = true;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_MODE_DEFAULT, &graphemeCluster);

    // Enable Kitty Graphics Protocol image storage (32 MiB per screen)
    uint64_t kittyLimit = 32 * 1024 * 1024;
    ghostty_terminal_set(m_terminal,
                         GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &kittyLimit);

    // Enable file medium for Kitty Graphics (allows t=f file path loading)
    bool kittyFileMedium = true;
    ghostty_terminal_set(m_terminal,
                         GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &kittyFileMedium);

    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         reinterpret_cast<const void *>(writePtyCallback));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         reinterpret_cast<const void *>(titleChangedCallback));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_BELL,
                         reinterpret_cast<const void *>(bellCallback));

    res = ghostty_render_state_new(nullptr, &m_renderState);
    if (res != GHOSTTY_SUCCESS) {
        qWarning() << "ghostty_render_state_new failed:" << res;
        destroy();
        return false;
    }

    res = ghostty_key_encoder_new(nullptr, &m_keyEncoder);
    if (res != GHOSTTY_SUCCESS) {
        qWarning() << "ghostty_key_encoder_new failed:" << res;
        destroy();
        return false;
    }

    ghostty_key_encoder_setopt_from_terminal(m_keyEncoder, m_terminal);

    res = ghostty_mouse_encoder_new(nullptr, &m_mouseEncoder);
    if (res != GHOSTTY_SUCCESS) {
        qWarning() << "ghostty_mouse_encoder_new failed:" << res;
        destroy();
        return false;
    }

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

        // Fast-path: skip both OSC scanners when idle and byte isn't ESC.
        // Avoids ~99% of switch evaluations for normal terminal output.
        if (c != 0x1b && m_osc777State == OSC777_IDLE && m_osc52State == OSC52_IDLE)
            continue;

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

bool GhosttyVt::isWideSpacerTailCell(GhosttyCell cell)
{
    if (cell == 0)
        return false;
    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    return wide == GHOSTTY_CELL_WIDE_SPACER_TAIL;
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

VtSearchText GhosttyVt::extractSearchText()
{
    VtSearchText out;
    if (!m_terminal)
        return out;

    size_t totalRows = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_TOTAL_ROWS, &totalRows);
    uint16_t cols = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols);

    if (totalRows == 0 || cols == 0)
        return out;

    const int colsInt = static_cast<int>(cols);
    out.lines.reserve(static_cast<int>(totalRows));
    out.mapping.reserve(static_cast<int>(totalRows));
    uint32_t graphemeBuf[128];

    // Single pass over the grid: build the row text and, simultaneously, the
    // cell->QChar mapping. Wide chars take 2 cells; supplementary-plane
    // codepoints (emoji) expand to 2 QChars (a surrogate pair). Each cell's
    // mapping records the QChar offset where its grapheme cluster starts.
    for (size_t row = 0; row < totalRows; row++) {
        QString line;
        line.reserve(cols);
        QVector<int> rowMapping(colsInt, 0);
        int charIdx = 0;

        for (uint16_t col = 0; col < cols; col++) {
            rowMapping[col] = charIdx;

            GhosttyPoint point = {};
            point.tag = GHOSTTY_POINT_TAG_SCREEN;
            point.value.coordinate.x = col;
            point.value.coordinate.y = static_cast<uint32_t>(row);

            GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
            if (ghostty_terminal_grid_ref(m_terminal, point, &ref) != GHOSTTY_SUCCESS) {
                line += QLatin1Char(' ');
                if (charIdx < line.size())
                    charIdx = std::min(charIdx + 1, line.size());
                continue;
            }

            // Wide-char spacer — no text content; charIdx stays put. Mark the
            // cell with -1 so consumers can distinguish spacers from real
            // cells: a spacer must never be treated as the start of a match.
            // Before this mapping, a spacer aliased the next cell's offset
            // (charIdx had already advanced past the head cell's grapheme),
            // so the reverse-map could land one cell early on the spacer.
            // The spacer's cell must still be counted when a match spans it.
            GhosttyCell cell = 0;
            if (ghostty_grid_ref_cell(&ref, &cell) == GHOSTTY_SUCCESS
                    && isWideSpacerCell(cell)) {
                rowMapping[col] = -1;
                continue;
            }

            size_t graphemeLen = 0;
            int advance = 1; // blank cell — the row text appends one space
            if (ghostty_grid_ref_graphemes(&ref, graphemeBuf, 128, &graphemeLen)
                    == GHOSTTY_SUCCESS && graphemeLen > 0) {
                line += QString::fromUcs4(graphemeBuf, graphemeLen);
                advance = 0;
                for (size_t g = 0; g < graphemeLen; ++g)
                    advance += (graphemeBuf[g] > 0xFFFF) ? 2 : 1;
            } else {
                line += QLatin1Char(' ');
            }
            if (charIdx < line.size())
                charIdx = std::min(charIdx + advance, line.size());
        }

        out.lines.append(line);
        out.mapping.append(rowMapping);
    }

    m_searchTextDirty = false;
    return out;
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

    // Using grid_ref API — the formatter API passes GhosttyFormatterTerminalOptions
    // by value, which corrupts on 32-bit x86 (i486 emulator target).
    QByteArray result;
    result.reserve(static_cast<int>(totalRows * outCols * 4));
    uint32_t graphemeBuf[128];
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

        while (!line.isEmpty() && line.endsWith(' '))
            line.chop(1);

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
            lineBuf.append(line);
        } else {
            if (!lineBuf.isEmpty()) {
                result.append(lineBuf);
                result.append("\r\n");
            }
            lineBuf = line;
        }
    }

    if (!lineBuf.isEmpty()) {
        result.append(lineBuf);
        result.append("\r\n");
    }

    while (result.endsWith("\r\n"))
        result.chop(2);

    if (!result.isEmpty()) {
        uint16_t cursorX = 0;
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &cursorX);

        int lastNl = result.lastIndexOf('\n');
        QByteArray lastLine = (lastNl >= 0) ? result.mid(lastNl + 1) : result;
        QByteArray trimmed = lastLine;
        while (trimmed.endsWith('\r'))
            trimmed.chop(1);

        if (cursorX > 0 && cursorX >= terminalStringWidth(QString::fromUtf8(trimmed))) {
            if (lastNl >= 0)
                result.resize(lastNl);
            else
                result.clear();

            while (result.endsWith("\r\n"))
                result.chop(2);
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
    // double-skip: a full-width line sets a pending-wrap flag that \r\n
    // would resolve twice (extra blank line). Lines still full-width after
    // trimming use \r instead — the wrap resolves on \r, one line break.
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
