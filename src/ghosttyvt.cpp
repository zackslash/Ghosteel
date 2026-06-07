#include "ghosttyvt.h"
#include <QDebug>
#include <QStringList>
#include <QByteArray>

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
    // This runs alongside the terminal parser to intercept notification sequences.
    static const char notify[] = "notify;";
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
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
    uint32_t graphemeBuf[128];

    for (size_t row = 0; row < totalRows; row++) {
        QString line;
        line.reserve(cols);

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

            size_t graphemeLen = 0;
            if (ghostty_grid_ref_graphemes(&ref, graphemeBuf, 128, &graphemeLen)
                    == GHOSTTY_SUCCESS && graphemeLen > 0) {
                line += QString::fromUcs4(graphemeBuf, graphemeLen);
            } else {
                line += QLatin1Char(' ');
            }
        }

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

    // Get terminal dimensions
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLS, &outCols);
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_ROWS, &outRows);

    if (outCols == 0 || outRows == 0)
        return {};

    // Use VT formatter to serialize full terminal content (scrollback + active)
    GhosttyFormatterTerminalOptions fmtOpts = GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
    // Workaround: QObject re-defines `emit` as a macro after the header's #undef
    #ifdef emit
    #undef emit
    #endif
    fmtOpts.emit = GHOSTTY_FORMATTER_FORMAT_VT;
    fmtOpts.trim = false;
    fmtOpts.unwrap = false;

    GhosttyFormatter formatter = nullptr;
    GhosttyResult res = ghostty_formatter_terminal_new(nullptr, &formatter, m_terminal, fmtOpts);
    if (res != GHOSTTY_SUCCESS || !formatter) {
        qWarning() << "exportScrollback: formatter_terminal_new failed:" << res;
        return {};
    }

    uint8_t *buf = nullptr;
    size_t len = 0;
    res = ghostty_formatter_format_alloc(formatter, nullptr, &buf, &len);
    ghostty_formatter_free(formatter);

    if (res != GHOSTTY_SUCCESS || !buf || len == 0) {
        qWarning() << "exportScrollback: format_alloc failed:" << res << "len=" << len;
        return {};
    }

    // Build file: header + VT data
    QByteArray header = QStringLiteral("GHOSTTY_SCROLLBACK_V1\nCOLS=%1\nROWS=%2\n\n")
                            .arg(outCols).arg(outRows).toUtf8();
    QByteArray vtData(reinterpret_cast<const char *>(buf), static_cast<int>(len));
    ghostty_free(nullptr, buf, len);

    return header + vtData;
}

void GhosttyVt::restoreScrollback(const QByteArray &data, uint16_t actualCols, uint16_t actualRows)
{
    if (!m_terminal || data.isEmpty())
        return;

    qDebug() << "restoreScrollback: data size=" << data.size() << "actual=" << actualCols << "x" << actualRows;

    // Parse header: GHOSTTY_SCROLLBACK_V1\nCOLS=X\nROWS=Y\n\n<VT data>
    int headerEnd = data.indexOf("\n\n");
    if (headerEnd < 0) {
        qWarning() << "restoreScrollback: no header found (no double newline)";
        return;
    }

    QByteArray header = data.left(headerEnd);
    QByteArray vtData = data.mid(headerEnd + 2);

    qDebug() << "restoreScrollback: header=" << header << "vtData size=" << vtData.size();

    if (vtData.isEmpty())
        return;

    // Extract saved dimensions from header
    uint16_t savedCols = 0, savedRows = 0;
    for (const QByteArray &line : header.split('\n')) {
        if (line.startsWith("COLS="))
            savedCols = line.mid(5).toUShort();
        else if (line.startsWith("ROWS="))
            savedRows = line.mid(5).toUShort();
    }

    qDebug() << "restoreScrollback: saved=" << savedCols << "x" << savedRows;

    if (savedCols == 0 || savedRows == 0)
        return;

    // Resize to saved dimensions first (so VT sequences have correct column widths)
    if (savedCols != actualCols || savedRows != actualRows) {
        qDebug() << "restoreScrollback: resizing to saved dims";
        ghostty_terminal_resize(m_terminal, savedCols, savedRows, 0, 0);
    }

    // Replay VT data to reconstruct scrollback content
    qDebug() << "restoreScrollback: writing" << vtData.size() << "bytes of VT data";
    ghostty_terminal_vt_write(m_terminal,
                              reinterpret_cast<const uint8_t *>(vtData.constData()),
                              vtData.size());

    // Check result
    size_t totalRows = 0, scrollbackRows = 0;
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_TOTAL_ROWS, &totalRows);
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &scrollbackRows);
    qDebug() << "restoreScrollback: after replay totalRows=" << totalRows << "scrollbackRows=" << scrollbackRows;

    // Resize to actual dimensions — triggers reflow on primary screen
    if (savedCols != actualCols || savedRows != actualRows) {
        qDebug() << "restoreScrollback: resizing to actual dims";
        ghostty_terminal_resize(m_terminal, actualCols, actualRows, 0, 0);
    }
}
