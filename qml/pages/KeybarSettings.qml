import QtQuick 2.0
import Sailfish.Silica 1.0
import com.zackslash.ghosteel 1.0
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
            "Next session": qsTr("Next session")
        }
        return map[description] || description
    }

    function toggleKey(keyId, enabled) {
        var keys = Settings.keybarKeys.slice()
        var idx = keys.indexOf(keyId)
        if (enabled && idx < 0) {
            keys.push(keyId)
        } else if (!enabled && idx >= 0) {
            keys.splice(idx, 1)
        }
        Settings.keybarKeys = keys
    }

    function moveKey(fromIdx, toIdx) {
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
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("Reset to defaults")
                onClicked: Settings.keybarKeys = KeyCatalog.defaults.slice()
            }
        }

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingSmall

            PageHeader {
                title: qsTr("Extra keys")
            }

            // --- Enabled keys (ordered list) ---
            SectionHeader {
                text: qsTr("Enabled keys")
            }

            Repeater {
                model: Settings.keybarKeys

                delegate: ListItem {
                    id: enabledItem
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
