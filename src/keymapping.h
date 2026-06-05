#ifndef KEYMAPPING_H
#define KEYMAPPING_H

// Pure key/modifier mapping functions extracted from TerminalView.
// No Qt Quick or QQuickPaintedItem dependency.

#include <QtCore>

// Ghostty types (defined in ghostty/vt/key/event.h)
#include <ghostty/vt/key/event.h>

namespace KeyMapping {

// Map a Qt::Key_* value to the corresponding GhosttyKey.
// Returns GHOSTTY_KEY_UNIDENTIFIED for unrecognized keys.
GhosttyKey mapQtKey(int qtKey);

// Map Qt::KeyboardModifiers to GhosttyMods bitmask.
GhosttyMods mapQtModifiers(Qt::KeyboardModifiers mods);

// Map an IME character to a GhosttyKey for sticky modifier support.
// Used when the user toggles Ctrl/Alt on the keybar and types via IME.
// Maps a-z, 0-9, and common punctuation. Returns GHOSTTY_KEY_UNIDENTIFIED
// for characters that can't be mapped.
GhosttyKey mapCharToKey(QChar ch);

} // namespace KeyMapping

#endif // KEYMAPPING_H
