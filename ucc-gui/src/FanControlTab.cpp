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

#include "FanControlTab.hpp"

#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QFormLayout>
#include <QListWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QSplitter>
#include <QTreeWidget>
#include <QScrollArea>
#include <QColorDialog>
#include <QMainWindow>
#include <QMenu>
#include <QStatusBar>
#include <QDBusReply>
#include <QDebug>
#include <QDropEvent>
#include "CommonTypes.hpp"

namespace ucc
{

class ThermalSourceTableWidget : public QTableWidget
{
public:
  explicit ThermalSourceTableWidget( QWidget *parent = nullptr )
    : QTableWidget( parent )
  {}

  std::function< void( int, const QString & ) > onSensorDropped;

protected:
  void dropEvent( QDropEvent *event ) override
  {
    if ( !event )
      return;

    if ( auto *tree = qobject_cast< QTreeWidget * >( event->source() ) )
    {
      if ( QTreeWidgetItem *item = tree->currentItem(); item )
      {
        const QString sensorId = item->data( 0, Qt::UserRole ).toString();
        if ( !sensorId.isEmpty() )
        {
          const int targetRow = rowAt( event->position().toPoint().y() );
          if ( onSensorDropped )
          {
            onSensorDropped( targetRow, sensorId );
            if ( targetRow >= 0 )
              setCurrentCell( targetRow, 0 );
            event->acceptProposedAction();
            return;
          }
        }
      }
    }

    QTableWidget::dropEvent( event );
  }
};

static const QStringList &strategyChoices()
{
  static const QStringList list = {
    QStringLiteral( "single" ),
    QStringLiteral( "max" ),
    QStringLiteral( "average" ),
    QStringLiteral( "weightedAvg" ),
    QStringLiteral( "percentile90" ),
    QStringLiteral( "safetyClampedAvg" ),
  };
  return list;
}

FanControlTab::FanControlTab( UccdClient *client,
                              ProfileManager *profileManager,
                              bool waterCoolerSupported,
                              QWidget *parent )
  : QWidget( parent )
  , m_uccdClient( client )
  , m_profileManager( profileManager )
  , m_waterCoolerSupported( waterCoolerSupported )
{
  // DBus interface for water cooler hardware controls (only if water cooler supported)
  if ( m_waterCoolerSupported )
  {
    m_waterCoolerDbus = new QDBusInterface(
      QStringLiteral( "com.uniwill.uccd" ),
      QStringLiteral( "/com/uniwill/uccd" ),
      QStringLiteral( "com.uniwill.uccd" ),
      QDBusConnection::systemBus(), this );

    m_waterCoolerPollTimer = new QTimer( this );
    connect( m_waterCoolerPollTimer, &QTimer::timeout, this, [this]() {
      // Only poll if water cooler is actually enabled
      if ( not m_waterCoolerDbus ) return;
      if ( bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;
           !wcEnabled ) {
        // If water cooler becomes disabled, force disconnect
        onDisconnected();
        return;
      }
      if ( QDBusReply< bool > conn = m_waterCoolerDbus->call( QStringLiteral( "GetWaterCoolerConnected" ) );
           conn.isValid() && conn.value() )
        onConnected();
      else
        onDisconnected();
    } );
    // Don't start timer immediately - wait for water cooler to be enabled
  }

  setupUI();
  connectSignals();
}

// ── UI construction ─────────────────────────────────────────────────

void FanControlTab::setupUI()
{
  QVBoxLayout *mainLayout = new QVBoxLayout( this );
  mainLayout->setContentsMargins( 0, 0, 0, 0 );
  mainLayout->setSpacing( 0 );

  // ── Top bar: fan profile selection ──
  QHBoxLayout *selectLayout = new QHBoxLayout();
  m_fanProfileCombo = new QComboBox();
  m_fanProfileCombo->setEditable( true );
  m_fanProfileCombo->setInsertPolicy( QComboBox::NoInsert );

  for ( const auto &v : m_profileManager->fanProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    m_fanProfileCombo->addItem( name, id );
  }

  m_applyFanProfilesButton = new QPushButton( "Apply" );
  m_applyFanProfilesButton->setMaximumWidth( 80 );
  m_applyFanProfilesButton->setEnabled( false );

  m_saveFanProfilesButton = new QPushButton( "Save" );
  m_saveFanProfilesButton->setMaximumWidth( 80 );
  m_saveFanProfilesButton->setEnabled( false );

  m_copyFanProfileButton = new QPushButton( "Copy" );
  m_copyFanProfileButton->setMaximumWidth( 60 );
  m_copyFanProfileButton->setEnabled( false );

  m_removeFanProfileButton = new QPushButton( "Remove" );
  m_removeFanProfileButton->setMaximumWidth( 70 );

  selectLayout->addWidget( m_fanProfileCombo, 1 );
  selectLayout->addWidget( m_applyFanProfilesButton );
  selectLayout->addWidget( m_saveFanProfilesButton );
  selectLayout->addWidget( m_copyFanProfileButton );
  selectLayout->addWidget( m_removeFanProfileButton );
  mainLayout->addLayout( selectLayout );

  QFrame *separator = new QFrame();
  separator->setFrameShape( QFrame::HLine );
  mainLayout->addWidget( separator );

  // ── Water cooler hardware controls (compact bar above zone tabs) ──
  if ( m_waterCoolerSupported )
  {
    m_wcHardwareWidget = new QWidget();
    QHBoxLayout *wcHw = new QHBoxLayout( m_wcHardwareWidget );
    wcHw->setContentsMargins( 5, 2, 5, 2 );
    wcHw->setSpacing( 4 );

    m_waterCoolerEnableCheckBox = new QPushButton( "Enable" );
    m_waterCoolerEnableCheckBox->setCheckable( true );
    m_waterCoolerEnableCheckBox->setChecked( ucc::WATER_COOLER_INITIAL_STATE );
    m_waterCoolerEnableCheckBox->setToolTip( tr( "When enabled the daemon will scan for water cooler devices" ) );
    m_waterCoolerEnableCheckBox->setFixedHeight( 24 );
    m_waterCoolerEnableCheckBox->setStyleSheet([
      ]() {
        const QString enabledColor = QStringLiteral("#4caf50");
        const QString disabledColor = QStringLiteral("#d32f2f");
        return QStringLiteral("QPushButton { font-size: 11px; padding: 2px 12px; border: 1px solid palette(mid); border-radius: 4px; background-color: %1; }"
                              "QPushButton:checked { background-color: %2; font-weight: bold; }")
               .arg(disabledColor, enabledColor);
      }() );
    wcHw->addWidget( m_waterCoolerEnableCheckBox );

    QLabel *pumpVoltageLabel = new QLabel( "Pump Voltage:" );
    m_pumpVoltageCombo = new QComboBox();
    m_pumpVoltageCombo->addItem( "Off", QVariant::fromValue( PumpVoltage::Off ) );
    m_pumpVoltageCombo->addItem( "7V",  QVariant::fromValue( PumpVoltage::V7  ) );
    m_pumpVoltageCombo->addItem( "8V",  QVariant::fromValue( PumpVoltage::V8  ) );
    m_pumpVoltageCombo->addItem( "11V", QVariant::fromValue( PumpVoltage::V11 ) );
    m_pumpVoltageCombo->setCurrentIndex( 0 );
    m_pumpVoltageCombo->setEnabled( false );
    m_pumpVoltageCombo->setMaximumWidth( 70 );
    wcHw->addWidget( pumpVoltageLabel );
    wcHw->addWidget( m_pumpVoltageCombo );

    QLabel *fanSpeedLabel = new QLabel( "Fan Speed:" );
    m_fanSpeedSlider = new QSlider( Qt::Horizontal );
    m_fanSpeedSlider->setMinimum( 0 );
    m_fanSpeedSlider->setMaximum( 100 );
    m_fanSpeedSlider->setValue( 0 );
    m_fanSpeedSlider->setEnabled( false );
    wcHw->addWidget( fanSpeedLabel );
    wcHw->addWidget( m_fanSpeedSlider );

    m_ledOnOffCheckBox = new QCheckBox( "LED" );
    m_ledOnOffCheckBox->setChecked( true );
    m_ledOnOffCheckBox->setEnabled( true );
    m_ledOnOffCheckBox->setLayoutDirection( Qt::RightToLeft );
    wcHw->addWidget( m_ledOnOffCheckBox );

    m_colorPickerButton = new QPushButton( "Choose Color" );
    m_colorPickerButton->setEnabled( false );
    wcHw->addWidget( m_colorPickerButton );

    QLabel *ledModeLabel = new QLabel( "Mode:" );
    m_ledModeCombo = new QComboBox();
    m_ledModeCombo->addItem( "Static",        QVariant::fromValue( RGBState::Static ) );
    m_ledModeCombo->addItem( "Breathe",       QVariant::fromValue( RGBState::Breathe ) );
    m_ledModeCombo->addItem( "Colorful",      QVariant::fromValue( RGBState::Colorful ) );
    m_ledModeCombo->addItem( "Breathe Color", QVariant::fromValue( RGBState::BreatheColor ) );
    m_ledModeCombo->addItem( "Temperature",   QVariant::fromValue( RGBState::Temperature ) );
    m_ledModeCombo->setCurrentIndex( 0 );
    m_ledModeCombo->setEnabled( true );
    wcHw->addWidget( ledModeLabel );
    wcHw->addWidget( m_ledModeCombo );

    updateColorButtonState();

    mainLayout->addWidget( m_wcHardwareWidget );
  }

  // ── Sub-tabs (one per zone, populated by buildZoneEditors()) ──
  m_subTabs = new QTabWidget();
  m_subTabs->setStyleSheet(
    "QTabWidget::pane { border: none; }"
    "QTabBar::tab { padding: 6px 18px; }" );

  mainLayout->addWidget( m_subTabs );
}

// ── Signal wiring ───────────────────────────────────────────────────

void FanControlTab::connectSignals()
{
  // Fan profile combo - use index-based signal to avoid rename keystrokes triggering profile load
  connect( m_fanProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, [this]( int index ) {
    if ( index >= 0 )
      emit fanProfileChanged( m_fanProfileCombo->itemData( index ).toString() );
  } );

  // Fan profile combo rename handling
  connect( m_fanProfileCombo->lineEdit(), &QLineEdit::editingFinished,
           this, &FanControlTab::onFanProfileComboRenamed );

  // NOTE: zone editor → signal connections are wired dynamically in
  //       buildZoneEditors() so they keep in sync with the zone set.

  // Action buttons → signals
  connect( m_applyFanProfilesButton, &QPushButton::clicked,
           this, &FanControlTab::applyRequested );
  connect( m_saveFanProfilesButton, &QPushButton::clicked,
           this, &FanControlTab::saveRequested );
  connect( m_copyFanProfileButton, &QPushButton::clicked,
           this, &FanControlTab::copyRequested );
  connect( m_removeFanProfileButton, &QPushButton::clicked,
           this, &FanControlTab::removeRequested );

  // Water cooler hardware controls
  if ( m_waterCoolerSupported )
  {
    connect( m_waterCoolerEnableCheckBox, &QPushButton::toggled,
             this, &FanControlTab::onWaterCoolerEnableToggled );
    connect( m_pumpVoltageCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &FanControlTab::onPumpVoltageChanged );
    connect( m_fanSpeedSlider, &QSlider::valueChanged,
             this, &FanControlTab::onFanSpeedChanged );
    connect( m_ledOnOffCheckBox, &QCheckBox::toggled,
             this, &FanControlTab::onLEDOnOffChanged );
    connect( m_colorPickerButton, &QPushButton::clicked,
             this, &FanControlTab::onColorPickerClicked );
    connect( m_ledModeCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &FanControlTab::onLEDModeChanged );
  }

  // Initialize water cooler polling based on initial enabled state
  updateWaterCoolerPolling();
}

// ── Public helpers ──────────────────────────────────────────────────

void FanControlTab::reloadFanProfiles()
{
  QString prevId = m_fanProfileCombo ? m_fanProfileCombo->currentData().toString() : QString();
  if ( m_fanProfileCombo ) m_fanProfileCombo->clear();

  for ( const auto &v : m_profileManager->fanProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    m_fanProfileCombo->addItem( name, id );
  }

  // Restore selection by ID

  if ( !prevId.isEmpty() )
  {
    for ( int i = 0; i < m_fanProfileCombo->count(); ++i )
    {
      if ( m_fanProfileCombo->itemData( i ).toString() == prevId )
      { m_fanProfileCombo->setCurrentIndex( i ); return; }
    }
  }
  if ( m_fanProfileCombo->count() > 0 )
    m_fanProfileCombo->setCurrentIndex( 0 );
}

void FanControlTab::updateButtonStates( bool uccdConnected )
{
  const QString id = m_fanProfileCombo ? m_fanProfileCombo->currentData().toString() : QString();
  bool isCustom = !id.isEmpty()
                 && m_profileManager->isProfileEditable( id, m_profileManager->fanProfilesData() );

  if ( m_applyFanProfilesButton )   m_applyFanProfilesButton->setEnabled( uccdConnected );
  if ( m_saveFanProfilesButton )    m_saveFanProfilesButton->setEnabled( isCustom );
  if ( m_copyFanProfileButton )     m_copyFanProfileButton->setEnabled( !id.isEmpty() );
  if ( m_revertFanProfilesButton )  m_revertFanProfilesButton->setEnabled( isCustom && uccdConnected );

  // Only allow renaming custom fan profiles
  if ( m_fanProfileCombo && m_fanProfileCombo->lineEdit() )
    m_fanProfileCombo->lineEdit()->setReadOnly( !isCustom );
}

void FanControlTab::setEditorsEditable( bool editable )
{
  for ( auto *e : m_fanEditors )
    if ( e ) e->setEditable( editable );
  for ( auto *e : m_pumpEditors )
    if ( e ) e->setEditable( editable );
  for ( auto *c : m_thermalSourceCombos )
    if ( c ) c->setEnabled( editable );
}

void FanControlTab::onFanProfileComboRenamed()
{
  if ( !m_fanProfileCombo || !m_fanProfileCombo->lineEdit() ) return;

  int idx = m_fanProfileCombo->currentIndex();
  if ( idx < 0 ) return;

  QString fanProfileId = m_fanProfileCombo->itemData( idx ).toString();
  QString oldName = m_fanProfileCombo->itemText( idx );
  QString newName = m_fanProfileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName ) {
    m_fanProfileCombo->setEditText( oldName );
    return;
  }

