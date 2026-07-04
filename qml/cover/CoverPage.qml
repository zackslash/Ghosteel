import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    // Revision counter — bumped on sort/session changes to force
    // stale displayToActual() bindings to re-evaluate.
    property int _sortRevision: 0

    readonly property int _visibleCount: Math.min(5, SessionManager.sessionCount)

    Label {
        id: titleLabel
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            margins: Theme.paddingSmall
        }
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.highlightColor
        text: appWindow.windowTitle || qsTr("Ghosteel")
        truncationMode: TruncationMode.Fade
    }

    Connections {
        target: SessionManager
        onSortOrderChanged: _sortRevision++
        onActiveSessionIndexChanged: _sortRevision++
        onSessionNameChanged: _sortRevision++
        onSessionKeepAwakeChanged: _sortRevision++
        // Rebuilds after remove/restore don't emit sortOrderChanged —
        // catch them via the post-rebuild signal.
        onSessionsChanged: _sortRevision++
    }

    Column {
        id: sessionList
        anchors {
            top: titleLabel.bottom
            topMargin: Theme.paddingSmall
            left: parent.left
            right: parent.right
            bottom: coverAction.top
            bottomMargin: Theme.paddingSmall
            margins: Theme.paddingLarge
        }
        spacing: Theme.paddingSmall

        // Session count header
        Label {
            width: parent.width
            text: qsTr("%n session(s)", "", SessionManager.sessionCount)
            color: Theme.secondaryHighlightColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }

        // Compact status line — icon ties to the per-row badges; count omitted
        // (redundant with the session-count header and the badges themselves).
        Row {
            width: parent.width
            visible: SessionManager.keepAwakeActive
            spacing: Theme.paddingSmall

            Image {
                source: "image://theme/icon-m-charging"
                width: Theme.iconSizeSmall
                height: Theme.iconSizeSmall
                anchors.verticalCenter: parent.verticalCenter
            }

            Label {
                width: parent.width - Theme.iconSizeSmall - Theme.paddingSmall
                text: qsTr("Keeping device awake")
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeExtraSmall
                truncationMode: TruncationMode.Fade
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Repeater {
            id: sessionRepeater
            model: _visibleCount

            Row {
                property int actualIndex: {
                    // Read _sortRevision so this binding re-evaluates on
                    // sort/rename/switch — QML can't track side-effects
                    // inside displayToActual(), so we need an explicit dep.
                    var _ = _sortRevision
                    return SessionManager.displayToActual(index)
                }
                property bool isActive: actualIndex === SessionManager.activeSessionIndex
                property bool keepAwake: {
                    // Read _sortRevision so this binding re-evaluates on
                    // keepAwake toggle — sessionKeepAwake is Q_INVOKABLE
                    // so QML can't track it without an explicit dependency
                    // (same workaround as actualIndex above).
                    var _ = _sortRevision
                    return SessionManager.sessionKeepAwake(actualIndex)
                }

                spacing: Theme.paddingSmall
                width: parent.width

                // Active indicator dot
                Rectangle {
                    width: Theme.iconSizeSmall
                    height: Theme.iconSizeSmall
                    radius: width / 2
                    color: parent.isActive ? Theme.highlightColor : Theme.secondaryColor
                    anchors.verticalCenter: parent.verticalCenter
                }

                Image {
                    visible: parent.keepAwake
                    source: "image://theme/icon-m-charging"
                    width: Theme.iconSizeSmall
                    height: Theme.iconSizeSmall
                    anchors.verticalCenter: parent.verticalCenter
                }

                Label {
                    width: parent.width - (Theme.iconSizeSmall * 2) - (Theme.paddingSmall * 2)
                    text: {
                        // Depend on _sortRevision so the binding re-evaluates
                        // when a session is renamed (even if the actual index
                        // didn't change, e.g. in manual sort mode).
                        var _ = _sortRevision
                        return SessionManager.sessionDisplayName(parent.actualIndex)
                    }
                    color: parent.isActive ? Theme.highlightColor : Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                    truncationMode: TruncationMode.Fade
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // Overflow indicator when more than 5 sessions exist
        Label {
            width: parent.width
            visible: SessionManager.sessionCount > _visibleCount
            text: "..."
            color: Theme.secondaryColor
            font.pixelSize: Theme.fontSizeSmall
            horizontalAlignment: Text.AlignHCenter
        }
    }

    CoverActionList {
        id: coverAction

        // First action: New session
        CoverAction {
            iconSource: "image://theme/icon-cover-new"
            onTriggered: {
                SessionManager.createSession()
                appWindow.activate()
            }
        }
    }
}
