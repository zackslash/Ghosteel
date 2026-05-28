import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
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
        text: appWindow.windowTitle || "Ghosteel"
        truncationMode: TruncationMode.Fade
    }

    // Session count indicator
    Label {
        id: sessionCountLabel
        anchors {
            top: titleLabel.bottom
            topMargin: Theme.paddingSmall
            left: parent.left
            right: parent.right
            margins: Theme.paddingSmall
        }
        font.pixelSize: Theme.fontSizeExtraSmall
        color: Theme.secondaryColor
        text: SessionManager.sessionCount + " session" + (SessionManager.sessionCount !== 1 ? "s" : "")
        visible: SessionManager.sessionCount > 1
    }

    Label {
        anchors {
            top: sessionCountLabel.visible ? sessionCountLabel.bottom : titleLabel.bottom
            topMargin: Theme.paddingSmall
            left: parent.left
            right: parent.right
            bottom: coverAction.top
            margins: Theme.paddingSmall
        }
        font.pixelSize: Theme.fontSizeTiny
        font.family: "DejaVu Sans Mono"
        color: Theme.primaryColor
        verticalAlignment: Text.AlignBottom
        wrapMode: Text.Wrap
        maximumLineCount: 8
        text: "Ghosteel terminal\nfor SailfishOS"
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
