import QtQuick 2.0
import Sailfish.Silica 1.0
import com.zackslash.ghosteel 1.0

CoverBackground {
    // Revision counter — bumped on sort/session changes to force
    // stale displayToActual() bindings to re-evaluate.
    property int _sortRevision: 0

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

        Repeater {
            id: sessionRepeater
            model: Math.min(3, SessionManager.sessionCount)

            Row {
                property int actualIndex: {
                    var _ = _sortRevision // force re-evaluation on sort change
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
                    text: SessionManager.sessionName(parent.actualIndex)
                    color: parent.isActive ? Theme.highlightColor : Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                    truncationMode: TruncationMode.Fade
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
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
