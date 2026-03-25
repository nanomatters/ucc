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
#include <QTableWidget>
#include <QHeaderView>
#include <QSplitter>
#include <QTreeWidget>
#include <QScrollArea>
#include <QColorDialog>
#include <QInputDialog>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QDBusReply>
#include <QDebug>
#include <QDropEvent>
#include <QDir>
#include <QSettings>
#include <QSignalBlocker>
#include <QUuid>
#include "CommonTypes.hpp"

namespace ucc
{

namespace
{

struct FanCurveTemplate
{
  QString id;
  QString name;
  QVector< FanCurveEditorWidget::Point > points;
};

const QVector< FanCurveTemplate > kFanCurveTemplates = {
  {
    QStringLiteral( "fan-silent" ),
    QStringLiteral( "Silent" ),
    { {20,0}, {25,0}, {30,0}, {35,0}, {40,0}, {45,0}, {50,0}, {55,0}, {60,0},
      {65,20}, {70,28}, {75,40}, {80,53}, {85,65}, {90,83}, {95,96}, {100,100} }
  },
  {
    QStringLiteral( "fan-quiet" ),
    QStringLiteral( "Office" ),
    { {20,0}, {25,0}, {30,0}, {35,0}, {40,0}, {45,0}, {50,0}, {55,10}, {60,20},
      {65,24}, {70,33}, {75,46}, {80,55}, {85,68}, {90,85}, {95,96}, {100,100} }
  },
  {
    QStringLiteral( "fan-balanced" ),
    QStringLiteral( "Balanced" ),
    { {20,0}, {25,0}, {30,0}, {35,0}, {40,0}, {45,0}, {50,17}, {55,25}, {60,31},
      {65,38}, {70,50}, {75,55}, {80,65}, {85,78}, {90,88}, {95,96}, {100,100} }
  },
  {
    QStringLiteral( "fan-cool" ),
    QStringLiteral( "Cool" ),
    { {20,0}, {25,0}, {30,0}, {35,0}, {40,3}, {45,20}, {50,25}, {55,29}, {60,35},
      {65,43}, {70,50}, {75,58}, {80,72}, {85,85}, {90,93}, {95,96}, {100,100} }
  },
  {
    QStringLiteral( "fan-freezy" ),
    QStringLiteral( "Freezy" ),
    { {20,20}, {25,20}, {30,21}, {35,23}, {40,26}, {45,30}, {50,40}, {55,40}, {60,45},
      {65,50}, {70,55}, {75,60}, {80,73}, {85,85}, {90,91}, {95,96}, {100,100} }
  },
};

const FanCurveTemplate *findStaticFanCurveTemplate( const QString &templateId )
{
  for ( const FanCurveTemplate &tmpl : kFanCurveTemplates )
  {
    if ( tmpl.id == templateId )
      return &tmpl;
  }
  return nullptr;
}

} // namespace

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

    auto *tree = qobject_cast< QTreeWidget * >( event->source() );
    if ( !tree )
      return QTableWidget::dropEvent( event );

    QTreeWidgetItem *item = tree->currentItem();
    if ( !item )
      return QTableWidget::dropEvent( event );

    const QString sensorId = item->data( 0, Qt::UserRole ).toString();
    if ( sensorId.isEmpty() || !onSensorDropped )
      return QTableWidget::dropEvent( event );

    const int targetRow = rowAt( event->position().toPoint().y() );
    onSensorDropped( targetRow, sensorId );
    if ( targetRow >= 0 )
      setCurrentCell( targetRow, 0 );
    event->acceptProposedAction();
  }
};

class ZoneTableWidget : public QTableWidget
{
public:
  explicit ZoneTableWidget( QWidget *parent = nullptr )
    : QTableWidget( parent )
  {}