  // Cannot rename built-in profiles
  if ( !m_profileManager->isProfileEditable( fanProfileId, m_profileManager->fanProfilesData() ) ) {
    m_fanProfileCombo->setEditText( oldName );
    return;
  }

  if ( m_profileManager->renameFanProfile( fanProfileId, newName ) ) {
    m_fanProfileCombo->setItemText( idx, newName );
    emit fanProfileRenamed( oldName, newName );

    // Find the parent MainWindow to update the status bar
    if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    {
      if ( auto *sb = mw->statusBar() )
        sb->showMessage( QString("Fan profile renamed from '%1' to '%2'").arg( oldName, newName ) );
    }
  } else {
    m_fanProfileCombo->setEditText( oldName );
  }
}

void FanControlTab::setWaterCoolerEnabled( bool enabled )
{
  if ( m_waterCoolerEnableCheckBox )
  {
    m_waterCoolerEnableCheckBox->blockSignals( true );
    m_waterCoolerEnableCheckBox->setChecked( enabled );
    m_waterCoolerEnableCheckBox->blockSignals( false );
  }

  // Update polling state when programmatically setting enabled state
  updateWaterCoolerPolling();

  // NOTE: Do NOT call EnableWaterCooler on D-Bus here.
  // This method is called during profile loading to update the UI checkbox.
  // Calling EnableWaterCooler would restart BLE scanning (destroying any
  // active connection) or disconnect the water cooler, causing the
  // connected → disconnected → reconnecting oscillation on GUI startup.
  // The D-Bus call only happens via onWaterCoolerEnableToggled() when the
  // user explicitly toggles the checkbox.
}

