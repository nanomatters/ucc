/*
 * SPDX-FileCopyrightText: 2026 Uniwill Control Center Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Dashboard tab — live system monitoring overview.
 */

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.plasma.extras as PlasmaExtras
import org.kde.kirigami as Kirigami

PC3.ScrollView {
    id: dashTab
    required property var backend
    clip: true

    PC3.ScrollBar.horizontal.policy: PC3.ScrollBar.AlwaysOff

    contentItem: Flickable {
        contentHeight: col.implicitHeight + Kirigami.Units.largeSpacing * 2

        ColumnLayout {
            id: col
            width: dashTab.availableWidth
            spacing: Kirigami.Units.largeSpacing

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                margins: Kirigami.Units.largeSpacing
            }

            // ── Active Profile ──
            PlasmaExtras.Heading {
                level: 4
                text: i18n("Active Profile")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                PC3.Label {
                    text: dashTab.backend.activeProfileName || "—"
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize + 2
                    font.bold: true
                }

                Item { Layout.fillWidth: true }

                PC3.Label {
                    text: dashTab.backend.powerState || ""
                    opacity: 0.7
                }
            }

            Kirigami.Separator { Layout.fillWidth: true }

            // ── CPU Section ──
            PlasmaExtras.Heading {
                level: 4
                text: dashTab.backend.cpuModel || i18n("CPU")
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: Kirigami.Units.largeSpacing
                rowSpacing: Kirigami.Units.smallSpacing

                PC3.Label { text: i18n("Temperature"); opacity: 0.7 }
                PC3.Label { text: dashTab.backend.cpuTemp + " °C"; font.bold: true }
                PC3.Label { text: i18n("Frequency"); opacity: 0.7 }
                PC3.Label { text: dashTab.backend.cpuFreqMHz + " MHz"; font.bold: true }

                PC3.Label { text: i18n("Power"); opacity: 0.7 }
                PC3.Label { text: dashTab.backend.cpuPowerW.toFixed(1) + " W"; font.bold: true }
                PC3.Label { text: i18n("Fan"); opacity: 0.7 }
                PC3.Label { text: dashTab.backend.cpuFanRPM + " RPM (" + dashTab.backend.cpuFanPercent + "%)"; font.bold: true }
            }

            Kirigami.Separator { Layout.fillWidth: true }

            // ── GPU Section ──
            PlasmaExtras.Heading {
                level: 4
                text: dashTab.backend.dGpuModel || dashTab.backend.iGpuModel || i18n("GPU")
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: Kirigami.Units.largeSpacing
                rowSpacing: Kirigami.Units.smallSpacing

                PC3.Label { text: i18n("Temperature"); opacity: 0.7 }
                PC3.Label { text: dashTab.backend.gpuTemp + " °C"; font.bold: true }
                PC3.Label { text: i18n("Frequency"); opacity: 0.7 }
                PC3.Label { text: dashTab.backend.gpuFreqMHz + " MHz"; font.bold: true }

                PC3.Label { text: i18n("Power"); opacity: 0.7 }
                PC3.Label { text: dashTab.backend.gpuPowerW.toFixed(1) + " W"; font.bold: true }
                PC3.Label { text: i18n("Fan"); opacity: 0.7 }
                PC3.Label { text: dashTab.backend.gpuFanRPM + " RPM (" + dashTab.backend.gpuFanPercent + "%)"; font.bold: true }

                // Extended NVIDIA metrics — shown only when data is available
                PC3.Label {
                    text: i18n("GPU Load")
                    opacity: 0.7
                    visible: dashTab.backend.gpuComputeUtilPct >= 0
                }
                PC3.Label {
                    text: dashTab.backend.gpuComputeUtilPct + " %"
                    font.bold: true
                    visible: dashTab.backend.gpuComputeUtilPct >= 0
                }
                PC3.Label {
                    text: i18n("VRAM Load")
                    opacity: 0.7
                    visible: dashTab.backend.gpuMemoryUtilPct >= 0
                }
                PC3.Label {
                    text: dashTab.backend.gpuMemoryUtilPct + " %"
                    font.bold: true
                    visible: dashTab.backend.gpuMemoryUtilPct >= 0
                }

                PC3.Label {
                    text: i18n("P-State")
                    opacity: 0.7
                    visible: dashTab.backend.gpuCurrentPstate >= 0
                }
                PC3.Label {
                    text: "P" + dashTab.backend.gpuCurrentPstate
                    font.bold: true
                    visible: dashTab.backend.gpuCurrentPstate >= 0
                }
                PC3.Label {
                    text: i18n("VRAM Freq")
                    opacity: 0.7
                    visible: dashTab.backend.gpuVramFreqMHz >= 0
                }
                PC3.Label {
                    text: dashTab.backend.gpuVramFreqMHz + " MHz"
                    font.bold: true
                    visible: dashTab.backend.gpuVramFreqMHz >= 0
                }

                PC3.Label {
                    text: i18n("Core Voltage")
                    opacity: 0.7
                    visible: dashTab.backend.gpuCoreVoltageMv >= 0
                }
                PC3.Label {
                    text: dashTab.backend.gpuCoreVoltageMv + " mV"
                    font.bold: true
                    visible: dashTab.backend.gpuCoreVoltageMv >= 0
                }
                PC3.Label {
                    text: i18n("Clock Offsets")
                    opacity: 0.7
                    visible: dashTab.backend.gpuGrClockOffsetMHz !== -999
                }
                PC3.Label {
                    text: (dashTab.backend.gpuGrClockOffsetMHz >= 0 ? "+" : "") + dashTab.backend.gpuGrClockOffsetMHz
                          + " / " + (dashTab.backend.gpuMemClockOffsetMHz >= 0 ? "+" : "") + dashTab.backend.gpuMemClockOffsetMHz + " MHz"
                    font.bold: true
                    visible: dashTab.backend.gpuGrClockOffsetMHz !== -999
                }

                PC3.Label {
                    text: i18n("VRAM")
                    opacity: 0.7
                    visible: dashTab.backend.gpuVramTotalMiB > 0
                }
                PC3.Label {
                    text: dashTab.backend.gpuVramUsedMiB + " / " + dashTab.backend.gpuVramTotalMiB + " MiB"
                    font.bold: true
                    visible: dashTab.backend.gpuVramTotalMiB > 0
                }
                PC3.Label {
                    text: i18n("Perf Limit")
                    opacity: 0.7
                    visible: dashTab.backend.gpuPerfLimitReason.length > 0
                }
                PC3.Label {
                    text: dashTab.backend.gpuPerfLimitReason
                    font.bold: true
                    visible: dashTab.backend.gpuPerfLimitReason.length > 0
                }

                PC3.Label {
                    text: i18n("NVENC/DEC")
                    opacity: 0.7
                    visible: dashTab.backend.gpuEncoderUtilPct >= 0 || dashTab.backend.gpuDecoderUtilPct >= 0
                }
                PC3.Label {
                    text: (dashTab.backend.gpuEncoderUtilPct >= 0 ? dashTab.backend.gpuEncoderUtilPct : "--")
                          + " / "
                          + (dashTab.backend.gpuDecoderUtilPct >= 0 ? dashTab.backend.gpuDecoderUtilPct : "--")
                          + " %"
                    font.bold: true
                    visible: dashTab.backend.gpuEncoderUtilPct >= 0 || dashTab.backend.gpuDecoderUtilPct >= 0
                }
            }
        }
    }
}