  std::function< void( int, const QString & ) > onDeviceDropped;

protected:
  void dropEvent( QDropEvent *event ) override
  {
    if ( !event )
      return;

    auto *tree = qobject_cast< QTreeWidget * >( event->source() );
    if ( !tree )
      return QTableWidget::dropEvent( event );

    QTreeWidgetItem *item = tree->currentItem();
    if ( !item )
      return QTableWidget::dropEvent( event );

    const QString deviceId = item->data( 0, Qt::UserRole ).toString();
    if ( deviceId.isEmpty() || !onDeviceDropped )
      return QTableWidget::dropEvent( event );

    const int targetRow = rowAt( event->position().toPoint().y() );
    onDeviceDropped( targetRow, deviceId );
    if ( targetRow >= 0 )
      setCurrentCell( targetRow, 0 );
    event->acceptProposedAction();
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
    connect( m_waterCoolerPollTimer, &QTimer::timeout, this, &FanControlTab::onWaterCoolerPollTimeout );
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

  for ( int row = 0; row < m_zoneCache.size(); ++row )
  {
    const auto &zone = m_zoneCache[row];
    const QString &zoneId = zone.id;
    if ( auto *combo = m_templateSourceCombos.value( zoneId ) )
      populateZoneTemplateCombo( combo );
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
  for ( auto *c : m_templateSourceCombos )
    if ( c ) c->setEnabled( editable );
}

void FanControlTab::clearZoneGraphModifiedFlags()
{
  for ( auto it = m_zoneGraphModifiedByUser.begin(); it != m_zoneGraphModifiedByUser.end(); ++it )
    it.value() = false;
}

void FanControlTab::onFanProfileComboRenamed()
{
  if ( !m_fanProfileCombo || !m_fanProfileCombo->lineEdit() ) return;

  const int idx = m_fanProfileCombo->currentIndex();
  if ( idx < 0 ) return;

  const QString fanProfileId = m_fanProfileCombo->itemData( idx ).toString();
  const QString oldName = m_fanProfileCombo->itemText( idx );
  const QString newName = m_fanProfileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName ) {
    m_fanProfileCombo->setEditText( oldName );
    return;
  }

  if ( !m_profileManager->isProfileEditable( fanProfileId, m_profileManager->fanProfilesData() ) ) {
    m_fanProfileCombo->setEditText( oldName );
    return;
  }

  if ( !m_profileManager->renameFanProfile( fanProfileId, newName ) ) {
    m_fanProfileCombo->setEditText( oldName );
    return;
  }

  m_fanProfileCombo->setItemText( idx, newName );
  emit fanProfileRenamed( oldName, newName );
  showStatusMessage( QStringLiteral( "Fan profile renamed from '%1' to '%2'" ).arg( oldName, newName ) );
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
  const QColor currentColor( m_currentRed, m_currentGreen, m_currentBlue );
  const QColor color = QColorDialog::getColor( currentColor, this, "Choose LED Color" );
  if ( !color.isValid() ) return;

  m_currentRed = color.red();
  m_currentGreen = color.green();
  m_currentBlue = color.blue();

  if ( m_ledOnOffCheckBox->isChecked() && m_waterCoolerDbus )
  {
    const QDBusReply< bool > conn = m_waterCoolerDbus->call( QStringLiteral( "GetWaterCoolerConnected" ) );
    if ( conn.isValid() && conn.value() )
    {
      const RGBState mode = static_cast< RGBState >( m_ledModeCombo->currentData().toInt() );
      m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerLEDColor" ),
                               m_currentRed, m_currentGreen, m_currentBlue, static_cast< int >( mode ) );
    }
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
  const bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;
  const bool enableManualControls = wcEnabled && m_isWcConnected && !m_autoControl;

  if ( m_pumpVoltageCombo )
    m_pumpVoltageCombo->setEnabled( enableManualControls );
  if ( m_fanSpeedSlider )
    m_fanSpeedSlider->setEnabled( enableManualControls );

  if ( !enableManualControls || m_manualControlInitialized || !m_waterCoolerDbus )
    return;

  // First enable after connection or auto-control change: set safe defaults
  m_manualControlInitialized = true;
  if ( m_pumpVoltageCombo )
  {
    m_pumpVoltageCombo->setCurrentIndex( 0 );
    m_waterCoolerDbus->call( QStringLiteral( "TurnOffWaterCoolerPump" ) );
  }
  if ( m_fanSpeedSlider )
  {
    m_fanSpeedSlider->setValue( 0 );
    m_waterCoolerDbus->call( QStringLiteral( "SetWaterCoolerFanSpeed" ), 0 );
  }
}

void FanControlTab::updateWaterCoolerPolling()
{
  if ( !m_waterCoolerPollTimer ) return;

  const bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;

  if ( wcEnabled )
  {
    if ( !m_waterCoolerPollTimer->isActive() )
      m_waterCoolerPollTimer->start( 1000 );
    return;
  }

  if ( !m_waterCoolerPollTimer->isActive() )
    return;

  m_waterCoolerPollTimer->stop();
  onDisconnected();
  if ( m_waterCoolerDbus )
    m_waterCoolerDbus->call( QStringLiteral( "TurnOffWaterCoolerPump" ) );
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

void FanControlTab::buildZoneEditors( const ucc::dbus::FanZoneDtoList &zones,
                                      const ucc::dbus::ThermalSourceDtoList &thermalSources,
                                      const ucc::dbus::HardwareFanDeviceDtoList &hardwareFanDevices,
                                      const ucc::dbus::HardwareSensorDtoList &hardwareSensors )
{
  m_fanEditors.clear();
  m_pumpEditors.clear();
  m_thermalSourceCombos.clear();
  m_templateSourceCombos.clear();
  m_selectedTemplateByZone.clear();
  m_zoneGraphModifiedByUser.clear();
  m_zoneProgrammaticUpdate.clear();
  m_zoneTable = nullptr;
  m_sourceTable = nullptr;
  m_deviceTree = nullptr;

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
  for ( const auto &src : thermalSources )
    m_sourceEditorModel.push_back( src );

  m_allSourceCombos.clear();
  m_strategyCombos.clear();
  m_zoneSourceCombos.clear();

  // Load per-user aliases from GUI settings before building labels.
  loadSensorAliasesFromSettings();

  // Build sensor label map
  m_sensorLabelById.clear();
  for ( const auto &s : hardwareSensors )
  {
    const QString &sid = s.id;
    const QString d = s.displayLabel.trimmed();
    const QString &l = s.label;
    const QString defaultLabel = d.isEmpty() ? l : d;
    const QString alias = m_sensorAliasById.value( sid ).trimmed();
    m_sensorLabelById[sid] = alias.isEmpty() ? defaultLabel : alias;
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

    // Left column: hardware trees stacked vertically (Sensors above Devices)
    auto *leftPane = new QWidget();
    auto *leftPaneLayout = new QVBoxLayout( leftPane );
    leftPaneLayout->setContentsMargins( 0, 0, 0, 0 );
    leftPaneLayout->setSpacing( 8 );

    // ── Sensor tree pane ──
    auto *sensorPane = new QGroupBox( QString() );
    auto *sensorPaneLayout = new QVBoxLayout( sensorPane );
    sensorPaneLayout->setContentsMargins( 6, 6, 6, 6 );
    auto *sensorTree = new QTreeWidget();
    m_sensorTree = sensorTree;
    sensorTree->setColumnCount( 2 );
    sensorTree->setHeaderLabels( { QStringLiteral( "Hardware Sensors" ), QStringLiteral( "°C" ) } );
    sensorTree->header()->setStretchLastSection( false );
    sensorTree->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
    sensorTree->header()->setSectionResizeMode( 1, QHeaderView::ResizeToContents );
    sensorTree->setDragEnabled( true );
    sensorTree->setDragDropMode( QAbstractItemView::DragOnly );
    sensorTree->setEditTriggers( QAbstractItemView::DoubleClicked
                   | QAbstractItemView::EditKeyPressed
                   | QAbstractItemView::SelectedClicked );

    QMap< QString, QTreeWidgetItem * > sourceGroups;
    for ( const auto &s : hardwareSensors )
    {
      const QString &source = s.source;
      const QString sourceKey = normalizedSourceGroup( s );

      if ( !sourceGroups.contains( sourceKey ) )
      {
        auto *group = new QTreeWidgetItem( sensorTree );
        group->setText( 0, sourceKey );
        group->setFirstColumnSpanned( true );
        sourceGroups.insert( sourceKey, group );
      }

      auto *child = new QTreeWidgetItem( sourceGroups.value( sourceKey ) );
      const QString displayLabel = s.displayLabel.trimmed();
      const QString &rawLabel = s.label;
      const QString &sid = s.id;
      const QString defaultLabel = displayLabel.isEmpty() ? rawLabel : displayLabel;
      const QString alias = m_sensorAliasById.value( sid ).trimmed();
      child->setText( 0, alias.isEmpty() ? defaultLabel : alias );
      child->setData( 0, Qt::UserRole, sid );
      child->setData( 0, Qt::UserRole + 1, defaultLabel );
      child->setFlags( child->flags() | Qt::ItemIsEditable );
      child->setToolTip( 0, QStringLiteral( "Drag this sensor onto a temperature source row to assign it.\n\nRaw source: %1\nRaw label: %2" )
               .arg( source.isEmpty() ? QStringLiteral( "(none)" ) : source,
                 rawLabel.isEmpty() ? QStringLiteral( "(none)" ) : rawLabel ) );
    }

    for ( auto *group : sourceGroups )
    {
      group->setExpanded( false );
      group->setFlags( group->flags() & ~Qt::ItemIsEditable );
    }

    connect( sensorTree, &QTreeWidget::itemChanged, this,
             [this, sensorTree]( QTreeWidgetItem *item, int column ) {
               if ( column == 0 )
                 onSensorTreeItemChanged( sensorTree, item );
             } );

    sensorPaneLayout->addWidget( sensorTree );

    // ── Device tree pane ──
    auto *devicePane = new QGroupBox( QString() );
    auto *devicePaneLayout = new QVBoxLayout( devicePane );
    devicePaneLayout->setContentsMargins( 6, 6, 6, 6 );
    auto *deviceTree = new QTreeWidget();
    m_deviceTree = deviceTree;
    deviceTree->setColumnCount( 2 );
    deviceTree->setHeaderLabels( { QStringLiteral( "Hardware Devices" ), QStringLiteral( "RPM" ) } );
    deviceTree->header()->setStretchLastSection( false );
    deviceTree->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
    deviceTree->header()->setSectionResizeMode( 1, QHeaderView::ResizeToContents );
    deviceTree->setDragEnabled( true );
    deviceTree->setDragDropMode( QAbstractItemView::DragOnly );

    m_fanLabelById.clear();

    QMap< QString, QTreeWidgetItem * > deviceGroups;
    for ( const auto &d : hardwareFanDevices )
    {
      const QString groupKey = normalizedDeviceGroup( d );

      if ( !deviceGroups.contains( groupKey ) )
      {
        auto *group = new QTreeWidgetItem( deviceTree );
        group->setText( 0, groupKey );
        group->setFirstColumnSpanned( true );
        deviceGroups.insert( groupKey, group );
      }

      auto *child = new QTreeWidgetItem( deviceGroups.value( groupKey ) );
      const QString &label = d.label;
      const QString &did = d.id;
      const QString defaultLabel = label.isEmpty() ? did : label;
      const QString alias = m_deviceAliasById.value( did ).trimmed();
      const QString displayLabel = alias.isEmpty() ? defaultLabel : alias;
      child->setText( 0, displayLabel );
      child->setData( 0, Qt::UserRole, did );
      child->setData( 0, Qt::UserRole + 1, defaultLabel );
      child->setFlags( child->flags() | Qt::ItemIsEditable );
      child->setToolTip( 0, QStringLiteral( "Drag this device onto a zone row to assign it.\n\nID: %1\nType: %2" )
               .arg( did, d.deviceType ) );
      m_fanLabelById[did] = displayLabel;
    }

    for ( auto *group : deviceGroups )
    {
      group->setExpanded( true );
      group->setFlags( group->flags() & ~Qt::ItemIsEditable );
    }

    connect( deviceTree, &QTreeWidget::itemChanged, this,
             [this, deviceTree]( QTreeWidgetItem *item, int column ) {
               if ( column == 0 )
                 onDeviceTreeItemChanged( deviceTree, item );
             } );

    devicePaneLayout->addWidget( deviceTree );
    leftPaneLayout->addWidget( sensorPane, 1 );
    leftPaneLayout->addWidget( devicePane, 1 );

    // Right column: Temperature Sources above Fan Zones
    auto *rightPane = new QWidget();
    auto *rightPaneLayout = new QVBoxLayout( rightPane );
    rightPaneLayout->setContentsMargins( 0, 0, 0, 0 );
    rightPaneLayout->setSpacing( 8 );

    auto *sourcesGroup = new QGroupBox( QStringLiteral( "Temperature Sources" ) );
    auto *sourcesGroupLayout = new QVBoxLayout( sourcesGroup );
    sourcesGroupLayout->setContentsMargins( 6, 6, 6, 6 );

    auto *sourceTable = new ThermalSourceTableWidget();
    m_sourceTable = sourceTable;
    m_sourceTable->setColumnCount( 4 );
    m_sourceTable->setHorizontalHeaderLabels( { QStringLiteral( "Label" ),
                           QStringLiteral( "Strategy" ),
                           QStringLiteral( "Sensors" ),
                           QStringLiteral( "°C" ) } );
    m_sourceTable->horizontalHeader()->setStretchLastSection( false );
    m_sourceTable->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::Stretch );
    m_sourceTable->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::ResizeToContents );
    m_sourceTable->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::Stretch );
    m_sourceTable->horizontalHeader()->setSectionResizeMode( 3, QHeaderView::ResizeToContents );
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
    auto *zonesGroup = new QGroupBox( QStringLiteral( "Fan Zones" ) );
    auto *zonesGroupLayout = new QVBoxLayout( zonesGroup );
    zonesGroupLayout->setContentsMargins( 6, 6, 6, 6 );

