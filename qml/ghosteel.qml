import QtQuick 2.0
import Sailfish.Silica 1.0
import harbour.ghosteel 1.0
import "pages"

ApplicationWindow {
    id: appWindow

    // Shared properties for cover page and settings
    property string windowTitle: ""
    property int terminalFontSize: Settings.fontSize

    onTerminalFontSizeChanged: Settings.fontSize = terminalFontSize

    initialPage: Component {
        FirstPage {
            objectName: "firstPage"
        }
    }
    cover: Qt.resolvedUrl("cover/CoverPage.qml")
    allowedOrientations: defaultAllowedOrientations

    Component.onCompleted: {
        // Restore saved sessions if available, otherwise create a fresh one
        if (!SessionManager.restoreSessions()) {
            SessionManager.createSession()
        }
    }
}