void FanControlTab::sendWaterCoolerEnable( bool enabled )
{
  if ( m_waterCoolerDbus )
    m_waterCoolerDbus->call( QStringLiteral( "EnableWaterCooler" ), enabled );
}

bool FanControlTab::isWaterCoolerEnabled() const
{
  return m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : true;
}

// ── Water cooler hardware slots ─────────────────────────────────────

void FanControlTab::onWaterCoolerEnableToggled( bool enabled )
{
  if ( m_waterCoolerDbus )
    m_waterCoolerDbus->call( QStringLiteral( "EnableWaterCooler" ), enabled );

  // Update polling state based on new enable state
  updateWaterCoolerPolling();

  // Reset initialization flag when water cooler is enabled
  if ( enabled )
    m_manualControlInitialized = false;

  // Update manual control state when water cooler enable state changes
  updateManualControlState();

  emit waterCoolerEnableChanged( enabled );
}

void FanControlTab::onConnected()
{
  // Don't connect if water cooler is disabled
  bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;
  if ( !wcEnabled ) {
    onDisconnected(); // Force disconnect state
    return;
  }

  if ( m_isWcConnected ) return;
  m_isWcConnected = true;

  // Reset manual control initialization when reconnecting
  m_manualControlInitialized = false;

  // Update manual control state now that water cooler is connected
  updateManualControlState();

  // LED control mode and LED checkbox are always enabled
  // Color button is only enabled if mode is Static
  updateColorButtonState();
  m_isWcConnected = true;
  if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    mw->statusBar()->showMessage( tr( "Connection to water cooler successful" ) );
}

void FanControlTab::onDisconnected()
{
  if ( !m_isWcConnected ) return;
  m_isWcConnected = false;

  // Reset initialization flag when disconnecting
  m_manualControlInitialized = false;

  // Update manual control state now that water cooler is disconnected
  updateManualControlState();

  // LED control mode and LED checkbox remain always enabled
  if ( m_colorPickerButton ) m_colorPickerButton->setEnabled( false );
  m_isWcConnected = false;
  if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    mw->statusBar()->clearMessage();
}

void FanControlTab::onPumpVoltageChanged( int index )
{
  if ( !m_waterCoolerDbus ) return;
  if ( index == static_cast< int >( PumpVoltage::Off ) )
    m_waterCoolerDbus->call( QStringLiteral( "TurnOffWaterCoolerPump" ) );
  else
  {
    PumpVoltage voltage = static_cast< PumpVoltage >( m_pumpVoltageCombo->itemData( index ).toInt() );
    m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerPumpVoltage" ), static_cast< int >( voltage ) );
  }
}

void FanControlTab::onFanSpeedChanged( int speed )
{
  if ( !m_waterCoolerDbus ) return;
  m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerFanSpeed" ), speed );
}

void FanControlTab::onLEDOnOffChanged( bool enabled )
{
  if ( !m_waterCoolerDbus ) return;
  updateColorButtonState();
  if ( enabled )
  {
    RGBState mode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
    m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerLEDColor" ),
                             m_currentRed, m_currentGreen, m_currentBlue, static_cast< int >( mode ) );
  }
  else
    m_waterCoolerDbus->call( QStringLiteral( "TurnOffWaterCoolerLED" ) );
}

void FanControlTab::onLEDModeChanged( int /*index*/ )
{
  updateColorButtonState();
  if ( !m_waterCoolerDbus ) return;
  if ( m_ledOnOffCheckBox->isChecked() )
  {
    RGBState mode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
    m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerLEDColor" ),
                             m_currentRed, m_currentGreen, m_currentBlue, static_cast< int >( mode ) );
  }
}

void FanControlTab::onColorPickerClicked()
{
  QColor currentColor( m_currentRed, m_currentGreen, m_currentBlue );
  QColor color = QColorDialog::getColor( currentColor, this, "Choose LED Color" );
  if ( !color.isValid() ) return;
  m_currentRed = color.red();
  m_currentGreen = color.green();
  m_currentBlue = color.blue();
  RGBState mode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
  if ( m_ledOnOffCheckBox->isChecked() && m_waterCoolerDbus )
  {
    if ( QDBusReply< bool > conn = m_waterCoolerDbus->call( QStringLiteral( "GetWaterCoolerConnected" ) );
         conn.isValid() && conn.value() )
      m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerLEDColor" ),
                               m_currentRed, m_currentGreen, m_currentBlue, static_cast< int >( mode ) );
  }
  m_colorPickerButton->setStyleSheet(
    QString( "background-color: rgb(%1, %2, %3);" ).arg( m_currentRed ).arg( m_currentGreen ).arg( m_currentBlue ) );
}

void FanControlTab::setWaterCoolerAutoControl( bool autoControl )
{
  bool wasAutoControl = m_autoControl;
  m_autoControl = autoControl;

  // Reset initialization flag when switching to manual control
  if ( wasAutoControl && !autoControl )
    m_manualControlInitialized = false;

  // Update manual control state considering all factors
  updateManualControlState();
}

void FanControlTab::updateManualControlState()
{
  // Manual controls are enabled when:
  // 1. Water cooler is enabled (checkbox checked)
  // 2. Water cooler is connected (hardware connection)
  // 3. Auto control is disabled (manual control allowed)
  bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;
  bool enableManualControls = wcEnabled && m_isWcConnected && !m_autoControl;

  if ( m_pumpVoltageCombo )
    m_pumpVoltageCombo->setEnabled( enableManualControls );
  if ( m_fanSpeedSlider )
    m_fanSpeedSlider->setEnabled( enableManualControls );

  // When manual controls are first enabled after connection or auto control change,
  // ensure pump is set to off for safety
  if ( enableManualControls && !m_manualControlInitialized && m_waterCoolerDbus )
  {
    m_manualControlInitialized = true;
    // Set pump to off and fan speed to minimum for safety
    if ( m_pumpVoltageCombo )
    {
      m_pumpVoltageCombo->setCurrentIndex( 0 ); // Set to "Off"
      m_waterCoolerDbus->call( QStringLiteral( "TurnOffWaterCoolerPump" ) );
    }
    if ( m_fanSpeedSlider )
    {
      m_fanSpeedSlider->setValue( 0 ); // Set to minimum
      m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerFanSpeed" ), 0 );
    }
  }
}

