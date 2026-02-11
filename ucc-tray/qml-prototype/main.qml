import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

ApplicationWindow {
    id: root
    visible: true
    width: 820
    height: 520
    title: qsTr("UCC Tray Prototype")

    // Dark theme
    Material.theme: Material.Dark
    Material.accent: Material.Blue

    TabView {
        anchors.fill: parent
        anchors.margins: 48
        currentIndex: 0

        Tab {
            title: "Dashboard"
            DashboardTab { anchors.fill: parent }
        }

        Tab {
            title: "Performance"
            PerformanceTab { anchors.fill: parent }
        }

        Tab {
            title: "Hardware"
            HardwareTab { anchors.fill: parent }
        }
    }
}
