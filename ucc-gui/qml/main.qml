// main.qml removed: QML frontend is unused by the native C++ application.
// If you need the QML UI later, restore from history or re-add files.

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Sidebar
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            color: "#2c2c2c"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 5

                Label {
                    text: qsTr("Navigation")
                    font.bold: true
                    color: "#ffffff"
                }

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Dashboard")
                    onClicked: stackView.currentIndex = 0
                }

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Profiles")
                    onClicked: stackView.currentIndex = 1
                }

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Performance")
                    onClicked: stackView.currentIndex = 2
                }

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Hardware")
                    onClicked: stackView.currentIndex = 3
                }

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Fan Control")
                    onClicked: stackView.currentIndex = 4
                }

                Item {
                    Layout.fillHeight: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: "#555555"
                }

                Label {
                    text: profileManager.connected ? qsTr("Connected") : qsTr("Disconnected")
                    color: profileManager.connected ? "#4caf50" : "#f44336"
                    font.pixelSize: 12
                }
            }
        }

        // Main content area
        StackLayout {
            id: stackView
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: 0

            // Control monitoring based on current tab
            onCurrentIndexChanged: {
                // Only monitor when dashboard (index 0) is visible
                systemMonitor.monitoringActive = (currentIndex === 0);
            }

            // Dashboard page
            Page {
                padding: 20

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 20

                    Label {
                        text: qsTr("Dashboard")
                        font.pixelSize: 24
                        font.bold: true
                    }

                    GridLayout {
                        columns: 2
                        rowSpacing: 10
                        columnSpacing: 20

                        Label {
                            text: qsTr("Active Profile:")
                            font.bold: true
                        }
                        Label {
                            text: profileManager.activeProfile || qsTr("None")
                        }

                        Label {
                            text: qsTr("GPU Temperature:")
                            font.bold: true
                        }
                        Label {
                            text: systemMonitor.gpuTemp
                        }

                        Label {
                            text: qsTr("Fan Speed:")
                            font.bold: true
                        }
                        Label {
                            text: systemMonitor.fanSpeed
                        }

                        Label {
                            text: qsTr("Display Brightness:")
                            font.bold: true
                        }
                        Slider {
                            from: 0
                            to: 100
                            value: systemMonitor.displayBrightness
                            onMoved: systemMonitor.displayBrightness = value
                        }
                    }

                    GroupBox {
                        title: qsTr("Quick Controls")
                        Layout.fillWidth: true

                        RowLayout {
                            Switch {
                                text: qsTr("Webcam")
                                checked: systemMonitor.webcamEnabled
                                onToggled: systemMonitor.webcamEnabled = checked
                            }

                            Switch {
                                text: qsTr("Fn Lock")
                                checked: systemMonitor.fnLock
                                onToggled: systemMonitor.fnLock = checked
                            }
                        }
                    }

                    Item {
                        Layout.fillHeight: true
                    }
                }
            }

            // Profiles page
            Page {
                padding: 20

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 15

                    Label {
                        text: qsTr("Profiles")
                        font.pixelSize: 24
                        font.bold: true
                    }

                    GroupBox {
                        title: qsTr("Select Profile")
                        Layout.fillWidth: true
                        Layout.preferredHeight: 100

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10

                            Label {
                                text: qsTr("Active Profile:")
                                font.bold: true
                            }

                            ComboBox {
                                id: profileCombo
                                Layout.fillWidth: true
                                
                                model: profileManager.allProfiles
                                currentIndex: profileManager.activeProfileIndex

                                onCurrentIndexChanged: {
                                    if (currentIndex >= 0) {
                                        profileManager.setActiveProfileByIndex(currentIndex);
                                    }
                                }

                                delegate: ItemDelegate {
                                    width: profileCombo.width
                                    text: modelData
                                    highlighted: profileCombo.highlightedIndex === index
                                    padding: 8
                                }

                                contentItem: Text {
                                    text: profileCombo.displayText
                                    font: profileCombo.font
                                    color: profileCombo.enabled ? "#000000" : "#999999"
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                    padding: 8
                                }

                                background: Rectangle {
                                    implicitWidth: 200
                                    implicitHeight: 40
                                    border.color: profileCombo.activeFocus ? "#1976d2" : "#cccccc"
                                    border.width: profileCombo.activeFocus ? 2 : 1
                                    radius: 3
                                    color: profileCombo.enabled ? "#ffffff" : "#f5f5f5"
                                }

                                popup: Popup {
                                    y: profileCombo.height + 3
                                    width: profileCombo.width
                                    implicitHeight: contentItem.implicitHeight

                                    contentItem: ListView {
                                        clip: true
                                        model: profileCombo.model
                                        currentIndex: profileCombo.highlightedIndex

                                        delegate: ItemDelegate {
                                            width: parent ? parent.width : 200
                                            text: modelData
                                            highlighted: ListView.isCurrentItem
                                            onClicked: {
                                                profileCombo.currentIndex = index;
                                                profileCombo.popup.close();
                                            }
                                            padding: 8
                                        }

                                        ScrollIndicator.vertical: ScrollIndicator { }
                                    }

                                    background: Rectangle {
                                        border.color: "#cccccc"
                                        border.width: 1
                                        radius: 3
                                    }
                                }
                            }

                    GroupBox {
                        title: qsTr("Select Profile")
                        Layout.fillWidth: true
                        Layout.preferredHeight: 100

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10

                            Label {
                                text: qsTr("Active Profile:")
                                font.bold: true
                            }

                            ComboBox {
                                id: profileCombo
                                Layout.fillWidth: true
                                
                                model: profileManager.allProfiles
                                currentIndex: profileManager.activeProfileIndex

                                onCurrentIndexChanged: {
                                    if (currentIndex >= 0) {
                                        profileManager.setActiveProfileByIndex(currentIndex);
                                    }
                                }

                                delegate: ItemDelegate {
                                    width: profileCombo.width
                                    text: modelData
                                    highlighted: profileCombo.highlightedIndex === index
                                    padding: 8
                                }

                                contentItem: Text {
                                    text: profileCombo.displayText
                                    font: profileCombo.font
                                    color: profileCombo.enabled ? "#000000" : "#999999"
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                    padding: 8
                                }

                                background: Rectangle {
                                    implicitWidth: 200
                                    implicitHeight: 40
                                    border.color: profileCombo.activeFocus ? "#1976d2" : "#cccccc"
                                    border.width: profileCombo.activeFocus ? 2 : 1
                                    radius: 3
                                    color: profileCombo.enabled ? "#ffffff" : "#f5f5f5"
                                }

                                popup: Popup {
                                    y: profileCombo.height + 3
                                    width: profileCombo.width
                                    implicitHeight: contentItem.implicitHeight

                                    contentItem: ListView {
                                        clip: true
                                        model: profileCombo.model
                                        currentIndex: profileCombo.highlightedIndex

                                        delegate: ItemDelegate {
                                            width: parent ? parent.width : 200
                                            text: modelData
                                            highlighted: ListView.isCurrentItem
                                            onClicked: {
                                                profileCombo.currentIndex = index;
                                                profileCombo.popup.close();
                                            }
                                            padding: 8
                                        }

                                        ScrollIndicator.vertical: ScrollIndicator { }
                                    }

                                    background: Rectangle {
                                        border.color: "#cccccc"
                                        border.width: 1
                                        radius: 3
                                    }
                                }
                            }

                            Label {
                                text: qsTr("Select a profile to activate it")
                                font.pixelSize: 12
                                color: "#666666"
                            }


                        }
                    }

                    GroupBox {
                        title: qsTr("Custom Profiles")
                        Layout.fillWidth: true

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 10

                            RowLayout {
                                spacing: 10

                                TextField {
                                    id: newProfileName
                                    placeholderText: qsTr("Enter profile name")
                                    Layout.fillWidth: true
                                }

                                Button {
                                    text: qsTr("Create from Default")
                                    onClicked: {
                                        if (newProfileName.text.trim() !== "") {
                                            var profileJson = profileManager.createProfileFromDefault(newProfileName.text.trim());
                                            if (profileJson !== "") {
                                                newProfileName.text = "";
                                                console.log("Profile created successfully");
                                            } else {
                                                console.log("Failed to create profile");
                                            }
                                        }
                                    }
                                }
                            }

                            Label {
                                text: qsTr("Custom Profiles:")
                                font.bold: true
                            }

                            ListView {
                                id: customProfilesList
                                Layout.fillWidth: true
                                Layout.preferredHeight: 200
                                model: profileManager.customProfiles
                                clip: true

                                delegate: ItemDelegate {
                                    width: parent.width
                                    height: 40

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 5
                                        spacing: 10

                                        Label {
                                            text: modelData
                                            Layout.fillWidth: true
                                            verticalAlignment: Text.AlignVCenter
                                        }

                                        Button {
                                            text: qsTr("Delete")
                                            onClicked: {
                                                profileManager.deleteProfile(profileManager.getProfileIdByName(modelData));
                                            }
                                        }
                                    }
                                }

                                ScrollIndicator.vertical: ScrollIndicator { }
                            }

                            Label {
                                text: customProfilesList.count === 0 ? qsTr("No custom profiles") : ""
                                font.pixelSize: 12
                                color: "#666666"
                            }
                        }
                    }
                }
            }

            // Performance page placeholder
            Page {
                padding: 20
                Label {
                    text: qsTr("Performance Controls")
                    font.pixelSize: 24
                    font.bold: true
                }
            }

            // Hardware page placeholder
            Page {
                padding: 20
                Label {
                    text: qsTr("Hardware Information")
                    font.pixelSize: 24
                    font.bold: true
                }
            }

            // Fan Control Tab
            Page {
                padding: 20
                FanControlTab {
                    anchors.fill: parent
                }
            }
        }
    }
}