void FanControlTab::updateWaterCoolerPolling()
{
  if ( !m_waterCoolerPollTimer ) return;

  bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;

  if ( wcEnabled ) {
    if ( !m_waterCoolerPollTimer->isActive() )
      m_waterCoolerPollTimer->start( 1000 );
  } else {
    if ( m_waterCoolerPollTimer->isActive() ) {
      m_waterCoolerPollTimer->stop();
      // Force disconnect when disabled
      onDisconnected();
      // Ensure pump is turned off when disabling
      if ( m_waterCoolerDbus )
        m_waterCoolerDbus->call( QStringLiteral( "TurnOffWaterCoolerPump" ) );
    }
  }
}

void FanControlTab::updateColorButtonState()
{
  if ( !m_colorPickerButton || !m_ledModeCombo ) return;

  // Enable color button only when mode is Static and LED is enabled
  RGBState currentMode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
  bool isStaticMode = ( currentMode == RGBState::Static );
  bool isLEDEnabled = m_ledOnOffCheckBox ? m_ledOnOffCheckBox->isChecked() : false;

  m_colorPickerButton->setEnabled( isStaticMode && isLEDEnabled );
}

// ── Dynamic zone editor construction ────────────────────────────────

FanCurveEditorWidget *FanControlTab::fanEditor( const QString &zoneId ) const
{
  return m_fanEditors.value( zoneId, nullptr );
}

PumpCurveEditorWidget *FanControlTab::pumpEditor( const QString &zoneId ) const
{
  return m_pumpEditors.value( zoneId, nullptr );
}

