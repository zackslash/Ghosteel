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

                Label {
                    width: parent.width - Theme.iconSizeSmall - Theme.paddingSmall
                    text: {
                        // Depend on _sortRevision so the binding re-evaluates
                        // when a session is renamed (even if the actual index
                        // didn't change, e.g. in manual sort mode).
                        var _ = _sortRevision
                        return SessionManager.sessionName(parent.actualIndex)
                               || SessionManager.sessionExecCommand(parent.actualIndex)
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
