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

    // Distinguish app-initiated Qt.inputMethod.hide() from compositor-initiated
    // drag-dismiss. On Sailfish/Wayland, drag-dismiss deactivates the
    // wl_text_input context (stops hardware key delivery), while app-initiated
    // hide() only hides the VKB panel (context stays active). Set this flag
    // before every programmatic hide() so the handler below knows to keep the
    // VKB hidden rather than re-showing it.
    property bool _programmaticKeyboardHide: false

    onKeyboardVisibleChanged: {
        if (keyboardVisible) {
            // Keyboard is being shown — clear any stale programmatic-hide flag
            // from a previous no-op hide() (keyboard was already hidden).
            _programmaticKeyboardHide = false
            return
        }
        if (!terminal) return
        if (_programmaticKeyboardHide) {
            // App-initiated hide — context stays active, just suppress
            // auto-show on re-focus so the VKB stays hidden.
            terminal.suppressNextKeyboardAutoShow()
            _programmaticKeyboardHide = false
            terminal.forceActiveFocus()
        } else {
            // Compositor drag-dismiss — context was deactivated. Re-show
            // the VKB after a short delay to avoid racing with the
            // compositor's dismiss processing. Drag-dismiss becomes a
            // no-op (VKB re-appears). Use the toggle button to hide.
            dragDismissReShowTimer.start()
        }
    }

    Timer {
        id: dragDismissReShowTimer
        interval: 300
        onTriggered: {
            // Without this guard, navigating to another page (e.g. Sessions)
            // within the 300ms window would pop the keyboard on that page.
            if (terminal && terminal.visible
                    && page.status === PageStatus.Active
                    && !keyboardVisible) {
                terminal.forceActiveFocus()
                Qt.inputMethod.show()
            }
        }
    }
    // Show session indicator when returning from background
    Connections {
        target: Qt.application
        onStateChanged: {
            if (Qt.application.state === Qt.ApplicationActive && terminal) {
                var idx = currentSessionIndex >= 0 ? currentSessionIndex : SessionManager.activeSessionIndex
                var name = SessionManager.sessionName(idx)
                sessionIndicator.show(name || SessionManager.sessionExecCommand(idx) || qsTr("Session %1").arg(idx + 1))
                // Re-focus and restore keyboard state. The compositor
                // deactivates the text input context when the app is
                // backgrounded, same as drag-dismiss.
                if (SessionManager.sessionKeyboardVisible(idx)) {
                    dragDismissReShowTimer.start()
                } else {
                    terminal.suppressNextKeyboardAutoShow()
                    terminal.forceActiveFocus()
                }
            }
        }
    }

    // Re-suppress keyboard when returning to this page (e.g. from Sessions page).
    // When the user taps the same session, onSessionSwitched fires during the pop
    // animation (page still Inactive) and consumes the suppress flag via
    // forceActiveFocus(). Silica then fires focusInEvent when the page becomes
    // Active, which calls im->show() since the flag is already consumed.
    onStatusChanged: {
        if (status === PageStatus.Active && terminal) {
            var idx = currentSessionIndex >= 0 ? currentSessionIndex : SessionManager.activeSessionIndex
            if (!SessionManager.sessionKeyboardVisible(idx)) {
                terminal.suppressNextKeyboardAutoShow()
                terminal.forceActiveFocus()
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

    // Pending clipboard read dialog parameters (used by keyboard dismiss timer)
    property string pendingClipboardPreview: ""
    property string pendingClipboardKind: ""
    property int pendingClipboardSessionId: -1
    property string pendingClipboardSessionName: ""
    property bool clipboardDialogActive: false  // Guards against stacking read dialogs

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

    // Rate limit clipboard read requests to prevent dialog flooding
    Timer {
        id: clipboardReadCooldown
        interval: 2000
    }

    // Delay pushing clipboard dialog to allow keyboard to dismiss
    Timer {
        id: clipboardReadPushTimer
        interval: 200
        onTriggered: {
            clipboardDialogActive = true
            pageStack.push(clipboardReadDialogComponent, {
                "previewText": pendingClipboardPreview,
                "requestKind": pendingClipboardKind,
                "requestSessionId": pendingClipboardSessionId,
                "sessionName": pendingClipboardSessionName
            })
        }
    }

    // Clipboard read confirmation dialog — shown before sending clipboard to terminal programs
    Component {
        id: clipboardReadDialogComponent
        Dialog {
            id: clipboardReadDialog
            property string previewText: ""
            property string requestKind: "c"
            property int requestSessionId: -1
            property string sessionName: ""
            property bool previewVisible: false
            property int maxPreviewLength: 500
            canAccept: true

            Column {
                width: parent.width

                DialogHeader {
                    title: qsTr("Clipboard access")
                    acceptText: qsTr("Send")
                    cancelText: qsTr("Deny")
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    text: sessionName.length > 0
                          ? qsTr("A program in \"%1\" wants to read your clipboard.").arg(sessionName)
                          : qsTr("A terminal program wants to read your clipboard.")
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.Wrap
                }

                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.horizontalPageMargin
                    spacing: Theme.paddingSmall

                    Label {
                        text: clipboardReadDialog.previewVisible ? qsTr("Hide") : qsTr("Show")
                        color: Theme.highlightColor
                        font.pixelSize: Theme.fontSizeSmall
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    IconButton {
                        icon.source: clipboardReadDialog.previewVisible
                                    ? "image://theme/icon-m-device-upload"
                                    : "image://theme/icon-m-device-download"
                        onClicked: clipboardReadDialog.previewVisible = !clipboardReadDialog.previewVisible
                    }
                }

                Rectangle {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    height: previewLabel.implicitHeight + Theme.paddingMedium * 2
                    color: Theme.rgba(Theme.highlightBackgroundColor, 0.1)
                    radius: Theme.paddingSmall

                    Label {
                        id: previewLabel
                        anchors.fill: parent
                        anchors.margins: Theme.paddingMedium
                        text: clipboardReadDialog.previewVisible
                              ? (clipboardReadDialog.previewText.length > clipboardReadDialog.maxPreviewLength
                                 ? clipboardReadDialog.previewText.substring(0, clipboardReadDialog.maxPreviewLength) + "…"
                                 : clipboardReadDialog.previewText)
                              : "••••••••"
                        color: clipboardReadDialog.previewVisible ? Theme.highlightColor : Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: clipboardReadDialog.previewVisible ? Text.WrapAtWordBoundaryOrAnywhere : Text.PlainText
                        clip: true
                    }
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    visible: clipboardReadDialog.previewVisible
                             && clipboardReadDialog.previewText.length > clipboardReadDialog.maxPreviewLength
                    text: qsTr("Showing first %1 of %2 characters")
                          .arg(clipboardReadDialog.maxPreviewLength)
                          .arg(clipboardReadDialog.previewText.length)
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    wrapMode: Text.Wrap
                }
            }

            onAccepted: {
                clipboardDialogActive = false
                var t = SessionManager.sessionById(requestSessionId)
                if (t) {
                    t.sendClipboardText(previewText, requestKind)
                }
            }
            onRejected: clipboardDialogActive = false
        }
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

    // Link confirmation dialog — shown before opening external URLs
    Component {
        id: linkDialogComponent
        Dialog {
            id: linkDialog
            property string url: ""
            canAccept: url.length > 0

            Column {
                width: parent.width

                DialogHeader {
                    title: qsTr("Open Link")
                    acceptText: qsTr("Open")
                    cancelText: qsTr("Cancel")
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    text: linkDialog.url
                    color: Theme.highlightColor
                    font.pixelSize: Theme.fontSizeMedium
                    wrapMode: Text.Wrap
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    text: qsTr("This will open in your browser")
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                }
            }

            onAccepted: Qt.openUrlExternally(url)
        }
    }

    onCtrlActiveChanged: updateModifiers()
    onAltActiveChanged: updateModifiers()
    onActiveModifiersChanged: {
        if (terminal)
            terminal.stickyModifiers = activeModifiers
    }

    function showLinkDialog(uri) {
        pageStack.push(linkDialogComponent, { "url": uri })
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
        t.pullDownZoneHeight = Theme.itemSizeLarge
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
        var sessionFontSize = SessionManager.activeSessionFontSize()
        t.fontSize = sessionFontSize > 0 ? sessionFontSize : Settings.fontSize
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
        t.linkActivated.disconnect(showLinkDialog)
        t.linkActivated.connect(showLinkDialog)
        t.zoomRequested.disconnect(onZoomRequested)
        t.zoomRequested.connect(onZoomRequested)
        t.pinchingChanged.disconnect(onPinchingChanged)
        t.pinchingChanged.connect(onPinchingChanged)
        t.requestParentInteractive.disconnect(onRequestParentInteractive)
        t.requestParentInteractive.connect(onRequestParentInteractive)
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
        t.linkActivated.disconnect(showLinkDialog)
        t.zoomRequested.disconnect(onZoomRequested)
        t.pinchingChanged.disconnect(onPinchingChanged)
        t.requestParentInteractive.disconnect(onRequestParentInteractive)
        fontSizeOverlay.hide()
        t.visible = false
    }

    function switchSession(direction) {
        var count = SessionManager.sessionCount
        if (count <= 1) return
        var displayIdx = SessionManager.actualToDisplay(SessionManager.activeSessionIndex)
        var nextDisplay = ((displayIdx + direction) % count + count) % count
        SessionManager.switchToSession(nextDisplay)
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
            if (!SessionManager.sessionKeyboardVisible(idx)) {
                programmaticHide()
            } else {
                // im->show() from focusInEvent may not work on startup if the
                // Wayland surface isn't mapped yet. Delay and show explicitly.
                dragDismissReShowTimer.start()
            }
            // Restore keybar state (global setting takes precedence)
            setKeybarOpen(Settings.keybarVisible && SessionManager.sessionKeybarOpen(idx))
            if (keybar.open && keybarFlickable.contentWidth > keybarFlickable.width)
                scrollIndicator.flash()
            // Show session indicator on launch so the user knows which session they're in
            var name = SessionManager.sessionName(idx)
            sessionIndicator.show(name || SessionManager.sessionExecCommand(idx) || qsTr("Session %1").arg(idx + 1))
        }
    }

    // Listen for global keybar setting changes (e.g. from SettingsPage)
    Connections {
        target: Settings
        onKeybarVisibleChanged: {
            if (!Settings.keybarVisible) {
                setKeybarOpen(false)
            } else {
                // Restore per-session state when re-enabled
                var state = sessionUIState[currentSessionId]
                setKeybarOpen(state
                    ? state.kbbar
                    : SessionManager.sessionKeybarOpen(currentSessionIndex))
            }
        }
    }

    // Listen for session switches from SessionManager
    Connections {
        target: SessionManager
        onSessionSwitched: {
            // Save outgoing session's UI state using session ID (stable across removals)
            if (terminal && currentSessionId >= 0) {
                // Keybar: read directly from the panel property (reliable, sync, local)
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
                setKeybarOpen(Settings.keybarVisible && state.kbbar)
            } else {
                setKeybarOpen(Settings.keybarVisible && SessionManager.sessionKeybarOpen(index))
            }
            if (keybar.open && keybarFlickable.contentWidth > keybarFlickable.width)
                scrollIndicator.flash()
            // Ensure keyboard hidden if needed — suppress prevents focus-triggered show,
            // but we also need explicit hide for the case where keyboard was already visible
            var incomingKbRestore = state ? state.kb : SessionManager.sessionKeyboardVisible(index)
            if (!incomingKbRestore) {
                programmaticHide()
            }

            // Show session switch indicator (only when multiple sessions exist)
            if (SessionManager.sessionCount > 1) {
                var name = SessionManager.sessionName(index)
                sessionIndicator.show(name || SessionManager.sessionExecCommand(index) || qsTr("Session %1").arg(index + 1))
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
        onClipboardReadRequest: {
            if (clipboardReadCooldown.running) return
            if (clipboardDialogActive) return  // A read dialog is already open

            var policy = Settings.clipboardReadPolicy  // 0=ask, 1=allow, 2=deny
            var preview = Clipboard.text || ""
            if (policy === 2) return  // deny
            if (policy === 1) {       // allow
                var t = SessionManager.sessionById(sessionId)
                if (t) t.sendClipboardText(preview, kind)
                return
            }

            clipboardReadCooldown.start()
            var idx = SessionManager.sessionIndexById(sessionId)
            var name = idx >= 0 ? SessionManager.sessionName(idx) : ""

            pendingClipboardPreview = preview
            pendingClipboardKind = kind
            pendingClipboardSessionId = sessionId
            pendingClipboardSessionName = name.length > 0 ? name : qsTr("Unknown session")

            programmaticHide()
            clipboardReadPushTimer.start()
        }
        onClipboardTextReady: Clipboard.text = text
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

    // Hide the VKB as an app-initiated action (not compositor drag-dismiss).
    // Bundles the flag + timer stop + hide so the invariant can't be broken
    // by a future call site forgetting one of the three.
    function programmaticHide() {
        dragDismissReShowTimer.stop()
        _programmaticKeyboardHide = true
        Qt.inputMethod.hide()
    }

    function setKeybarOpen(value) {
        keybar.open = value
    }
    function onToggleKeybar() {
        if (!Settings.keybarVisible) return
        setKeybarOpen(!keybar.open)
        SessionManager.setSessionKeybarOpen(currentSessionIndex, keybar.open)
        var state = sessionUIState[currentSessionId] || {
            kb: SessionManager.sessionKeyboardVisible(currentSessionIndex),
            kbbar: keybar.open
        }
        state.kbbar = keybar.open
        sessionUIState[currentSessionId] = state
    }

    function onZoomRequested(delta) {
        if (!terminal) return
        SessionManager.setActiveSessionFontSize(
            Math.max(6, Math.min(32, terminal.fontSize + delta)))
    }

    function onPinchingChanged(pinching) {
        if (pinching) {
            fontSizeOverlay.show()
        } else {
            fontSizeOverlay.hide()
            if (terminal)
                SessionManager.setActiveSessionFontSize(terminal.fontSize)
        }
    }

    // Disable the wrapping SilicaFlickable during multi-touch gestures so it
    // cannot steal the sequence and open the PullDownMenu. Re-enabled on end.
    function onRequestParentInteractive(interactive) {
        terminalFlickable.interactive = interactive
    }

    SilicaFlickable {
        id: terminalFlickable
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: keybar.top
        contentHeight: height

        PullDownMenu {
            MenuItem {
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"))
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
                text: qsTr("New session")
                onClicked: {
                    SessionManager.createSession()
                }
            }
            MenuItem {
                text: qsTr("Sessions")
                onClicked: pageStack.push(Qt.resolvedUrl("SessionPage.qml"))
            }
        }

        // Container for the active terminal session
        Item {
            id: terminalContainer
            anchors.fill: parent
        }

        // GL Renderer overlay
        GLRenderer {
            id: glOverlay
            anchors.fill: terminalContainer
            source: terminal
            z: 1
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

    // Font size indicator overlay (shown during pinch-to-zoom)
    Rectangle {
        id: fontSizeOverlay
        anchors.centerIn: parent
        width: Math.max(fontSizeLabel.implicitWidth, 120) + Theme.horizontalPageMargin * 4
        height: Theme.paddingLarge + fontSizeLabel.implicitHeight + Theme.paddingMedium + barTrack.height + Theme.paddingLarge
        radius: Theme.paddingMedium
        color: Theme.rgba(Theme.highlightBackgroundColor, 0.9)
        opacity: 0.0
        visible: opacity > 0
        z: 100  // Same layer as sessionIndicator

        Behavior on opacity {
            FadeAnimator { duration: 200 }  // Match existing sessionIndicator timing
        }

        Label {
            id: fontSizeLabel
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Theme.paddingLarge
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeExtraLarge
            text: terminal ? terminal.fontSize + "pt" : ""
        }

        Rectangle {
            id: barTrack
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: fontSizeLabel.bottom
            anchors.topMargin: Theme.paddingMedium
            width: parent.width - Theme.paddingLarge * 2
            height: Theme.paddingSmall
            radius: height / 2
            color: Theme.rgba(Theme.highlightColor, 0.2)

            Rectangle {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width * Math.max(0, Math.min(1, ((terminal ? terminal.fontSize : 6) - 6) / (32 - 6)))
                height: parent.height
                radius: height / 2
                color: Theme.highlightColor
            }
        }

        function show() {
            opacity = 1.0
        }

        function hide() {
            opacity = 0.0
        }
    }

    // Extra terminal keys panel — plain Item (not DockedPanel, whose C++
    // drag-to-close cannot be reliably overridden from QML). We animate y
    // manually; the terminal flickable anchors to keybar.top and tracks it.
    Item {
        id: keybar
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.itemSizeMedium + Theme.paddingSmall
        y: open ? parent.height - height : parent.height
        property bool open: false  // set by setKeybarOpen(), onToggleKeybar(), etc.

        Behavior on y {
            NumberAnimation { duration: 200; easing.type: Easing.InOutQuad }
        }

        // Match DockedPanel's default translucent gradient background
        PanelBackground {
            anchors.fill: parent
            position: Dock.Bottom
        }

        SilicaFlickable {
            id: keybarFlickable
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
                                if (newVisible) {
                                    if (terminal) terminal.forceActiveFocus()
                                    Qt.inputMethod.show()
                                } else {
                                    programmaticHide()
                                }
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
                            } else if (keyDef.id === "zoomIn") {
                                SessionManager.setActiveSessionFontSize(Math.min(32, terminal.fontSize + 1))
                            } else if (keyDef.id === "zoomOut") {
                                SessionManager.setActiveSessionFontSize(Math.max(6, terminal.fontSize - 1))
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

        // Custom scroll indicator — HorizontalScrollDecorator has no
        // programmatic flash API.
        Rectangle {
            id: scrollIndicator
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2
            color: "transparent"
            visible: keybarFlickable.contentWidth > keybarFlickable.width
            opacity: 0.0

            Rectangle {
                id: indicatorBar
                height: parent.height
                color: Theme.primaryColor
                radius: height / 2
                width: keybarFlickable.contentWidth > 0
                       ? Math.max(20, keybarFlickable.width * (keybarFlickable.width / keybarFlickable.contentWidth))
                       : 20
                x: {
                    var maxScroll = keybarFlickable.contentWidth - keybarFlickable.width
                    if (maxScroll <= 0) return 0
                    return (keybarFlickable.width - width)
                        * Math.max(0, Math.min(1, keybarFlickable.contentX / maxScroll))
                }
            }

            function flash() {
                flashAnim.stop()
                scrollFadeIn.stop()
                scrollFadeOut.stop()
                flashTimer.start()
            }

            // Delay so the panel slide animation finishes first
            Timer {
                id: flashTimer
                interval: 300
                onTriggered: flashAnim.start()
            }

            SequentialAnimation {
                id: flashAnim
                NumberAnimation { target: scrollIndicator; property: "opacity"; to: 0.8; duration: 200 }
                PauseAnimation { duration: 800 }
                NumberAnimation { target: scrollIndicator; property: "opacity"; to: 0.0; duration: 800 }
            }

            // Show on manual scroll (fade in/out like stock HorizontalScrollDecorator)
            Connections {
                target: keybarFlickable
                onMovementStarted: {
                    if (!flashAnim.running)
                        scrollFadeIn.start()
                }
                onMovementEnded: {
                    if (!flashAnim.running)
                        scrollFadeOut.start()
                }
            }

            NumberAnimation {
                id: scrollFadeIn
                target: scrollIndicator
                property: "opacity"
                to: 0.6
                duration: 200
            }

            NumberAnimation {
                id: scrollFadeOut
                target: scrollIndicator
                property: "opacity"
                to: 0.0
                duration: 500
            }
        }

        onOpenChanged: {
            if (open && keybarFlickable.contentWidth > keybarFlickable.width)
                scrollIndicator.flash()
            else if (!open) {
                flashAnim.stop()
                scrollFadeIn.stop()
                scrollFadeOut.stop()
            }
        }
    }
}
