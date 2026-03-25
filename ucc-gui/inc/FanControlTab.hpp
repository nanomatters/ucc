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
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QDBusInterface>
#include <QTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

#include "FanCurveEditorWidget.hpp"
#include "PumpCurveEditorWidget.hpp"
#include "LCTWaterCoolerController.hpp"
#include "CommonTypes.hpp"
#include "UccDbusTypes.hpp"
#include "../../libucc-dbus/UccdClient.hpp"
#include "ProfileManager.hpp"

class QTableWidget;
class QTableWidgetItem;
class QLineEdit;

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

  /** Build/rebuild zone editors from topology/source metadata and raw hardware inventory. */
  void buildZoneEditors( const ucc::dbus::FanZoneDtoList &zones,
                         const ucc::dbus::ThermalSourceDtoList &thermalSources,
                         const ucc::dbus::HardwareFanDeviceDtoList &hardwareFanDevices,
                         const ucc::dbus::HardwareSensorDtoList &hardwareSensors );

  /** Return the currently selected thermal source ID for a zone. */
  QString thermalSourceForZone( const QString &zoneId ) const;

  /** Set the thermal source combo for a zone (used when loading profile overrides). */
  void setThermalSourceForZone( const QString &zoneId, const QString &thermalSourceId );

  /** Return current temperature-source editor model for fan profile persistence. */
  ucc::dbus::ThermalSourceDtoList thermalSourcesData() const;

  /** Return current zone editor model (name/type/fanIds/source) for profile persistence. */
  ucc::dbus::FanZoneDtoList fanZonesData() const;

  /** Persist user-defined sensor aliases to ~/.config/uccrc. */
  void saveSensorAliasesToSettings() const;

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

  /** Mark all zone graphs as clean (e.g. right after profile load). */
  void clearZoneGraphModifiedFlags();

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
  void onWaterCoolerPollTimeout();

private:
  void setupUI();
  void connectSignals();
  void updateColorButtonState();
  void updateManualControlState();
  void populateZoneTemplateCombo( QComboBox *combo ) const;
  void applyCurveTemplateToZone( const QString &zoneId,
                                 const QString &templateId );

  // ── Temperature source / zone editor helpers ──
  void showStatusMessage( const QString &msg, int timeoutMs = 3000 );
  void loadSensorAliasesFromSettings();
  static QString normalizedSourceGroup( const ucc::dbus::HardwareSensorDto &sensor );
  void ensureValidStrategy( ucc::dbus::ThermalSourceDto &src );
  QString sensorsSummaryText( const ucc::dbus::ThermalSourceDto &src ) const;
  void refreshAllSourceCombos();
  void installStrategyComboForRow( int row );
  void onStrategyComboChanged( int row );
  void refreshSourceRow( int row );
  void addSensorToSource( int row, const QString &sensorId );
  void createSourceWithSensor( const QString &sensorId );
  void onSourceItemChanged( QTableWidgetItem *item );
  void onSourceContextMenu( const QPoint &pos );
  void handleRemoveSource( int row );
  void handleRemoveSensor( int row, const QString &sensorId );
  void onSensorDropped( int row, const QString &sensorId );
  void refreshZoneRow( int row );
  void onZoneItemChanged( QTableWidgetItem *item );
  void installZoneSourceComboForRow( int row );
  void installZoneTypeComboForRow( int row );
  void onZoneSourceComboChanged( int row );
  void onCurveTabSourceComboChanged( const QString &zoneId, const QString &tsId );
  void onSensorTreeItemChanged( QTreeWidget *tree, QTreeWidgetItem *item );
  void onDeviceTreeItemChanged( QTreeWidget *tree, QTreeWidgetItem *item );
  void onTemplateComboChanged( const QString &zoneId, const QString &templateId, QComboBox *combo );
  void onFanEditorPointsChanged( const QString &zoneId, const QVector< FanCurveEditorWidget::Point > &pts );
  void rebuildZoneEditorsWithState( const ucc::dbus::FanZoneDtoList &zones,
                                    const QMap< QString, QVector< FanCurveEditorWidget::Point > > &fanPoints,
                                    const QMap< QString, QVector< PumpCurveEditorWidget::Point > > &pumpPoints,
                                    const QMap< QString, QString > &sourceByZone );
  void onZoneContextMenu( const QPoint &pos );
  void onDeviceDroppedOnZone( int row, const QString &deviceId );
  void addDeviceToZone( int row, const QString &deviceId );
  void handleRemoveDevice( int row, const QString &deviceId );
  QString devicesSummaryText( const ucc::dbus::FanZoneDto &zone ) const;
  static QString normalizedDeviceGroup( const ucc::dbus::HardwareFanDeviceDto &device );
  static void selectComboByData( QComboBox *combo, const QString &data );

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
  QMap< QString, QComboBox * > m_templateSourceCombos;
  QMap< QString, QString > m_selectedTemplateByZone;
  QMap< QString, bool > m_zoneGraphModifiedByUser;
  QMap< QString, bool > m_zoneProgrammaticUpdate;

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

  // ── Source editor state (populated by buildZoneEditors) ──
  QVector< ucc::dbus::ThermalSourceDto > m_sourceEditorModel;
  QVector< QComboBox * > m_strategyCombos;
  QVector< QComboBox * > m_allSourceCombos;
  QTableWidget *m_sourceTable = nullptr;
  QMap< QString, QString > m_sensorLabelById;
  QMap< QString, QString > m_sensorAliasById;
  QMap< QString, QString > m_deviceAliasById;

  // ── Zone editing state ──
  QVector< ucc::dbus::FanZoneDto > m_zoneCache;
  QMap< QString, QString > m_fanLabelById;
  QTableWidget *m_zoneTable = nullptr;
  QVector< QComboBox * > m_zoneSourceCombos;

  // Last build args (needed for rebuild on source removal)
  ucc::dbus::FanZoneDtoList m_lastZones;
  ucc::dbus::HardwareFanDeviceDtoList m_lastFanDevices;
  ucc::dbus::HardwareSensorDtoList m_lastSensors;

  // ── Live sensor readings polling ──
  QTimer *m_sensorPollTimer = nullptr;
  QTreeWidget *m_sensorTree = nullptr;
  QTreeWidget *m_deviceTree = nullptr;
  QVariantMap m_sensorReadings;      // sensorId → value, _source:id → value, fan:id → RPM
  QVariantMap m_zoneTelemetry;       // zoneId → {temp, duty}
  void pollSensorReadings();
  void updateSensorTreeValues();
  void updateSourceTableValues();
  void updateZoneTableValues();
  void updateDeviceTreeValues();
};

} // namespace ucc