void FanControlTab::buildZoneEditors( const QJsonArray &zones,
                                      const QJsonArray &thermalSources,
                                      const QJsonArray &hardwareFanDevices,
                                      const QJsonArray &hardwareSensors )
{
  m_fanEditors.clear();
  m_pumpEditors.clear();
  m_thermalSourceCombos.clear();

  while ( m_subTabs->count() > 0 )
  {
    QWidget *w = m_subTabs->widget( 0 );
    m_subTabs->removeTab( 0 );
    w->deleteLater();
  }

  // Store build args for rebuild support (source removal rebuilds)
  m_lastZones = zones;
  m_lastFanDevices = hardwareFanDevices;
  m_lastSensors = hardwareSensors;

  // Populate editor model
  m_sourceEditorModel.clear();
  m_sourceEditorModel.reserve( thermalSources.size() );
  for ( const QJsonValue &sv : thermalSources )
    m_sourceEditorModel.push_back( sv.toObject() );

  m_allSourceCombos.clear();
  m_strategyCombos.clear();

  // Build sensor label map
  m_sensorLabelById.clear();
  for ( const QJsonValue &sv : hardwareSensors )
  {
    const QJsonObject s = sv.toObject();
    const QString sid = s[QStringLiteral( "id" )].toString();
    const QString d = s[QStringLiteral( "displayLabel" )].toString().trimmed();
    const QString l = s[QStringLiteral( "label" )].toString();
    m_sensorLabelById[sid] = d.isEmpty() ? l : d;
  }

  // ---------------------------------------------------------------------
  // First tab: Temperature Sources
  // ---------------------------------------------------------------------
  {
    auto *page = new QWidget();
    auto *pageLayout = new QVBoxLayout( page );
    pageLayout->setContentsMargins( 6, 6, 6, 6 );
    pageLayout->setSpacing( 8 );

    auto *split = new QSplitter( Qt::Horizontal );

    // ── Sensor tree pane ──
    auto *sensorPane = new QGroupBox( QStringLiteral( "Hardware Sensors" ) );
    auto *sensorPaneLayout = new QVBoxLayout( sensorPane );
    sensorPaneLayout->setContentsMargins( 6, 6, 6, 6 );
    auto *sensorTree = new QTreeWidget();
    sensorTree->setColumnCount( 1 );
    sensorTree->setHeaderLabels( { QStringLiteral( "Sensor" ) } );
    sensorTree->header()->setStretchLastSection( true );
    sensorTree->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
    sensorTree->setDragEnabled( true );
    sensorTree->setDragDropMode( QAbstractItemView::DragOnly );

    QMap< QString, QTreeWidgetItem * > sourceGroups;
    for ( const QJsonValue &sv : hardwareSensors )
    {
      const QJsonObject s = sv.toObject();
      const QString source = s[QStringLiteral( "source" )].toString();
      const QString sourceKey = normalizedSourceGroup( s );

      if ( !sourceGroups.contains( sourceKey ) )
      {
        auto *group = new QTreeWidgetItem( sensorTree );
        group->setText( 0, sourceKey );
        group->setFirstColumnSpanned( true );
        sourceGroups.insert( sourceKey, group );
      }

      auto *child = new QTreeWidgetItem( sourceGroups.value( sourceKey ) );
      const QString displayLabel = s[QStringLiteral( "displayLabel" )].toString().trimmed();
      const QString rawLabel = s[QStringLiteral( "label" )].toString();
      child->setText( 0, displayLabel.isEmpty() ? rawLabel : displayLabel );
      child->setData( 0, Qt::UserRole, s[QStringLiteral( "id" )].toString() );
      child->setToolTip( 0, QStringLiteral( "Raw source: %1\nRaw label: %2" )
               .arg( source.isEmpty() ? QStringLiteral( "(none)" ) : source,
                 rawLabel.isEmpty() ? QStringLiteral( "(none)" ) : rawLabel ) );
    }

    for ( auto *group : sourceGroups )
      group->setExpanded( false );

    sensorPaneLayout->addWidget( sensorTree );

    // ── Source table pane ──
    auto *sourcePane = new QWidget();
    auto *sourcePaneLayout = new QVBoxLayout( sourcePane );
    sourcePaneLayout->setContentsMargins( 0, 0, 0, 0 );
    sourcePaneLayout->setSpacing( 8 );

    auto *sourcesGroup = new QGroupBox( QStringLiteral( "Temperature Sources" ) );
    auto *sourcesGroupLayout = new QVBoxLayout( sourcesGroup );
    sourcesGroupLayout->setContentsMargins( 6, 6, 6, 6 );

    auto *sourceTable = new ThermalSourceTableWidget();
    m_sourceTable = sourceTable;
    m_sourceTable->setColumnCount( 3 );
    m_sourceTable->setHorizontalHeaderLabels( { QStringLiteral( "Label" ),
                           QStringLiteral( "Strategy" ),
                           QStringLiteral( "Sensors" ) } );
    m_sourceTable->horizontalHeader()->setStretchLastSection( true );
    m_sourceTable->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::Stretch );
    m_sourceTable->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::ResizeToContents );
    m_sourceTable->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::Stretch );
    m_sourceTable->setSelectionBehavior( QAbstractItemView::SelectRows );
    m_sourceTable->setSelectionMode( QAbstractItemView::SingleSelection );
    m_sourceTable->setEditTriggers( QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed );
    m_sourceTable->setContextMenuPolicy( Qt::CustomContextMenu );
    m_sourceTable->setAcceptDrops( true );
    m_sourceTable->viewport()->setAcceptDrops( true );
    m_sourceTable->setDragDropMode( QAbstractItemView::DropOnly );
    m_sourceTable->setDefaultDropAction( Qt::CopyAction );

    m_sourceTable->setRowCount( m_sourceEditorModel.size() );
    m_strategyCombos.resize( m_sourceEditorModel.size() );

    sourceTable->onSensorDropped = [this]( int row, const QString &sid ) {
      onSensorDropped( row, sid );
    };

    for ( int row = 0; row < m_sourceEditorModel.size(); ++row )
    {
      ensureValidStrategy( m_sourceEditorModel[row] );
      installStrategyComboForRow( row );
      refreshSourceRow( row );
    }

    connect( m_sourceTable, &QTableWidget::itemChanged, this, &FanControlTab::onSourceItemChanged );
    connect( m_sourceTable, &QWidget::customContextMenuRequested, this, &FanControlTab::onSourceContextMenu );

    sourcesGroupLayout->addWidget( m_sourceTable );
    sourcePaneLayout->addWidget( sourcesGroup, 3 );

    // ── Source details panel ──
    auto *srcDetailsGroup = new QGroupBox( QStringLiteral( "Source Details" ) );
    auto *srcDetailsLayout = new QFormLayout( srcDetailsGroup );
    srcDetailsLayout->setContentsMargins( 6, 6, 6, 6 );
    m_srcLabelValue = new QLabel( QStringLiteral( "-" ) );
    m_srcStrategyValue = new QLabel( QStringLiteral( "-" ) );
    m_srcSensorsValue = new QLabel( QStringLiteral( "-" ) );
    m_srcSensorsValue->setWordWrap( true );
    auto *srcNotesValue = new QLabel( QStringLiteral( "Sensor tree is grouped by provider/source (e.g. nvme, gpu)." ) );
    srcNotesValue->setWordWrap( true );
    srcDetailsLayout->addRow( QStringLiteral( "Source Label:" ), m_srcLabelValue );
    srcDetailsLayout->addRow( QStringLiteral( "Strategy:" ), m_srcStrategyValue );
    srcDetailsLayout->addRow( QStringLiteral( "Sensors:" ), m_srcSensorsValue );
    srcDetailsLayout->addRow( QStringLiteral( "Notes:" ), srcNotesValue );
    sourcePaneLayout->addWidget( srcDetailsGroup, 2 );

    split->addWidget( sensorPane );
    split->addWidget( sourcePane );
    split->setStretchFactor( 0, 1 );
    split->setStretchFactor( 1, 1 );
    pageLayout->addWidget( split, 1 );

    connect( m_sourceTable, &QTableWidget::currentCellChanged, this,
             [this]( int currentRow, int, int, int ) { onSourceCellChanged( currentRow ); } );

    if ( m_sourceTable->rowCount() > 0 )
      m_sourceTable->selectRow( 0 );

    m_subTabs->addTab( page, QStringLiteral( "Temperature Sources" ) );
  }

  // ---------------------------------------------------------------------
  // Second tab: Zone Editing
  // ---------------------------------------------------------------------
  {
    auto *page = new QWidget();
    auto *pageLayout = new QVBoxLayout( page );
    pageLayout->setContentsMargins( 6, 6, 6, 6 );
    pageLayout->setSpacing( 8 );

    auto *split = new QSplitter( Qt::Horizontal );

    auto *zonesGroup = new QGroupBox( QStringLiteral( "Fan Zones" ) );
    auto *zonesGroupLayout = new QVBoxLayout( zonesGroup );
    zonesGroupLayout->setContentsMargins( 6, 6, 6, 6 );
    m_zoneList = new QListWidget();
    m_zoneList->setContextMenuPolicy( Qt::CustomContextMenu );
    zonesGroupLayout->addWidget( m_zoneList );
    split->addWidget( zonesGroup );

    auto *zoneDetailsGroup = new QGroupBox( QStringLiteral( "Zone Details" ) );
    auto *zoneForm = new QFormLayout( zoneDetailsGroup );
    zoneForm->setContentsMargins( 6, 6, 6, 6 );
    zoneForm->setHorizontalSpacing( 8 );
    zoneForm->setVerticalSpacing( 6 );

    m_zoneNameEdit = new QLineEdit();
    m_zoneTypeEdit = new QLineEdit();
    m_zoneSourceCombo = new QComboBox();
    m_zoneFansList = new QListWidget();

    m_zoneTypeEdit->setReadOnly( true );
    m_zoneNameEdit->setPlaceholderText( QStringLiteral( "Zone name" ) );
    m_zoneFansList->setMinimumHeight( 120 );

    for ( const QJsonObject &src : m_sourceEditorModel )
    {
      if ( src[QStringLiteral( "id" )].toString().trimmed().isEmpty() )
        continue;
      m_zoneSourceCombo->addItem( src[QStringLiteral( "label" )].toString(),
                                  src[QStringLiteral( "id" )].toString() );
    }
    m_allSourceCombos.push_back( m_zoneSourceCombo );

    zoneForm->addRow( QStringLiteral( "Name:" ), m_zoneNameEdit );
    zoneForm->addRow( QStringLiteral( "Device Type:" ), m_zoneTypeEdit );
    zoneForm->addRow( QStringLiteral( "Temp Source:" ), m_zoneSourceCombo );
    zoneForm->addRow( QStringLiteral( "Assigned Devices:" ), m_zoneFansList );
    split->addWidget( zoneDetailsGroup );
    split->setStretchFactor( 0, 1 );
    split->setStretchFactor( 1, 1 );
    pageLayout->addWidget( split, 1 );

    m_zoneCache.clear();
    m_zoneCache.reserve( zones.size() );
    for ( const QJsonValue &zv : zones )
    {
      const QJsonObject zone = zv.toObject();
      m_zoneCache.push_back( zone );
      m_zoneList->addItem( QStringLiteral( "%1 (%2)" )
                           .arg( zone[QStringLiteral( "name" )].toString(),
                                 zone[QStringLiteral( "id" )].toString() ) );
    }

    m_fanLabelById.clear();
    for ( const QJsonValue &dv : hardwareFanDevices )
    {
      const QJsonObject d = dv.toObject();
      m_fanLabelById[d[QStringLiteral( "id" )].toString()] = d[QStringLiteral( "label" )].toString();
    }

    connect( m_zoneList, &QListWidget::currentRowChanged, this, &FanControlTab::onZoneListRowChanged );
    connect( m_zoneList, &QWidget::customContextMenuRequested, this, &FanControlTab::onZoneListContextMenu );

    connect( m_zoneSourceCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
             [this]( int ) {
               const int row = m_zoneList->currentRow();
               if ( row < 0 || row >= m_zoneCache.size() ) return;
               emit thermalSourceChanged( m_zoneCache[row][QStringLiteral( "id" )].toString(),
                                          m_zoneSourceCombo->currentData().toString() );
             } );

    if ( m_zoneList->count() > 0 )
      m_zoneList->setCurrentRow( 0 );

    m_subTabs->addTab( page, QStringLiteral( "Zone Editing" ) );
  }

  // ---------------------------------------------------------------------
  // Per-zone curve editor tabs
  // ---------------------------------------------------------------------
  for ( const QJsonValue &zv : zones )
  {
    QJsonObject zone = zv.toObject();
    QString id = zone[QStringLiteral( "id" )].toString();
    QString name = zone[QStringLiteral( "name" )].toString();
    QString devType = zone[QStringLiteral( "deviceType" )].toString();
    QString tsId = zone[QStringLiteral( "thermalSourceId" )].toString();

    auto *page = new QWidget();
    auto *pageLayout = new QVBoxLayout( page );
    pageLayout->setContentsMargins( 4, 4, 4, 4 );
    pageLayout->setSpacing( 4 );

    // Thermal source combo
    auto *tsBar = new QHBoxLayout();
    tsBar->setContentsMargins( 0, 0, 0, 0 );
    auto *tsLabel = new QLabel( QStringLiteral( "Temperature Source:" ) );
    auto *tsCombo = new QComboBox();
    for ( const QJsonObject &src : m_sourceEditorModel )
    {
      if ( src[QStringLiteral( "id" )].toString().trimmed().isEmpty() )
        continue;
      tsCombo->addItem( src[QStringLiteral( "label" )].toString(),
                        src[QStringLiteral( "id" )].toString() );
    }
    int tsIdx = -1;
    for ( int i = 0; i < tsCombo->count(); ++i )
    {
      if ( tsCombo->itemData( i ).toString() == tsId )
      {
        tsIdx = i;
        break;
      }
    }
    tsCombo->setCurrentIndex( tsIdx );
    tsBar->addWidget( tsLabel );
    tsBar->addWidget( tsCombo, 1 );
    pageLayout->addLayout( tsBar );
    m_thermalSourceCombos[id] = tsCombo;
    m_allSourceCombos.push_back( tsCombo );

    connect( tsCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
             [this, zoneId = id, tsCombo]( int ) {
               emit thermalSourceChanged( zoneId, tsCombo->currentData().toString() );
             } );

    if ( devType == QStringLiteral( "stagedPump" ) )
    {
      auto *editor = new PumpCurveEditorWidget();
      m_pumpEditors[id] = editor;
      connect( editor, &PumpCurveEditorWidget::pointsChanged, this,
               [this, zoneId = id]( const QVector< PumpCurveEditorWidget::Point > &pts ) {
                 emit pumpCurveChanged( zoneId, pts );
               } );
      pageLayout->addWidget( editor, 1 );
    }
    else
    {
      auto *editor = new FanCurveEditorWidget();
      m_fanEditors[id] = editor;
      connect( editor, &FanCurveEditorWidget::pointsChanged, this,
               [this, zoneId = id]( const QVector< FanCurveEditorWidget::Point > &pts ) {
                 emit fanCurveChanged( zoneId, pts );
               } );
      pageLayout->addWidget( editor, 1 );
    }

    m_subTabs->addTab( page, name );
  }
}

