import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page
    objectName: "queue"

    signal openMessage(var row)
    signal showActivity()
    signal signInRequested()

    readonly property int rowH: 280
    readonly property bool ended: triage.sessionState === "ended"
    readonly property int perPage: Math.max(1, Math.floor(listArea.height / rowH))
    property int pageIndex: 0
    readonly property int pageCount: Math.max(1, Math.ceil(list.count / perPage))

    function goToPage(p) {
        pageIndex = Math.max(0, Math.min(p, pageCount - 1));
        list.positionViewAtIndex(Math.min(pageIndex * perPage, Math.max(0, list.count - 1)), ListView.Beginning);
    }
    function settle() { goToPage(pageIndex); }

    Rectangle { anchors.fill: parent; color: Theme.paper }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- top bar -------------------------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 150
            color: Theme.paper
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.margin
                anchors.rightMargin: Theme.margin
                spacing: 24
                Text {
                    text: "Slow Mail"
                    font.pixelSize: Theme.fontTitle
                    font.bold: true
                    color: Theme.ink
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                SMButton {
                    // Failed actions are never hidden: the count opens the Activity page, which lists them.
                    visible: triage.failedCount > 0
                    Layout.preferredWidth: 260
                    Layout.preferredHeight: Theme.tap
                    inverted: true
                    text: "Failed " + triage.failedCount
                    onClicked: page.showActivity()
                }
                SMButton {
                    // One delivery attempt for queued actions, independent of the refresh cooldown.
                    visible: triage.pendingCount > 0
                    Layout.preferredWidth: 440
                    Layout.preferredHeight: Theme.tap
                    text: "Send pending (" + triage.pendingCount + ")"
                    enabled: triage.signedIn && !triage.busy
                    onClicked: triage.flush()
                }
                SMButton {
                    Layout.preferredWidth: (triage.failedCount > 0 || triage.pendingCount > 0) ? 400 : 520
                    Layout.preferredHeight: Theme.tap
                    text: !triage.signedIn ? "Sign in"
                          : triage.refreshEnabled ? "Refresh"
                          : triage.busy ? "Working..."
                          : triage.nextEligibleAt !== "" ? "Next refresh " + triage.nextEligibleAt
                          : "Refresh"
                    enabled: !triage.signedIn || triage.refreshEnabled
                    onClicked: triage.signedIn ? triage.refresh() : page.signInRequested()
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.border; color: Theme.ink }
        }

        // ---- status line ---------------------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 84
            color: Theme.paper
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.margin
                anchors.rightMargin: Theme.margin
                spacing: 40
                Text {
                    text: "Downloaded at " + triage.downloadedAt
                    font.pixelSize: Theme.fontBody
                    color: Theme.ink
                }
                Text {
                    visible: triage.pendingCount > 0
                    text: "Pending " + triage.pendingCount
                    font.pixelSize: Theme.fontBody
                    font.bold: true
                    color: Theme.ink
                }
                Text {
                    visible: triage.deferredCount > 0
                    text: "Deferred " + triage.deferredCount
                    font.pixelSize: Theme.fontBody
                    color: Theme.secondary
                }
                Text {
                    visible: triage.offline
                    text: "Offline"
                    font.pixelSize: Theme.fontBody
                    font.bold: true
                    color: Theme.ink
                }
                Item { Layout.fillWidth: true }
                Text {
                    visible: triage.notice !== ""
                    text: triage.notice
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.fontBody
                    color: Theme.secondary
                    elide: Text.ElideRight
                    Layout.maximumWidth: 700
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.border; color: Theme.ink }
        }

        // ---- queue ---------------------------------------------------------------------------------------
        Item {
            id: listArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            onHeightChanged: Qt.callLater(page.settle)

            ListView {
                id: list
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: page.perPage * page.rowH
                visible: !page.ended
                clip: true
                interactive: false                       // paging is explicit: Previous / Next page buttons
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                highlightResizeDuration: 0
                model: triage.queueModel
                onCountChanged: Qt.callLater(page.settle)

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string id
                    required property string subject
                    required property string fromName
                    required property string fromAddress
                    required property string receivedAt
                    required property string preview
                    required property bool hasAttachments
                    required property bool isRead
                    required property bool flagged

                    width: list.width
                    height: page.rowH
                    color: tap.pressed ? Theme.ink : Theme.paper
                    readonly property color fg: tap.pressed ? Theme.paper : Theme.ink
                    readonly property color fg2: tap.pressed ? Theme.paper : Theme.secondary

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.margin
                        anchors.rightMargin: Theme.margin
                        anchors.topMargin: 18
                        anchors.bottomMargin: 18
                        spacing: 6
                        Text {
                            Layout.fillWidth: true
                            text: row.subject === "" ? "(no subject)" : row.subject
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fontSubject
                            font.bold: !row.isRead
                            color: row.fg
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 20
                            Text {
                                Layout.fillWidth: true
                                text: row.fromName + (row.flagged ? "  (flagged)" : "")
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                font.pixelSize: Theme.fontBody
                                color: row.fg
                            }
                            PaperclipIcon {
                                visible: row.hasAttachments
                                stroke: row.fg
                                Layout.preferredHeight: 44
                                Layout.preferredWidth: 28
                            }
                            Text {
                                text: row.receivedAt
                                textFormat: Text.PlainText
                                font.pixelSize: Theme.fontBody
                                color: row.fg
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: row.preview
                            textFormat: Text.PlainText
                            maximumLineCount: 1
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fontBody
                            color: row.fg2
                        }
                        Item { Layout.fillHeight: true }
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.border; color: Theme.ink }
                    MouseArea {
                        id: tap
                        anchors.fill: parent
                        onClicked: page.openMessage({
                            "id": row.id, "subject": row.subject, "fromName": row.fromName,
                            "fromAddress": row.fromAddress, "receivedAt": row.receivedAt,
                            "hasAttachments": row.hasAttachments
                        })
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.margin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: !page.ended && list.count === 0
                font.pixelSize: Theme.fontSubject
                color: Theme.ink
                text: !triage.ready ? "Loading..."
                      : !triage.signedIn ? "Sign in to download your inbox."
                      : triage.downloadedAt === "never" ? "No mail downloaded yet.\nTap Refresh to download your inbox."
                      : "Inbox is clear."
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.margin
                spacing: 40
                visible: page.ended
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSubject
                    color: Theme.ink
                    text: "Session ended.\nPending changes are kept and will be sent when you are online."
                }
                SMButton {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 600
                    text: "Start new session"
                    onClicked: triage.startSession()
                }
            }
        }

        // ---- paging --------------------------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 130
            color: Theme.paper
            visible: !page.ended
            Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.border; color: Theme.ink }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.margin
                anchors.rightMargin: Theme.margin
                spacing: 24
                SMButton {
                    Layout.preferredWidth: 480
                    text: "Previous page"
                    enabled: page.pageIndex > 0
                    onClicked: page.goToPage(page.pageIndex - 1)
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Page " + (page.pageIndex + 1) + " of " + page.pageCount
                    font.pixelSize: Theme.fontBody
                    color: Theme.ink
                }
                SMButton {
                    Layout.preferredWidth: 480
                    text: "Next page"
                    enabled: page.pageIndex < page.pageCount - 1
                    onClicked: page.goToPage(page.pageIndex + 1)
                }
            }
        }

        // ---- bottom actions ------------------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            color: Theme.paper
            Rectangle { anchors.top: parent.top; width: parent.width; height: Theme.border; color: Theme.ink }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.margin
                anchors.rightMargin: Theme.margin
                spacing: 24
                SMButton {
                    Layout.fillWidth: true
                    text: "Activity"
                    onClicked: page.showActivity()
                }
                SMButton {
                    Layout.fillWidth: true
                    text: "End session"
                    enabled: !page.ended && triage.ready && !triage.busy
                    onClicked: triage.endSession()
                }
            }
        }
    }
}
