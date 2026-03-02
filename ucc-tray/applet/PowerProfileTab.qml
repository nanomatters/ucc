/*
 * SPDX-FileCopyrightText: 2026 Uniwill Control Center Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Profile tab — select profiles and adjust ODM performance mode.
 */

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.plasma.extras as PlasmaExtras
import org.kde.kirigami as Kirigami

PC3.ScrollView {
    id: powerTab
    required property var backend
    clip: true

    PC3.ScrollBar.horizontal.policy: PC3.ScrollBar.AlwaysOff

    contentItem: Flickable {
        contentHeight: col.implicitHeight + Kirigami.Units.largeSpacing * 2

        ColumnLayout {
            id: col
            width: powerTab.availableWidth
            spacing: Kirigami.Units.largeSpacing

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                margins: Kirigami.Units.largeSpacing
            }

            // ── System Profile ──
            PC3.Label { text: i18n("System Profile:"); opacity: 0.7 }
            PC3.ComboBox {
                id: profileCombo
                Layout.fillWidth: true
                model: powerTab.backend.profileNames
                currentIndex: powerTab.backend.profileIds.indexOf(powerTab.backend.activeProfileId)

                Connections {
                    target: powerTab.backend
                    function onActiveProfileChanged() {
                        var idx = powerTab.backend.profileIds.indexOf(powerTab.backend.activeProfileId)
                        if (idx >= 0 && profileCombo.currentIndex !== idx)
                            profileCombo.currentIndex = idx
                    }
                    function onProfilesChanged() {
                        Qt.callLater(function() {
                            profileCombo.currentIndex = powerTab.backend.profileIds.indexOf(powerTab.backend.activeProfileId)
                        })
                    }
                }

                onActivated: function(index) {
                    var id = powerTab.backend.profileIds[index]
                    if (id && id !== powerTab.backend.activeProfileId)
                        powerTab.backend.setActiveProfile(id)
                }
            }

            // ── Fan Profile ──
            PC3.Label {
                text: i18n("Fan profile:")
                opacity: 0.7
                visible: powerTab.backend.fanProfileIds.length > 0
            }
            PC3.ComboBox {
                id: fanCombo
                Layout.fillWidth: true
                model: powerTab.backend.fanProfileNames
                visible: powerTab.backend.fanProfileIds.length > 0
                currentIndex: powerTab.backend.fanProfileIds.indexOf(powerTab.backend.activeProfileFanId)

                Connections {
                    target: powerTab.backend
                    function onActiveProfileChanged() {
                        var idx = powerTab.backend.fanProfileIds.indexOf(powerTab.backend.activeProfileFanId)
                        if (idx >= 0 && fanCombo.currentIndex !== idx)
                            fanCombo.currentIndex = idx
                    }
                    function onFanProfilesChanged() {
                        Qt.callLater(function() {
                            fanCombo.currentIndex = powerTab.backend.fanProfileIds.indexOf(powerTab.backend.activeProfileFanId)
                        })
                    }
                }

                onActivated: function(index) {
                    var id = powerTab.backend.fanProfileIds[index]
                    if (id && id !== powerTab.backend.activeProfileFanId)
                        powerTab.backend.setActiveFanProfile(id)
                }
            }

            // ── Keyboard Profile ──
            PC3.Label {
                text: i18n("Keyboard profile:")
                opacity: 0.7
                visible: powerTab.backend.keyboardProfileIds.length > 0
            }
            PC3.ComboBox {
                id: kbCombo
                Layout.fillWidth: true
                model: powerTab.backend.keyboardProfileNames
                visible: powerTab.backend.keyboardProfileIds.length > 0
                currentIndex: {
                    var idx = powerTab.backend.keyboardProfileIds.indexOf(powerTab.backend.activeProfileKeyboardId)
                    return idx >= 0 ? idx : 0
                }

                Connections {
                    target: powerTab.backend
                    function onActiveProfileChanged() {
                        var idx = powerTab.backend.keyboardProfileIds.indexOf(powerTab.backend.activeProfileKeyboardId)
                        kbCombo.currentIndex = idx >= 0 ? idx : 0
                    }
                    function onKeyboardProfilesChanged() {
                        Qt.callLater(function() {
                            var idx = powerTab.backend.keyboardProfileIds.indexOf(powerTab.backend.activeProfileKeyboardId)
                            kbCombo.currentIndex = idx >= 0 ? idx : 0
                        })
                    }
                }

                onActivated: function(index) {
                    var id = powerTab.backend.keyboardProfileIds[index]
                    if (id) powerTab.backend.setActiveKeyboardProfile(id)
                }
            }

            // ── GPU OC Profile ──
            PC3.Label {
                text: i18n("GPU OC profile:")
                opacity: 0.7
                visible: powerTab.backend.gpuProfileIds.length > 0
            }
            PC3.ComboBox {
                id: gpuCombo
                Layout.fillWidth: true
                model: powerTab.backend.gpuProfileNames
                visible: powerTab.backend.gpuProfileIds.length > 0
                currentIndex: {
                    var idx = powerTab.backend.gpuProfileIds.indexOf(powerTab.backend.activeProfileGpuId)
                    return idx >= 0 ? idx : 0
                }

                Connections {
                    target: powerTab.backend
                    function onActiveProfileChanged() {
                        var idx = powerTab.backend.gpuProfileIds.indexOf(powerTab.backend.activeProfileGpuId)
                        gpuCombo.currentIndex = idx >= 0 ? idx : 0
                    }
                    function onGpuProfilesChanged() {
                        Qt.callLater(function() {
                            var idx = powerTab.backend.gpuProfileIds.indexOf(powerTab.backend.activeProfileGpuId)
                            gpuCombo.currentIndex = idx >= 0 ? idx : 0
                        })
                    }
                }

                onActivated: function(index) {
                    var id = powerTab.backend.gpuProfileIds[index]
                    if (id) powerTab.backend.setActiveGpuProfile(id)
                }
            }

            PC3.Label {
                text: i18n("Switching profile applies it immediately.")
                opacity: 0.6
            }

            Kirigami.Separator { Layout.fillWidth: true }

            // ── ODM Performance Profile ──
            PlasmaExtras.Heading {
                level: 4
                text: i18n("ODM Performance Mode")
                visible: powerTab.backend.availableODMProfiles.length > 0
            }

            PC3.ComboBox {
                id: odmProfileCombo
                Layout.fillWidth: true
                model: powerTab.backend.availableODMProfiles
                visible: powerTab.backend.availableODMProfiles.length > 0
                currentIndex: Math.max(0, powerTab.backend.availableODMProfiles.indexOf(powerTab.backend.odmPerformanceProfile))

                Connections {
                    target: powerTab.backend
                    function onOdmPerformanceProfileChanged() {
                        odmProfileCombo.currentIndex = Math.max(0,
                            powerTab.backend.availableODMProfiles.indexOf(powerTab.backend.odmPerformanceProfile));
                    }
                }

                onActivated: function(index) {
                    powerTab.backend.setODMPerformanceProfile(model[index]);
                }
            }
        }
    }
}
