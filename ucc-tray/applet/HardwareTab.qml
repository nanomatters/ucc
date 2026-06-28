/*
 * SPDX-FileCopyrightText: 2026 Uniwill Control Center Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware tab — quick toggle controls for webcam, Fn Lock,
 * and display brightness.
 */

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.plasma.extras as PlasmaExtras
import org.kde.kirigami as Kirigami

PC3.ScrollView {
    id: hwTab
    required property var backend
    clip: true

    PC3.ScrollBar.horizontal.policy: PC3.ScrollBar.AlwaysOff

    contentItem: Flickable {
        contentHeight: col.implicitHeight + Kirigami.Units.largeSpacing * 2

        ColumnLayout {
            id: col
            width: hwTab.availableWidth
            spacing: Kirigami.Units.largeSpacing

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                margins: Kirigami.Units.largeSpacing
            }

            // Quick Toggles
            PlasmaExtras.Heading {
                level: 4
                text: i18n("Quick Controls")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.gridUnit

                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    PC3.Label { text: i18n("Fn Lock") }
                    PC3.Switch {
                        id: fnLockSwitch
                        checked: hwTab.backend.fnLock
                        Connections {
                            target: hwTab.backend
                            function onFnLockChanged() { fnLockSwitch.checked = hwTab.backend.fnLock }
                        }
                        onToggled: hwTab.backend.setFnLock(checked)
                    }
                }

                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    PC3.Label { text: i18n("Webcam") }
                    PC3.Switch {
                        id: webcamSwitch
                        checked: hwTab.backend.webcamEnabled
                        Connections {
                            target: hwTab.backend
                            function onWebcamEnabledChanged() { webcamSwitch.checked = hwTab.backend.webcamEnabled }
                        }
                        onToggled: hwTab.backend.setWebcamEnabled(checked)
                    }
                }
            }

            Kirigami.Separator { Layout.fillWidth: true }

            // Display Brightness
            PlasmaExtras.Heading {
                level: 4
                text: i18n("Display Brightness")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    source: "brightness-low"
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                }

                PC3.Slider {
                    id: brightnessSlider
                    Layout.fillWidth: true
                    from: 0; to: 100; stepSize: 1
                    value: hwTab.backend.displayBrightness
                    Connections {
                        target: hwTab.backend
                        function onDisplayBrightnessChanged() { brightnessSlider.value = hwTab.backend.displayBrightness }
                    }
                    onPressedChanged: {
                        if (!pressed) hwTab.backend.setDisplayBrightness(Math.round(value))
                    }
                }

                PC3.Label {
                    text: Math.round(brightnessSlider.value) + "%"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 2.5
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
    }
}