    auto *zoneTable = new ZoneTableWidget();
    m_zoneTable = zoneTable;
    m_zoneTable->setColumnCount( 6 );
    m_zoneTable->setHorizontalHeaderLabels( { QStringLiteral( "Name" ),
                                 QStringLiteral( "Temp Source" ),
                                 QStringLiteral( "Devices" ),
                                 QStringLiteral( "Type" ),
                                 QStringLiteral( "°C" ),
                                 QStringLiteral( "Fan %" ) } );
    m_zoneTable->horizontalHeader()->setStretchLastSection( false );
    m_zoneTable->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::Stretch );
    m_zoneTable->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::ResizeToContents );
    m_zoneTable->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::Stretch );
    m_zoneTable->horizontalHeader()->setSectionResizeMode( 3, QHeaderView::ResizeToContents );
    m_zoneTable->horizontalHeader()->setSectionResizeMode( 4, QHeaderView::ResizeToContents );
    m_zoneTable->horizontalHeader()->setSectionResizeMode( 5, QHeaderView::ResizeToContents );
    m_zoneTable->setSelectionBehavior( QAbstractItemView::SelectRows );
    m_zoneTable->setSelectionMode( QAbstractItemView::SingleSelection );
    m_zoneTable->setEditTriggers( QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed );
    m_zoneTable->setContextMenuPolicy( Qt::CustomContextMenu );
    m_zoneTable->setAcceptDrops( true );
    m_zoneTable->viewport()->setAcceptDrops( true );
    m_zoneTable->setDragDropMode( QAbstractItemView::DropOnly );
    m_zoneTable->setDefaultDropAction( Qt::CopyAction );

    m_zoneCache.clear();
    m_zoneCache.reserve( zones.size() );
    for ( const auto &z : zones )
      m_zoneCache.push_back( z );

    m_zoneTable->setRowCount( m_zoneCache.size() );
    m_zoneSourceCombos.clear();
    m_zoneSourceCombos.resize( m_zoneCache.size() );

    zoneTable->onDeviceDropped = [this]( int row, const QString &did ) {
      onDeviceDroppedOnZone( row, did );
    };

    for ( int row = 0; row < m_zoneCache.size(); ++row )
    {
      installZoneSourceComboForRow( row );
      refreshZoneRow( row );
    }

    connect( m_zoneTable, &QTableWidget::itemChanged, this, &FanControlTab::onZoneItemChanged );
    connect( m_zoneTable, &QWidget::customContextMenuRequested, this, &FanControlTab::onZoneContextMenu );

    zonesGroupLayout->addWidget( m_zoneTable );
    rightPaneLayout->addWidget( sourcesGroup, 1 );
    rightPaneLayout->addWidget( zonesGroup, 1 );

    split->addWidget( leftPane );
    split->addWidget( rightPane );
    split->setStretchFactor( 0, 1 );
    split->setStretchFactor( 1, 2 );
    pageLayout->addWidget( split, 1 );

    if ( m_sourceTable->rowCount() > 0 )
      m_sourceTable->selectRow( 0 );
    if ( m_zoneTable->rowCount() > 0 )
      m_zoneTable->selectRow( 0 );

    m_subTabs->addTab( page, QStringLiteral( "Zone Setup" ) );
  }

  // Start polling sensor readings for the Zone Setup tab
  if ( !m_sensorPollTimer )
  {
    m_sensorPollTimer = new QTimer( this );
    m_sensorPollTimer->setInterval( 1000 );
    connect( m_sensorPollTimer, &QTimer::timeout, this, &FanControlTab::pollSensorReadings );
  }
  m_sensorPollTimer->start();
  // Do an initial poll immediately
  pollSensorReadings();

  // ---------------------------------------------------------------------
  // Per-zone curve editor tabs
  // ---------------------------------------------------------------------
  for ( const auto &zone : zones )
  {
    const QString &id = zone.id;
    const QString &name = zone.name;
    const QString &devType = zone.deviceType;
    const QString &tsId = zone.thermalSourceId;

    auto *page = new QWidget();
    auto *pageLayout = new QVBoxLayout( page );
    pageLayout->setContentsMargins( 4, 4, 4, 4 );
    pageLayout->setSpacing( 4 );

    // Thermal source combo
    auto *tsBar = new QHBoxLayout();
    tsBar->setContentsMargins( 0, 0, 0, 0 );
    auto *tsLabel = new QLabel( QStringLiteral( "Temperature Source:" ) );
    auto *tsCombo = new QComboBox();
    for ( const auto &src : m_sourceEditorModel )
    {
      if ( src.id.trimmed().isEmpty() )
        continue;
      tsCombo->addItem( src.label, src.id );
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
               onCurveTabSourceComboChanged( zoneId, tsCombo->currentData().toString() );
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
      auto *templateLabel = new QLabel( QStringLiteral( "Based On:" ) );
      auto *templateCombo = new QComboBox();
      populateZoneTemplateCombo( templateCombo );
      tsBar->addWidget( templateLabel );
      tsBar->addWidget( templateCombo, 1 );
      m_templateSourceCombos[id] = templateCombo;

      connect( templateCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
               [this, zoneId = id, templateCombo]( int ) {
                 onTemplateComboChanged( zoneId, templateCombo->currentData().toString(), templateCombo );
               } );

      auto *editor = new FanCurveEditorWidget();
      m_fanEditors[id] = editor;
      connect( editor, &FanCurveEditorWidget::pointsChanged, this,
               [this, zoneId = id]( const QVector< FanCurveEditorWidget::Point > &pts ) {
                 onFanEditorPointsChanged( zoneId, pts );
               } );
      pageLayout->addWidget( editor, 1 );
    }

    m_subTabs->addTab( page, name );
  }
}