// ── Source / zone editor member functions ────────────────────────────

void FanControlTab::showStatusMessage( const QString &msg, int timeoutMs )
{
  if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    if ( auto *sb = mw->statusBar() )
      sb->showMessage( msg, timeoutMs );
}

QString FanControlTab::normalizedSourceGroup( const QJsonObject &sensor )
{
  const QString rawSource = sensor[QStringLiteral( "source" )].toString();
  const QString sourceDisplay = sensor[QStringLiteral( "sourceDisplay" )].toString().trimmed();
  const QString raw = sourceDisplay.isEmpty() ? rawSource : sourceDisplay;
  const QString s = raw.trimmed().toLower();
  const QString category = sensor[QStringLiteral( "category" )].toString().trimmed().toLower();

  if ( category == QStringLiteral( "cpu" ) )   return QStringLiteral( "CPU" );
  if ( category == QStringLiteral( "nvme" ) )  return QStringLiteral( "NVMe" );
  if ( category == QStringLiteral( "ddr5" ) )  return QStringLiteral( "DDR5" );
  if ( category == QStringLiteral( "board" ) ) return QStringLiteral( "Board" );
  if ( category == QStringLiteral( "gpu" ) )
  {
    if ( s != QStringLiteral( "gpu" )
         && s != QStringLiteral( "dgpu" )
         && s != QStringLiteral( "igpu" )
         && !raw.trimmed().isEmpty() )
      return raw.trimmed();
    return QStringLiteral( "GPU" );
  }

  if ( s.isEmpty() ) return QStringLiteral( "Other" );

  if ( s != QStringLiteral( "gpu" )
    && s != QStringLiteral( "dgpu" )
    && s != QStringLiteral( "igpu" )
    && ( s.contains( QStringLiteral( "gpu" ) )
      || s.contains( QStringLiteral( "nvidia" ) )
      || s.contains( QStringLiteral( "amdgpu" ) )
      || s.contains( QStringLiteral( "radeon" ) ) ) )
    return raw.trimmed();

  if ( s.contains( QStringLiteral( "gpu" ) ) || s.contains( QStringLiteral( "nvidia" ) )
       || s.contains( QStringLiteral( "amdgpu" ) ) || s.contains( QStringLiteral( "radeon" ) ) )
    return QStringLiteral( "GPU" );
  if ( s.contains( QStringLiteral( "k10temp" ) ) || s.contains( QStringLiteral( "coretemp" ) )
       || s.contains( QStringLiteral( "cpu" ) ) )
    return QStringLiteral( "CPU" );
  if ( s.contains( QStringLiteral( "nvme" ) ) )
    return QStringLiteral( "NVMe" );
  if ( s.contains( QStringLiteral( "spd5118" ) ) || s.contains( QStringLiteral( "spd" ) ) )
    return QStringLiteral( "DDR5" );
  if ( s.contains( QStringLiteral( "nct6799" ) ) || s.contains( QStringLiteral( "nct" ) ) )
    return QStringLiteral( "Board" );
  if ( s.contains( QStringLiteral( "acpi" ) ) || s.contains( QStringLiteral( "ec" ) )
       || s.contains( QStringLiteral( "board" ) ) || s.contains( QStringLiteral( "pch" ) )
       || s.contains( QStringLiteral( "chipset" ) ) )
    return QStringLiteral( "Board" );
  return raw;
}

void FanControlTab::ensureValidStrategy( QJsonObject &src )
{
  const int sensorCount = src[QStringLiteral( "sensorIds" )].toArray().size();
  if ( sensorCount <= 1 )
  {
    src[QStringLiteral( "strategy" )] = QStringLiteral( "single" );
    return;
  }
  const QString strategy = src[QStringLiteral( "strategy" )].toString();
  if ( !strategyChoices().contains( strategy ) )
    src[QStringLiteral( "strategy" )] = QStringLiteral( "average" );
}

QString FanControlTab::sensorsSummaryText( const QJsonObject &src ) const
{
  QStringList labels;
  const QJsonArray sensorIds = src[QStringLiteral( "sensorIds" )].toArray();
  for ( const QJsonValue &v : sensorIds )
    labels << m_sensorLabelById.value( v.toString(), v.toString() );
  return labels.isEmpty() ? QStringLiteral( "(none)" ) : labels.join( QStringLiteral( ", " ) );
}

void FanControlTab::refreshAllSourceCombos()
{
  for ( QComboBox *combo : m_allSourceCombos )
  {
    if ( !combo ) continue;
    const QString currentId = combo->currentData().toString();
    combo->blockSignals( true );
    combo->clear();
    for ( const QJsonObject &src : m_sourceEditorModel )
    {
      if ( src[QStringLiteral( "id" )].toString().trimmed().isEmpty() )
        continue;
      combo->addItem( src[QStringLiteral( "label" )].toString(),
                      src[QStringLiteral( "id" )].toString() );
    }
    for ( int i = 0; i < combo->count(); ++i )
    {
      if ( combo->itemData( i ).toString() == currentId )
      {
        combo->setCurrentIndex( i );
        break;
      }
    }
    combo->blockSignals( false );
  }
}

