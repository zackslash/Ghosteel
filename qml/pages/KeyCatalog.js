.pragma library

var categories = [
    { id: "nav",      label: "Navigation" },
    { id: "modifier", label: "Modifiers" },
    { id: "utility",  label: "Utility" },
    { id: "session",  label: "Session Navigation" },
    { id: "function", label: "Function keys" }
]

var keys = [
    // Navigation
    { id: "left",  label: "\u2190", description: "Left",       category: "nav", action: "key", qtKey: 0x01000012, iconSource: "icon-m-back" },    // Qt.Key_Left
    { id: "down",  label: "\u2193", description: "Down",       category: "nav", action: "key", qtKey: 0x01000015, iconSource: "icon-m-down" },    // Qt.Key_Down
    { id: "up",    label: "\u2191", description: "Up",         category: "nav", action: "key", qtKey: 0x01000013, iconSource: "icon-m-up" },      // Qt.Key_Up
    { id: "right", label: "\u2192", description: "Right",      category: "nav", action: "key", qtKey: 0x01000014, iconSource: "icon-m-forward" }, // Qt.Key_Right
    { id: "tab",   label: "Tab",    description: "Tab",        category: "nav", action: "key", qtKey: 0x01000001 },                                // Qt.Key_Tab
    { id: "esc",   label: "Esc",    description: "Escape",     category: "nav", action: "key", qtKey: 0x01000000 },                                // Qt.Key_Escape
    { id: "pgup",  label: "PgUp",   description: "Page Up",    category: "nav", action: "key", qtKey: 0x01000016 },                                // Qt.Key_PageUp
    { id: "pgdn",  label: "PgDn",   description: "Page Down",  category: "nav", action: "key", qtKey: 0x01000017 },                                // Qt.Key_PageDown
    { id: "home",  label: "Home",   description: "Home",       category: "nav", action: "key", qtKey: 0x01000010 },                                // Qt.Key_Home
    { id: "end",   label: "End",    description: "End",        category: "nav", action: "key", qtKey: 0x01000011 },                                // Qt.Key_End
    { id: "del",   label: "Del",    description: "Delete",     category: "nav", action: "key", qtKey: 0x01000007 },                                // Qt.Key_Delete

    // Modifiers
    { id: "ctrl", label: "Ctrl", description: "Control modifier", category: "modifier", action: "special" },
    { id: "alt",  label: "Alt",  description: "Alt modifier",     category: "modifier", action: "special" },

    // Utility
    { id: "keyboard", label: "\u2328", description: "Toggle keyboard", category: "utility", action: "special" },
    { id: "zoomIn",  label: "+",  description: "Zoom in",  category: "utility", action: "special" },
    { id: "zoomOut", label: "\u2212", description: "Zoom out", category: "utility", action: "special" },

    // Session
    { id: "prevSession", label: "\u25C0", description: "Previous session", category: "session", action: "special" },
    { id: "nextSession", label: "\u25B6", description: "Next session",     category: "session", action: "special" },

    // Function keys
    { id: "f1",  label: "F1",  description: "F1",  category: "function", action: "key", qtKey: 0x01000030 },  // Qt.Key_F1
    { id: "f2",  label: "F2",  description: "F2",  category: "function", action: "key", qtKey: 0x01000031 },  // Qt.Key_F2
    { id: "f3",  label: "F3",  description: "F3",  category: "function", action: "key", qtKey: 0x01000032 },  // Qt.Key_F3
    { id: "f4",  label: "F4",  description: "F4",  category: "function", action: "key", qtKey: 0x01000033 },  // Qt.Key_F4
    { id: "f5",  label: "F5",  description: "F5",  category: "function", action: "key", qtKey: 0x01000034 },  // Qt.Key_F5
    { id: "f6",  label: "F6",  description: "F6",  category: "function", action: "key", qtKey: 0x01000035 },  // Qt.Key_F6
    { id: "f7",  label: "F7",  description: "F7",  category: "function", action: "key", qtKey: 0x01000036 },  // Qt.Key_F7
    { id: "f8",  label: "F8",  description: "F8",  category: "function", action: "key", qtKey: 0x01000037 },  // Qt.Key_F8
    { id: "f9",  label: "F9",  description: "F9",  category: "function", action: "key", qtKey: 0x01000038 },  // Qt.Key_F9
    { id: "f10", label: "F10", description: "F10", category: "function", action: "key", qtKey: 0x01000039 },  // Qt.Key_F10
    { id: "f11", label: "F11", description: "F11", category: "function", action: "key", qtKey: 0x0100003a },  // Qt.Key_F11
    { id: "f12", label: "F12", description: "F12", category: "function", action: "key", qtKey: 0x0100003b }   // Qt.Key_F12
]

var defaults = ["left", "down", "up", "right", "tab", "ctrl", "alt", "esc", "keyboard"]

function findById(id) {
    for (var i = 0; i < keys.length; i++) {
        if (keys[i].id === id) return keys[i]
    }
    return null
}

// Row number (0-based) for a key at global index idx, given the row breaks.
function rowForIndex(idx, breaks) {
    for (var i = 0; i < breaks.length; i++) {
        if (idx < breaks[i])
            return i
    }
    return breaks.length
}
