import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Share 1.0
import Nemo.Notifications 1.0
import com.zackslash.ghosteel 1.0
import QtGraphicalEffects 1.0
import "KeyCatalog.js" as KeyCatalog

Page {
    id: page
    allowedOrientations: Orientation.All
    // Free the horizontal axis for session-swiping by disarming the PageStack's
    // horizontal back-gesture filter (same idiom as Jolla Gallery's full-screen
    // photo page). The system edge-peek is compositor-owned and unaffected.
    navigationStyle: PageNavigation.Vertical

    // GhosttyMods bitmask constants
    readonly property int modsCtrl: 2    // GHOSTTY_MODS_CTRL
    readonly property int modsAlt: 4     // GHOSTTY_MODS_ALT

    // Track active modifiers for virtual keyboard sticky keys
    property int activeModifiers: 0

    // --- Session-swipe (horizontal drag → switch session) animation state ---
    property real swipePanX: 0          // drives glOverlayWrapper.transform.x (live content)
    property real snapshotPanX: 0       // drives snapshotSource.transform.x (frozen old frame)
    property bool swipeActive: false    // true during a live/animating swipe
    property string swipePhase: "idle"  // state-machine phase: idle|transit|sliding|return
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
                sessionIndicator.show(SessionManager.sessionDisplayName(idx))
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
    // onSessionSwitched may have consumed the suppress flag during the pop
    // animation (page still Inactive). Re-apply before forceActiveFocus() to
    // prevent im->show() when the session's keyboard is hidden.
    onStatusChanged: {
        if (status === PageStatus.Active && terminal) {
            var idx = currentSessionIndex >= 0 ? currentSessionIndex : SessionManager.activeSessionIndex
            if (!SessionManager.sessionKeyboardVisible(idx)) {
                terminal.suppressNextKeyboardAutoShow()
            }
            terminal.forceActiveFocus()
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

    // Haptic feedback notification — publishing a notification triggers system vibration
    Notification {
        id: bellNotification
        appName: "Ghosteel"
        summary: qsTr("Bell")
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

    // Returns text/indicator color for the current terminal color scheme.
    // Dark scheme: ambience primary; light scheme: dark primary for white keybar.
    function schemeTextColor() {
        return Settings.colorScheme === "light" ? Theme.darkPrimaryColor : Theme.primaryColor
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

    function attachTerminal(t, focus) {
        if (!t) return
        // Parent into container once — never reparent, just toggle visibility
        if (t.parent !== terminalContainer) {
            t.parent = terminalContainer
            t.anchors.fill = terminalContainer
        }
        t.visible = true
        t.opacity = 1
        var sessionFontSize = SessionManager.activeSessionFontSize
        t.fontSize = sessionFontSize > 0 ? sessionFontSize : Settings.fontSize
        applyTerminalTheme(t)
        if (focus !== false)
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
        t.sessionSwipeStarted.disconnect(onSessionSwipeStarted)
        t.sessionSwipeStarted.connect(onSessionSwipeStarted)
        t.sessionSwipeProgress.disconnect(onSessionSwipeProgress)
        t.sessionSwipeProgress.connect(onSessionSwipeProgress)
        t.sessionSwipeCommitted.disconnect(onSessionSwipeCommitted)
        t.sessionSwipeCommitted.connect(onSessionSwipeCommitted)
        t.sessionSwipeCancelled.disconnect(onSessionSwipeCancelled)
        t.sessionSwipeCancelled.connect(onSessionSwipeCancelled)
        // Gate the C++ classifier: only arm it when more than one session
        // exists, so a horizontal drag with a single session can't kill the
        // long-press (text-selection) timer before QML rejects the gesture.
        t.sessionSwipeEnabled = SessionManager.sessionCount > 1
        terminal = t
        updateWindowTitle()

        // Sync keybar modifier display to the incoming terminal's actual state.
        // Without this, switching back to a session where Ctrl was toggled shows
        // the keybar as inactive while the C++ side still applies the modifier.
        var mods = t.stickyModifiers
        ctrlActive = (mods & modsCtrl) !== 0
        altActive = (mods & modsAlt) !== 0
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
        t.sessionSwipeStarted.disconnect(onSessionSwipeStarted)
        t.sessionSwipeProgress.disconnect(onSessionSwipeProgress)
        t.sessionSwipeCommitted.disconnect(onSessionSwipeCommitted)
        t.sessionSwipeCancelled.disconnect(onSessionSwipeCancelled)
        fontSizeOverlay.hide()
        t.visible = false
    }

    function switchSession(direction) {
        var count = SessionManager.sessionCount
        if (count <= 1) return
        // Navigate by ACTUAL (vector) index, not display index. Under
        // SortLastUsed (the default), setActiveSessionIndex bumps lastUsedAt
        // and rebuilds the sorted indices on every switch, so the just-
        // activated session always lands at display index 0. Stepping the
        // display index by +1 then forever selects display index 1 (2nd-most-
        // recent), making "next" bounce between two sessions. Actual-index
        // order only changes on add/remove, so both directions cycle through
        // every session.
        var actualIdx = SessionManager.activeSessionIndex
        var nextActual = ((actualIdx + direction) % count + count) % count
        SessionManager.activeSessionIndex = nextActual
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
            sessionIndicator.show(SessionManager.sessionDisplayName(idx))
        }
    }

    // Keep the swipe gate fresh when sessions are added/removed without a
    // switch (the active terminal stays attached, so attachTerminal /
    // onSessionSwitched won't re-apply it).
    Connections {
        target: SessionManager
        onSessionCountChanged: {
            if (terminal)
                terminal.sessionSwipeEnabled = SessionManager.sessionCount > 1
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
            // Abort an in-flight swipe if the switch came from a non-swipe path
            // (keybar/keyboard) mid-gesture. swipePhase "transit" marks the swipe's
            // own synchronous switchSession call and is exempt — without it the
            // swipe would cancel its own commit.
            if (swipeActive && swipePhase !== "transit") {
                swipePhase = "idle"
                swipeActive = false
                snapshotOutAnim.stop(); swipeInAnim.stop(); swipeReturnAnim.stop()
                swipePanX = 0
                snapshotPanX = 0
            }

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

            // Clear modifiers on switch-out; attachTerminal re-syncs from the
            // incoming terminal's stickyModifiers on switch-in.
            ctrlActive = false
            altActive = false

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
                attachTerminal(newTerminal, status === PageStatus.Active)
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
            // Skip the scroll-indicator flash when this switch was the swipe's
            // own commit (swipePhase "transit"); show it for keybar/keyboard/
            // SessionPage switches as before.
            if (keybar.open && keybarFlickable.contentWidth > keybarFlickable.width && swipePhase !== "transit")
                scrollIndicator.flash()
            // Ensure keyboard hidden if needed — suppress prevents focus-triggered show,
            // but we also need explicit hide for the case where keyboard was already visible
            var incomingKbRestore = state ? state.kb : SessionManager.sessionKeyboardVisible(index)
            if (!incomingKbRestore) {
                programmaticHide()
            }

            // Show session switch indicator (only when multiple sessions exist)
            if (SessionManager.sessionCount > 1) {
                sessionIndicator.show(SessionManager.sessionDisplayName(index))
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
            var preview = SessionManager.clipboardText()
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
        onClipboardTextReady: SessionManager.setClipboardText(text)
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
            bellFeedback.playBell()
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
            Math.max(Settings.minFontSize, Math.min(Settings.maxFontSize, terminal.fontSize + delta)), false)
    }

    function onPinchingChanged(pinching) {
        if (pinching) {
            fontSizeOverlay.show()
        } else {
            fontSizeOverlay.hide()
            if (terminal) {
                if (terminal.pinchAtDefault)
                    SessionManager.setActiveSessionFontSize(0, false)
                else
                    SessionManager.setActiveSessionFontSize(terminal.fontSize, false)
            }
        }
    }

    // Disable the wrapping SilicaFlickable during multi-touch gestures so it
    // cannot steal the sequence and open the PullDownMenu. Re-enabled on end.
    function onRequestParentInteractive(interactive) {
        terminalFlickable.interactive = interactive
    }

    // --- Session-swipe handlers (connected to the active terminal in
    //     attachTerminal / detached in detachTerminal) ---
    // UX: the live drag still shows the current session following the finger;
    // on commit, a grabbed frame of the OLD session slides out while the NEW
    // (synchronously swapped) content slides in from the opposite side — both
    // visible, tiling edge-to-edge, so the transition looks connected.
    function onSessionSwipeStarted() {
        if (SessionManager.sessionCount <= 1) {
            swipePanX = 0
            return
        }
        swipePhase = "idle"
        snapshotOutAnim.stop(); swipeInAnim.stop(); swipeReturnAnim.stop()
        swipeActive = true
        // Freeze a frame for the commit slide. ShaderEffectSource samples
        // glOverlay's texture (FBO content, ignoring the live transform);
        // live:false holds the frame, scheduleUpdate() refreshes it per swipe.
        // (grabToImage is broken on Qt 5.6/Sailfish — itemgrabber:// URL
        // isn't resolvable by Image.)
        snapshotSource.scheduleUpdate()
    }
    function onSessionSwipeProgress(deltaX) {
        if (!swipeActive || swipePhase !== "idle") return
        swipePanX = deltaX
    }
    function onSessionSwipeCommitted(dir) {
        if (!swipeActive) return
        if (SessionManager.sessionCount <= 1) { swipePanX = 0; swipeActive = false; return }

        var startPan = swipePanX   // signed finger position at lift-off

        // Reveal the frozen old frame at A's current position
        snapshotPanX = startPan

        // Synchronous swap: glOverlay rebinds to B. Phase "transit" exempts the
        // swipe's own switchSession from the abort guard in onSessionSwitched.
        swipePhase = "transit"
        switchSession(dir)

        // Position B on the opposite side of A so they tile edge-to-edge:
        //   dir>0 (leftward/next): A exits left, B enters from the right
        //   dir<0 (rightward/prev): A exits right, B enters from the left
        // All of the above ran in one JS tick — Qt Quick paints no intermediate
        // frame, so B never flashes mid-screen.
        swipePanX = startPan + (dir > 0 ? width : -width)
        swipePhase = "sliding"

        // Parallel slide: snapshot (old) out one side, live (new) in from the other.
        snapshotOutAnim.stop()
        snapshotOutAnim.from = snapshotPanX
        snapshotOutAnim.to = (dir > 0 ? -width : width)
        snapshotOutAnim.start()

        swipeInAnim.stop()
        swipeInAnim.from = swipePanX
        swipeInAnim.to = 0
        swipeInAnim.start()
    }
    function onSessionSwipeCancelled() {
        if (!swipeActive) return
        swipePhase = "return"
        snapshotOutAnim.stop(); swipeInAnim.stop()
        swipeReturnAnim.stop()
        swipeReturnAnim.from = swipePanX
        swipeReturnAnim.to = 0
        swipeReturnAnim.start()
    }

    // Parallel commit slide. snapshotOutAnim drives the grabbed old frame
    // (snapshotPanX) out one side; swipeInAnim drives the new live content
    // (swipePanX) in from the other. Both start in the same JS tick and share
    // duration/easing, so they finish together and tile edge-to-edge throughout.
    // onRunningChanged fires on both natural completion and stop(); cleanup is
    // centralized in swipeInAnim and gated by swipePhase to disambiguate.
    NumberAnimation {
        id: snapshotOutAnim
        target: page; property: "snapshotPanX"
        duration: 200; easing.type: Easing.InOutQuad
        // No onRunningChanged — cleanup lives in swipeInAnim
    }
    NumberAnimation {
        id: swipeInAnim
        target: page; property: "swipePanX"
        duration: 200; easing.type: Easing.InOutQuad
        onRunningChanged: if (!running && swipePhase === "sliding") {
            swipePanX = 0
            snapshotPanX = 0
            swipeActive = false
            swipePhase = "idle"
        }
    }
    NumberAnimation {
        id: swipeReturnAnim
        target: page; property: "swipePanX"
        duration: 200; easing.type: Easing.InOutQuad
        onRunningChanged: if (!running && swipePhase === "return") {
            swipePanX = 0
            swipeActive = false
            swipePhase = "idle"
        }
    }

    SilicaFlickable {
        id: terminalFlickable
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // Decouple from keybar's animated y to avoid per-frame terminal resize
        // (C++ geometryChanged → PTY SIGWINCH) during the 200ms slide. The margin
        // snaps to the keybar's resting height immediately; the keybar's own
        // Behavior-on-y provides the visual slide on top (higher z).
        anchors.bottomMargin: keybar.open ? keybar.height : 0
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

        // Frozen frame of the previous session, shown only during the parallel
        // commit slide. live:false + scheduleUpdate() at swipe-start captures a
        // single frame; clip prevents overpainting the keybar while sliding.
        // z:0.5 — below glOverlay(z:1) so fresh content wins sub-pixel seams.
        ShaderEffectSource {
            id: snapshotSource
            sourceItem: glOverlay
            live: false
            hideSource: false
            anchors.fill: terminalContainer
            z: 0.5
            visible: swipePhase === "sliding"
            transform: Translate { x: Math.round(snapshotPanX) }   // snap: tile exactly with the snapped live content during the commit slide
            clip: true
        }

        // GL Renderer overlay. Wrapped in a plain Item whose transform follows
        // swipePanX — never animate the FBO-backed GLRenderer directly (Qt 5
        // transform-node sensitivity), and never move terminalContainer/terminal
        // (cellFromPixel maps event->pos() against the stationary terminal).
        Item {
            id: glOverlayWrapper
            anchors.fill: terminalContainer
            z: 1
            transform: Translate { x: Math.round(swipePanX) }

            GLRenderer {
                id: glOverlay
                anchors.fill: parent
                source: terminal
            }

            // Fills the void the translated GLRenderer vacates. Nested INSIDE the
            // wrapper so it shares the SAME transform as the GLRenderer — they tile
            // pixel-perfectly by construction (a sibling-filler seamed: textured
            // quads and filled rects rasterize through different paths). The FBO is
            // genuinely semi-transparent (bg premultiplied as scheme_bg·bgOpacity),
            // so a filler of the same scheme color at Settings.backgroundOpacity
            // composites identically over the page — no solidify, no double-tint.
            Rectangle {
                id: voidFiller
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                color: Settings.colorScheme === "light" ? "#FFFFFF" : "#1E1E1E"
                opacity: Settings.backgroundOpacity
                visible: swipeActive && swipePhase !== "sliding"
                //   _d < 0 (content slid left):  past the right edge → fills right void
                //   _d > 0 (content slid right): before the left edge → fills left void
                readonly property real _d: Math.round(swipePanX)
                x: _d < 0 ? glOverlay.width : _d * -1
                width: Math.abs(_d)
            }
        }

        // Drag hint: shows which session a release will switch to, sitting in
        // the void the live content vacated. Fades in with drag progress toward
        // the commit threshold and clears the instant the swipe commits or
        // cancels (gated on swipePhase "idle"). The arrow conveys next vs prev.
        // Sits above glOverlayWrapper (z:1) + the voidFiller nested inside it,
        // so the hint text stays visible in the void the content vacated.
        Label {
            id: swipeDragHint
            visible: swipeActive && swipePhase === "idle"
                     && SessionManager.sessionCount > 1
                     && Math.abs(swipePanX) > 4
            anchors.verticalCenter: terminalContainer.verticalCenter
            color: Theme.primaryColor
            font.pixelSize: Theme.fontSizeMedium
            z: 1.5
            opacity: {
                var p = Math.abs(swipePanX) / (terminalContainer.width * 0.25)
                return Math.max(0, Math.min(1, p)) * 0.85
            }
            text: {
                var dir = swipePanX < 0 ? 1 : -1   // leftward → next
                var count = SessionManager.sessionCount
                // Mirror switchSession(): navigate by ACTUAL (vector) index, not
                // display index. Under SortLastUsed the display order shifts on
                // every switch, so display-index math would name the wrong session.
                var actualIdx = SessionManager.activeSessionIndex
                var targetActual = ((actualIdx + dir) % count + count) % count
                var name = SessionManager.sessionDisplayName(targetActual)
                return dir > 0 ? (name + "  ›") : ("‹  " + name)
            }
            x: {
                // Centre the label in the strip the content vacated, clamped on-screen.
                var voidCenter = swipePanX < 0
                                  ? (terminalContainer.width + swipePanX / 2)
                                  : (swipePanX / 2)
                return Math.max(0, Math.min(terminalContainer.width - swipeDragHint.implicitWidth,
                                            voidCenter - swipeDragHint.implicitWidth / 2))
            }
        }

        // Transparent overlay that captures taps to dismiss search panel.
        // Only enabled when search is open; consumes the tap to dismiss (does not pass through).
        MouseArea {
            anchors.fill: parent
            enabled: searchPanel.open
            visible: searchPanel.open
            z: 1
            onPressed: {
                searchPanel.open = false
                if (terminal) terminal.forceActiveFocus()
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
            updateSearchPanelHeight()
        }
        onHeightChanged: updateSearchPanelHeight()

        function updateSearchPanelHeight() {
            if (terminal)
                terminal.searchPanelHeight = open ? height : 0
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
            text: {
                if (!terminal) return ""
                if (terminal.pinchAtDefault) return qsTr("Default")
                return terminal.fontSize + "pt"
            }
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
                width: parent.width * Math.max(0, Math.min(1, ((terminal ? terminal.fontSize : Settings.minFontSize) - Settings.minFontSize) / (Settings.maxFontSize - Settings.minFontSize)))
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
    // manually; the terminal flickable uses a static bottomMargin instead of
    // anchoring to keybar.top, to avoid per-frame resize during the slide.
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

        // Match DockedPanel's default translucent gradient background.
        // Kept visible in both schemes — the overlay tints it for light mode.
        PanelBackground {
            anchors.fill: parent
            position: Dock.Bottom
        }

        // Scheme-dependent tint over PanelBackground. Ensures the keybar
        // always matches the terminal scheme regardless of the ambience.
        // PanelBackground provides the gradient translucency underneath.
        Rectangle {
            anchors.fill: parent
            color: Settings.colorScheme === "light"
                   ? Qt.rgba(1, 1, 1, 0.6)
                   : Qt.rgba(0, 0, 0, 0.5)
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
                                SessionManager.setActiveSessionFontSize(Math.min(Settings.maxFontSize, terminal.fontSize + 1), false)
                            } else if (keyDef.id === "zoomOut") {
                                SessionManager.setActiveSessionFontSize(Math.max(Settings.minFontSize, terminal.fontSize - 1), false)
                            }
                        }

                        // Icon for keys with iconSource (arrows).
                        // ColorOverlay tints icons dark for the light scheme
                        // where Silica theme icons would be invisible on white.
                        Image {
                            id: keyIcon
                            anchors.centerIn: parent
                            visible: keyDef && keyDef.iconSource !== undefined
                            source: keyDef && keyDef.iconSource !== undefined
                                   ? "image://theme/" + keyDef.iconSource : ""
                            width: Theme.iconSizeMedium
                            height: Theme.iconSizeMedium
                        }

                        ColorOverlay {
                            anchors.fill: keyIcon
                            source: keyIcon
                            visible: keyIcon.visible
                            // Highlighted (Ctrl/Alt/keyboard active): accent color.
                            // Light scheme: dark tint. Dark scheme: #FFFFFF = no-op.
                            color: keyDelegate.highlighted ? Theme.highlightColor
                                : (Settings.colorScheme === "light" ? Theme.darkPrimaryColor : "#FFFFFF")
                        }

                        // Label for text keys (Tab, Esc, Ctrl, Alt, F-keys, etc.)
                        Label {
                            anchors.centerIn: parent
                            visible: !keyDef || keyDef.iconSource === undefined
                            text: keyDef ? keyDef.label : ""
                            font.pixelSize: Theme.fontSizeSmall
                            color: keyDelegate.highlighted ? Theme.highlightColor : page.schemeTextColor()
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
                color: page.schemeTextColor()
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
