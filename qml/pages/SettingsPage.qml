import QtQuick 2.0
import Sailfish.Silica 1.0
import com.zackslash.ghosteel 1.0

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