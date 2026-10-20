import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page
    objectName: "message"

    property string messageId: ""
    property string subject: ""
    property string fromName: ""
    property string fromAddress: ""
    property string receivedAt: ""
    property bool hasAttachments: false

    property bool loading: true
    property string bodyText: ""
    property bool truncated: false
    property var attachments: []
    property string failure: ""
    property bool confirmingDelete: false

    signal back()
    signal actionDone()

    readonly property real maxY: Math.max(0, flick.contentHeight - flick.height)
    readonly property int pageCount: Math.max(1, Math.ceil(flick.contentHeight / Math.max(1, flick.height)))
    readonly property int pageNumber: maxY <= 0 ? 1 : Math.min(pageCount, 1 + Math.round(flick.contentY / Math.max(1, flick.height - 60)))

    function scrollBy(delta) {
        flick.contentY = Math.max(0, Math.min(maxY, flick.contentY + delta));
    }

    Connections {
        target: triage
        function onMessageOpened(id, text, attachmentsJson, truncated) {
            if (id !== page.messageId) return;
            page.bodyText = text;
            page.truncated = truncated;
            try { page.attachments = JSON.parse(attachmentsJson); } catch (e) { page.attachments = []; }
            page.loading = false;
        }
        function onMessageOpenFailed(id, reason) {
            if (id !== page.messageId) return;
            page.failure = reason;
            page.loading = false;
        }
    }

    Rectangle { anchors.fill: parent; color: Theme.paper }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- header --------------------------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: header.implicitHeight + 48
            color: Theme.paper
            RowLayout {
                id: header
                anchors.fill: parent
                anchors.margins: Theme.margin
                spacing: 24
                SMButton {
                    Layout.alignment: Qt.AlignTop
                    text: "Back"
                    onClicked: page.back()
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        Layout.fillWidth: true
                        text: page.subject === "" ? "(no subject)" : page.subject
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        font.pixelSize: Theme.fontSubject
                        font.bold: true
                        color: Theme.ink
                    }
                    Text {
                        Layout.fillWidth: true
                        text: page.fromName + (page.fromAddress !== "" && page.fromAddress !== page.fromName ? "  <" + page.fromAddress + ">" : "")
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.pixelSize: Theme.fontBody
                        color: Theme.ink
                    }
                    Text {
                        text: page.receivedAt
                        textFormat: Text.PlainText
                        font.pixelSize: Theme.fontBody
                        color: Theme.secondary
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.border; color: Theme.ink }
        }

        // ---- body (explicit paging, no kinetic scrolling) -----------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Flickable {
                id: flick
                anchors.fill: parent
                anchors.leftMargin: Theme.margin
                anchors.rightMargin: Theme.margin
                clip: true
                interactive: false
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: width
                contentHeight: content.implicitHeight + 40

                ColumnLayout {
                    id: content
                    width: flick.width
                    y: 20
                    spacing: 28

                    Text {
                        Layout.fillWidth: true
                        visible: page.loading
                        text: "Loading message..."
                        font.pixelSize: Theme.fontBody
                        color: Theme.ink
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: page.failure !== ""
                        text: "Could not open this message.\n" + page.failure
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.fontBody
                        font.bold: true
                        color: Theme.ink
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !page.loading && page.failure === ""
                        text: page.bodyText === "" ? "(empty message)" : page.bodyText
                        textFormat: Text.PlainText      // never rich text: no links, no remote content
                        wrapMode: Text.Wrap
                        font.pixelSize: 40
                        lineHeight: 1.25
                        color: Theme.ink
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: page.truncated
                        text: "[Message truncated: too long to show in full]"
                        font.pixelSize: Theme.fontBody
                        font.italic: true
                        color: Theme.secondary
                    }

                    // attachments: metadata only
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: !page.loading && (page.attachments.length > 0 || page.hasAttachments)
                        spacing: 12
                        Rectangle { Layout.fillWidth: true; height: Theme.border; color: Theme.ink }
                        Text {
                            text: "Attachments (" + page.attachments.length + ")"
                            font.pixelSize: Theme.fontSubject
                            font.bold: true
                            color: Theme.ink
                        }
                        Repeater {
                            model: page.attachments
                            delegate: Text {
                                required property var modelData
                                Layout.fillWidth: true
                                text: modelData.name + " — " + modelData.type + " — " + modelData.size
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                font.pixelSize: Theme.fontBody
                                color: Theme.ink
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: page.attachments.length === 0
                            text: "This message has attachments, but none could be listed."
                            wrapMode: Text.Wrap
                            font.pixelSize: Theme.fontBody
                            color: Theme.secondary
                        }
                        Text {
                            Layout.fillWidth: true
                            text: "Attachments are listed only. Files are not downloaded."
                            wrapMode: Text.Wrap
                            font.pixelSize: Theme.fontBody
                            font.bold: true
                            color: Theme.ink
                        }
                    }
                }
            }
        }

        // ---- page controls -------------------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 130
            color: Theme.paper
            Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.border; color: Theme.ink }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.margin
                anchors.rightMargin: Theme.margin
                spacing: 24
                SMButton {
                    Layout.preferredWidth: 420
                    text: "Page up"
                    enabled: flick.contentY > 0
                    onClicked: page.scrollBy(-(flick.height - 60))
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Page " + page.pageNumber + " of " + page.pageCount
                    font.pixelSize: Theme.fontBody
                    color: Theme.ink
                }
                SMButton {
                    Layout.preferredWidth: 420
                    text: "Page down"
                    enabled: flick.contentY < page.maxY
                    onClicked: page.scrollBy(flick.height - 60)
                }
            }
        }

        // ---- action rail (fixed) -------------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 170
            color: Theme.paper
            Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.border; color: Theme.ink }
            RowLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 20
                SMButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    text: "Do / Archive"
                    onClicked: { triage.archive(page.messageId); page.actionDone(); }
                }
                SMButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    text: "Delete"
                    onClicked: page.confirmingDelete = true
                }
                SMButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    text: "Defer"
                    onClicked: { triage.defer(page.messageId); page.actionDone(); }
                }
                SMButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    text: "Delegate (later)"
                    enabled: false
                }
            }
        }
    }

    // ---- single confirmation for the destructive action ------------------------------------------------
    Rectangle {
        anchors.fill: parent
        visible: page.confirmingDelete
        color: Theme.paper
        z: 10
        MouseArea { anchors.fill: parent }   // swallow taps outside the dialog
        Rectangle {
            anchors.centerIn: parent
            width: parent.width - 2 * 120
            height: dialogCol.implicitHeight + 2 * 48
            color: Theme.paper
            border.width: 6
            border.color: Theme.ink
            ColumnLayout {
                id: dialogCol
                anchors.fill: parent
                anchors.margins: 48
                spacing: 36
                Text {
                    Layout.fillWidth: true
                    text: "Delete this message?"
                    font.pixelSize: Theme.fontTitle
                    font.bold: true
                    color: Theme.ink
                }
                Text {
                    Layout.fillWidth: true
                    text: page.subject
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontSubject
                    color: Theme.ink
                }
                Text {
                    Layout.fillWidth: true
                    text: "It moves to Deleted Items in Outlook."
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontBody
                    color: Theme.secondary
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 24
                    SMButton {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 130
                        text: "Cancel"
                        onClicked: page.confirmingDelete = false
                    }
                    SMButton {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 130
                        inverted: true
                        text: "Delete"
                        onClicked: {
                            page.confirmingDelete = false;
                            triage.remove(page.messageId);
                            page.actionDone();
                        }
                    }
                }
            }
        }
    }
}
