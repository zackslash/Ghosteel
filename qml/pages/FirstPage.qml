import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Share 1.0
import QtMultimedia 5.0
import Nemo.Notifications 1.0
import com.zackslash.ghosteel 1.0
import "KeyCatalog.js" as KeyCatalog

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
    // Show session indicator when returning from background
    Connections {
        target: Qt.application
        onStateChanged: {
            if (Qt.application.state === Qt.ApplicationActive && terminal) {
                var idx = SessionManager.activeSessionIndex
                var name = SessionManager.sessionName(idx)
                sessionIndicator.show(name || qsTr("Session %1").arg(idx + 1))
            }
        }
    }

    // Key definition lookup map (O(1) access by ID)
    property var keyLookup: {
        var lookup = {}
        for (var i = 0; i < KeyCatalog.keys.length; i++)
            lookup[KeyCatalog.keys[i].id] = KeyCatalog.keys[i]
        return lookup
    }

    // Per-session UI state (keyboard + keybar visibility), keyed by session ID
    property var sessionUIState: ({})


    property int currentSessionIndex: -1  // Tracked imperatively to avoid binding race
    property int currentSessionId: -1     // Stable key for sessionUIState lookups
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

    // Share selected text to other Sailfish apps
    ShareAction {
        id: shareAction
        mimeType: "text/plain"
        resources: [{
            "data": terminal ? terminal.selectedText : "",
            "name": "selected-text"
        }]
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

    // Apply Sailfish Theme colors to terminal UI overlays
    function applyTerminalTheme(t) {
        if (!t) return
        t.selectionHighlightColor = Theme.rgba(Theme.highlightBackgroundColor, Theme.highlightBackgroundOpacity)
        t.selectionHandleColor = Theme.rgba(Theme.highlightColor, 0.8)
        t.selectionHandleBorderColor = Theme.rgba(Theme.highlightColor, 0.5)
        t.searchHighlightColor = Theme.rgba(Theme.highlightColor, 0.3)
        t.searchCurrentColor = Theme.rgba(Theme.primaryColor, 0.5)
        t.shellExitOverlayColor = Qt.rgba(0, 0, 0, 0.7)
        t.shellExitTextColor = Theme.highlightColor
        t.magnifierBorderColor = Theme.rgba(Theme.highlightColor, 0.5)
        t.topPadding = Theme.paddingSmall
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
        applyTerminalTheme(t)
        t.forceActiveFocus()

        // Connect signals if not already connected
        t.titleChanged.disconnect(updateWindowTitle)
        t.titleChanged.connect(updateWindowTitle)
        t.stickyModifiersChanged.disconnect(onTerminalStickyModifiersChanged)
        t.stickyModifiersChanged.connect(onTerminalStickyModifiersChanged)
        t.terminalBell.disconnect(onTerminalBell)
        t.terminalBell.connect(onTerminalBell)
        t.navigateSession.disconnect(onNavigateSession)
        t.navigateSession.connect(onNavigateSession)
        t.toggleKeybar.disconnect(onToggleKeybar)
        t.toggleKeybar.connect(onToggleKeybar)
        terminal = t
        updateWindowTitle()
    }

    function detachTerminal(t) {
        if (!t) return
        t.titleChanged.disconnect(updateWindowTitle)
        t.stickyModifiersChanged.disconnect(onTerminalStickyModifiersChanged)
        t.terminalBell.disconnect(onTerminalBell)
        t.navigateSession.disconnect(onNavigateSession)
        t.toggleKeybar.disconnect(onToggleKeybar)
        t.visible = false
    }

    function switchSession(direction) {
        var count = SessionManager.sessionCount
        if (count <= 1) return
        var idx = SessionManager.activeSessionIndex + direction
        SessionManager.switchToSession(((idx % count) + count) % count)
    }

    Component.onCompleted: {
        var t = SessionManager.activeSession()
        if (t) {
            var idx = SessionManager.activeSessionIndex
            // Suppress keyboard BEFORE attach if persisted state is hidden
            if (!SessionManager.sessionKeyboardVisible(idx))
                t.suppressNextKeyboardAutoShow()
            attachTerminal(t)
            // Apply persisted UI state for the initial session
            currentSessionIndex = idx
            currentSessionId = SessionManager.sessionId(idx)
            // Ensure keyboard hidden if persisted state says so
            if (!SessionManager.sessionKeyboardVisible(idx))
                Qt.inputMethod.hide()
            // Restore keybar state (global setting takes precedence)
            keybar.open = Settings.keybarVisible && SessionManager.sessionKeybarOpen(idx)
            // Show session indicator on launch so the user knows which session they're in
            var name = SessionManager.sessionName(idx)
            sessionIndicator.show(name || qsTr("Session %1").arg(idx + 1))
        }
    }

    // Listen for global keybar setting changes (e.g. from SettingsPage)
    Connections {
        target: Settings
        onKeybarVisibleChanged: {
            if (!Settings.keybarVisible) {
                keybar.open = false
            } else {
                // Restore per-session state when re-enabled
                var state = sessionUIState[currentSessionId]
                keybar.open = state
                    ? state.kbbar
                    : SessionManager.sessionKeybarOpen(currentSessionIndex)
            }
        }
    }

    // Listen for session switches from SessionManager
    Connections {
        target: SessionManager
        onSessionSwitched: {
            // Save outgoing session's UI state using session ID (stable across removals)
            if (terminal && currentSessionId >= 0) {
                // Keybar: read directly from DockedPanel (reliable, sync, local)
                var keybarState = keybar.open
                // Keyboard: use persisted preference (not Qt.inputMethod.visible which
                // is global/async and affected by focus changes, not just user intent)
                var kbState = sessionUIState[currentSessionId]
                    ? sessionUIState[currentSessionId].kb
                    : SessionManager.sessionKeyboardVisible(currentSessionIndex)
                sessionUIState[currentSessionId] = {
                    kb: kbState,
                    kbbar: keybarState
                }
                // Only persist if the index still points to the same session
                // (avoids writing to the wrong session after removal shifts the vector)
                if (currentSessionId == SessionManager.sessionId(currentSessionIndex)) {
                    SessionManager.setSessionKeybarOpen(currentSessionIndex, keybarState)
                    SessionManager.setSessionKeyboardVisible(currentSessionIndex, kbState)
                }
            }

            var newTerminal = SessionManager.activeSession()
            var incomingSid = SessionManager.sessionId(index)
            if (newTerminal && newTerminal !== terminal) {
                // Suppress keyboard BEFORE attach/focus to prevent flash
                var incomingState = sessionUIState[incomingSid]
                var incomingKb = incomingState
                    ? incomingState.kb
                    : SessionManager.sessionKeyboardVisible(index)
                if (!incomingKb)
                    newTerminal.suppressNextKeyboardAutoShow()

                detachTerminal(terminal)
                attachTerminal(newTerminal)
            }

            currentSessionIndex = index
            currentSessionId = SessionManager.sessionId(index)

            // Restore incoming session's keybar state (global setting takes precedence)
            var state = sessionUIState[incomingSid]
            if (state) {
                keybar.open = Settings.keybarVisible && state.kbbar
            } else {
                keybar.open = Settings.keybarVisible && SessionManager.sessionKeybarOpen(index)
            }
            // Ensure keyboard hidden if needed — suppress prevents focus-triggered show,
            // but we also need explicit hide for the case where keyboard was already visible
            var incomingKbRestore = state ? state.kb : SessionManager.sessionKeyboardVisible(index)
            if (!incomingKbRestore) {
                Qt.inputMethod.hide()
            }

            // Show session switch indicator (only when multiple sessions exist)
            if (SessionManager.sessionCount > 1) {
                var name = SessionManager.sessionName(index)
                sessionIndicator.show(name || qsTr("Session %1").arg(index + 1))
            }
        }
        onSessionRemoved: {
            // Clean up runtime UI state for removed session (ID never reused)
            delete sessionUIState[sessionId]
            // Adjust for vector shift when removal was before current index
            if (index < currentSessionIndex)
                currentSessionIndex--
        }
        onDesktopNotification: {
            terminalNotification.summary = summary
            terminalNotification.body = body || ""
            if (SessionManager.dbusRegistered) {
                terminalNotification.remoteActions = [{
                    "name": "default",
                    "displayName": qsTr("Switch to session"),
                    "icon": "image://theme/icon-m-tabs",
                    "service": "com.zackslash.ghosteel",
                    "path": "/com/zackslash/ghosteel",
                    "iface": "com.zackslash.ghosteel",
                    "method": "activateSession",
                    "arguments": [sessionId]
                }]
            } else {
                terminalNotification.remoteActions = []
            }
            terminalNotification.publish()
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

    function onNavigateSession(direction) {
        switchSession(direction)
    }

    function onToggleKeybar() {
        if (!Settings.keybarVisible) return
        keybar.open = !keybar.open
        SessionManager.setSessionKeybarOpen(currentSessionIndex, keybar.open)
        var state = sessionUIState[currentSessionId] || {
            kb: SessionManager.sessionKeyboardVisible(currentSessionIndex),
            kbbar: keybar.open
        }
        state.kbbar = keybar.open
        sessionUIState[currentSessionId] = state
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
                text: qsTr("Share selection")
                visible: terminal && terminal.selectedText.length > 0
                onClicked: shareAction.trigger()
            }
            MenuItem {
                text: searchPanel.open ? qsTr("Hide search") : qsTr("Search terminal")
                onClicked: {
                    if (searchPanel.open) {
                        searchPanel.open = false
                        // closeSearch() called by searchPanel.onOpenChanged
                    } else {
                        if (terminal) terminal.openSearch()
                        searchPanel.open = true
                        searchField.forceActiveFocus()
                    }
                }
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

        // Transparent overlay that captures taps to dismiss search panel.
        // Only enabled when search is open; passes the tap through to the terminal.
        MouseArea {
            anchors.fill: parent
            enabled: searchPanel.open
            visible: searchPanel.open
            z: 1
            onPressed: {
                searchPanel.open = false
                if (terminal) terminal.forceActiveFocus()
                mouse.accepted = false // Let the terminal receive the event
            }
        }
    }

    // Top-docked search bar for scrollback search
    DockedPanel {
        id: searchPanel
        dock: Dock.Top
        width: parent.width
        height: searchRow.height + Theme.paddingSmall * 2
        open: false

        Rectangle {
            anchors.fill: parent
            color: Theme.highlightDimmerColor
        }

        onOpenChanged: {
            if (!open && terminal) {
                terminal.closeSearch()
            }
        }

        Row {
            id: searchRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0

            SearchField {
                id: searchField
                width: parent.width - (navButtons.visible ? navButtons.width : 0)
                placeholderText: qsTr("Search terminal")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                canHide: true

                onTextChanged: {
                    if (terminal) terminal.setSearchPattern(text)
                }
                onActiveChanged: {
                    if (!active && text === "") {
                        searchPanel.open = false
                    }
                }
                EnterKey.iconSource: text !== "" ? "image://theme/icon-m-enter-accept" : "image://theme/icon-m-enter-close"
                EnterKey.onClicked: {
                    if (terminal && text !== "") terminal.findNext()
                    focus = false
                }
            }

            Row {
                id: navButtons
                anchors.verticalCenter: parent.verticalCenter
                visible: terminal && terminal.searchMatchCount > 0

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: terminal ? (terminal.currentMatchIndex + 1) + "/" + terminal.searchMatchCount : ""
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.secondaryColor
                    width: Math.max(implicitWidth, Theme.itemSizeSmall)
                    horizontalAlignment: Text.AlignHCenter
                }

                IconButton {
                    icon.source: "image://theme/icon-m-left"
                    onClicked: {
                        if (terminal) terminal.findPrevious()
                    }
                }

                IconButton {
                    icon.source: "image://theme/icon-m-right"
                    onClicked: {
                        if (terminal) terminal.findNext()
                    }
                }
            }
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
        open: false  // set by Component.onCompleted with per-session state

        SilicaFlickable {
            anchors.fill: parent
            clip: true
            flickableDirection: Flickable.HorizontalFlick
            contentWidth: keyRow.implicitWidth

            Row {
                id: keyRow
                anchors.verticalCenter: parent.verticalCenter
                x: Theme.paddingSmall
                spacing: Theme.paddingSmall

                Repeater {
                    model: Settings.keybarKeys

                    delegate: BackgroundItem {
                        id: keyDelegate
                        property var keyDef: page.keyLookup[modelData]

                        width: Theme.itemSizeMedium
                        height: Theme.itemSizeMedium

                        highlighted: {
                            if (!keyDef) return false
                            if (keyDef.id === "ctrl") return page.ctrlActive
                            if (keyDef.id === "alt") return page.altActive
                            if (keyDef.id === "keyboard") return page.keyboardVisible
                            return false
                        }

                        onClicked: {
                            if (!terminal || !keyDef) return

                            if (keyDef.action === "key") {
                                terminal.sendKey(keyDef.qtKey, page.activeModifiers)
                            } else if (keyDef.id === "ctrl") {
                                page.ctrlActive = !page.ctrlActive
                            } else if (keyDef.id === "alt") {
                                page.altActive = !page.altActive
                            } else if (keyDef.id === "keyboard") {
                                var newVisible = !page.keyboardVisible
                                if (newVisible)
                                    Qt.inputMethod.show()
                                else
                                    Qt.inputMethod.hide()
                                SessionManager.setSessionKeyboardVisible(currentSessionIndex, newVisible)
                                var state = sessionUIState[currentSessionId] || {
                                    kb: SessionManager.sessionKeyboardVisible(currentSessionIndex),
                                    kbbar: keybar.open
                                }
                                state.kb = newVisible
                                sessionUIState[currentSessionId] = state
                            } else if (keyDef.id === "prevSession" || keyDef.id === "nextSession") {
                                var dir = keyDef.id === "prevSession" ? -1 : 1
                                switchSession(dir)
                            }
                        }

                        // Icon for keys with iconSource (arrows, keyboard)
                        IconButton {
                            anchors.centerIn: parent
                            visible: keyDef && keyDef.iconSource !== undefined
                            icon.source: keyDef && keyDef.iconSource !== undefined
                                       ? "image://theme/" + keyDef.iconSource : ""
                            highlighted: keyDelegate.highlighted
                            enabled: false
                        }

                        // Label for text keys (Tab, Esc, Ctrl, Alt, F-keys, etc.)
                        Label {
                            anchors.centerIn: parent
                            visible: !keyDef || keyDef.iconSource === undefined
                            text: keyDef ? keyDef.label : ""
                            font.pixelSize: Theme.fontSizeSmall
                            color: keyDelegate.highlighted ? Theme.highlightColor : Theme.primaryColor
                        }
                    }
                }
            }
        }
    }
}
