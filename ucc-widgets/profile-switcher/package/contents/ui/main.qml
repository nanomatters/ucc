// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Uniwill Control Center Contributors

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras
import QtDBus

PlasmoidItem {
    id: root

    width: PlasmaCore.Units.gridUnit * 8
    height: PlasmaCore.Units.gridUnit * 4

    Plasmoid.backgroundHints: PlasmaCore.Types.DefaultBackground

    property string activeProfile: "Default"
    property var profiles: []
    property var profileMap: ({})  // Maps profile names to IDs

    // DBus interface to uccd
    DBusInterface {
        id: tccdInterface
        service: "com.uniwill.uccd"
        path: "/com/uniwill/uccd"
        iface: "com.uniwill.uccd"
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        onTriggered: updateActiveProfile()
    }

    Component.onCompleted: {
        loadProfiles()
        updateActiveProfile()
    }

    function loadProfiles() {
        try {
            var result = tccdInterface.call("GetProfilesJSON", [])
            if (result.length > 0) {
                var profilesJSON = result[0]
                var parsedProfiles = JSON.parse(profilesJSON)
                
                profiles = []
                profileMap = {}
                
                for (var i = 0; i < parsedProfiles.length; i++) {
                    var profile = parsedProfiles[i]
                    profiles.push(profile.name)
                    profileMap[profile.name] = profile.id
                }
                
                console.log("Loaded " + profiles.length + " profiles")
            }
        } catch (e) {
            console.log("Error loading profiles:", e)
            // Fallback profiles
            profiles = ["Default", "Cool and breezy", "Powersave extreme"]
        }
    }

    function updateActiveProfile() {
        try {
            var result = tccdInterface.call("GetActiveProfileJSON", [])
            if (result.length > 0) {
                var activeJSON = result[0]
                var active = JSON.parse(activeJSON)
                activeProfile = active.name || "Default"
            }
        } catch (e) {
            console.log("Error getting active profile:", e)
        }
    }

    function setProfile(profileName) {
        try {
            console.log("Switching to profile:", profileName)
            
            // Find the profile ID
            var profileId = profileMap[profileName]
            if (profileId) {
                console.log("Using profile ID:", profileId)
                tccdInterface.call("SetTempProfileById", [profileId])
            } else {
                // Fallback: try using name directly
                console.log("Profile ID not found, using name directly")
                tccdInterface.call("SetTempProfile", [profileName])
            }
            
            // Update active profile after a short delay
            updateTimer.start()
        } catch (e) {
            console.log("Error setting profile:", e)
        }
    }

    Timer {
        id: updateTimer
        interval: 500
        onTriggered: updateActiveProfile()
    }

    fullRepresentation: Item {
        Layout.preferredWidth: PlasmaCore.Units.gridUnit * 15
        Layout.preferredHeight: PlasmaCore.Units.gridUnit * 12

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: PlasmaCore.Units.smallSpacing
            spacing: PlasmaCore.Units.largeSpacing

            PlasmaComponents.Label {
                text: "Profile Switcher"
                font.pointSize: 14
                font.bold: true
                Layout.alignment: Qt.AlignHCenter
            }

            PlasmaComponents.Label {
                text: "Active: " + root.activeProfile
                font.italic: true
                Layout.alignment: Qt.AlignHCenter
            }

            PlasmaExtras.ScrollArea {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    model: root.profiles
                    spacing: PlasmaCore.Units.smallSpacing

                    delegate: PlasmaComponents.Button {
                        width: ListView.view.width
                        text: modelData
                        highlighted: modelData === root.activeProfile
                        onClicked: root.setProfile(modelData)
                    }
                }
            }

            PlasmaComponents.Button {
                Layout.fillWidth: true
                text: "Reload Profiles"
                onClicked: {
                    root.loadProfiles()
                    root.updateActiveProfile()
                }
            }
        }
    }

    compactRepresentation: Item {
        PlasmaComponents.Label {
            anchors.centerIn: parent
            text: root.activeProfile
            elide: Text.ElideRight
            font.bold: true
        }

        MouseArea {
            anchors.fill: parent
            onClicked: root.expanded = !root.expanded
        }
    }
}

