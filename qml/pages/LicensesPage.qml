import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: licensesPage

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        VerticalScrollDecorator {}

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge

            PageHeader {
                title: qsTr("Licenses")
            }

            // Ghosteel
            SectionHeader {
                text: qsTr("Ghosteel")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text:
                    "MIT License\n\n" +
                    "Copyright (c) 2026 Luke Hines\n\n" +
                    "Permission is hereby granted, free of charge, to any person obtaining a copy " +
                    "of this software and associated documentation files (the \"Software\"), to deal " +
                    "in the Software without restriction, including without limitation the rights " +
                    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell " +
                    "copies of the Software, and to permit persons to whom the Software is " +
                    "furnished to do so, subject to the following conditions:\n\n" +
                    "The above copyright notice and this permission notice shall be included in all " +
                    "copies or substantial portions of the Software.\n\n" +
                    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR " +
                    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, " +
                    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE " +
                    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER " +
                    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, " +
                    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE " +
                    "SOFTWARE."
            }

            // Ghostty
            SectionHeader {
                text: qsTr("Ghostty (libghostty-vt)")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text:
                    "MIT License\n\n" +
                    "Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors\n\n" +
                    "Permission is hereby granted, free of charge, to any person obtaining a copy " +
                    "of this software and associated documentation files (the \"Software\"), to deal " +
                    "in the Software without restriction, including without limitation the rights " +
                    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell " +
                    "copies of the Software, and to permit persons to whom the Software is " +
                    "furnished to do so, subject to the following conditions:\n\n" +
                    "The above copyright notice and this permission notice shall be included in all " +
                    "copies or substantial portions of the Software.\n\n" +
                    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR " +
                    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, " +
                    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE " +
                    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER " +
                    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, " +
                    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE " +
                    "SOFTWARE."
            }

            // Cursor trail shader
            SectionHeader {
                text: qsTr("Cursor trail shader")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.primaryColor
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text:
                    "MIT License\n\n" +
                    "Copyright (c) 2026 H. Cederblad\n\n" +
                    "Based on https://github.com/hced/ghostty-cursor-trails\n\n" +
                    "Permission is hereby granted, free of charge, to any person obtaining a copy " +
                    "of this software and associated documentation files (the \"Software\"), to deal " +
                    "in the Software without restriction, including without limitation the rights " +
                    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell " +
                    "copies of the Software, and to permit persons to whom the Software is " +
                    "furnished to do so, subject to the following conditions:\n\n" +
                    "The above copyright notice and this permission notice shall be included in all " +
                    "copies or substantial portions of the Software.\n\n" +
                    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR " +
                    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, " +
                    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE " +
                    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER " +
                    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, " +
                    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE " +
                    "SOFTWARE."
            }

            Item { width: 1; height: Theme.paddingLarge }
        }
    }
}
