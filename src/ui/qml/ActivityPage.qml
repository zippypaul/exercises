import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page
    objectName: "activity"
    signal back()

    function scrollBy(delta) {
        var minY = list.originY;
        var maxY = Math.max(minY, list.originY + list.contentHeight - list.height);
        list.contentY = Math.max(minY, Math.min(maxY, list.contentY + delta));
    }
    readonly property bool atTop: list.contentY <= list.originY
    readonly property bool atBottom: list.contentY + list.height >= list.originY + list.contentHeight - 1

    Rectangle { anchors.fill: parent; color: Theme.paper }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

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
                    Layout.fillWidth: true
                    text: "Session activity"
                    font.pixelSize: Theme.fontTitle
                    font.bold: true
                    color: Theme.ink
                }
                SMButton {
                    text: "Back"
                    onClicked: page.back()
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.border; color: Theme.ink }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                interactive: false
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                model: triage.activityModel

                section.property: "type"
                section.criteria: ViewSection.FullString
                section.delegate: Rectangle {
                    required property string section
                    width: list.width
                    height: 84
                    color: Theme.ink
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.margin
                        text: parent.section
                        font.pixelSize: Theme.fontSubject
                        font.bold: true
                        color: Theme.paper
                    }
                }

                delegate: Rectangle {
                    id: row
                    required property string id
                    required property string type
                    required property string subject
                    required property string from
                    required property string time
                    required property string confirmation
                    required property string failureReason
                    required property string commandId
                    required property bool canUndo

                    width: list.width
                    height: col.implicitHeight + 40
                    color: Theme.paper

                    ColumnLayout {
                        id: col
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.margin
                        anchors.rightMargin: Theme.margin
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 20
                            Text {
                                Layout.fillWidth: true
                                text: row.subject === "" ? "(no subject)" : row.subject
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                font.pixelSize: Theme.fontSubject
                                color: Theme.ink
                            }
                            Rectangle {
                                // Status badge: Pending (outlined), Confirmed (outlined), Failed (filled)
                                implicitWidth: badge.implicitWidth + 40
                                implicitHeight: 70
                                color: row.confirmation === "Failed" ? Theme.ink : Theme.paper
                                border.width: Theme.border
                                border.color: Theme.ink
                                Text {
                                    id: badge
                                    anchors.centerIn: parent
                                    text: row.confirmation
                                    font.pixelSize: Theme.fontBody
                                    font.bold: true
                                    color: row.confirmation === "Failed" ? Theme.paper : Theme.ink
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 20
                            Text {
                                Layout.fillWidth: true
                                text: row.from + "  •  " + row.time
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                font.pixelSize: Theme.fontBody
                                color: Theme.secondary
                            }
                            SMButton {
                                visible: row.canUndo
                                text: "Undo"
                                Layout.preferredWidth: 240
                                onClicked: triage.undo(row.commandId)
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: row.confirmation === "Failed" && row.failureReason !== ""
                            text: "Reason: " + row.failureReason
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            font.pixelSize: Theme.fontBody
                            font.bold: true
                            color: Theme.ink
                        }
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.border; color: Theme.ink }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "Nothing yet in this session."
                font.pixelSize: Theme.fontSubject
                color: Theme.ink
            }
        }

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
                    Layout.preferredWidth: 480
                    text: "Previous page"
                    enabled: !page.atTop
                    onClicked: page.scrollBy(-(list.height - 80))
                }
                Item { Layout.fillWidth: true }
                SMButton {
                    Layout.preferredWidth: 480
                    text: "Next page"
                    enabled: !page.atBottom
                    onClicked: page.scrollBy(list.height - 80)
                }
            }
        }
    }
}
