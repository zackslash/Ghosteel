import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    allowedOrientations: Orientation.All

    // Fetched once on entry; the font scan must not re-run on every binding eval.
    property var families: []

    Component.onCompleted: {
        families = FontCatalog.monospaceFamilies(Settings.fontFamily)
    }

    SilicaListView {
        anchors.fill: parent
        model: families

        header: PageHeader {
            title: qsTr("Font")
        }

        delegate: BackgroundItem {
            id: fontDelegate
            width: parent.width
            height: Theme.itemSizeMedium
            onClicked: {
                Settings.fontFamily = modelData
                pageStack.pop()
            }

            Column {
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: Theme.horizontalPageMargin
                    rightMargin: Theme.horizontalPageMargin
                }
                spacing: Theme.paddingSmall

                Label {
                    text: modelData === "monospace" ? qsTr("Monospace (default)") : modelData
                    color: fontDelegate.highlighted ? Theme.highlightColor : Theme.primaryColor
                    font.pixelSize: Theme.fontSizeMedium
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                }

                Label {
                    text: "nemo@phone:~ $ ls -la"
                    font.family: modelData
                    font.pixelSize: Theme.fontSizeSmall
                    color: fontDelegate.highlighted ? Theme.highlightColor : Theme.secondaryColor
                }
            }

            Image {
                source: "image://theme/icon-m-accept"
                anchors.right: parent.right
                anchors.rightMargin: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                // Exact string equality is intentional: a stored family whose
                // casing differs from the catalog entry gets no checkmark.
                visible: modelData === Settings.fontFamily
            }
        }

        VerticalScrollDecorator {}
    }
}