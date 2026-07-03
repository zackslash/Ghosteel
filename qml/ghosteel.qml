import QtQuick 2.0
import Sailfish.Silica 1.0
import Nemo.KeepAlive 1.2
import Nemo.Notifications 1.0
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

    // App-global KeepAlive lock — held iff ≥1 flagged alive session
    KeepAlive {
        enabled: SessionManager.keepAwakeActive
    }

    // Persistent low-priority notification while keep-awake is active.
    // expireTimeout: 0 = never auto-close. (-1 means the notification manager
    // picks a default expiry, which is NOT persistent.)
    Notification {
        id: keepAwakeNotification
        appName: "Ghosteel"
        summary: qsTr("Keeping device awake")
        urgency: Notification.Low
        expireTimeout: 0
    }

    Connections {
        target: SessionManager
        // Single handler covers all four transitions (0->1, 1->N, N->1, 1->0):
        // keepAwakeActive is updated before keepAwakeActiveCountChanged emits,
        // so this sees the correct active state in every case. publish() also
        // auto-maintains replacesId as a side effect, so repeated posts update
        // the existing notification in place — no manual ID tracking needed.
        onKeepAwakeActiveCountChanged: {
            if (SessionManager.keepAwakeActive) {
                keepAwakeNotification.body = qsTr("Ghosteel is keeping the device awake — %n session(s) active.", "", SessionManager.keepAwakeActiveCount)
                // remoteActions set per-post (mirrors terminalNotification); disabled
                // when D-Bus isn't registered (secondary-launch fallback).
                if (SessionManager.dbusRegistered) {
                    keepAwakeNotification.remoteActions = [{
                        "name": "default",
                        "displayName": qsTr("Open Ghosteel"),
                        "icon": "image://theme/icon-m-activities",
                        "service": "com.zackslash.ghosteel",
                        "path": "/com/zackslash.ghosteel",
                        "iface": "com.zackslash.ghosteel",
                        "method": "activateSession",
                        // Jump to the active session. activateSession takes a session ID.
                        "arguments": [SessionManager.sessionId(SessionManager.activeSessionIndex)]
                    }]
                } else {
                    keepAwakeNotification.remoteActions = []
                }
                keepAwakeNotification.publish()
            } else {
                keepAwakeNotification.close()
            }
        }
    }

    Component.onCompleted: {
        // Restore saved sessions if available, otherwise create a fresh one
        if (!SessionManager.restoreSessions()) {
            SessionManager.createSession()
        }
        // Process CLI args (-e/--exec, -s/--session) after sessions are restored
        SessionManager.processCliArgs()
    }

    // Close persistent notification on teardown — otherwise it lingers after quit
    Component.onDestruction: keepAwakeNotification.close()

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
