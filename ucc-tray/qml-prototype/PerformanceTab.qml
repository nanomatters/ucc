import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
  anchors.fill: parent; color: "transparent"
  ColumnLayout { anchors.fill: parent; spacing: 12 }

  ColumnLayout { spacing: 4; Layout.margins: 0 }
  Text { text: "Performance Controls"; color: "white"; font.pointSize: 18 }
  Text { text: "Profiles, power sliders and presets"; color: "#9aa0a6"; font.pointSize: 11 }

  // Profile presets
  RowLayout { spacing: 12 }
  RowLayout {
    spacing: 10
    Button { text: "Eco" }
    Button { text: "Balanced" }
    Button { text: "Performance"; background: Rectangle { color: "#0e72ff" } }
  }

  // Sliders
  ColumnLayout { spacing: 10 }
  RowLayout { spacing: 8; Layout.alignment: Qt.AlignLeft }
  Text { text: "CPU Power Limit (W)"; color: "#cfcfcf" }
  Slider { id: cpuSlider; from: 0; to: 150; value: 75 }
  Text { text: Math.round(cpuSlider.value) + " W"; color: "#9aa0a6" }

  Text { text: "GPU Power Limit (W)"; color: "#cfcfcf" }
  Slider { id: gpuSlider; from: 0; to: 250; value: 95 }
  Text { text: Math.round(gpuSlider.value) + " W"; color: "#9aa0a6" }

  // Apply / Save
  RowLayout { spacing: 12 }
  Button { text: "Apply"; background: Rectangle { color: "#0e72ff" } }
  Button { text: "Save Preset" }
  Button { text: "Advanced Settings" }
}
