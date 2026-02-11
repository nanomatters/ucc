import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Shapes 1.15

Item {
    id: root
    property alias points: pointModel.points
    property string title: "Fan Curve"
    property int minTemp: 20
    property int maxTemp: 100
    property int minDuty: 0
    property int maxDuty: 100
    property color curveColor: "#3fa9f5"
    property color pointColor: "#fff"
    property color pointBorderColor: "#3fa9f5"
    property int pointRadius: 10
    signal pointsChanged(var points)

    width: 500; height: 300

    Rectangle {
        anchors.fill: parent
        color: "#181e26"
        radius: 8
    }

    ListModel {
        id: pointModel
        property var points: [
            { temp: minTemp, duty: 15 },
            { temp: 50, duty: 30 },
            { temp: 70, duty: 50 },
            { temp: 90, duty: 75 },
            { temp: maxTemp, duty: 100 }
        ]
    }

    function sortPoints() {
        pointModel.points.sort(function(a, b) { return a.temp - b.temp; });
    }

    function clamp(val, min, max) {
        return Math.max(min, Math.min(max, val));
    }

    function addPoint(temp, duty) {
        temp = clamp(temp, minTemp, maxTemp);
        duty = clamp(duty, minDuty, maxDuty);
        pointModel.points.push({ temp: temp, duty: duty });
        sortPoints();
        pointsChanged(pointModel.points);
    }

    function removePoint(index) {
        if (pointModel.points.length > 2) {
            pointModel.points.splice(index, 1);
            pointsChanged(pointModel.points);
        }
    }

    Text {
        text: root.title
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 8
        font.pixelSize: 18
        color: "#fff"
    }

    Row {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 8
        anchors.rightMargin: 12
        spacing: 8
        Button {
            text: "+"
            onClicked: {
                // Add a point at the middle
                var t = Math.round((minTemp + maxTemp) / 2);
                var d = Math.round((minDuty + maxDuty) / 2);
                addPoint(t, d);
            }
        }
    }

    Canvas {
        id: curveCanvas
        anchors.fill: parent
        anchors.margins: 40
        onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            // Draw grid
            ctx.strokeStyle = "#333";
            ctx.lineWidth = 1;
            for (var i = 0; i <= 5; ++i) {
                var y = height * i / 5;
                ctx.beginPath();
                ctx.moveTo(0, y);
                ctx.lineTo(width, y);
                ctx.stroke();
            }
            for (var j = 0; j <= 5; ++j) {
                var x = width * j / 5;
                ctx.beginPath();
                ctx.moveTo(x, 0);
                ctx.lineTo(x, height);
                ctx.stroke();
            }
            // Draw curve
            ctx.strokeStyle = root.curveColor;
            ctx.lineWidth = 4;
            ctx.beginPath();
            var pts = pointModel.points;
            for (var k = 0; k < pts.length; ++k) {
                var px = (pts[k].temp - minTemp) / (maxTemp - minTemp) * width;
                var py = height - (pts[k].duty - minDuty) / (maxDuty - minDuty) * height;
                if (k === 0)
                    ctx.moveTo(px, py);
                else
                    ctx.lineTo(px, py);
            }
            ctx.stroke();
        }
        Component.onCompleted: requestPaint()
        Connections {
            target: pointModel
            onPointsChanged: curveCanvas.requestPaint()
        }
    }

    Repeater {
        model: pointModel.points.length
        delegate: MouseArea {
            id: dragArea
            property int idx: index
            property real px: (pointModel.points[idx].temp - minTemp) / (maxTemp - minTemp) * (curveCanvas.width)
            property real py: curveCanvas.height - (pointModel.points[idx].duty - minDuty) / (maxDuty - minDuty) * (curveCanvas.height)
            x: curveCanvas.x + px - root.pointRadius
            y: curveCanvas.y + py - root.pointRadius
            width: root.pointRadius * 2
            height: root.pointRadius * 2
            drag.target: dragCircle
            drag.axis: Drag.XAndYAxis
            drag.minimumX: curveCanvas.x - root.pointRadius
            drag.maximumX: curveCanvas.x + curveCanvas.width - root.pointRadius
            drag.minimumY: curveCanvas.y - root.pointRadius
            drag.maximumY: curveCanvas.y + curveCanvas.height - root.pointRadius
            onReleased: {
                // Update model
                var t = Math.round((dragCircle.x + root.pointRadius - curveCanvas.x) / curveCanvas.width * (maxTemp - minTemp) + minTemp);
                var d = Math.round((curveCanvas.height - (dragCircle.y + root.pointRadius - curveCanvas.y)) / curveCanvas.height * (maxDuty - minDuty) + minDuty);
                pointModel.points[idx].temp = clamp(t, minTemp, maxTemp);
                pointModel.points[idx].duty = clamp(d, minDuty, maxDuty);
                sortPoints();
                pointsChanged(pointModel.points);
            }
            Rectangle {
                id: dragCircle
                anchors.centerIn: parent
                width: root.pointRadius * 2
                height: root.pointRadius * 2
                radius: root.pointRadius
                color: root.pointColor
                border.color: root.pointBorderColor
                border.width: 2
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    removePoint(idx);
                }
                visible: pointModel.points.length > 2
            }
        }
    }
}
