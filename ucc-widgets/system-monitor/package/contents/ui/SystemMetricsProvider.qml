// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Uniwill Control Center Contributors

import QtQuick
import org.kde.kdbusaddons as DBusAddons

QtObject {
    id: metricsProvider

    property string cpuTemp: "0°C"
    property string gpuTemp: "0°C"
    property string cpuFrequency: "0 MHz"
    property string gpuFrequency: "0 MHz"
    property string cpuPower: "0 W"
    property string gpuPower: "0 W"
    property int fanSpeed: 0

    // DBus service details
    readonly property string dbusService: "com.uniwill.uccd"
    readonly property string dbusPath: "/com/uniwill/uccd"
    readonly property string dbusInterface: "com.uniwill.uccd"

    signal metricsUpdated()

    function updateMetrics() {
        // Call DBus methods to get metrics
        var dbusCall = dbusAddons.DBusInterface.call(
            dbusService,
            dbusPath,
            dbusInterface,
            "GetCpuTemperature"
        );
        
        if (dbusCall !== undefined) {
            cpuTemp = dbusCall + "°C";
        }

        var gpuTempResult = dbusAddons.DBusInterface.call(
            dbusService,
            dbusPath,
            dbusInterface,
            "GetGpuTemperature"
        );
        
        if (gpuTempResult !== undefined) {
            gpuTemp = gpuTempResult + "°C";
        }

        var cpuFreqResult = dbusAddons.DBusInterface.call(
            dbusService,
            dbusPath,
            dbusInterface,
            "GetCpuFrequency"
        );
        
        if (cpuFreqResult !== undefined) {
            cpuFrequency = cpuFreqResult + " MHz";
        }

        var gpuFreqResult = dbusAddons.DBusInterface.call(
            dbusService,
            dbusPath,
            dbusInterface,
            "GetGpuFrequency"
        );
        
        if (gpuFreqResult !== undefined) {
            gpuFrequency = gpuFreqResult + " MHz";
        }

        var cpuPowerResult = dbusAddons.DBusInterface.call(
            dbusService,
            dbusPath,
            dbusInterface,
            "GetCpuPower"
        );
        
        if (cpuPowerResult !== undefined) {
            cpuPower = cpuPowerResult + " W";
        }

        var gpuPowerResult = dbusAddons.DBusInterface.call(
            dbusService,
            dbusPath,
            dbusInterface,
            "GetGpuPower"
        );
        
        if (gpuPowerResult !== undefined) {
            gpuPower = gpuPowerResult + " W";
        }

        var fanSpeedResult = dbusAddons.DBusInterface.call(
            dbusService,
            dbusPath,
            dbusInterface,
            "GetFanSpeed"
        );
        
        if (fanSpeedResult !== undefined) {
            fanSpeed = fanSpeedResult;
        }

        metricsUpdated();
    }
}
