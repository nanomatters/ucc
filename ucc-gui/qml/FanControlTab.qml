import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

ColumnLayout {
    anchors.fill: parent
    spacing: 24

    Label {
        text: qsTr("Fan Control")
        font.pixelSize: 24
        font.bold: true
        Layout.alignment: Qt.AlignHCenter
    }

    GroupBox {
        title: qsTr("CPU Fan Curve")
        Layout.fillWidth: true
        Layout.preferredHeight: 340
        FanCurveEditor {
            id: cpuFanCurve
            title: qsTr("CPU Fan Curve")
            onPointsChanged: {
                // TODO: Integrate with profile backend or tccd
                // Example: profileManager.setCpuFanCurve(points)
            }
        }
    }

    GroupBox {
        title: qsTr("GPU Fan Curve")
        Layout.fillWidth: true
        Layout.preferredHeight: 340
        FanCurveEditor {
            id: gpuFanCurve
            title: qsTr("GPU Fan Curve")
            onPointsChanged: {
                // TODO: Integrate with profile backend or tccd
                // Example: profileManager.setGpuFanCurve(points)
            }
        }
    }
}
