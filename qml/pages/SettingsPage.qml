import QtQuick 2.0
import Sailfish.Silica 1.0
import com.zackslash.ghosteel 1.0
import "KeyCatalog.js" as KeyCatalog

Page {
    id: settingsPage
    allowedOrientations: Orientation.All

    property var colorSchemes: ["dark", "light", "solarized-dark", "solarized-light", "monokai"]

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("Reset to defaults")
                onClicked: {
                    fontSlider.value = 18
                    shellField.text = ""
                    bellModeCombo.currentIndex = 1
                    schemeCombo.currentIndex = 0
                    opacitySlider.value = 0.6
                    scrollbackToggle.checked = false
                    retentionCombo.currentIndex = 1  // 30 days
                    Settings.keybarKeys = KeyCatalog.defaults.slice()
                }
            }
        }

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge

            PageHeader {
                title: qsTr("Settings")
            }

            // Terminal section
            SectionHeader {
                text: qsTr("Terminal")
            }

            TextField {
                id: shellField
                width: parent.width
                label: qsTr("Shell command")
                placeholderText: qsTr("Default: /bin/sh")
                text: Settings.shellCommand
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false

                onTextChanged: Settings.shellCommand = text
            }

            ComboBox {
                id: bellModeCombo
                width: parent.width
                label: qsTr("Bell")
                currentIndex: Settings.bellMode

                menu: ContextMenu {
                    MenuItem { text: qsTr("None") }
                    MenuItem { text: qsTr("Vibrate") }
                    MenuItem { text: qsTr("Sound") }
                    MenuItem { text: qsTr("Vibrate + Sound") }
                }

                onCurrentIndexChanged: Settings.bellMode = currentIndex
            }

            // Appearance section
            SectionHeader {
                text: qsTr("Appearance")
            }

            ComboBox {
                id: schemeCombo
                width: parent.width
                label: qsTr("Color scheme")
                currentIndex: {
                    var idx = colorSchemes.indexOf(Settings.colorScheme)
                    return idx >= 0 ? idx : 0
                }

                menu: ContextMenu {
                    MenuItem { text: qsTr("Dark") }
                    MenuItem { text: qsTr("Light") }
                    MenuItem { text: qsTr("Solarized Dark") }
                    MenuItem { text: qsTr("Solarized Light") }
                    MenuItem { text: qsTr("Monokai") }
                }

                onCurrentIndexChanged: {
                    Settings.colorScheme = colorSchemes[currentIndex]
                }
            }

            Slider {
                id: fontSlider
                width: parent.width
                label: qsTr("Font size")
                minimumValue: 6
                maximumValue: 32
                stepSize: 1
                value: Settings.fontSize
                valueText: qsTr("%1 px").arg(value)

                onValueChanged: Settings.fontSize = value
            }

            Slider {
                id: opacitySlider
                width: parent.width
                label: qsTr("Background opacity")
                minimumValue: 0.3
                maximumValue: 1.0
                stepSize: 0.05
                value: Settings.backgroundOpacity
                valueText: qsTr("%1%").arg(Math.round(value * 100))

                onValueChanged: Settings.backgroundOpacity = value
            }

            // Extra keys section
            SectionHeader {
                text: qsTr("Extra keys")
            }

            BackgroundItem {
                width: parent.width
                height: Theme.itemSizeMedium
                onClicked: pageStack.push(Qt.resolvedUrl("KeybarSettings.qml"))

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    text: qsTr("Configure keybar")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            // Scrollback section
            SectionHeader {
                text: qsTr("Scrollback")
            }

            TextSwitch {
                id: scrollbackToggle
                width: parent.width
                text: qsTr("Persist scrollback")
                description: qsTr("Save terminal history when app closes. Disabled by default for privacy.")
                checked: Settings.scrollbackPersistence
                onCheckedChanged: Settings.scrollbackPersistence = checked
            }

            ComboBox {
                id: retentionCombo
                width: parent.width
                label: qsTr("Keep history for")
                enabled: Settings.scrollbackPersistence
                opacity: enabled ? 1.0 : 0.4
                currentIndex: {
                    var days = Settings.scrollbackRetentionDays
                    if (days <= 7) return 0
                    if (days <= 30) return 1
                    if (days <= 90) return 2
                    return 3
                }

                menu: ContextMenu {
                    MenuItem { text: qsTr("7 days") }
                    MenuItem { text: qsTr("30 days") }
                    MenuItem { text: qsTr("90 days") }
                    MenuItem { text: qsTr("1 year") }
                }

                onCurrentIndexChanged: {
                    var days = [7, 30, 90, 365][currentIndex]
                    Settings.scrollbackRetentionDays = days
                }
            }

            // About section
            SectionHeader {
                text: qsTr("About")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.secondaryColor
                wrapMode: Text.Wrap
                text: qsTr("Ghosteel terminal for SailfishOS\nPowered by libghostty terminal engine")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                wrapMode: Text.Wrap
                text: appName + " " + appVersion + "\nlibghostty " + ghosttyVersion
            }
        }

        VerticalScrollDecorator {}
    }
}