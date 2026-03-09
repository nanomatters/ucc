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
#include <QScrollArea>
#include <QColorDialog>
#include <QMainWindow>
#include <QStatusBar>
#include <QDBusReply>
#include <QDebug>
#include "CommonTypes.hpp"

namespace ucc
{

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

void FanControlTab::buildZoneEditors( const QJsonArray &zones, const QJsonArray &thermalSources )
{
  // Clear editor maps — old widgets are children of the tab pages
  // which will be destroyed when we remove tabs below.
  m_fanEditors.clear();
  m_pumpEditors.clear();
  m_thermalSourceCombos.clear();

  // Remove all existing zone tabs.
  while ( m_subTabs->count() > 0 )
  {
    QWidget *w = m_subTabs->widget( 0 );
    m_subTabs->removeTab( 0 );
    w->deleteLater();
  }

  // Create one sub-tab per zone.
  for ( const QJsonValue &zv : zones )
  {
    QJsonObject zone = zv.toObject();
    QString id = zone[QStringLiteral( "id" )].toString();
    QString name = zone[QStringLiteral( "name" )].toString();
    QString devType = zone[QStringLiteral( "deviceType" )].toString();
    QString tsId = zone[QStringLiteral( "thermalSourceId" )].toString();

    // Container: thermal source selector bar + editor
    auto *page = new QWidget();
    auto *pageLayout = new QVBoxLayout( page );
    pageLayout->setContentsMargins( 4, 4, 4, 4 );
    pageLayout->setSpacing( 4 );

    // Thermal source combo
    auto *tsBar = new QHBoxLayout();
    tsBar->setContentsMargins( 0, 0, 0, 0 );
    auto *tsLabel = new QLabel( QStringLiteral( "Temperature Source:" ) );
    auto *tsCombo = new QComboBox();
    for ( const QJsonValue &sv : thermalSources )
    {
      QJsonObject src = sv.toObject();
      tsCombo->addItem( src[QStringLiteral( "label" )].toString(),
                        src[QStringLiteral( "id" )].toString() );
    }
    // Select the zone's current thermal source
    for ( int i = 0; i < tsCombo->count(); ++i )
    {
      if ( tsCombo->itemData( i ).toString() == tsId )
      {
        tsCombo->setCurrentIndex( i );
        break;
      }
    }
    tsBar->addWidget( tsLabel );
    tsBar->addWidget( tsCombo, 1 );
    pageLayout->addLayout( tsBar );
    m_thermalSourceCombos[id] = tsCombo;

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
