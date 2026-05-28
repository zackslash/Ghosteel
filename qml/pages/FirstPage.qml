import QtQuick 2.0
import Sailfish.Silica 1.0
import QtMultimedia 5.0
import Nemo.Notifications 1.0
import harbour.ghosteel 1.0

Page {
    id: page
    allowedOrientations: Orientation.All

    // GhosttyMods bitmask constants
    readonly property int modsCtrl: 2    // GHOSTTY_MODS_CTRL
    readonly property int modsAlt: 4     // GHOSTTY_MODS_ALT

    // Track active modifiers for virtual keyboard sticky keys
    property int activeModifiers: 0
    property bool ctrlActive: false
    property bool altActive: false
    property bool keyboardVisible: Qt.inputMethod && Qt.inputMethod.visible
    property TerminalView terminal: null

    // Bell sound for terminal BEL character
    SoundEffect {
        id: bellSound
        source: "/usr/share/sounds/jolla-ambient/stereo/jolla-notification.wav"
    }

    // Haptic feedback notification — publishing a notification triggers system vibration
    Notification {
        id: bellNotification
        appName: "Ghosteel"
        summary: ""
        body: ""
        urgency: Notification.Critical
        expireTimeout: 1
    }

    // Rate limit bell feedback to prevent haptic motor/audio spam
    Timer {
        id: bellCooldown
        interval: 200
    }

    // System notification for OSC 777 desktop notifications
    Notification {
        id: terminalNotification
        appName: "Ghosteel"
        summary: ""
        body: ""
        urgency: Notification.Normal
        expireTimeout: 5000
    }

    onCtrlActiveChanged: updateModifiers()
    onAltActiveChanged: updateModifiers()
    onActiveModifiersChanged: {
        if (terminal)
            terminal.stickyModifiers = activeModifiers
    }

    function attachTerminal(t) {
        if (!t) return
        // Parent into container once — never reparent, just toggle visibility
        if (t.parent !== terminalContainer) {
            t.parent = terminalContainer
            t.anchors.fill = terminalContainer
        }
        t.visible = true
        t.opacity = 1
        t.fontSize = appWindow.terminalFontSize
        t.forceActiveFocus()

        // Connect signals if not already connected
        t.titleChanged.disconnect(updateWindowTitle)
        t.titleChanged.connect(updateWindowTitle)
        t.stickyModifiersChanged.disconnect(onTerminalStickyModifiersChanged)
        t.stickyModifiersChanged.connect(onTerminalStickyModifiersChanged)
        t.terminalBell.disconnect(onTerminalBell)
        t.terminalBell.connect(onTerminalBell)
        t.desktopNotification.disconnect(onDesktopNotification)
        t.desktopNotification.connect(onDesktopNotification)

        terminal = t
        updateWindowTitle()
    }

    function detachTerminal(t) {
        if (!t) return
        t.titleChanged.disconnect(updateWindowTitle)
        t.stickyModifiersChanged.disconnect(onTerminalStickyModifiersChanged)
        t.terminalBell.disconnect(onTerminalBell)
        t.desktopNotification.disconnect(onDesktopNotification)
        t.visible = false
    }

    Component.onCompleted: {
        var t = SessionManager.activeSession()
        if (t) attachTerminal(t)
    }

    // Listen for session switches from SessionManager
    Connections {
        target: SessionManager
        onSessionSwitched: {
            var newTerminal = SessionManager.activeSession()
            if (newTerminal && newTerminal !== terminal) {
                detachTerminal(terminal)
                attachTerminal(newTerminal)
            }

            // Show session switch indicator (only when multiple sessions exist)
            if (SessionManager.sessionCount > 1) {
                var name = SessionManager.sessionName(index)
                sessionIndicator.show(name || qsTr("Session %1").arg(index + 1))
            }
        }
    }

    function updateWindowTitle() {
        if (terminal)
            appWindow.windowTitle = terminal.title
    }

    function onTerminalStickyModifiersChanged() {
        if (terminal && terminal.stickyModifiers === 0) {
            page.ctrlActive = false
            page.altActive = false
        }
    }

    function updateModifiers() {
        var mods = 0
        if (ctrlActive) mods |= modsCtrl
        if (altActive) mods |= modsAlt
        activeModifiers = mods
    }

    function onTerminalBell() {
        var mode = Settings.bellMode
        if (mode === 0) return // None
        if (bellCooldown.running) return
        bellCooldown.start()

        // Vibrate: mode 1 or 3 — publish a notification to trigger system vibration
        if (mode === 1 || mode === 3) {
            bellNotification.publish()
        }

        // Sound: mode 2 or 3
        if (mode === 2 || mode === 3) {
            bellSound.play()
        }
    }

    function onDesktopNotification(summary, body) {
        terminalNotification.summary = summary
        terminalNotification.body = body || ""
        terminalNotification.publish()
    }

    SilicaFlickable {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: keybar.top
        contentHeight: height

        PullDownMenu {
            MenuItem {
                text: qsTr("New session")
                onClicked: {
                    SessionManager.createSession()
                }
            }
            MenuItem {
                text: qsTr("Sessions")
                onClicked: pageStack.push(Qt.resolvedUrl("SessionPage.qml"))
            }
            MenuItem {
                text: qsTr("Next session")
                visible: SessionManager.sessionCount > 1
                onClicked: {
                    var next = (SessionManager.activeSessionIndex + 1) % SessionManager.sessionCount
                    SessionManager.switchToSession(next)
                }
            }
            MenuItem {
                text: qsTr("Previous session")
                visible: SessionManager.sessionCount > 1
                onClicked: {
                    var prev = SessionManager.activeSessionIndex - 1
                    if (prev < 0) prev = SessionManager.sessionCount - 1
                    SessionManager.switchToSession(prev)
                }
            }
            MenuItem {
                text: keybar.open ? qsTr("Hide extra keys") : qsTr("Show extra keys")
                onClicked: keybar.open = !keybar.open
            }
            MenuItem {
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"))
            }
        }

        // Container for the active terminal session
        Item {
            id: terminalContainer
            anchors.fill: parent
        }
    }

    // Session switch indicator overlay
    Rectangle {
        id: sessionIndicator
        anchors.centerIn: parent
        width: sessionLabel.width + Theme.paddingLarge * 4
        height: sessionLabel.height + Theme.paddingMedium * 2
        radius: Theme.paddingSmall
        color: Theme.rgba(Theme.highlightBackgroundColor, 0.8)
        opacity: 0
        visible: opacity > 0
        z: 100

        Behavior on opacity {
            FadeAnimator { duration: 200 }
        }

        Label {
            id: sessionLabel
            anchors.centerIn: parent
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeLarge
        }

        Timer {
            id: indicatorTimer
            interval: 1500
            onTriggered: sessionIndicator.opacity = 0
        }

        function show(text) {
            sessionLabel.text = text
            opacity = 1
            indicatorTimer.restart()
        }
    }

    // Extra terminal keys panel
    DockedPanel {
        id: keybar
        dock: Dock.Bottom
        width: parent.width
        height: Theme.itemSizeMedium + Theme.paddingSmall
        open: true

        Column {
            anchors.centerIn: parent
            spacing: Theme.paddingSmall

            // Navigation and modifier row
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.paddingSmall

                // Arrow keys
                IconButton {
                    icon.source: "image://theme/icon-m-back"
                    onClicked: terminal.sendKey(Qt.Key_Left, page.activeModifiers)
                }
                IconButton {
                    icon.source: "image://theme/icon-m-down"
                    onClicked: terminal.sendKey(Qt.Key_Down, page.activeModifiers)
                }
                IconButton {
                    icon.source: "image://theme/icon-m-up"
                    onClicked: terminal.sendKey(Qt.Key_Up, page.activeModifiers)
                }
                IconButton {
                    icon.source: "image://theme/icon-m-forward"
                    onClicked: terminal.sendKey(Qt.Key_Right, page.activeModifiers)
                }

                // Separator
                Item { width: Theme.paddingMedium; height: 1 }

                // Tab
                BackgroundItem {
                    width: Theme.itemSizeSmall
                    height: Theme.itemSizeSmall
                    onClicked: terminal.sendKey(Qt.Key_Tab, 0)

                    Label {
                        anchors.centerIn: parent
                        text: "Tab"
                        font.pixelSize: Theme.fontSizeSmall
                        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }

                // Separator
                Item { width: Theme.paddingMedium; height: 1 }

                // Ctrl toggle
                BackgroundItem {
                    width: Theme.itemSizeSmall
                    height: Theme.itemSizeSmall
                    highlighted: page.ctrlActive
                    onClicked: page.ctrlActive = !page.ctrlActive

                    Label {
                        anchors.centerIn: parent
                        text: "Ctrl"
                        font.pixelSize: Theme.fontSizeSmall
                        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }

                // Alt toggle
                BackgroundItem {
                    width: Theme.itemSizeSmall
                    height: Theme.itemSizeSmall
                    highlighted: page.altActive
                    onClicked: page.altActive = !page.altActive

                    Label {
                        anchors.centerIn: parent
                        text: "Alt"
                        font.pixelSize: Theme.fontSizeSmall
                        color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }

                // Toggle software keyboard
                IconButton {
                    icon.source: "image://theme/icon-m-keyboard"
                    highlighted: page.keyboardVisible
                    onClicked: {
                        if (page.keyboardVisible)
                            Qt.inputMethod.hide()
                        else
                            Qt.inputMethod.show()
                    }
                }
            }
        }
    }
}
