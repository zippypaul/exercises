import QtQuick

// Drawn attachment icon (no emoji font needed on the device, no image files).
Canvas {
    id: icon
    property color stroke: Theme.ink
    width: 32
    height: 52
    renderTarget: Canvas.Image
    renderStrategy: Canvas.Immediate
    onStrokeChanged: requestPaint()
    onPaint: {
        var c = getContext("2d");
        c.reset();
        c.strokeStyle = icon.stroke;
        c.lineWidth = 4;
        c.lineCap = "round";
        c.lineJoin = "round";
        c.beginPath();
        c.moveTo(24, 16);
        c.lineTo(24, 40);
        c.arc(16, 40, 8, 0, Math.PI, false);
        c.lineTo(8, 12);
        c.arc(15, 12, 7, Math.PI, 2 * Math.PI, false);
        c.lineTo(22, 32);
        c.stroke();
    }
}