// ── Source / zone editor member functions ────────────────────────────

void FanControlTab::populateZoneTemplateCombo( QComboBox *combo ) const
{
  if ( !combo )
    return;

  const QString currentId = combo->currentData().toString();
  combo->blockSignals( true );
  combo->clear();

  for ( const FanCurveTemplate &tmpl : kFanCurveTemplates )
  {
    if ( tmpl.id.isEmpty() || tmpl.points.isEmpty() )
      continue;

    combo->addItem( tmpl.name, tmpl.id );
  }

  int newIndex = 0;
  for ( int i = 0; i < combo->count(); ++i )
  {
    if ( combo->itemData( i ).toString() == currentId )
    {
      newIndex = i;
      break;
    }
  }
  combo->setCurrentIndex( newIndex );
  combo->blockSignals( false );
}

void FanControlTab::applyCurveTemplateToZone( const QString &zoneId,
                                              const QString &templateId )
{
  const FanCurveTemplate *tmpl = findStaticFanCurveTemplate( templateId );
  if ( !tmpl || tmpl->points.isEmpty() )
  {
    showStatusMessage( QStringLiteral( "Selected template has no curve for this zone." ) );
    return;
  }

  auto *editor = fanEditor( zoneId );
  if ( !editor )
    return;

  m_zoneProgrammaticUpdate[zoneId] = true;
  editor->setPoints( tmpl->points );
  m_zoneProgrammaticUpdate[zoneId] = false;
  emit fanCurveChanged( zoneId, tmpl->points );

  showStatusMessage( QStringLiteral( "Applied template curve to zone." ) );
}

void FanControlTab::showStatusMessage( const QString &msg, int timeoutMs )
{
  if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    if ( auto *sb = mw->statusBar() )
      sb->showMessage( msg, timeoutMs );
}

void FanControlTab::loadSensorAliasesFromSettings()
{
  QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
  settings.beginGroup( QStringLiteral( "sensorAliases" ) );

  m_sensorAliasById.clear();
  const QStringList keys = settings.childKeys();
  for ( const QString &key : keys )
  {
    const QString alias = settings.value( key ).toString().trimmed();
    if ( !alias.isEmpty() )
      m_sensorAliasById[key] = alias;
  }

  settings.endGroup();

  settings.beginGroup( QStringLiteral( "deviceAliases" ) );

  m_deviceAliasById.clear();
  const QStringList deviceKeys = settings.childKeys();
  for ( const QString &key : deviceKeys )
  {
    const QString alias = settings.value( key ).toString().trimmed();
    if ( !alias.isEmpty() )
      m_deviceAliasById[key] = alias;
  }

  settings.endGroup();
}

void FanControlTab::saveSensorAliasesToSettings() const
{
  QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
  settings.beginGroup( QStringLiteral( "sensorAliases" ) );
  settings.remove( QString() );

  for ( auto it = m_sensorAliasById.constBegin(); it != m_sensorAliasById.constEnd(); ++it )
  {
    const QString alias = it.value().trimmed();
    if ( !alias.isEmpty() )
      settings.setValue( it.key(), alias );
  }

  settings.endGroup();

  settings.beginGroup( QStringLiteral( "deviceAliases" ) );
  settings.remove( QString() );

  for ( auto it = m_deviceAliasById.constBegin(); it != m_deviceAliasById.constEnd(); ++it )
  {
    const QString alias = it.value().trimmed();
    if ( !alias.isEmpty() )
      settings.setValue( it.key(), alias );
  }

  settings.endGroup();
  settings.sync();
}

void FanControlTab::onWaterCoolerPollTimeout()
{
  if ( !m_waterCoolerDbus ) return;

  const bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;
  if ( !wcEnabled )
  {
    onDisconnected();
    return;
  }

  const QDBusReply< bool > conn = m_waterCoolerDbus->call( QStringLiteral( "GetWaterCoolerConnected" ) );
  if ( conn.isValid() && conn.value() )
    onConnected();
  else
    onDisconnected();
}

void FanControlTab::onStrategyComboChanged( int row )
{
  if ( row < 0 || row >= m_sourceEditorModel.size() ) return;

  auto *combo = m_strategyCombos.value( row, nullptr );
  if ( !combo ) return;

  auto &src = m_sourceEditorModel[row];
  if ( src.sensorIds.size() <= 1 )
  {
    src.strategy = QStringLiteral( "single" );
    combo->blockSignals( true );
    const int idx = combo->findData( QStringLiteral( "single" ) );
    combo->setCurrentIndex( idx >= 0 ? idx : 0 );
    combo->blockSignals( false );
  }
  else
  {
    src.strategy = combo->currentData().toString();
  }

  if ( m_sourceTable->currentRow() == row )
    m_sourceTable->setCurrentCell( row, m_sourceTable->currentColumn() );
}

QString FanControlTab::normalizedSourceGroup( const ucc::dbus::HardwareSensorDto &sensor )
{
  const QString &rawSource = sensor.source;
  const QString sourceDisplay = sensor.sourceDisplay.trimmed();
  const QString raw = sourceDisplay.isEmpty() ? rawSource : sourceDisplay;
  const QString s = raw.trimmed().toLower();
  const QString category = sensor.category.trimmed().toLower();

  if ( category == QStringLiteral( "cpu" ) )   return QStringLiteral( "CPU" );
  if ( category == QStringLiteral( "nvme" ) )  return QStringLiteral( "NVMe" );
  if ( category == QStringLiteral( "ddr5" ) )  return QStringLiteral( "DDR5" );
  if ( category == QStringLiteral( "board" ) ) return QStringLiteral( "Board" );
  if ( category == QStringLiteral( "network" ) ) return QStringLiteral( "Network" );
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

  // Collapse verbose NIC descriptors into a stable group label.
  if ( s.contains( QStringLiteral( "ethernet" ) )
       || s.contains( QStringLiteral( "network" ) )
       || s.contains( QStringLiteral( "aqtion" ) )
       || s.contains( QStringLiteral( "atlantic" ) )
       || s.contains( QStringLiteral( "nic" ) ) )
    return QStringLiteral( "Network" );

  if ( raw.size() > 32 )
    return raw.left( 29 ) + QStringLiteral( "..." );
  return raw;
}

void FanControlTab::ensureValidStrategy( ucc::dbus::ThermalSourceDto &src )
{
  const int sensorCount = src.sensorIds.size();
  if ( sensorCount <= 1 )
  {
    src.strategy = QStringLiteral( "single" );
    return;
  }
  if ( src.strategy == QStringLiteral( "single" ) || !strategyChoices().contains( src.strategy ) )
    src.strategy = QStringLiteral( "average" );
}

QString FanControlTab::sensorsSummaryText( const ucc::dbus::ThermalSourceDto &src ) const
{
  QStringList labels;
  for ( const QString &sid : src.sensorIds )
    labels << m_sensorLabelById.value( sid, sid );
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
    for ( const auto &src : m_sourceEditorModel )
    {
      if ( src.id.trimmed().isEmpty() )
        continue;
      combo->addItem( src.label, src.id );
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
           [this, row]( int ) { onStrategyComboChanged( row ); } );
}

