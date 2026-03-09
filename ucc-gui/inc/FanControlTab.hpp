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

#include <QWidget>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QDBusInterface>
#include <QTimer>
#include <QJsonArray>
#include <QMap>

#include "FanCurveEditorWidget.hpp"
#include "PumpCurveEditorWidget.hpp"
#include "LCTWaterCoolerController.hpp"
#include "CommonTypes.hpp"
#include "../../libucc-dbus/UccdClient.hpp"
#include "ProfileManager.hpp"

namespace ucc
{

/**
 * @brief Fan control tab widget.
 *
 * Contains the fan profile selection bar, dynamic sub-tabs for system
 * and water-cooler zones, all fan curve editors, pump voltage curve
 * editors, and the hardware water-cooler controls (enable, pump manual,
 * LED, colour).
 *
 * Zone editors are created dynamically from profile data via
 * buildZoneEditors().
 */
class FanControlTab : public QWidget
{
  Q_OBJECT

public:
  explicit FanControlTab( UccdClient *client,
                          ProfileManager *profileManager,
                          bool waterCoolerSupported,
                          QWidget *parent = nullptr );
  ~FanControlTab() override = default;

  // ── Accessors used by MainWindow ──
  QComboBox *fanProfileCombo() const { return m_fanProfileCombo; }

  // Dynamic zone editor accessors (return nullptr if zone not present)
  FanCurveEditorWidget *fanEditor( const QString &zoneId ) const;
  PumpCurveEditorWidget *pumpEditor( const QString &zoneId ) const;
  const QMap< QString, FanCurveEditorWidget * > &fanEditors() const { return m_fanEditors; }
  const QMap< QString, PumpCurveEditorWidget * > &pumpEditors() const { return m_pumpEditors; }

  /** Build/rebuild zone editors from profile zone metadata. */
  void buildZoneEditors( const QJsonArray &zones, const QJsonArray &thermalSources );

  /** Return the currently selected thermal source ID for a zone. */
  QString thermalSourceForZone( const QString &zoneId ) const;

  /** Set the thermal source combo for a zone (used when loading profile overrides). */
  void setThermalSourceForZone( const QString &zoneId, const QString &thermalSourceId );

  QPushButton *applyButton()  const { return m_applyFanProfilesButton; }
  QPushButton *saveButton()   const { return m_saveFanProfilesButton; }
  QPushButton *copyButton()   const { return m_copyFanProfileButton; }
  QPushButton *removeButton() const { return m_removeFanProfileButton; }
  QPushButton *revertButton() const { return m_revertFanProfilesButton; }

  /** Update the water-cooler enable checkbox without re-triggering signals. */
  void setWaterCoolerEnabled( bool enabled );
  void sendWaterCoolerEnable( bool enabled );
  bool isWaterCoolerEnabled() const;

  /** Update water cooler manual control state based on auto control setting. */
  void setWaterCoolerAutoControl( bool autoControl );

  /** Start/stop water cooler polling based on enable state. */
  void updateWaterCoolerPolling();

  QString currentFanProfile() const { return m_currentFanProfile; }
  void setCurrentFanProfile( const QString &name ) { m_currentFanProfile = name; }

  /** Reload combo items from daemon + custom store. */
  void reloadFanProfiles();

  /** Update button enable states. */
  void updateButtonStates( bool uccdConnected );

  /** Set editor editability. */
  void setEditorsEditable( bool editable );

signals:
  void applyRequested();
  void saveRequested();
  void revertRequested();
  void copyRequested();
  void removeRequested();
  void fanProfileChanged( const QString &fanProfileId );
  void fanProfileRenamed( const QString &oldName, const QString &newName );
  void fanCurveChanged( const QString &zoneId, const QVector<FanCurveEditorWidget::Point> &points );
  void pumpCurveChanged( const QString &zoneId, const QVector<PumpCurveEditorWidget::Point> &points );
  void thermalSourceChanged( const QString &zoneId, const QString &thermalSourceId );
  void waterCoolerEnableChanged( bool enabled );

private slots:
  // Water cooler hardware slots
  void onWaterCoolerEnableToggled( bool enabled );
  void onConnected();
  void onDisconnected();
  void onPumpVoltageChanged( int index );
  void onFanSpeedChanged( int speed );
  void onLEDOnOffChanged( bool enabled );
  void onLEDModeChanged( int index );
  void onColorPickerClicked();
  void onFanProfileComboRenamed();

private:
  void setupUI();
  void connectSignals();
  void updateColorButtonState();
  void updateManualControlState();

  UccdClient *m_uccdClient;
  ProfileManager *m_profileManager;

  // Fan profile selection bar
  QComboBox *m_fanProfileCombo = nullptr;
  QPushButton *m_applyFanProfilesButton = nullptr;
  QPushButton *m_saveFanProfilesButton = nullptr;
  QPushButton *m_revertFanProfilesButton = nullptr;
  QPushButton *m_copyFanProfileButton = nullptr;
  QPushButton *m_removeFanProfileButton = nullptr;
  QString m_currentFanProfile;

  // Dynamic zone editors (created by buildZoneEditors)
  QMap< QString, FanCurveEditorWidget * > m_fanEditors;
  QMap< QString, PumpCurveEditorWidget * > m_pumpEditors;
  QMap< QString, QComboBox * > m_thermalSourceCombos;

  // Sub-tab infrastructure (one tab per zone, created by buildZoneEditors)
  QTabWidget *m_subTabs = nullptr;

  // Water cooler hardware controls (moved from HardwareTab)
  QDBusInterface *m_waterCoolerDbus = nullptr;
  QTimer *m_waterCoolerPollTimer = nullptr;
  bool m_isWcConnected = false;
  QPushButton *m_waterCoolerEnableCheckBox = nullptr;
  QComboBox *m_pumpVoltageCombo = nullptr;
  QCheckBox *m_ledOnOffCheckBox = nullptr;
  QPushButton *m_colorPickerButton = nullptr;
  QComboBox *m_ledModeCombo = nullptr;
  QSlider *m_fanSpeedSlider = nullptr;
  int m_currentRed = 255;
  int m_currentGreen = 0;
  int m_currentBlue = 0;

  bool m_autoControl = true;
  bool m_manualControlInitialized = false;
  bool m_waterCoolerSupported = false;
  QWidget *m_wcHardwareWidget = nullptr;
};

} // namespace ucc
