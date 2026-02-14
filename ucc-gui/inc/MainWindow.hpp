/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QMainWindow>
#include <QStatusBar>
#include <QComboBox>
#include <QLabel>
#include <QSlider>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QTabWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QSpinBox>
#include <QListWidget>
#include <QLineEdit>
#include <QInputDialog>
#include <QColorDialog>
#include <QStackedWidget>
#include <QtWidgets/QTableWidget>
#include <memory>
#include "ProfileManager.hpp"
#include "SystemMonitor.hpp"
#include "../libucc-dbus/UccdClient.hpp"
#include "FanCurveEditorWidget.hpp"
#include "PumpCurveEditorWidget.hpp"
#include "KeyboardVisualizerWidget.hpp"
#include "DashboardTab.hpp"
#include "HardwareTab.hpp"
#include "FanControlTab.hpp"

namespace ucc
{
  /**
   * @brief Main application window with C++ Qt widgets
   */
  class MainWindow : public QMainWindow
  {
    Q_OBJECT

  public:
    explicit MainWindow( QWidget *parent = nullptr );
    ~MainWindow() override;

  private slots:
    // Profile page slots
    void onProfileIndexChanged( int index );
    void onAllProfilesChanged();
    void onActiveProfileIndexChanged();
    void onCustomKeyboardProfilesChanged();
    void onBrightnessSliderChanged( int value );
    void onOffsetFanSpeedChanged( int value );
    void onCpuCoresChanged( int value );
    void onMaxFrequencyChanged( int value );
    void onODMPowerLimit1Changed( int value );
    void onODMPowerLimit2Changed( int value );
    void onODMPowerLimit3Changed( int value );
    void onGpuPowerChanged( int value );
    void onApplyClicked();
    void onSaveClicked();
    void onApplyFanProfilesClicked();
    void onSaveFanProfilesClicked();
    void onRevertFanProfilesClicked();
    void onAddProfileClicked();
    void onCopyProfileClicked();
    void onRemoveProfileClicked();
    void onAddFanProfileClicked();
    void onRemoveFanProfileClicked();
    void onCpuFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points);
    void onGpuFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points);
    void onWaterCoolerFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points);
    void onPumpPointsChanged(const QVector<PumpCurveEditorWidget::Point>& points);
    void onFanProfileChanged(const QString& profileName);
    void onCopyFanProfileClicked();

    // Dashboard page slots
    void onTabChanged( int index );
    void onKeyboardBrightnessChanged( int value );
    void onKeyboardColorClicked();
    void onKeyboardVisualizerColorsChanged();
    void onKeyboardProfileChanged(const QString& profileName);
    void onAddKeyboardProfileClicked();
    void onCopyKeyboardProfileClicked();
    void onSaveKeyboardProfileClicked();
    void onRemoveKeyboardProfileClicked();
    void reloadKeyboardProfiles();
    void updateKeyboardProfileButtonStates();
    void onProfileComboRenamed();
    void onKeyboardProfileComboRenamed();

  private:
    struct FanPoint {
        int temp;
        int speed;
    };

    void setupUI();
    void setupDashboardPage();
    void setupProfilesPage();
    void setupHardwarePage();
    void setupKeyboardBacklightPage();
    void connectKeyboardBacklightPageWidgets();
    void loadFanPoints();
    void saveFanPoints();
    void connectSignals();
    void populateGovernorCombo();

    // Update fan profile combo from daemon and custom store
    void reloadFanProfiles();

    // Slot: called when DBus connection status changes
    void onUccdConnectionChanged( bool connected );
    void loadProfileDetails( const QString &profileName );
    void markChanged();
    void updateButtonStates();
    void setupFanControlTab();
    void connectFanControlTab();
    void updateProfileEditingWidgets( bool isCustom );

    std::unique_ptr< ProfileManager > m_profileManager;
    std::unique_ptr< SystemMonitor > m_systemMonitor;
    std::unique_ptr< UccdClient > m_UccdClient;

    // Tab widget
    QTabWidget *m_tabs = nullptr;

    // Dashboard tab
    DashboardTab *m_dashboardTab = nullptr;

    // Hardware tab
    HardwareTab *m_hardwareTab = nullptr;

    // Profiles widgets
    QComboBox *m_profileCombo = nullptr;
    int m_selectedProfileIndex = -1;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_copyProfileButton = nullptr;
    QPushButton *m_removeProfileButton = nullptr;
    QTextEdit *m_descriptionEdit = nullptr;
    QPushButton *m_mainsButton = nullptr;
    QPushButton *m_batteryButton = nullptr;
    QPushButton *m_waterCoolerButton = nullptr;

    constexpr bool profileTopWidgetsAvailable() const
    { return m_applyButton && m_saveButton && m_copyProfileButton && m_removeProfileButton && m_profileCombo; }

    // Display controls
    QCheckBox *m_setBrightnessCheckBox = nullptr;
    QSlider *m_brightnessSlider = nullptr;
    QLabel *m_brightnessValueLabel = nullptr;
    QComboBox *m_profileKeyboardProfileCombo = nullptr;

    // Charging profile (per-profile firmware-level mode)
    QComboBox *m_profileChargingProfileCombo = nullptr;
    QComboBox *m_profileChargingPriorityCombo = nullptr;
    QComboBox *m_profileChargeLimitCombo = nullptr;

    // Fan control widgets (profile page)
    QSlider *m_offsetFanSpeedSlider = nullptr;
    QLabel *m_offsetFanSpeedValue = nullptr;
    QCheckBox *m_sameFanSpeedCheckBox = nullptr;
    QCheckBox *m_autoWaterControlCheckBox = nullptr;
    QComboBox *m_profileFanProfileCombo = nullptr;
    QVector<FanPoint> m_cpuFanPoints;
    QVector<FanPoint> m_gpuFanPoints;
    QVector<FanPoint> m_waterCoolerFanPoints;

    // Fan control tab (owns editors, combo, buttons, water cooler hw controls)
    FanControlTab *m_fanControlTab = nullptr;

    // CPU frequency control widgets
    QSlider *m_cpuCoresSlider = nullptr;
    QLabel *m_cpuCoresValue = nullptr;
    QComboBox *m_governorCombo = nullptr;
    QSlider *m_minFrequencySlider = nullptr;
    QLabel *m_minFrequencyValue = nullptr;
    QSlider *m_maxFrequencySlider = nullptr;
    QLabel *m_maxFrequencyValue = nullptr;
    QVector< int > m_availableFrequenciesMHz;  // Available CPU frequencies in MHz

    /**
     * @brief Snap a frequency value to the closest available hardware frequency.
     *
     * If the available-frequencies list is empty the input value is returned
     * unchanged so the caller can always use the result directly.
     */
    int snapToAvailableFrequency( int valueMHz ) const;

    // ODM Power Limit (TDP) widgets
    QSlider *m_odmPowerLimit1Slider = nullptr;
    QLabel *m_odmPowerLimit1Value = nullptr;
    QSlider *m_odmPowerLimit2Slider = nullptr;
    QLabel *m_odmPowerLimit2Value = nullptr;
    QSlider *m_odmPowerLimit3Slider = nullptr;
    QLabel *m_odmPowerLimit3Value = nullptr;

    // GPU power control
    QSlider *m_gpuPowerSlider = nullptr;
    QLabel *m_gpuPowerValue = nullptr;

    // Keyboard backlight widgets
    QSlider *m_keyboardBrightnessSlider = nullptr;
    QLabel *m_keyboardBrightnessValueLabel = nullptr;
    QPushButton *m_keyboardColorButton = nullptr;
    KeyboardVisualizerWidget *m_keyboardVisualizer = nullptr;

    // Keyboard profile widgets
    QComboBox *m_keyboardProfileCombo = nullptr;
    QPushButton *m_addKeyboardProfileButton = nullptr;
    QPushButton *m_copyKeyboardProfileButton = nullptr;
    QPushButton *m_saveKeyboardProfileButton = nullptr;
    QPushButton *m_removeKeyboardProfileButton = nullptr;

    // Keyboard color widgets
    QLabel *m_keyboardColorLabel = nullptr;

    // Change tracking
    bool m_profileChanged = false;
    QString m_currentLoadedProfile;
    QString m_currentFanProfile;
    bool m_loadedMainsAssignment = false;
    bool m_loadedBatteryAssignment = false;
    bool m_loadedWaterCoolerAssignment = false;
    bool m_saveInProgress = false;
    bool m_initializing = true;  // true during constructor, prevents hardware writes

    // Device capability flags (queried from daemon at startup)
    bool m_waterCoolerSupported = false;
    bool m_cTGPAdjustmentSupported = true;
  };
}
