#ifndef KEYMAPPING_H
#define KEYMAPPING_H

// Pure key/modifier mapping functions extracted from TerminalView.
// No Qt Quick or QQuickPaintedItem dependency.

#include <QtCore>

// Ghostty types (defined in ghostty/vt/key/event.h). The header carries no
// extern "C" guards of its own, so include it inside one to keep C linkage.
#ifdef __cplusplus
extern "C" {
#endif
#include <ghostty/vt/key/event.h>
#ifdef __cplusplus
}
#endif

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

// Reverse of mapCharToKey: the base-layout ASCII codepoint a text-producing
// GhosttyKey encodes as, or 0 for functional/modifier keys. This is the
// "unshifted codepoint" the ghostty key-event API wants for protocol-level
// (kitty/CSI-u) encoding of modified keys.
uint32_t keyToUnshiftedCodepoint(GhosttyKey key);

} // namespace KeyMapping

#endif // KEYMAPPING_H
