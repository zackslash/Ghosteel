import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: sessionPage
    objectName: "sessionPage"
    allowedOrientations: Orientation.All

    property string sortDescription: ""
    Component.onCompleted: updateSortDescription()
    function updateSortDescription() {
        var mode = SessionManager.sortMode()
        if (mode === 1) sortDescription = qsTr("Sorted by last used")
        else if (mode === 2) sortDescription = qsTr("Sorted by created")
        else if (mode === 3) sortDescription = qsTr("Sorted by name")
        else sortDescription = ""
    }

    // Keep the sort description in sync when the sort mode changes.
    Connections {
        target: SessionManager
        onSortOrderChanged: updateSortDescription()
    }

    // Rename dialog
    Component {
        id: renameDialogComponent
        Dialog {
            id: renameDialog
            // Capture the stable session id; resolve the index fresh on accept (removal-safe).
            property int sessionId: -1
            property string currentName: ""
            canAccept: renameField.text.trim().length > 0
            onAccepted: {
                var idx = SessionManager.sessionIndexById(sessionId)
                if (idx < 0) return
                SessionManager.setSessionName(idx, renameField.text.trim())
            }
            onStatusChanged: {
                if (status === PageStatus.Active) {
                    renameField.forceActiveFocus()
                }
            }
            Column {
                width: parent.width
                DialogHeader {
                    acceptText: qsTr("Rename")
                }
                TextField {
                    id: renameField
                    width: parent.width
                    label: qsTr("Session name")
                    text: renameDialog.currentName
                    focus: true
                    EnterKey.enabled: text.trim().length > 0
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.onClicked: renameDialog.accept()
                }
            }
        }
    }

    // Autorun command dialog
    Component {
        id: autorunDialogComponent
        Dialog {
            id: autorunDialog
            // Capture the stable session id; resolve the index fresh on accept (removal-safe).
            property int sessionId: -1
            property string currentCommand: ""
            canAccept: true // allow empty to clear autorun
            onAccepted: {
                var idx = SessionManager.sessionIndexById(sessionId)
                if (idx < 0) return
                SessionManager.setSessionAutorunCommand(idx, autorunField.text.trim())
            }
            onStatusChanged: {
                if (status === PageStatus.Active) {
                    autorunField.forceActiveFocus()
                }
            }
            Column {
                width: parent.width
                DialogHeader {
                    acceptText: qsTr("Autorun")
                }
                TextField {
                    id: autorunField
                    width: parent.width
                    label: qsTr("Command to run on startup")
                    text: autorunDialog.currentCommand
                    placeholderText: qsTr("e.g. htop")
                    focus: true
                    EnterKey.iconSource: "image://theme/icon-m-enter-accept"
                    EnterKey.onClicked: autorunDialog.accept()
                }
            }
        }
    }

    SilicaListView {
        id: sessionList
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: newSessionButton.top
        }

        model: SessionManager

        header: PageHeader {
            title: qsTr("Sessions")
            description: sessionPage.sortDescription
        }

        PullDownMenu {
            MenuItem {
                text: qsTr("Sort by last used")
                onClicked: SessionManager.setSortMode(1)
            }
            MenuItem {
                text: qsTr("Sort by name")
                onClicked: SessionManager.setSortMode(3)
            }
            MenuItem {
                text: qsTr("Sort by created")
                onClicked: SessionManager.setSortMode(2)
            }
        }

        delegate: ListItem {
            id: sessionDelegate
            contentHeight: delegateContent.height + Theme.paddingSmall * 2
            highlighted: model.isActive

            onClicked: {
                pageStack.pop()
                SessionManager.switchToSession(index)
            }

            Column {
                id: delegateContent
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: Theme.horizontalPageMargin
                    rightMargin: Theme.horizontalPageMargin
                }
                spacing: Theme.paddingSmall

                Row {
                    spacing: Theme.paddingMedium
                    width: parent.width

                    // Active session indicator dot
                    Rectangle {
                        width: Theme.iconSizeSmall
                        height: Theme.iconSizeSmall
                        radius: width / 2
                        color: sessionDelegate.highlighted
                               ? Theme.highlightColor
                               : Theme.primaryColor
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    // Session name (or exec command for -e sessions)
                    Label {
                        text: model.displayName
                        color: sessionDelegate.highlighted
                               ? Theme.highlightColor
                               : Theme.primaryColor
                        font.pixelSize: Theme.fontSizeMedium
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                // Working directory subtitle
                Label {
                    visible: text.length > 0
                    text: model.workingDirectory
                    color: sessionDelegate.highlighted
                           ? Theme.secondaryHighlightColor
                           : Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    anchors.leftMargin: Theme.iconSizeSmall + Theme.paddingMedium
                }

                // Autorun command subtitle
                Label {
                    visible: model.autorunCommand.length > 0
                    text: "\u25B6 " + model.autorunCommand
                    color: sessionDelegate.highlighted
                           ? Theme.secondaryHighlightColor
                           : Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    font.italic: true
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    anchors.leftMargin: Theme.iconSizeSmall + Theme.paddingMedium
                }
            }

            menu: ContextMenu {
                MenuItem {
                    text: qsTr("Rename")
                    onClicked: {
                        var dialog = renameDialogComponent.createObject(sessionPage, {
                            sessionId: model.id,
                            currentName: model.name
                        })
                        pageStack.push(dialog)
                    }
                }
                MenuItem {
                    text: qsTr("Autorun command")
                    onClicked: {
                        var dialog = autorunDialogComponent.createObject(sessionPage, {
                            sessionId: model.id,
                            currentCommand: model.autorunCommand
                        })
                        pageStack.push(dialog)
                    }
                }
                MenuItem {
                    text: qsTr("Remove")
                    enabled: SessionManager.sessionCount > 1
                    onClicked: {
                        var id = model.id
                        sessionDelegate.remorseAction(
                            qsTr("Removing session"),
                            function() { SessionManager.removeSessionById(id) }
                        )
                    }
                }
            }

            // Visual separator between items
            Separator {
                anchors.bottom: parent.bottom
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.horizontalCenter: parent.horizontalCenter
                color: Theme.rgba(Theme.highlightColor, 0.2)
                horizontalAlignment: Qt.AlignHCenter
            }
        }

        VerticalScrollDecorator {}
    }

    // New session button at bottom
    BackgroundItem {
        id: newSessionButton
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.itemSizeMedium

        onClicked: {
            pageStack.pop()
            SessionManager.createSession()
        }

        Row {
            anchors.centerIn: parent
            spacing: Theme.paddingMedium

            Image {
                source: "image://theme/icon-m-add"
                anchors.verticalCenter: parent.verticalCenter
            }

            Label {
                text: qsTr("New Session")
                color: newSessionButton.highlighted
                       ? Theme.highlightColor
                       : Theme.primaryColor
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
}
