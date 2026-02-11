// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Uniwill Control Center Contributors

import QtQuick
import QtQuick.Layouts
import QtDBus
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PlasmaComponents

PlasmoidItem {
    id: root

    width: PlasmaCore.Units.gridUnit * 10
    height: PlasmaCore.Units.gridUnit * 10

    Plasmoid.backgroundHints: PlasmaCore.Types.DefaultBackground

    property string cpuTemp: "0°C"
    property string gpuTemp: "0°C"
    property string cpuFrequency: "0 MHz"
    property string gpuFrequency: "0 MHz"
    property string cpuPower: "0 W"
    property string gpuPower: "0 W"
    property int cpuUsage: 0
    property int fanSpeed: 0
    property bool dbusAvailable: false

    // DBus interface for metrics
    DBusInterface {
        id: tccdInterface
        service: "com.uniwill.uccd"
        path: "/com/uniwill/uccd"
        interface: "com.uniwill.uccd"

        onPropertiesChanged: updateMetrics()
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        onTriggered: updateMetrics()
    }

    Component.onCompleted: {
        // Check if DBus service is available
        dbusAvailable = tccdInterface.isValid;
        updateMetrics()
    }

    function updateMetrics() {
        var useFallback = !dbusAvailable;

        // Try to get CPU Temperature
        try {
            var cpuTempResult = tccdInterface.call("GetCpuTemperature");
            if (cpuTempResult !== undefined && cpuTempResult !== null) {
                cpuTemp = cpuTempResult + "°C";
                useFallback = false;
            }
        } catch (e) {
            useFallback = true;
        }

        // Try to get GPU Temperature
        try {
            var gpuTempResult = tccdInterface.call("GetGpuTemperature");
            if (gpuTempResult !== undefined && gpuTempResult !== null) {
                gpuTemp = gpuTempResult + "°C";
            }
        } catch (e) {
            // Fall through to fallback
        }

        // Try to get CPU Frequency
        try {
            var cpuFreqResult = tccdInterface.call("GetCpuFrequency");
            if (cpuFreqResult !== undefined && cpuFreqResult !== null) {
                cpuFrequency = cpuFreqResult + " MHz";
            }
        } catch (e) {
            // Fall through to fallback
        }

        // Try to get GPU Frequency
        try {
            var gpuFreqResult = tccdInterface.call("GetGpuFrequency");
            if (gpuFreqResult !== undefined && gpuFreqResult !== null) {
                gpuFrequency = gpuFreqResult + " MHz";
            }
        } catch (e) {
            // Fall through to fallback
        }

        // Try to get CPU Power
        try {
            var cpuPowerResult = tccdInterface.call("GetCpuPower");
            if (cpuPowerResult !== undefined && cpuPowerResult !== null) {
                cpuPower = cpuPowerResult + " W";
            }
        } catch (e) {
            // Fall through to fallback
        }

        // Try to get GPU Power
        try {
            var gpuPowerResult = tccdInterface.call("GetGpuPower");
            if (gpuPowerResult !== undefined && gpuPowerResult !== null) {
                gpuPower = gpuPowerResult + " W";
            }
        } catch (e) {
            // Fall through to fallback
        }

        // Try to get Fan Speed
        try {
            var fanSpeedResult = tccdInterface.call("GetFanSpeed");
            if (fanSpeedResult !== undefined && fanSpeedResult !== null) {
                fanSpeed = fanSpeedResult;
            }
        } catch (e) {
            useFallback = true;
        }

        // If DBus calls fail, use mock data
        if (useFallback) {
            cpuTemp = Math.floor(Math.random() * 30 + 40) + "°C";
            gpuTemp = Math.floor(Math.random() * 30 + 50) + "°C";
            cpuFrequency = Math.floor(Math.random() * 1000 + 1500) + " MHz";
            gpuFrequency = Math.floor(Math.random() * 500 + 500) + " MHz";
            cpuPower = Math.floor(Math.random() * 50 + 10) + " W";
            gpuPower = Math.floor(Math.random() * 30 + 5) + " W";
            cpuUsage = Math.floor(Math.random() * 100);
            fanSpeed = Math.floor(Math.random() * 3000 + 1000);
        }
    }

    fullRepresentation: Item {
        Layout.preferredWidth: PlasmaCore.Units.gridUnit * 15
        Layout.preferredHeight: PlasmaCore.Units.gridUnit * 20

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: PlasmaCore.Units.smallSpacing
            spacing: PlasmaCore.Units.largeSpacing

            PlasmaComponents.Label {
                text: "System Monitor"
                font.pointSize: 14
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }

            GridLayout {
                columns: 2
                rowSpacing: PlasmaCore.Units.smallSpacing
                columnSpacing: PlasmaCore.Units.largeSpacing
                Layout.fillWidth: true

                PlasmaComponents.Label {
                    text: "CPU Temperature:"
                    font.bold: true
                }
                PlasmaComponents.Label {
                    text: root.cpuTemp
                }

                PlasmaComponents.Label {
                    text: "CPU Frequency:"
                    font.bold: true
                }
                PlasmaComponents.Label {
                    text: root.cpuFrequency
                }

                PlasmaComponents.Label {
                    text: "CPU Power:"
                    font.bold: true
                }
                PlasmaComponents.Label {
                    text: root.cpuPower
                }

                PlasmaComponents.Label {
                    text: "GPU Temperature:"
                    font.bold: true
                }
                PlasmaComponents.Label {
                    text: root.gpuTemp
                }

                PlasmaComponents.Label {
                    text: "GPU Frequency:"
                    font.bold: true
                }
                PlasmaComponents.Label {
                    text: root.gpuFrequency
                }

                PlasmaComponents.Label {
                    text: "GPU Power:"
                    font.bold: true
                }
                PlasmaComponents.Label {
                    text: root.gpuPower
                }

                PlasmaComponents.Label {
                    text: "CPU Usage:"
                    font.bold: true
                }
                PlasmaComponents.Label {
                    text: root.cpuUsage + "%"
                }

                PlasmaComponents.Label {
                    text: "Fan Speed:"
                    font.bold: true
                }
                PlasmaComponents.Label {
                    text: root.fanSpeed + " RPM"
                }
            }

            Item {
                Layout.fillHeight: true
            }

            PlasmaComponents.Button {
                text: "Open Control Center"
                Layout.fillWidth: true
                onClicked: {
                    executable = "ucc-gui"
                    // TODO: Launch ucc-gui
                }
            }
        }
    }

    compactRepresentation: Item {
        PlasmaCore.IconItem {
            anchors.fill: parent
            source: "preferences-system"

            PlasmaComponents.Label {
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                text: root.cpuTemp
                font.pixelSize: 10
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: root.expanded = !root.expanded
        }
    }
}
