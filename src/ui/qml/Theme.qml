pragma Singleton
import QtQuick

// Monochrome palette and metrics for a low-refresh E Ink panel (1620x2160, ~6.5 mm tap targets).
// No animations anywhere: nothing in the app may use NumberAnimation, Behavior or Transition with a duration.
QtObject {
    readonly property color ink: "#000000"
    readonly property color paper: "#ffffff"
    readonly property color secondary: "#333333"   // secondary text: never lighter than #666
    readonly property color disabled: "#666666"

    readonly property int border: 3
    readonly property int margin: 36
    readonly property int tap: 110                 // minimum tap target height in px

    readonly property int fontBody: 36
    readonly property int fontSubject: 44
    readonly property int fontTitle: 56
    readonly property int fontButton: 38
    readonly property int fontCode: 120
}