void FanControlTab::installStrategyComboForRow( int row )
{
  if ( row < 0 || row >= m_sourceEditorModel.size() )
    return;

  if ( m_strategyCombos.size() <= row )
    m_strategyCombos.resize( row + 1 );

  if ( m_strategyCombos[row] )
    return;

  auto *combo = new QComboBox( m_sourceTable );
  for ( const QString &opt : strategyChoices() )
    combo->addItem( opt, opt );
  m_strategyCombos[row] = combo;
  m_sourceTable->setCellWidget( row, 1, combo );

  connect( combo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
           [this, row, combo]( int ) {
             if ( row < 0 || row >= m_sourceEditorModel.size() ) return;
             QJsonObject src = m_sourceEditorModel[row];
             if ( src[QStringLiteral( "sensorIds" )].toArray().size() <= 1 )
             {
               src[QStringLiteral( "strategy" )] = QStringLiteral( "single" );
               combo->blockSignals( true );
               const int idx = combo->findData( QStringLiteral( "single" ) );
               combo->setCurrentIndex( idx >= 0 ? idx : 0 );
               combo->blockSignals( false );
             }
             else
             {
               src[QStringLiteral( "strategy" )] = combo->currentData().toString();
             }
             m_sourceEditorModel[row] = src;
             if ( m_sourceTable->currentRow() == row )
               m_sourceTable->setCurrentCell( row, m_sourceTable->currentColumn() );
           } );
}

void FanControlTab::refreshSourceRow( int row )
{
  if ( row < 0 || row >= m_sourceEditorModel.size() ) return;

  QJsonObject src = m_sourceEditorModel[row];
  ensureValidStrategy( src );
  m_sourceEditorModel[row] = src;

  auto *labelItem = m_sourceTable->item( row, 0 );
  if ( !labelItem )
  {
    labelItem = new QTableWidgetItem();
    m_sourceTable->setItem( row, 0, labelItem );
  }
  labelItem->setText( src[QStringLiteral( "label" )].toString() );
  labelItem->setData( Qt::UserRole, src[QStringLiteral( "id" )].toString() );
  labelItem->setFlags( labelItem->flags() | Qt::ItemIsEditable );

  if ( row < m_strategyCombos.size() && m_strategyCombos[row] )
  {
    m_strategyCombos[row]->blockSignals( true );
    const QString strategy = src[QStringLiteral( "strategy" )].toString();
    const int idx = m_strategyCombos[row]->findData( strategy );
    m_strategyCombos[row]->setCurrentIndex( idx >= 0 ? idx : 0 );
    const int sensorCount = src[QStringLiteral( "sensorIds" )].toArray().size();
    m_strategyCombos[row]->setEnabled( sensorCount > 1 );
    m_strategyCombos[row]->blockSignals( false );
  }

  auto *sensorsItem = m_sourceTable->item( row, 2 );
  if ( !sensorsItem )
  {
    sensorsItem = new QTableWidgetItem();
    sensorsItem->setFlags( sensorsItem->flags() & ~Qt::ItemIsEditable );
    m_sourceTable->setItem( row, 2, sensorsItem );
  }
  sensorsItem->setText( sensorsSummaryText( src ) );
}

void FanControlTab::addSensorToSource( int row, const QString &sensorId )
{
  if ( row < 0 || row >= m_sourceEditorModel.size() || sensorId.isEmpty() )
    return;

  QJsonObject src = m_sourceEditorModel[row];
  QJsonArray sensorIds = src[QStringLiteral( "sensorIds" )].toArray();
  const int beforeCount = sensorIds.size();
  for ( const QJsonValue &v : sensorIds )
  {
    if ( v.toString() == sensorId )
      return; // already assigned
  }
  sensorIds.append( sensorId );
  src[QStringLiteral( "sensorIds" )] = sensorIds;

  if ( sensorIds.size() <= 1 )
    src[QStringLiteral( "strategy" )] = QStringLiteral( "single" );
  else if ( beforeCount == 1 )
    src[QStringLiteral( "strategy" )] = QStringLiteral( "max" );

  m_sourceEditorModel[row] = src;
  refreshSourceRow( row );
}

void FanControlTab::createSourceWithSensor( const QString &sensorId )
{
  if ( sensorId.isEmpty() )
    return;

  QString newId;
  for ( int n = 1; ; ++n )
  {
    newId = QStringLiteral( "custom-source-%1" ).arg( n );
    bool exists = false;
    for ( const QJsonObject &src : m_sourceEditorModel )
    {
      if ( src[QStringLiteral( "id" )].toString() == newId )
      {
        exists = true;
        break;
      }
    }
    if ( !exists ) break;
  }

  QJsonObject src;
  src[QStringLiteral( "id" )] = newId;
  src[QStringLiteral( "label" )] = m_sensorLabelById.value( sensorId, QStringLiteral( "New Source" ) );
  src[QStringLiteral( "strategy" )] = QStringLiteral( "single" );
  src[QStringLiteral( "sensorIds" )] = QJsonArray{ sensorId };
  src[QStringLiteral( "weights" )] = QJsonArray{};

  const int row = m_sourceEditorModel.size();
  m_sourceEditorModel.push_back( src );
  m_sourceTable->setRowCount( row + 1 );
  if ( m_strategyCombos.size() <= row )
    m_strategyCombos.resize( row + 1 );
  installStrategyComboForRow( row );
  refreshSourceRow( row );
  m_sourceTable->setCurrentCell( row, 0 );
  refreshAllSourceCombos();
}

void FanControlTab::onSourceItemChanged( QTableWidgetItem *item )
{
  if ( !item || item->column() != 0 ) return;
  const int row = item->row();
  if ( row < 0 || row >= m_sourceEditorModel.size() ) return;

  QJsonObject src = m_sourceEditorModel[row];
  src[QStringLiteral( "label" )] = item->text().trimmed();
  if ( src[QStringLiteral( "label" )].toString().isEmpty() )
    src[QStringLiteral( "label" )] = QStringLiteral( "Unnamed Source" );
  m_sourceEditorModel[row] = src;
  item->setText( src[QStringLiteral( "label" )].toString() );
  refreshAllSourceCombos();
}

void FanControlTab::onSourceCellChanged( int currentRow )
{
  if ( currentRow < 0 )
  {
    m_srcLabelValue->setText( QStringLiteral( "-" ) );
    m_srcStrategyValue->setText( QStringLiteral( "-" ) );
    m_srcSensorsValue->setText( QStringLiteral( "-" ) );
    return;
  }

  const auto *labelItem = m_sourceTable->item( currentRow, 0 );
  auto *combo = qobject_cast< QComboBox * >( m_sourceTable->cellWidget( currentRow, 1 ) );
  m_srcLabelValue->setText( labelItem ? labelItem->text() : QStringLiteral( "-" ) );
  m_srcStrategyValue->setText( combo ? combo->currentData().toString() : QStringLiteral( "-" ) );

  if ( currentRow < m_sourceEditorModel.size() )
    m_srcSensorsValue->setText( sensorsSummaryText( m_sourceEditorModel[currentRow] ) );
  else
    m_srcSensorsValue->setText( QStringLiteral( "-" ) );
}

