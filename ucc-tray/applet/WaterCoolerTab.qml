/*
 * SPDX-FileCopyrightText: 2026 Uniwill Control Center Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Water Cooler tab — live status plus fan speed, pump voltage,
 * and LED colour/mode controls.
 */

import QtQuick
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.plasma.components as PC3
import org.kde.plasma.extras as PlasmaExtras
import org.kde.kirigami as Kirigami

PC3.ScrollView {
    id: wcTab
    required property var backend
    clip: true

    // PumpVoltage enum values indexed by combo position:
    //   0->Off(4) 1->7V(2) 2->8V(3) 3->11V(0)
    readonly property var pumpVoltageCodes: [4, 2, 3, 0]
    readonly property var pumpVoltageLabels: ["High", "Max", "Low", "Medium", "Off"]

    function pumpLevelText(code) {
        if (code >= 0 && code < pumpVoltageLabels.length) return pumpVoltageLabels[code]
        return "—"
    }

    function voltageCodeToIndex(code) {
        for (var i = 0; i < pumpVoltageCodes.length; i++)
            if (pumpVoltageCodes[i] === code) return i
        return 0
    }

    PC3.ScrollBar.horizontal.policy: PC3.ScrollBar.AlwaysOff

    contentItem: Flickable {
        contentHeight: col.implicitHeight + Kirigami.Units.largeSpacing * 2

        ColumnLayout {
            id: col
            width: wcTab.availableWidth
            spacing: Kirigami.Units.largeSpacing

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                margins: Kirigami.Units.largeSpacing
            }

            // Status
            PlasmaExtras.Heading {
                level: 4
                text: i18n("Water Cooler Status")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.gridUnit

                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    PC3.Label { text: i18n("Enable") }
                    PC3.CheckBox {
                        id: wcEnableCheckBox
                        checked: wcTab.backend.wcEnabled
                        Connections {
                            target: wcTab.backend
                            function onWcEnabledChanged() { wcEnableCheckBox.checked = wcTab.backend.wcEnabled }
                        }
                        onToggled: wcTab.backend.setWcEnabled(checked)
                    }
                }

                GridLayout {
                    columns: 4
                    columnSpacing: Kirigami.Units.largeSpacing
                    rowSpacing: Kirigami.Units.smallSpacing
                    Layout.fillWidth: true

                    PC3.Label { text: i18n("Fan Duty Cycle"); opacity: 0.7 }
                    PC3.Label {
                        text: wcTab.backend.wcFanSpeed > 0 ? (wcTab.backend.wcFanSpeed + "%") : "—"
                        font.bold: true
                    }

                    PC3.Label { text: i18n("Pump Level"); opacity: 0.7 }
                    PC3.Label {
                        text: wcTab.pumpLevelText(wcTab.backend.wcPumpLevel)
                        font.bold: true
                    }
                }
            }

            Kirigami.Separator { Layout.fillWidth: true }

            // Fan Speed & Pump Voltage
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    PlasmaExtras.Heading {
                        level: 5
                        text: i18n("Fan Speed")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Slider {
                            id: wcFanSlider
                            Layout.fillWidth: true
                            from: 0; to: 100; stepSize: 5
                            value: wcTab.backend.wcFanPercent
                            enabled: wcTab.backend.wcConnected && !wcTab.backend.wcAutoControl

                            Connections {
                                target: wcTab.backend
                                function onWcControlStateChanged() {
                                    if (!wcFanSlider.pressed)
                                        wcFanSlider.value = wcTab.backend.wcFanPercent
                                }
                            }
                            onPressedChanged: {
                                if (!pressed)
                                    wcTab.backend.setWcFanSpeed(Math.round(value))
                            }
                        }

                        PC3.Label {
                            text: Math.round(wcFanSlider.value) + "%"
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    PlasmaExtras.Heading {
                        level: 5
                        text: i18n("Pump Voltage")
                    }

                    PC3.ComboBox {
                        id: pumpCombo
                        Layout.fillWidth: true
                        model: ["Off", "7 V", "8 V", "11 V"]
                        enabled: wcTab.backend.wcConnected && !wcTab.backend.wcAutoControl
                        currentIndex: wcTab.voltageCodeToIndex(wcTab.backend.wcPumpVoltageCode)

                        Connections {
                            target: wcTab.backend
                            function onWcControlStateChanged() {
                                pumpCombo.currentIndex = wcTab.voltageCodeToIndex(wcTab.backend.wcPumpVoltageCode)
                            }
                        }
                        onActivated: wcTab.backend.setWcPumpVoltageCode(wcTab.pumpVoltageCodes[currentIndex])
                    }
                }
            }

            Kirigami.Separator { Layout.fillWidth: true }

            // LED Control
            PlasmaExtras.Heading {
                level: 4
                text: i18n("LED")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                PC3.Label { text: i18n("LED") }
                PC3.CheckBox {
                    id: ledCheckBox
                    checked: wcTab.backend.wcLedEnabled
                    enabled: wcTab.backend.wcConnected

                    Connections {
                        target: wcTab.backend
                        function onWcControlStateChanged() {
                            ledCheckBox.checked = wcTab.backend.wcLedEnabled
                        }
                    }
                    onToggled: wcTab.backend.setWcLedEnabled(checked)
                }

                PC3.Button {
                    text: i18n("Choose Color")
                    enabled: wcTab.backend.wcConnected &&
                             ledCheckBox.checked &&
                             ledModeCombo.currentIndex === 0
                    onClicked: ledColorDialog.open()
                }

                ColorDialog {
                    id: ledColorDialog
                    title: i18n("Choose LED Colour")
                    selectedColor: Qt.rgba(wcTab.backend.wcLedRed / 255,
                                           wcTab.backend.wcLedGreen / 255,
                                           wcTab.backend.wcLedBlue / 255, 1.0)
                    onAccepted: wcTab.backend.setWcLed(
                        Math.round(selectedColor.r * 255),
                        Math.round(selectedColor.g * 255),
                        Math.round(selectedColor.b * 255),
                        ledModeCombo.currentIndex)
                }

                PC3.Label { text: i18n("Mode:") }

                PC3.ComboBox {
                    id: ledModeCombo
                    Layout.fillWidth: true
                    model: ["Static", "Breathe", "Colourful", "Breathe Colour", "Temperature"]
                    enabled: wcTab.backend.wcConnected && ledCheckBox.checked
                    currentIndex: wcTab.backend.wcLedMode

                    Connections {
                        target: wcTab.backend
                        function onWcControlStateChanged() {
                            ledModeCombo.currentIndex = wcTab.backend.wcLedMode
                        }
                    }
                    onActivated: wcTab.backend.setWcLed(
                        wcTab.backend.wcLedRed,
                        wcTab.backend.wcLedGreen,
                        wcTab.backend.wcLedBlue,
                        currentIndex)
                }
            }
        }
    }
}
