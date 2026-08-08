import QtQuick 2.0
import Sailfish.Silica 1.0
import "pages"

ApplicationWindow {
    id: appWindow

    // Shared properties for cover page and settings
    property string windowTitle: ""

    initialPage: Component {
        TerminalPage {
            objectName: "terminalPage"
        }
    }
    cover: Qt.resolvedUrl("cover/CoverPage.qml")
    allowedOrientations: defaultAllowedOrientations

    Component.onCompleted: {
        // Resolve ambience-following before sessions are restored so
        // terminals are created with the correct color scheme.
        resolveAmbience()
        // Restore saved sessions if available, otherwise create a fresh one
        if (!SessionManager.restoreSessions()) {
            SessionManager.createSession()
        }
        // Process CLI args (-e/--exec, -s/--session) after sessions are restored
        SessionManager.processCliArgs()
    }

    function resolveAmbience() {
        if (Settings.followAmbience)
            Settings.colorScheme = (Theme.colorScheme === Theme.DarkOnLight) ? "light" : "dark"
    }

    // Update terminal colors when the user switches ambiences
    Connections {
        target: Theme
        onColorSchemeChanged: resolveAmbience()
    }

    // IPC exec: navigate to terminal so user sees the result
    Connections {
        target: SessionManager
        onShowTerminal: pageStack.pop(null)
        onShowSessionList: {
            // Avoid pushing a duplicate SessionPage — the active anonymous
            // session may exit while the user is already viewing the list.
            if (pageStack.currentPage && pageStack.currentPage.objectName === "sessionPage")
                return
            pageStack.push(Qt.resolvedUrl("pages/SessionPage.qml"))
        }
    }
}
