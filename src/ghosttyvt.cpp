#include "ghosttyvt.h"
#include <QDebug>

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

    GhosttyTerminalOptions opts = {};
    opts.cols = cols;
    opts.rows = rows;
    opts.max_scrollback = 5000; // Reduced from 10000 to save ~5MB on low-memory devices; configurable via API

    GhosttyResult res = ghostty_terminal_new(nullptr, &m_terminal, opts);
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
