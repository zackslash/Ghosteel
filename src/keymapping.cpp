#include "keymapping.h"

namespace KeyMapping {

GhosttyKey mapQtKey(int qtKey)
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

GhosttyMods mapQtModifiers(Qt::KeyboardModifiers mods)
{
    GhosttyMods result = 0;
    if (mods & Qt::ShiftModifier)   result |= GHOSTTY_MODS_SHIFT;
    if (mods & Qt::ControlModifier) result |= GHOSTTY_MODS_CTRL;
    if (mods & Qt::AltModifier)     result |= GHOSTTY_MODS_ALT;
    if (mods & Qt::MetaModifier)    result |= GHOSTTY_MODS_SUPER;
    return result;
}

GhosttyKey mapCharToKey(QChar ch)
{
    ch = ch.toLower();

    if (ch >= 'a' && ch <= 'z')
        return static_cast<GhosttyKey>(GHOSTTY_KEY_A + (ch.unicode() - 'a'));

    if (ch >= '0' && ch <= '9')
        return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + (ch.unicode() - '0'));

    switch (ch.unicode()) {
    case '-': return GHOSTTY_KEY_MINUS;
    case '=': return GHOSTTY_KEY_EQUAL;
    case '[': return GHOSTTY_KEY_BRACKET_LEFT;
    case ']': return GHOSTTY_KEY_BRACKET_RIGHT;
    case '\\': return GHOSTTY_KEY_BACKSLASH;
    case ';': return GHOSTTY_KEY_SEMICOLON;
    case '\'': return GHOSTTY_KEY_QUOTE;
    case ',': return GHOSTTY_KEY_COMMA;
    case '.': return GHOSTTY_KEY_PERIOD;
    case '/': return GHOSTTY_KEY_SLASH;
    case '`': return GHOSTTY_KEY_BACKQUOTE;
    case ' ': return GHOSTTY_KEY_SPACE;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

} // namespace KeyMapping