void FanControlTab::refreshSourceRow( int row )
{
  if ( row < 0 || row >= m_sourceEditorModel.size() ) return;

  auto &src = m_sourceEditorModel[row];
  ensureValidStrategy( src );

  auto *labelItem = m_sourceTable->item( row, 0 );
  if ( !labelItem )
  {
    labelItem = new QTableWidgetItem();
    m_sourceTable->setItem( row, 0, labelItem );
  }
  labelItem->setText( src.label );
  labelItem->setData( Qt::UserRole, src.id );
  labelItem->setFlags( labelItem->flags() | Qt::ItemIsEditable );

  if ( row < m_strategyCombos.size() && m_strategyCombos[row] )
  {
    const int sensorCount = src.sensorIds.size();
    m_strategyCombos[row]->blockSignals( true );

    // Strategy options depend on sensor count:
    //  - single-sensor sources: only "single"
    //  - multi-sensor sources: everything except "single"
    m_strategyCombos[row]->clear();
    if ( sensorCount <= 1 )
    {
      m_strategyCombos[row]->addItem( QStringLiteral( "single" ), QStringLiteral( "single" ) );
    }
    else
    {
      for ( const QString &opt : strategyChoices() )
      {
        if ( opt == QStringLiteral( "single" ) )
          continue;
        m_strategyCombos[row]->addItem( opt, opt );
      }
    }

    const QString &strategy = src.strategy;
    const int idx = m_strategyCombos[row]->findData( strategy );
    m_strategyCombos[row]->setCurrentIndex( idx >= 0 ? idx : 0 );
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

  auto &src = m_sourceEditorModel[row];
  if ( src.sensorIds.contains( sensorId ) )
    return; // already assigned

  const int beforeCount = src.sensorIds.size();
  src.sensorIds.append( sensorId );

  if ( src.sensorIds.size() <= 1 )
    src.strategy = QStringLiteral( "single" );
  else if ( beforeCount == 1 )
    src.strategy = QStringLiteral( "max" );

  refreshSourceRow( row );
}

void FanControlTab::createSourceWithSensor( const QString &sensorId )
{
  if ( sensorId.isEmpty() )
    return;

  ucc::dbus::ThermalSourceDto src;
  src.id = QUuid::createUuid().toString( QUuid::WithoutBraces );
  src.label = m_sensorLabelById.value( sensorId, QStringLiteral( "New Source" ) );
  src.strategy = QStringLiteral( "single" );
  src.sensorIds = { sensorId };

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

  auto &src = m_sourceEditorModel[row];
  src.label = item->text().trimmed();
  if ( src.label.isEmpty() )
    src.label = QStringLiteral( "Unnamed Source" );
  item->setText( src.label );
  refreshAllSourceCombos();
}

void FanControlTab::onSourceContextMenu( const QPoint &pos )
{
  QMenu menu( m_sourceTable );
  QAction *removeSourceAction = menu.addAction( QStringLiteral( "Remove Source" ) );
  QMenu *removeSensorMenu = menu.addMenu( QStringLiteral( "Remove Sensor" ) );

  const int row = m_sourceTable->currentRow();
  removeSourceAction->setEnabled( row >= 0 );

  const bool validRow = row >= 0 && row < m_sourceEditorModel.size();
  if ( !validRow )
  {
    removeSensorMenu->setEnabled( false );
  }
  else
  {
    const QStringList &sensorIds = m_sourceEditorModel[row].sensorIds;
    if ( sensorIds.isEmpty() )
    {
      auto *noneAction = removeSensorMenu->addAction( QStringLiteral( "(none)" ) );
      noneAction->setEnabled( false );
    }
    for ( const QString &sid : sensorIds )
    {
      QAction *a = removeSensorMenu->addAction( m_sensorLabelById.value( sid, sid ) );
      a->setData( sid );
      if ( sensorIds.size() <= 1 )
        a->setEnabled( false );
    }
  }

  QAction *chosen = menu.exec( m_sourceTable->viewport()->mapToGlobal( pos ) );
  if ( !chosen ) return;

  if ( chosen == removeSourceAction )
  {
    handleRemoveSource( row );
    return;
  }

  const QVariant sensorData = chosen->data();
  if ( sensorData.isValid() && validRow )
    handleRemoveSensor( row, sensorData.toString() );
}

void FanControlTab::handleRemoveSource( int row )
{
  if ( row < 0 || row >= m_sourceEditorModel.size() ) return;

  if ( m_sourceEditorModel.size() <= 1 )
  {
    showStatusMessage( QStringLiteral( "At least one source must remain." ) );
    return;
  }

  const QString removedSourceId = m_sourceEditorModel[row].id;
  ucc::dbus::FanZoneDtoList updatedZones;
  QStringList affectedZones;
  for ( auto z : m_lastZones )
  {
    if ( z.thermalSourceId == removedSourceId )
    {
      affectedZones << z.name;
      z.thermalSourceId.clear();
    }
    updatedZones.append( z );
  }

  if ( !affectedZones.isEmpty() )
    showStatusMessage( QStringLiteral( "Removed source was used by: %1. Their sources were invalidated." )
                         .arg( affectedZones.join( QStringLiteral( ", " ) ) ) );

  m_sourceEditorModel.removeAt( row );

  ucc::dbus::ThermalSourceDtoList updatedSources( m_sourceEditorModel.cbegin(), m_sourceEditorModel.cend() );

  buildZoneEditors( updatedZones, updatedSources, m_lastFanDevices, m_lastSensors );
}

void FanControlTab::handleRemoveSensor( int row, const QString &sensorId )
{
  if ( row < 0 || row >= m_sourceEditorModel.size() || sensorId.isEmpty() )
    return;

  auto &src = m_sourceEditorModel[row];
  if ( src.sensorIds.size() <= 1 )
  {
    showStatusMessage( QStringLiteral( "A source must have at least one sensor." ) );
    return;
  }

  src.sensorIds.removeAll( sensorId );
  if ( src.sensorIds.size() <= 1 )
    src.strategy = QStringLiteral( "single" );

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

// ── Zone table helpers ──────────────────────────────────────────────

QString FanControlTab::normalizedDeviceGroup( const ucc::dbus::HardwareFanDeviceDto &device )
{
  const QString sourceName = device.sourceName.trimmed();
  if ( !sourceName.isEmpty() )
    return sourceName;

  const QString dt = device.deviceType.toLower();
  if ( dt == QStringLiteral( "fan" ) )         return QStringLiteral( "Fans" );
  if ( dt == QStringLiteral( "pump" ) )        return QStringLiteral( "Pumps" );
  if ( dt == QStringLiteral( "stagedpump" ) )  return QStringLiteral( "Staged Pumps" );
  if ( dt == QStringLiteral( "staged" ) )      return QStringLiteral( "Staged" );
  if ( dt == QStringLiteral( "virtual" ) )     return QStringLiteral( "Virtual" );
  return QStringLiteral( "Other" );
}

void FanControlTab::installZoneSourceComboForRow( int row )
{
  if ( row < 0 || row >= m_zoneCache.size() )
    return;

  if ( m_zoneSourceCombos.size() <= row )
    m_zoneSourceCombos.resize( row + 1 );

  auto *combo = new QComboBox( m_zoneTable );
  for ( const auto &src : m_sourceEditorModel )
  {
    if ( src.id.trimmed().isEmpty() )
      continue;
    combo->addItem( src.label, src.id );
  }

  const QString &tsId = m_zoneCache[row].thermalSourceId;
  for ( int i = 0; i < combo->count(); ++i )
  {
    if ( combo->itemData( i ).toString() == tsId )
    {
      combo->setCurrentIndex( i );
      break;
    }
  }

  m_zoneSourceCombos[row] = combo;
  m_zoneTable->setCellWidget( row, 1, combo );
  m_allSourceCombos.push_back( combo );

  connect( combo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
           [this, row]( int ) { onZoneSourceComboChanged( row ); } );
}

void FanControlTab::installZoneTypeComboForRow( int row )
{
  if ( row < 0 || row >= m_zoneCache.size() )
    return;

  auto *combo = new QComboBox( m_zoneTable );
  combo->addItem( QStringLiteral( "Fan" ),   QStringLiteral( "fan" ) );
  combo->addItem( QStringLiteral( "Pump" ),  QStringLiteral( "pump" ) );

  const QString dt = m_zoneCache[row].deviceType.toLower();
  for ( int i = 0; i < combo->count(); ++i )
  {
    if ( combo->itemData( i ).toString() == dt )
    {
      combo->setCurrentIndex( i );
      break;
    }
  }

  m_zoneTable->setCellWidget( row, 3, combo );

  connect( combo, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
           [this, row, combo]( int ) {
             if ( row < 0 || row >= m_zoneCache.size() )
               return;
             m_zoneCache[row].deviceType = combo->currentData().toString();
           } );
}

void FanControlTab::onZoneSourceComboChanged( int row )
{
  if ( row < 0 || row >= m_zoneCache.size() )
    return;
  auto *combo = m_zoneSourceCombos.value( row );
  if ( !combo )
    return;
  const QString &zoneId = m_zoneCache[row].id;
  const QString tsId = combo->currentData().toString();

  auto it = m_thermalSourceCombos.find( zoneId );
  if ( it != m_thermalSourceCombos.end() && it.value() )
  {
    QSignalBlocker blocker( it.value() );
    selectComboByData( it.value(), tsId );
  }

  emit thermalSourceChanged( zoneId, tsId );
}

void FanControlTab::onCurveTabSourceComboChanged( const QString &zoneId, const QString &tsId )
{
  for ( int r = 0; r < m_zoneCache.size(); ++r )
  {
    if ( m_zoneCache[r].id != zoneId )
      continue;
    if ( r < m_zoneSourceCombos.size() && m_zoneSourceCombos[r] )
    {
      QSignalBlocker blocker( m_zoneSourceCombos[r] );
      selectComboByData( m_zoneSourceCombos[r], tsId );
    }
    break;
  }
  emit thermalSourceChanged( zoneId, tsId );
}

void FanControlTab::selectComboByData( QComboBox *combo, const QString &data )
{
  for ( int i = 0; i < combo->count(); ++i )
  {
    if ( combo->itemData( i ).toString() == data )
    {
      combo->setCurrentIndex( i );
      return;
    }
  }
}

void FanControlTab::onSensorTreeItemChanged( QTreeWidget *tree, QTreeWidgetItem *item )
{
  if ( !item )
    return;
  const QString sid = item->data( 0, Qt::UserRole ).toString();
  if ( sid.isEmpty() )
    return; // group row
  const QString defaultLabel = item->data( 0, Qt::UserRole + 1 ).toString();
  const QString alias = item->text( 0 ).trimmed();
  if ( alias.isEmpty() )
  {
    m_sensorAliasById.remove( sid );
    QSignalBlocker blocker( tree );
    item->setText( 0, defaultLabel );
    m_sensorLabelById[sid] = defaultLabel;
  }
  else
  {
    m_sensorAliasById[sid] = alias;
    m_sensorLabelById[sid] = alias;
  }
  for ( int row = 0; row < m_sourceEditorModel.size(); ++row )
    refreshSourceRow( row );
}

void FanControlTab::onDeviceTreeItemChanged( QTreeWidget *tree, QTreeWidgetItem *item )
{
  if ( !item )
    return;
  const QString did = item->data( 0, Qt::UserRole ).toString();
  if ( did.isEmpty() )
    return; // group row
  const QString defaultLabel = item->data( 0, Qt::UserRole + 1 ).toString();
  const QString alias = item->text( 0 ).trimmed();
  if ( alias.isEmpty() )
  {
    m_deviceAliasById.remove( did );
    QSignalBlocker blocker( tree );
    item->setText( 0, defaultLabel );
    m_fanLabelById[did] = defaultLabel;
  }
  else
  {
    m_deviceAliasById[did] = alias;
    m_fanLabelById[did] = alias;
  }
  for ( int row = 0; row < m_zoneCache.size(); ++row )
    refreshZoneRow( row );
}

void FanControlTab::onTemplateComboChanged( const QString &zoneId, const QString &templateId, QComboBox *combo )
{
  if ( templateId.isEmpty() )
    return;
  const QString previousTemplate = m_selectedTemplateByZone.value( zoneId );
  if ( templateId == previousTemplate )
    return;
  if ( m_zoneGraphModifiedByUser.value( zoneId, false ) )
  {
    const int answer = QMessageBox::question(
        this,
        QStringLiteral( "Replace Curve" ),
        QStringLiteral( "Applying a new template will erase your graph changes. Continue?" ),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No );
    if ( answer != QMessageBox::Yes )
    {
      QSignalBlocker blocker( combo );
      const int prevIdx = previousTemplate.isEmpty() ? -1 : combo->findData( previousTemplate );
      combo->setCurrentIndex( prevIdx );
      return;
    }
  }
  applyCurveTemplateToZone( zoneId, templateId );
  m_selectedTemplateByZone[zoneId] = templateId;
  m_zoneGraphModifiedByUser[zoneId] = false;
}

void FanControlTab::onFanEditorPointsChanged( const QString &zoneId,
                                              const QVector< FanCurveEditorWidget::Point > &pts )
{
  if ( !m_zoneProgrammaticUpdate.value( zoneId, false ) )
    m_zoneGraphModifiedByUser[zoneId] = true;
  emit fanCurveChanged( zoneId, pts );
}

void FanControlTab::rebuildZoneEditorsWithState(
    const ucc::dbus::FanZoneDtoList &zones,
    const QMap< QString, QVector< FanCurveEditorWidget::Point > > &fanPoints,
    const QMap< QString, QVector< PumpCurveEditorWidget::Point > > &pumpPoints,
    const QMap< QString, QString > &sourceByZone )
{
  ucc::dbus::ThermalSourceDtoList sources( m_sourceEditorModel.cbegin(), m_sourceEditorModel.cend() );
  buildZoneEditors( zones, sources, m_lastFanDevices, m_lastSensors );
  for ( auto it = fanPoints.cbegin(); it != fanPoints.cend(); ++it )
    if ( auto *ed = fanEditor( it.key() ) )
      ed->setPoints( it.value() );
  for ( auto it = pumpPoints.cbegin(); it != pumpPoints.cend(); ++it )
    if ( auto *ed = pumpEditor( it.key() ) )
      ed->setPoints( it.value() );
  for ( auto it = sourceByZone.cbegin(); it != sourceByZone.cend(); ++it )
    setThermalSourceForZone( it.key(), it.value() );
}

void FanControlTab::onZoneItemChanged( QTableWidgetItem *item )
{
  if ( !item || item->column() != 0 )
    return;

  const int row = item->row();
  if ( row < 0 || row >= m_zoneCache.size() )
    return;

  auto &zone = m_zoneCache[row];
  QString name = item->text().trimmed();
  if ( name.isEmpty() )
    name = zone.name;
  if ( name.isEmpty() )
    name = QStringLiteral( "Unnamed Zone" );

  zone.name = name;

  if ( item->text() != name )
  {
    QSignalBlocker blocker( m_zoneTable );
    item->setText( name );
  }

  // Tab 0 is "Zone Setup"; zone rows map to tabs starting at index 1
  const int tabIndex = row + 1;
  if ( m_subTabs && tabIndex < m_subTabs->count() )
    m_subTabs->setTabText( tabIndex, name );
}

QString FanControlTab::devicesSummaryText( const ucc::dbus::FanZoneDto &zone ) const
{
  if ( zone.fanIds.isEmpty() )
    return QStringLiteral( "(none)" );

  QStringList parts;
  parts.reserve( zone.fanIds.size() );
  for ( const QString &fid : zone.fanIds )
  {
    const QString label = m_fanLabelById.value( fid );
    parts.append( label.isEmpty() ? fid : label );
  }
  return parts.join( QStringLiteral( ", " ) );
}

void FanControlTab::refreshZoneRow( int row )
{
  if ( row < 0 || row >= m_zoneCache.size() || !m_zoneTable )
    return;

  const auto &zone = m_zoneCache[row];

  // Column 0: Name
  auto *nameItem = new QTableWidgetItem( zone.name );
  nameItem->setFlags( nameItem->flags() | Qt::ItemIsEditable );
  m_zoneTable->setItem( row, 0, nameItem );

  // Column 1: Temp Source (combo widget — installed elsewhere)

  // Column 2: Devices summary (read-only)
  auto *devicesItem = new QTableWidgetItem( devicesSummaryText( zone ) );
  devicesItem->setFlags( devicesItem->flags() & ~Qt::ItemIsEditable );
  m_zoneTable->setItem( row, 2, devicesItem );

  // Column 3: Type combo (fan / pump)
  installZoneTypeComboForRow( row );
}

void FanControlTab::onDeviceDroppedOnZone( int row, const QString &deviceId )
{
  if ( row < 0 || row >= m_zoneCache.size() )
  {
    showStatusMessage( QStringLiteral( "Drop a device onto an existing zone row." ) );
    return;
  }
  addDeviceToZone( row, deviceId );
}

void FanControlTab::addDeviceToZone( int row, const QString &deviceId )
{
  if ( row < 0 || row >= m_zoneCache.size() )
    return;

  auto &zone = m_zoneCache[row];

  // Check if already in this zone
  if ( zone.fanIds.contains( deviceId ) )
  {
    showStatusMessage( QStringLiteral( "Device already assigned to this zone." ) );
    return;
  }

  // Check if device belongs to another zone — ask before moving
  for ( int other = 0; other < m_zoneCache.size(); ++other )
  {
    if ( other == row )
      continue;
    if ( !m_zoneCache[other].fanIds.contains( deviceId ) )
      continue;

    const QString devLabel = m_fanLabelById.value( deviceId, deviceId );
    const QString &otherName = m_zoneCache[other].name;
    const QString &targetName = zone.name;
    const int answer = QMessageBox::question(
        m_zoneTable,
        QStringLiteral( "Move Device" ),
        QStringLiteral( "%1 is currently assigned to %2.\nMove it to %3?" )
            .arg( devLabel, otherName, targetName ),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No );
    if ( answer != QMessageBox::Yes )
      return;

    // Remove from old zone
    m_zoneCache[other].fanIds.removeAll( deviceId );
    refreshZoneRow( other );
    break;
  }

  zone.fanIds.append( deviceId );
  refreshZoneRow( row );
  showStatusMessage( QStringLiteral( "Moved device to zone." ) );
}

void FanControlTab::handleRemoveDevice( int row, const QString &deviceId )
{
  if ( row < 0 || row >= m_zoneCache.size() )
    return;

  m_zoneCache[row].fanIds.removeAll( deviceId );
  refreshZoneRow( row );
  showStatusMessage( QStringLiteral( "Removed device from zone." ) );
}

void FanControlTab::onZoneContextMenu( const QPoint &pos )
{
  if ( !m_zoneTable )
    return;

  const int row = m_zoneTable->rowAt( pos.y() );
  const bool hasRow = row >= 0 && row < m_zoneCache.size();

  QMenu menu( m_zoneTable );
  QAction *addZoneAction = menu.addAction( QStringLiteral( "Add Zone" ) );
  QAction *removeZoneAction = menu.addAction( QStringLiteral( "Remove Zone" ) );
  removeZoneAction->setEnabled( hasRow );
  menu.addSeparator();

  if ( hasRow && !m_zoneCache[row].fanIds.isEmpty() )
  {
    QMenu *removeDevMenu = menu.addMenu( QStringLiteral( "Remove Device" ) );
    for ( const QString &fid : m_zoneCache[row].fanIds )
    {
      const QString label = m_fanLabelById.value( fid );
      const QString text = label.isEmpty() ? fid : QStringLiteral( "%1 (%2)" ).arg( label, fid );
      QAction *act = removeDevMenu->addAction( text );
      connect( act, &QAction::triggered, this, [this, row, fid]() {
        handleRemoveDevice( row, fid );
      } );
    }
  }

  QAction *chosen = menu.exec( m_zoneTable->viewport()->mapToGlobal( pos ) );
  if ( !chosen )
    return;

  // Snapshot current editor state so we can rebuild and keep existing zone tabs/curves.
  QMap< QString, QVector< FanCurveEditorWidget::Point > > fanPointsByZone;
  for ( auto it = m_fanEditors.cbegin(); it != m_fanEditors.cend(); ++it )
  {
    if ( it.value() )
      fanPointsByZone[it.key()] = it.value()->points();
  }

  QMap< QString, QVector< PumpCurveEditorWidget::Point > > pumpPointsByZone;
  for ( auto it = m_pumpEditors.cbegin(); it != m_pumpEditors.cend(); ++it )
  {
    if ( it.value() )
      pumpPointsByZone[it.key()] = it.value()->points();
  }

  QMap< QString, QString > sourceByZone;
  for ( const auto &z : m_zoneCache )
  {
    if ( z.id.isEmpty() )
      continue;
    const QString tsId = thermalSourceForZone( z.id );
    if ( !tsId.isEmpty() )
      sourceByZone[z.id] = tsId;
  }

  if ( chosen == addZoneAction )
  {
    const QString newId = QUuid::createUuid().toString( QUuid::WithoutBraces );

    const QString suggestedName = QStringLiteral( "Custom Zone" );
    bool ok = false;
    const QString enteredName = QInputDialog::getText(
        m_zoneTable,
        QStringLiteral( "Add Zone" ),
        QStringLiteral( "Zone name:" ),
        QLineEdit::Normal,
        suggestedName,
        &ok ).trimmed();
    if ( !ok )
      return;

    ucc::dbus::FanZoneDto newZone;
    newZone.id = newId;
    newZone.name = enteredName.isEmpty() ? suggestedName : enteredName;
    newZone.deviceType = QStringLiteral( "fan" );

    QString defaultSourceId;
    for ( const auto &src : m_sourceEditorModel )
    {
      if ( !src.id.trimmed().isEmpty() )
      {
        defaultSourceId = src.id;
        break;
      }
    }
    if ( !defaultSourceId.isEmpty() )
      newZone.thermalSourceId = defaultSourceId;

    ucc::dbus::FanZoneDtoList newZones( m_zoneCache.cbegin(), m_zoneCache.cend() );
    newZones.append( newZone );

    if ( !defaultSourceId.isEmpty() )
      sourceByZone[newId] = defaultSourceId;

    rebuildZoneEditorsWithState( newZones, fanPointsByZone, pumpPointsByZone, sourceByZone );
    showStatusMessage( QStringLiteral( "Zone added." ) );
    return;
  }

  if ( chosen == removeZoneAction )
  {
    const QString &zoneName = hasRow ? m_zoneCache[row].name : QString();
    const QString &zoneId = hasRow ? m_zoneCache[row].id : QString();
    const int answer = QMessageBox::question(
        m_zoneTable,
        QStringLiteral( "Remove Zone" ),
        QStringLiteral( "Remove zone %1?" ).arg( zoneName.isEmpty() ? zoneId : zoneName ),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No );

    if ( answer != QMessageBox::Yes )
      return;

    ucc::dbus::FanZoneDtoList newZones;
    for ( int i = 0; i < m_zoneCache.size(); ++i )
    {
      if ( i == row )
        continue;
      auto z = m_zoneCache[i];
      const QString tsId = sourceByZone.value( z.id );
      if ( !tsId.isEmpty() )
        z.thermalSourceId = tsId;
      newZones.append( z );
    }

    rebuildZoneEditorsWithState( newZones, fanPointsByZone, pumpPointsByZone, sourceByZone );

    showStatusMessage( QStringLiteral( "Zone removed." ) );
  }
}

QString FanControlTab::thermalSourceForZone( const QString &zoneId ) const
{
  auto it = m_thermalSourceCombos.find( zoneId );
  if ( it != m_thermalSourceCombos.end() && it.value() )
    return it.value()->currentData().toString();
  return {};
}

ucc::dbus::ThermalSourceDtoList FanControlTab::thermalSourcesData() const
{
  return { m_sourceEditorModel.cbegin(), m_sourceEditorModel.cend() };
}

ucc::dbus::FanZoneDtoList FanControlTab::fanZonesData() const
{
  ucc::dbus::FanZoneDtoList out;
  for ( int row = 0; row < m_zoneCache.size(); ++row )
  {
    auto zone = m_zoneCache[row];
    if ( zone.id.isEmpty() )
      continue;

    if ( row >= 0 && row < m_zoneSourceCombos.size() && m_zoneSourceCombos[row] )
    {
      const QString tsId = m_zoneSourceCombos[row]->currentData().toString();
      if ( !tsId.isEmpty() )
        zone.thermalSourceId = tsId;
    }

    out.append( zone );
  }
  return out;
}

void FanControlTab::setThermalSourceForZone( const QString &zoneId, const QString &thermalSourceId )
{
  // Update per-zone curve tab combo
  auto it = m_thermalSourceCombos.find( zoneId );
  if ( it != m_thermalSourceCombos.end() && it.value() )
  {
    QComboBox *combo = it.value();
    for ( int i = 0; i < combo->count(); ++i )
    {
      if ( combo->itemData( i ).toString() == thermalSourceId )
      {
        combo->setCurrentIndex( i );
        break;
      }
    }
  }

  // Update zone table row combo
  for ( int row = 0; row < m_zoneCache.size(); ++row )
  {
    if ( m_zoneCache[row].id != zoneId )
      continue;
    if ( row < m_zoneSourceCombos.size() && m_zoneSourceCombos[row] )
    {
      QComboBox *combo = m_zoneSourceCombos[row];
      for ( int i = 0; i < combo->count(); ++i )
      {
        if ( combo->itemData( i ).toString() == thermalSourceId )
        {
          combo->setCurrentIndex( i );
          break;
        }
      }
    }
    break;
  }
}

void FanControlTab::pollSensorReadings()
{
  auto readings = m_uccdClient->getSensorReadings();
  if ( !readings )
    return;

  m_sensorReadings = *readings;
  updateSensorTreeValues();
  updateDeviceTreeValues();
  updateSourceTableValues();
  updateZoneTableValues();

  // Also poll zone telemetry
  auto telemetry = m_uccdClient->getFanZoneTelemetry();
  if ( telemetry )
  {
    m_zoneTelemetry = *telemetry;
    updateZoneTableValues();
  }
}

void FanControlTab::updateSensorTreeValues()
{
  if ( !m_sensorTree )
    return;

  QTreeWidgetItemIterator it( m_sensorTree );
  while ( *it )
  {
    QTreeWidgetItem *item = *it;
    const QString sid = item->data( 0, Qt::UserRole ).toString();
    if ( !sid.isEmpty() )
    {
      auto val = m_sensorReadings.value( sid );
      if ( val.isValid() && !val.isNull() )
        item->setText( 1, QString::number( val.toInt() ) );
      else
        item->setText( 1, QStringLiteral( "--" ) );
    }
    ++it;
  }
}

void FanControlTab::updateDeviceTreeValues()
{
  if ( !m_deviceTree )
    return;

  QTreeWidgetItemIterator it( m_deviceTree );
  while ( *it )
  {
    QTreeWidgetItem *item = *it;
    const QString did = item->data( 0, Qt::UserRole ).toString();
    if ( !did.isEmpty() )
    {
      const QString key = QStringLiteral( "fan:" ) + did;
      auto val = m_sensorReadings.value( key );
      if ( val.isValid() && !val.isNull() )
        item->setText( 1, QString::number( val.toInt() ) + QStringLiteral( " RPM" ) );
      else
        item->setText( 1, QStringLiteral( "--" ) );
    }
    ++it;
  }
}

void FanControlTab::updateSourceTableValues()
{
  if ( !m_sourceTable )
    return;

  for ( int row = 0; row < m_sourceEditorModel.size(); ++row )
  {
    const auto &src = m_sourceEditorModel[row];
    const QString key = QStringLiteral( "_source:" ) + src.id;

    auto *valueItem = m_sourceTable->item( row, 3 );
    if ( !valueItem )
    {
      valueItem = new QTableWidgetItem();
      valueItem->setFlags( valueItem->flags() & ~Qt::ItemIsEditable );
      valueItem->setTextAlignment( Qt::AlignCenter );
      m_sourceTable->setItem( row, 3, valueItem );
    }

    auto val = m_sensorReadings.value( key );
    if ( val.isValid() && !val.isNull() )
      valueItem->setText( QString::number( val.toInt() ) + QStringLiteral( " °C" ) );
    else
      valueItem->setText( QStringLiteral( "--" ) );
  }
}

  void FanControlTab::updateZoneTableValues()
  {
    if ( !m_zoneTable )
      return;

    for ( int row = 0; row < m_zoneCache.size(); ++row )
    {
      const auto &zone = m_zoneCache[row];
      const QString &zoneId = zone.id;

      // Temp column (col 4)
      auto *tempItem = m_zoneTable->item( row, 4 );
      if ( !tempItem )
      {
        tempItem = new QTableWidgetItem();
        tempItem->setFlags( tempItem->flags() & ~Qt::ItemIsEditable );
        tempItem->setTextAlignment( Qt::AlignCenter );
        m_zoneTable->setItem( row, 4, tempItem );
      }

      auto telemetry = m_zoneTelemetry.value( zoneId );
      if ( telemetry.canConvert< QVariantMap >() )
      {
        QVariantMap tobj = telemetry.toMap();
        int temp = tobj.value( QStringLiteral( "temp" ), -1 ).toInt();
        if ( temp >= 0 )
          tempItem->setText( QString::number( temp ) );
        else
          tempItem->setText( QStringLiteral( "--" ) );
      }
      else
      {
        tempItem->setText( QStringLiteral( "--" ) );
      }

      // Fan duty column (col 5)
      auto *dutyItem = m_zoneTable->item( row, 5 );
      if ( !dutyItem )
      {
        dutyItem = new QTableWidgetItem();
        dutyItem->setFlags( dutyItem->flags() & ~Qt::ItemIsEditable );
        dutyItem->setTextAlignment( Qt::AlignCenter );
        m_zoneTable->setItem( row, 5, dutyItem );
      }

      if ( telemetry.canConvert< QVariantMap >() )
      {
        const QVariantMap tobj = telemetry.toMap();
        int duty = tobj.value( QStringLiteral( "duty" ), -1 ).toInt();
        if ( duty >= 0 )
          dutyItem->setText( QString::number( duty ) + QStringLiteral( "%" ) );
        else
          dutyItem->setText( QStringLiteral( "--" ) );
      }
      else
      {
        dutyItem->setText( QStringLiteral( "--" ) );
      }
    }
  }

} // namespace ucc
