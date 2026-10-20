import QtQuick
import QtQuick.Layouts

// Static, high-contrast error banner (inverted). No auto-dismiss, no animation.
Rectangle {
    id: banner
    property string message: ""
    signal dismissed()

    color: Theme.ink
    implicitHeight: row.implicitHeight + 2 * 24

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 24
        spacing: 24
        Text {
            Layout.fillWidth: true
            text: banner.message
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 4
            elide: Text.ElideRight
            color: Theme.paper
            font.pixelSize: Theme.fontBody
            font.bold: true
        }
        SMButton {
            text: "Dismiss"
            inverted: false
            onClicked: banner.dismissed()
        }
    }
}
