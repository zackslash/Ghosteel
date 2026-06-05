import QtQuick 2.0
import Sailfish.Silica 1.0
import com.zackslash.ghosteel 1.0

Page {
    id: sessionPage
    allowedOrientations: Orientation.All

    // Rename dialog
    Component {
        id: renameDialogComponent
        Dialog {
            id: renameDialog
            property int sessionIndex: -1
            property string currentName: ""
            canAccept: renameField.text.trim().length > 0
            onAccepted: {
                SessionManager.setSessionName(sessionIndex, renameField.text.trim())
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
            property int sessionIndex: -1
            property string currentCommand: ""
            canAccept: true // allow empty to clear autorun
            onAccepted: {
                SessionManager.setSessionAutorunCommand(sessionIndex, autorunField.text.trim())
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
        anchors.fill: parent

        model: SessionManager.sessionCount

        header: PageHeader {
            title: qsTr("Sessions")
        }

        delegate: ListItem {
            id: sessionDelegate
            contentHeight: delegateContent.height + Theme.paddingSmall * 2
            highlighted: index === SessionManager.activeSessionIndex

            property string sessionName: SessionManager.sessionName(index)
            property string autorunCommand: SessionManager.sessionAutorunCommand(index)

            onClicked: {
                pageStack.pop()
                SessionManager.switchToSession(index)
            }

            Connections {
                target: SessionManager
                onSessionNameChanged: {
                    if (idx === index)
                        sessionDelegate.sessionName = SessionManager.sessionName(index)
                }
                onSessionAutorunCommandChanged: {
                    if (idx === index)
                        sessionDelegate.autorunCommand = SessionManager.sessionAutorunCommand(index)
                }
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

                    // Session name
                    Label {
                        text: sessionDelegate.sessionName
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
                    text: SessionManager.sessionWorkingDirectory(index)
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
                    visible: sessionDelegate.autorunCommand.length > 0
                    text: "\u25B6 " + sessionDelegate.autorunCommand
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
                            sessionIndex: index,
                            currentName: sessionDelegate.sessionName
                        })
                        pageStack.push(dialog)
                    }
                }
                MenuItem {
                    text: qsTr("Autorun command")
                    onClicked: {
                        var dialog = autorunDialogComponent.createObject(sessionPage, {
                            sessionIndex: index,
                            currentCommand: SessionManager.sessionAutorunCommand(index)
                        })
                        pageStack.push(dialog)
                    }
                }
                MenuItem {
                    text: qsTr("Remove")
                    enabled: SessionManager.sessionCount > 1
                    onClicked: {
                        var id = SessionManager.sessionId(index)
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
