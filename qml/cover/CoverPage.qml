import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
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

    Column {
        id: sessionList
        anchors {
            top: titleLabel.bottom
            left: parent.left
            right: parent.right
            bottom: coverAction.top
            margins: Theme.paddingLarge
            topMargin: Theme.paddingSmall
            bottomMargin: Theme.paddingSmall
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
            model: SessionManager

            Row {
                // Cap the list at 5; hidden rows take no space in the Column.
                visible: index < _visibleCount
                spacing: Theme.paddingSmall
                width: parent.width

                // Active indicator dot
                Rectangle {
                    width: Theme.iconSizeSmall
                    height: Theme.iconSizeSmall
                    radius: width / 2
                    color: model.isActive ? Theme.highlightColor : Theme.secondaryColor
                    anchors.verticalCenter: parent.verticalCenter
                }

                Image {
                    visible: model.keepAwake
                    source: "image://theme/icon-m-charging"
                    width: Theme.iconSizeSmall
                    height: Theme.iconSizeSmall
                    anchors.verticalCenter: parent.verticalCenter
                }

                Label {
                    width: parent.width - Theme.iconSizeSmall - Theme.paddingSmall
                           - (model.keepAwake ? Theme.iconSizeSmall + Theme.paddingSmall : 0)
                    text: model.displayName
                    color: model.isActive ? Theme.highlightColor : Theme.secondaryColor
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
