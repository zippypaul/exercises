import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page
    objectName: "signin"
    signal cancelled()

    readonly property bool hasCode: triage.deviceCodeUserCode !== ""

    Rectangle { anchors.fill: parent; color: Theme.paper }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 60
        spacing: 48

        Text {
            Layout.fillWidth: true
            text: "Sign in to Microsoft 365"
            font.pixelSize: Theme.fontTitle
            font.bold: true
            color: Theme.ink
            wrapMode: Text.Wrap
        }
        Rectangle { Layout.fillWidth: true; height: Theme.border; color: Theme.ink }

        // --- not started yet ------------------------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 36
            visible: !page.hasCode
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSubject
                color: Theme.ink
                text: "Slow Mail needs permission to read and organise your work mailbox. You sign in on another device; no password is typed here."
            }
            Text {
                Layout.fillWidth: true
                visible: triage.signInMessage !== ""
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontBody
                font.bold: true
                color: Theme.ink
                textFormat: Text.PlainText
                text: triage.signInMessage
            }
            SMButton {
                Layout.fillWidth: true
                Layout.preferredHeight: 140
                text: "Start sign-in"
                enabled: !triage.busy
                onClicked: triage.beginSignIn()
            }
        }

        // --- code shown -----------------------------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 36
            visible: page.hasCode
            Text {
                Layout.fillWidth: true
                text: "On another device, open:"
                font.pixelSize: Theme.fontSubject
                color: Theme.ink
            }
            Text {
                Layout.fillWidth: true
                text: triage.deviceCodeUri
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                font.pixelSize: 52
                font.bold: true
                color: Theme.ink
            }
            Text {
                Layout.fillWidth: true
                text: "and enter this code:"
                font.pixelSize: Theme.fontSubject
                color: Theme.ink
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: codeText.implicitHeight + 80
                color: Theme.paper
                border.width: 6
                border.color: Theme.ink
                Text {
                    id: codeText
                    anchors.centerIn: parent
                    text: triage.deviceCodeUserCode
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.fontCode
                    font.bold: true
                    font.family: "monospace"
                    color: Theme.ink
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontBody
                color: Theme.ink
                textFormat: Text.PlainText
                text: triage.signInMessage
            }
            SMButton {
                Layout.fillWidth: true
                Layout.preferredHeight: 130
                text: "I have signed in"
                onClicked: triage.checkSignIn()
            }
        }

        Item { Layout.fillHeight: true }

        SMButton {
            Layout.fillWidth: true
            Layout.preferredHeight: 130
            text: "Cancel"
            onClicked: { triage.cancelSignIn(); page.cancelled(); }
        }
    }
}
