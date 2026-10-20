import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1620
    height: 2160
    minimumWidth: 540
    minimumHeight: 720
    visible: true
    title: "Slow Mail"
    color: Theme.paper

    // Set when the user cancels sign-in, so we do not push the page again until the state changes.
    property bool signInDismissed: false

    function atRoot() { return stack.depth <= 1; }
    function currentName() { return stack.currentItem ? stack.currentItem.objectName : ""; }

    function showQueue() {
        if (stack.depth > 1) stack.pop(null, StackView.Immediate);
    }
    function showActivity() {
        showQueue();
        stack.push(activityComp, {}, StackView.Immediate);
    }
    function showSignIn() {
        showQueue();
        stack.push(signInComp, {}, StackView.Immediate);
    }
    function openRow(row) {
        stack.push(messageComp, {
            "messageId": row.id, "subject": row.subject, "fromName": row.fromName,
            "fromAddress": row.fromAddress, "receivedAt": row.receivedAt,
            "hasAttachments": row.hasAttachments
        }, StackView.Immediate);
        triage.openMessage(row.id);
    }
    function showMessage(index) {
        var row = triage.queueModel.get(index);
        if (row && row.id !== undefined) {
            showQueue();
            openRow(row);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ErrorBanner {
            Layout.fillWidth: true
            visible: triage.lastError !== ""
            message: triage.lastError
            onDismissed: triage.clearError()
        }

        StackView {
            id: stack
            Layout.fillWidth: true
            Layout.fillHeight: true
            initialItem: queueComp
            // No page transitions: every operation is immediate.
            pushEnter: null
            pushExit: null
            popEnter: null
            popExit: null
            replaceEnter: null
            replaceExit: null
        }
    }

    Component {
        id: queueComp
        QueuePage {
            onOpenMessage: (row) => win.openRow(row)
            onShowActivity: stack.push(activityComp, {}, StackView.Immediate)
            onSignInRequested: { win.signInDismissed = false; stack.push(signInComp, {}, StackView.Immediate); }
        }
    }
    Component {
        id: messageComp
        MessagePage {
            onBack: stack.pop(StackView.Immediate)
            onActionDone: stack.pop(null, StackView.Immediate)
        }
    }
    Component {
        id: activityComp
        ActivityPage {
            onBack: stack.pop(StackView.Immediate)
        }
    }
    Component {
        id: signInComp
        SignInPage {
            onCancelled: { win.signInDismissed = true; stack.pop(StackView.Immediate); }
        }
    }

    // Sign-in is shown when the session needs it, and dismissed once it succeeded.
    Connections {
        target: triage
        function onStatusChanged() {
            if (!triage.ready) return;
            if (triage.signedIn) {
                win.signInDismissed = false;
                if (win.currentName() === "signin") stack.pop(null, StackView.Immediate);
            } else if (triage.sessionState === "auth_required" && !win.signInDismissed
                       && win.currentName() === "queue") {
                stack.push(signInComp, {}, StackView.Immediate);
            }
        }
    }
}
