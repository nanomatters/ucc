import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
  anchors.fill: parent
  color: "transparent"

  ColumnLayout {
    anchors.fill: parent
    spacing: 12

    // Header
    ColumnLayout {
      Layout.fillWidth: true
      spacing: 2
      Text { text: "System Dashboard"; color: "white"; font.pointSize: 18 }
      Text { text: "Quick overview"; color: "#9aa0a6"; font.pointSize: 11 }
    }

    // Gauges row
    RowLayout {
      spacing: 16
      Layout.fillWidth: true

      // CPU card
      Rectangle {
        width: 260; height: 200; radius: 8; color: "#151515"; border.color: "#2b2b2b"
        Column { anchors.fill: parent; anchors.margins: 12; spacing: 8 }
        Text { text: "CPU Temp"; color: "#cfcfcf"; anchors.horizontalCenter: parent.horizontalCenter }
        Item { width: parent.width; height: 120 }
        Canvas {
          anchors.horizontalCenter: parent.horizontalCenter; width: 96; height: 96
          onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0,0,width,height);
            ctx.beginPath(); ctx.arc(48,48,40,0,2*Math.PI); ctx.fillStyle="#111"; ctx.fill();
            ctx.beginPath(); ctx.lineWidth=8; ctx.strokeStyle="#3a82ff"; ctx.arc(48,48,40,-Math.PI/2,(-Math.PI/2)+(1.2*Math.PI)); ctx.stroke();
            ctx.fillStyle="#fff"; ctx.font = "18px sans-serif"; ctx.textAlign = "center"; ctx.fillText("62°C",48,60);
          }
        }
      }

      // Power card
      Rectangle {
        width: 300; height: 200; radius: 8; color: "#151515"; border.color: "#2b2b2b"
        Column { anchors.fill: parent; anchors.margins: 12; spacing: 12 }
        Text { text: "Power Profile"; color: "#cfcfcf" }
        Slider { from:0; to:100; value: 70; background: Rectangle { color: "#2b2b2b" } }
        Label { text: (Math.round(value)) + "% - Performance"; color: "#9aa0a6" }
      }

      // Battery card
      Rectangle {
        width: 180; height: 200; radius: 8; color: "#151515"; border.color: "#2b2b2b"
        Column { anchors.fill: parent; anchors.margins: 12; spacing: 10; horizontalAlignment: Text.AlignHCenter }
        Text { text: "Battery"; color: "#cfcfcf"; horizontalAlignment: Text.AlignHCenter }
        Rectangle { width: 120; height: 24; radius: 4; color: "#0e72ff" }
        Text { text: "100%"; color: "white" }
        Text { text: "Fully Charged"; color: "#9aa0a6"; font.pointSize: 12 }
      }
    }

    // Footer Buttons
    RowLayout { spacing: 12; Layout.alignment: Qt.AlignLeft }
    RowLayout {
      spacing: 12
      Button { text: "Open Control Center" }
      Button { text: "Show Hardware" }
      Button { text: "Notifications · Settings" }
    }
  }
}
