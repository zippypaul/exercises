import QtQuick
import QtQuick.Controls

// High-contrast, non-animated button. Pressed state inverts colours (one repaint, no transition).
Button {
    id: control
    property bool inverted: false
    readonly property bool dark: control.down || control.inverted

    implicitHeight: Theme.tap
    implicitWidth: Math.max(Theme.tap * 2, label.implicitWidth + 2 * Theme.margin)
    hoverEnabled: false
    focusPolicy: Qt.NoFocus
    font.pixelSize: Theme.fontButton
    font.bold: true

    contentItem: Text {
        id: label
        text: control.text
        font: control.font
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: !control.enabled ? Theme.disabled : (control.dark ? Theme.paper : Theme.ink)
    }
    background: Rectangle {
        color: control.enabled && control.dark ? Theme.ink : Theme.paper
        border.width: Theme.border
        border.color: control.enabled ? Theme.ink : Theme.disabled
    }
}
