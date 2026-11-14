import QtQuick 2.0
import Sailfish.Silica 1.0

Dialog {
    id: dialog

    allowedOrientations: Orientation.All

    onAccepted: Settings.sandboxNoticeShown = true

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge

            DialogHeader {
                //: "Sandboxed" refers to the restricted Jolla Store build
                title: qsTr("Sandboxed version")
                acceptText: qsTr("Got it")
            }

            Label {
                x: Theme.paddingLarge
                width: parent.width - 2 * Theme.paddingLarge
                wrapMode: Text.WordWrap
                text: qsTr("This is the sandboxed Jolla Store build of Ghosteel. It runs in a restricted environment: only the built-in busybox tools are available, installed apps are not visible to it, it sees just a few of your folders (such as Downloads), and it has no root access or package manager.")
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.highlightColor
            }

            Label {
                x: Theme.paddingLarge
                width: parent.width - 2 * Theme.paddingLarge
                wrapMode: Text.WordWrap
                text: qsTr("Files and settings you create inside it are kept in its own folder and survive restarts. Bring your own single-file tools: drop them in ~/bin inside the terminal and they become normal commands.")
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.highlightColor
            }

            Label {
                x: Theme.paddingLarge
                width: parent.width - 2 * Theme.paddingLarge
                wrapMode: Text.WordWrap
                text: qsTr("For the full experience (system shell, installed apps, desktop shortcuts) install the full Ghosteel from Chum or OpenRepos (Storeman).")
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
            }
        }
    }
}
