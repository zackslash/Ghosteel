import QtQuick 2.0
import Sailfish.Silica 1.0
import "KeyCatalog.js" as KeyCatalog

Page {
    id: keybarSettingsPage
    allowedOrientations: Orientation.All

    // Set of currently enabled key IDs for fast lookup in the Available section
    property var enabledKeys: {
        var set = {}
        var keys = Settings.keybarKeys
        for (var i = 0; i < keys.length; i++)
            set[keys[i]] = true
        return set
    }

    // Translation helpers for KeyCatalog.js strings (pragma library can't use qsTr)
    function translateCategory(label) {
        var map = {
            "Navigation": qsTr("Navigation"),
            "Modifiers": qsTr("Modifiers"),
            "Utility": qsTr("Utility"),
            "Session Navigation": qsTr("Session Navigation"),
            "Function keys": qsTr("Function keys")
        }
        return map[label] || label
    }

    function translateDescription(description) {
        var map = {
            "Left": qsTr("Left"),
            "Down": qsTr("Down"),
            "Up": qsTr("Up"),
            "Right": qsTr("Right"),
            "Tab": qsTr("Tab"),
            "Escape": qsTr("Escape"),
            "Page Up": qsTr("Page Up"),
            "Page Down": qsTr("Page Down"),
            "Home": qsTr("Home"),
            "End": qsTr("End"),
            "Delete": qsTr("Delete"),
            "Control modifier": qsTr("Control modifier"),
            "Alt modifier": qsTr("Alt modifier"),
            "Toggle keyboard": qsTr("Toggle keyboard"),
            "Previous session": qsTr("Previous session"),
            "Next session": qsTr("Next session"),
            "Zoom in": qsTr("Zoom in"),
            "Zoom out": qsTr("Zoom out")
        }
        return map[description] || description
    }

    function toggleKey(keyId, enabled) {
        var keys = Settings.keybarKeys.slice()
        var idx = keys.indexOf(keyId)
        if (enabled && idx < 0) {
            keys.push(keyId)
            Settings.keybarKeys = keys
        } else if (!enabled && idx >= 0) {
            keys.splice(idx, 1)
            Settings.keybarKeys = keys
            adjustBreaksForRemoval(idx)
        }
    }

    function moveKey(fromIdx, toIdx) {
        var breaks = Settings.keybarRowBreaks.slice()

        // If the move crosses a break boundary, shift the break instead of
        // reordering the key, so it joins the adjacent row without swapping.
        for (var i = 0; i < breaks.length; i++) {
            if (fromIdx === breaks[i] && toIdx === breaks[i] - 1) {
                breaks[i] = breaks[i] + 1
                Settings.keybarRowBreaks = breaks
                return
            }
            if (toIdx === breaks[i] && fromIdx === breaks[i] - 1) {
                breaks[i] = breaks[i] - 1
                Settings.keybarRowBreaks = breaks
                return
            }
        }

        // Normal reorder within the same row
        var keys = Settings.keybarKeys.slice()
        var item = keys.splice(fromIdx, 1)[0]
        keys.splice(toIdx, 0, item)
        Settings.keybarKeys = keys
    }

    function removeKey(keyId) {
        var keys = Settings.keybarKeys.slice()
        var idx = keys.indexOf(keyId)
        if (idx >= 0) {
            keys.splice(idx, 1)
            Settings.keybarKeys = keys
            adjustBreaksForRemoval(idx)
        }
    }

    // Decrement breaks past the removed index so keys stay in their rows.
    function adjustBreaksForRemoval(removedIdx) {
        var breaks = Settings.keybarRowBreaks.slice()
        for (var i = 0; i < breaks.length; i++) {
            if (breaks[i] > removedIdx)
                breaks[i] = breaks[i] - 1
        }
        Settings.keybarRowBreaks = breaks
    }

    // Toggle a row break at the given position (index after which a new row starts).
    function toggleBreak(breakValue) {
        var breaks = Settings.keybarRowBreaks.slice()
        var i = breaks.indexOf(breakValue)
        if (i >= 0) {
            breaks.splice(i, 1)
        } else if (breaks.length < 2) {
            breaks.push(breakValue)
            breaks.sort(function(a, b) { return a - b })
        }
        Settings.keybarRowBreaks = breaks
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("Reset to defaults")
                onClicked: {
                    Settings.keybarRowBreaks = []
                    Settings.keybarKeys = KeyCatalog.defaults.slice()
                }
            }
        }

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingSmall

            PageHeader {
                title: qsTr("Extra keys")
            }

            // --- Enabled keys (ordered list with tappable row-break dividers) ---
            SectionHeader {
                text: qsTr("Enabled keys")
            }

            // Hint shown only when no breaks exist
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: Settings.keybarRowBreaks.length === 0 && Settings.keybarKeys.length >= 2
                text: qsTr("Tap between two keys to start a new row")
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }

            Column {
                width: parent.width
                spacing: 0

                Repeater {
                    model: Settings.keybarKeys

                    delegate: Column {
                        width: parent.width
                        spacing: 0

                        ListItem {
                            id: enabledItem
                            width: parent.width
                            contentHeight: Theme.itemSizeSmall
                            menu: ContextMenu {
                                MenuItem {
                                    text: qsTr("Remove")
                                    onClicked: removeKey(modelData)
                                }
                            }

                            Item {
                                anchors {
                                    left: parent.left
                                    right: parent.right
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: Theme.horizontalPageMargin
                                    rightMargin: Theme.paddingSmall
                                }
                                height: Theme.itemSizeSmall

                                Label {
                                    anchors {
                                        left: parent.left
                                        right: upBtn.left
                                        verticalCenter: parent.verticalCenter
                                        rightMargin: Theme.paddingSmall
                                    }
                                    text: {
                                        var def = KeyCatalog.findById(modelData)
                                        if (!def) return modelData
                                        if (def.description && def.description !== def.label)
                                            return def.label + " (" + translateDescription(def.description) + ")"
                                        return def.label
                                    }
                                    color: enabledItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                                    truncationMode: TruncationMode.Fade
                                }

                                IconButton {
                                    id: upBtn
                                    anchors.right: downBtn.left
                                    icon.source: "image://theme/icon-m-up"
                                    enabled: index > 0
                                    opacity: enabled ? 1.0 : 0.3
                                    onClicked: moveKey(index, index - 1)
                                }

                                IconButton {
                                    id: downBtn
                                    anchors.right: parent.right
                                    icon.source: "image://theme/icon-m-down"
                                    enabled: index < Settings.keybarKeys.length - 1
                                    opacity: enabled ? 1.0 : 0.3
                                    onClicked: moveKey(index, index + 1)
                                }
                            }
                        }

                        // Tappable row-break divider between this key and the next.
                        Item {
                            id: breakDivider
                            width: parent.width
                            readonly property bool isBreak: Settings.keybarRowBreaks.indexOf(index + 1) >= 0
                            readonly property bool canAdd: Settings.keybarRowBreaks.length < 2
                            height: index < Settings.keybarKeys.length - 1
                                    ? (isBreak ? Theme.itemSizeSmall : Theme.paddingLarge)
                                    : 0
                            visible: height > 0

                            // Press feedback
                            Rectangle {
                                anchors.fill: parent
                                color: Theme.highlightBackgroundColor
                                opacity: dividerMA.pressed ? 0.4 : 0.0
                            }

                            // Active: "Row N" label
                            Label {
                                anchors.centerIn: parent
                                visible: breakDivider.isBreak
                                text: qsTr("Row") + " " + (Settings.keybarRowBreaks.indexOf(index + 1) + 2)
                                color: Theme.secondaryColor
                                font.pixelSize: Theme.fontSizeSmall
                            }

                            // Inactive: centered "+" mark (tap to create a row break)
                            Item {
                                anchors.centerIn: parent
                                visible: !breakDivider.isBreak
                                opacity: breakDivider.canAdd ? 0.5 : 0.1

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 12; height: 1
                                    color: Theme.primaryColor
                                }
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 1; height: 12
                                    color: Theme.primaryColor
                                }
                            }

                            MouseArea {
                                id: dividerMA
                                anchors.fill: parent
                                enabled: breakDivider.visible && (breakDivider.isBreak || breakDivider.canAdd)
                                onClicked: toggleBreak(index + 1)
                            }
                        }
                    }
                }
            }

            // Empty state hint
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: Settings.keybarKeys.length === 0
                text: qsTr("No keys enabled. Add keys from the list below.")
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }

            // --- Available keys (category-grouped toggles) ---
            SectionHeader {
                text: qsTr("Available keys")
            }

            Repeater {
                model: KeyCatalog.categories

                delegate: Column {
                    width: parent.width

                    SectionHeader {
                        text: translateCategory(modelData.label)
                    }

                    Grid {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * Theme.horizontalPageMargin
                        columns: 3
                        spacing: 0

                        Repeater {
                            model: {
                                var result = []
                                for (var i = 0; i < KeyCatalog.keys.length; i++) {
                                    if (KeyCatalog.keys[i].category === modelData.id)
                                        result.push(KeyCatalog.keys[i])
                                }
                                return result
                            }

                            delegate: TextSwitch {
                                text: modelData.label
                                automaticCheck: false
                                checked: enabledKeys[modelData.id] || false
                                width: parent.width / 3

                                onClicked: toggleKey(modelData.id, !checked)
                            }
                        }
                    }
                }
            }
        }

        VerticalScrollDecorator {}
    }
}
