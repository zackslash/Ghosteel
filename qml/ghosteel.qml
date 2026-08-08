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
            // Light ambience (DarkOnLight) → light terminal bg; everything else → dark
            Settings.colorScheme = (Theme.colorScheme === Theme.DarkOnLight) ? "light" : "dark"
    }

    // Re-resolve when the ambience changes (startup or user switch)
    Connections {
        target: Theme
        onColorSchemeChanged: resolveAmbience()
    }

    // Resolve immediately when followAmbience is toggled in settings
    Connections {
        target: Settings
        onFollowAmbienceChanged: resolveAmbience()
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