void FanControlTab::onSourceContextMenu( const QPoint &pos )
{
  QMenu menu( m_sourceTable );
  QAction *removeSourceAction = menu.addAction( QStringLiteral( "Remove Source" ) );
  QMenu *removeSensorMenu = menu.addMenu( QStringLiteral( "Remove Sensor" ) );

  const int row = m_sourceTable->currentRow();
  removeSourceAction->setEnabled( row >= 0 );

  if ( row >= 0 && row < m_sourceEditorModel.size() )
  {
    const QJsonObject src = m_sourceEditorModel[row];
    const QJsonArray sensorIds = src[QStringLiteral( "sensorIds" )].toArray();
    if ( sensorIds.isEmpty() )
    {
      auto *noneAction = removeSensorMenu->addAction( QStringLiteral( "(none)" ) );
      noneAction->setEnabled( false );
    }
    else
    {
      for ( const QJsonValue &v : sensorIds )
      {
        const QString sid = v.toString();
        QAction *a = removeSensorMenu->addAction( m_sensorLabelById.value( sid, sid ) );
        a->setData( sid );
        if ( sensorIds.size() <= 1 )
          a->setEnabled( false );
      }
    }
  }
  else
  {
    removeSensorMenu->setEnabled( false );
  }

  QAction *chosen = menu.exec( m_sourceTable->viewport()->mapToGlobal( pos ) );
  if ( !chosen ) return;

  if ( chosen == removeSourceAction )
  {
    if ( row < 0 || row >= m_sourceEditorModel.size() ) return;

    if ( m_sourceEditorModel.size() <= 1 )
    {
      showStatusMessage( QStringLiteral( "At least one source must remain." ) );
      return;
    }

    const QString removedSourceId = m_sourceEditorModel[row][QStringLiteral( "id" )].toString();
    QJsonArray updatedZones;
    QStringList affectedZones;
    for ( const QJsonValue &zv : m_lastZones )
    {
      QJsonObject z = zv.toObject();
      if ( z[QStringLiteral( "thermalSourceId" )].toString() == removedSourceId )
      {
        affectedZones << z[QStringLiteral( "name" )].toString();
        z[QStringLiteral( "thermalSourceId" )] = QString();
      }
      updatedZones.append( z );
    }

    if ( !affectedZones.isEmpty() )
      showStatusMessage( QStringLiteral( "Removed source was used by: %1. Their sources were invalidated." )
                           .arg( affectedZones.join( QStringLiteral( ", " ) ) ) );

    m_sourceEditorModel.removeAt( row );

    QJsonArray updatedSources;
    for ( const QJsonObject &s : m_sourceEditorModel )
      updatedSources.append( s );

    buildZoneEditors( updatedZones, updatedSources, m_lastFanDevices, m_lastSensors );
    return;
  }

  const QVariant sensorData = chosen->data();
  if ( !sensorData.isValid() || row < 0 || row >= m_sourceEditorModel.size() )
    return;

  const QString removeSensorId = sensorData.toString();
  QJsonObject src = m_sourceEditorModel[row];
  QJsonArray sensorIds = src[QStringLiteral( "sensorIds" )].toArray();
  if ( sensorIds.size() <= 1 )
  {
    showStatusMessage( QStringLiteral( "A source must have at least one sensor." ) );
    return;
  }

  QJsonArray filtered;
  for ( const QJsonValue &v : sensorIds )
  {
    if ( v.toString() != removeSensorId )
      filtered.append( v );
  }
  src[QStringLiteral( "sensorIds" )] = filtered;
  if ( filtered.size() <= 1 )
    src[QStringLiteral( "strategy" )] = QStringLiteral( "single" );

  m_sourceEditorModel[row] = src;
  refreshSourceRow( row );
  refreshAllSourceCombos();
}

void FanControlTab::onSensorDropped( int row, const QString &sensorId )
{
  if ( row >= 0 )
  {
    addSensorToSource( row, sensorId );
    refreshAllSourceCombos();
  }
  else
  {
    createSourceWithSensor( sensorId );
  }
}

void FanControlTab::onZoneListRowChanged( int row )
{
  if ( row < 0 || row >= m_zoneCache.size() )
  {
    m_zoneNameEdit->clear();
    m_zoneTypeEdit->clear();
    m_zoneSourceCombo->setCurrentIndex( -1 );
    m_zoneFansList->clear();
    return;
  }

  const QJsonObject zone = m_zoneCache[row];
  m_zoneNameEdit->setText( zone[QStringLiteral( "name" )].toString() );
  m_zoneTypeEdit->setText( zone[QStringLiteral( "deviceType" )].toString() );

  const QString tsId = zone[QStringLiteral( "thermalSourceId" )].toString();
  int idx = -1;
  for ( int i = 0; i < m_zoneSourceCombo->count(); ++i )
  {
    if ( m_zoneSourceCombo->itemData( i ).toString() == tsId )
    {
      idx = i;
      break;
    }
  }
  m_zoneSourceCombo->setCurrentIndex( idx );

  m_zoneFansList->clear();
  const QJsonArray fanIds = zone[QStringLiteral( "fanIds" )].toArray();
  for ( const QJsonValue &fv : fanIds )
  {
    const QString fanId = fv.toString();
    const QString fanLabel = m_fanLabelById.value( fanId );
    if ( fanLabel.isEmpty() )
      m_zoneFansList->addItem( fanId );
    else
      m_zoneFansList->addItem( QStringLiteral( "%1 (%2)" ).arg( fanLabel, fanId ) );
  }
}

void FanControlTab::onZoneListContextMenu( const QPoint &pos )
{
  QMenu menu( m_zoneList );
  QAction *newAction = menu.addAction( QStringLiteral( "New Zone" ) );
  QAction *renameAction = menu.addAction( QStringLiteral( "Rename Zone" ) );
  QAction *deleteAction = menu.addAction( QStringLiteral( "Delete Zone" ) );

  const bool hasSelection = m_zoneList->currentRow() >= 0;
  renameAction->setEnabled( hasSelection );
  deleteAction->setEnabled( hasSelection );

  QAction *chosen = menu.exec( m_zoneList->viewport()->mapToGlobal( pos ) );
  if ( !chosen ) return;

  if ( chosen == newAction )
    showStatusMessage( QStringLiteral( "Create zone from context menu is not implemented yet." ) );
  else if ( chosen == renameAction )
    showStatusMessage( QStringLiteral( "Rename zone from context menu is not implemented yet." ) );
  else if ( chosen == deleteAction )
    showStatusMessage( QStringLiteral( "Delete zone from context menu is not implemented yet." ) );
}

QString FanControlTab::thermalSourceForZone( const QString &zoneId ) const
{
  auto it = m_thermalSourceCombos.find( zoneId );
  if ( it != m_thermalSourceCombos.end() && it.value() )
    return it.value()->currentData().toString();
  return {};
}

void FanControlTab::setThermalSourceForZone( const QString &zoneId, const QString &thermalSourceId )
{
  auto it = m_thermalSourceCombos.find( zoneId );
  if ( it == m_thermalSourceCombos.end() || !it.value() )
    return;
  QComboBox *combo = it.value();
  for ( int i = 0; i < combo->count(); ++i )
  {
    if ( combo->itemData( i ).toString() == thermalSourceId )
    {
      combo->setCurrentIndex( i );
      return;
    }
  }
}

} // namespace ucc
