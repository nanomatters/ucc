import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
  anchors.fill: parent; color: "transparent"

  ColumnLayout { anchors.fill: parent; spacing: 12 }
  Text { text: "Hardware Controls"; color: "white"; font.pointSize: 18 }
  Text { text: "Brightness, fans and sensors"; color: "#9aa0a6"; font.pointSize: 11 }

  Text { text: "Display Brightness"; color: "#cfcfcf" }
  Slider { id: brightness; from: 0; to: 100; value: 80 }
  Text { text: Math.round(brightness.value) + "%"; color: "#9aa0a6" }

  Text { text: "Keyboard Backlight"; color: "#cfcfcf" }
  RowLayout { spacing: 8 }
  Button { text: "Off" }
  Button { text: "Low" }
  Button { text: "Auto" }

  Text { text: "CPU Fan Curve"; color: "#cfcfcf" }
  Rectangle { width: parent.width - 96; height: 80; radius: 6; color: "#151515"; border.color: "#2b2b2b" }
  Text { text: "(Editable curve placeholder)"; color: "#9aa0a6" }

  RowLayout { spacing: 12 }
  Button { text: "Save" }
  Button { text: "Revert" }
  Button { text: "Apply"; background: Rectangle { color: "#0e72ff" } }
}
