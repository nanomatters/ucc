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

#include "MainWindow.hpp"
#include "ProfileManager.hpp"
#include "SystemMonitor.hpp"

#include "FanControlTab.hpp"
#include "FanCurveEditorWidget.hpp"
#include "PumpCurveEditorWidget.hpp"
#include "../libucc-dbus/UccdClient.hpp"

#include "HardwareTab.hpp"

#include <QtWidgets/QTableWidget>
#include <QtWidgets/QHeaderView>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

// Helper widget for rotated y-axis label
class RotatedLabel : public QLabel
{
public:
  explicit RotatedLabel( const QString &text, QWidget *parent = nullptr )
    : QLabel( text, parent )
  {
  }

protected:
  void paintEvent( QPaintEvent *event ) override
  {
    ( void )event;
    QPainter p( this );
    p.setRenderHint( QPainter::Antialiasing );
    p.translate( width() / 2, height() / 2 );
    p.rotate( -90 );
    p.translate( -height() / 2, -width() / 2 );
    QRect r( 0, 0, height(), width() );
    p.setPen( QColor( "#bdbdbd" ) );
    QFont f = font();
    f.setPointSize( 11 );
    p.setFont( f );
    p.drawText( r, Qt::AlignCenter, "% Duty" );
  }

  QSize minimumSizeHint() const override
  {
    return QSize( 24, 80 );
  }

  QSize sizeHint() const override
  {
    return QSize( 24, 120 );
  }
};
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QTabWidget>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QTextEdit>
#include <QScrollArea>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QAction>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QPainter>

namespace ucc
{

MainWindow::MainWindow( QWidget *parent )
  : QMainWindow( parent )
  , m_profileManager( std::make_unique< ProfileManager >( this ) )
  , m_systemMonitor( std::make_unique< SystemMonitor >( this ) )
{
  m_UccdClient = std::make_unique< UccdClient >( this );

  // Query device capabilities from daemon
  if ( auto waterCooler = m_UccdClient->getWaterCoolerSupported() )
    m_waterCoolerSupported = *waterCooler;
  if ( auto ctgp = m_UccdClient->getCTGPAdjustmentSupported() )
    m_cTGPAdjustmentSupported = *ctgp;

  setWindowTitle( "Uniwill Control Center" );
  setGeometry( 100, 100, 900, 700 );

  setupUI();

  // Connect signals after UI elements are created but before loading data
  connectSignals();

  // Initialize status bar
  statusBar()->showMessage( "Ready" );

  // Load initial data — refresh() emits signals that populate the UI.
  // Block the profile combo to avoid cascading loadProfileDetails calls.
  m_profileCombo->blockSignals( true );
  m_profileManager->refresh();
  m_profileCombo->blockSignals( false );

  // Now populate the combo and load the active profile exactly once.
  onAllProfilesChanged();

  // Initialize current fan profile ID to first available fan profile (if any)
  m_currentFanProfile = ( m_fanControlTab && m_fanControlTab->fanProfileCombo() && m_fanControlTab->fanProfileCombo()->count() > 0 )
    ? m_fanControlTab->fanProfileCombo()->currentData().toString() : QString();

  // Startup complete — allow hardware interaction from now on
  m_initializing = false;

  // Start monitoring since dashboard is the first tab
  m_systemMonitor->setMonitoringActive( true );
}

MainWindow::~MainWindow()
{
  // Destructor
}

void MainWindow::setupUI()
{
  // Create tab widget
  m_tabs = new QTabWidget( this );
  setCentralWidget( m_tabs );

  // Connect tab changes to control monitoring
  connect( m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged );

  // Now create DashboardTab (daemon-backed water cooler; no controller pointer)
  m_dashboardTab = new DashboardTab( m_systemMonitor.get(), m_profileManager.get(), m_waterCoolerSupported, this );
  m_tabs->addTab( m_dashboardTab, "Dashboard" );
  setupProfilesPage();

  // Place the Fan Control tab directly after Profiles and rename it
  setupFanControlTab();
  setupKeyboardBacklightPage();
  setupHardwarePage();
}

void MainWindow::setupHardwarePage()
{
  //m_hardwareTab = new HardwareTab( m_systemMonitor.get(), this );
  //m_tabs->addTab( m_hardwareTab, "Hardware" );
}

void MainWindow::setupFanControlTab()
{
  m_fanControlTab = new FanControlTab( m_UccdClient.get(), m_profileManager.get(), m_waterCoolerSupported, this );
  connectFanControlTab();

  m_tabs->addTab( m_fanControlTab, "Profile Fan Control" );

  if ( m_fanControlTab->fanProfileCombo()->count() > 0 )
    onFanProfileChanged( m_fanControlTab->fanProfileCombo()->currentData().toString() );
}

void MainWindow::connectFanControlTab()
{
  connect( m_fanControlTab, &FanControlTab::fanProfileChanged,
           this, &MainWindow::onFanProfileChanged );
  connect( m_fanControlTab, &FanControlTab::cpuPointsChanged,
           this, &MainWindow::onCpuFanPointsChanged );
  connect( m_fanControlTab, &FanControlTab::gpuPointsChanged,
           this, &MainWindow::onGpuFanPointsChanged );
  connect( m_fanControlTab, &FanControlTab::wcFanPointsChanged,
           this, &MainWindow::onWaterCoolerFanPointsChanged );
  connect( m_fanControlTab, &FanControlTab::pumpPointsChanged,
           this, &MainWindow::onPumpPointsChanged );
  connect( m_fanControlTab, &FanControlTab::applyRequested,
           this, &MainWindow::onApplyFanProfilesClicked );
  connect( m_fanControlTab, &FanControlTab::saveRequested,
           this, &MainWindow::onSaveFanProfilesClicked );
  connect( m_fanControlTab, &FanControlTab::addRequested,
           this, &MainWindow::onAddFanProfileClicked );
  connect( m_fanControlTab, &FanControlTab::copyRequested,
           this, &MainWindow::onCopyFanProfileClicked );

  // Bidirectional water-cooler enable checkbox sync
  // FanControlTab toggle → D-Bus + sync dashboard checkbox
  connect( m_fanControlTab, &FanControlTab::waterCoolerEnableChanged,
           m_dashboardTab, &DashboardTab::setWaterCoolerEnabled );
  // DashboardTab toggle → D-Bus call + sync fan tab checkbox
  connect( m_dashboardTab, &DashboardTab::waterCoolerEnableChanged,
           this, [this]( bool enabled ) {
             m_fanControlTab->setWaterCoolerEnabled( enabled );
             m_fanControlTab->sendWaterCoolerEnable( enabled );
           } );
  connect( m_fanControlTab, &FanControlTab::removeRequested,
           this, &MainWindow::onRemoveFanProfileClicked );

  // Sync fan profile rename from fan tab to profile page fan combo
  connect( m_fanControlTab, &FanControlTab::fanProfileRenamed,
           this, [this]( const QString &oldName, const QString &newName ) {
    if ( m_profileFanProfileCombo ) {
      int idx = m_profileFanProfileCombo->findText( oldName );
      if ( idx != -1 )
        m_profileFanProfileCombo->setItemText( idx, newName );
    }
  } );
}



void MainWindow::setupProfilesPage()
{
  QWidget *profilesWidget = new QWidget();
  QVBoxLayout *mainLayout = new QVBoxLayout( profilesWidget );
  mainLayout->setContentsMargins( 0, 0, 0, 0 );
  mainLayout->setSpacing( 0 );

  // Create scroll area for the profile content
  QScrollArea *scrollArea = new QScrollArea();
  scrollArea->setWidgetResizable( true );

  QWidget *scrollWidget = new QWidget();
  QVBoxLayout *scrollLayout = new QVBoxLayout( scrollWidget );
  scrollLayout->setContentsMargins( 20, 20, 20, 20 );
  scrollLayout->setSpacing( 15 );

  // Profile Selection ComboBox (in top layout)
  QHBoxLayout *selectLayout = new QHBoxLayout();
  QLabel *selectLabel = new QLabel( "Active Profile:" );
  selectLabel->setStyleSheet( "font-weight: bold;" );

  m_profileCombo = new QComboBox();
  m_profileCombo->setEditable( true );
  m_profileCombo->setInsertPolicy( QComboBox::NoInsert );
  // Don't populate here - will be done by onAllProfilesChanged signal
  m_profileCombo->setCurrentIndex( m_profileManager->activeProfileIndex() );
  m_selectedProfileIndex = m_profileManager->activeProfileIndex();
  m_applyButton = new QPushButton( "Apply" );
  m_applyButton->setMaximumWidth( 80 );

  m_saveButton = new QPushButton( "Save" );
  m_saveButton->setMaximumWidth( 80 );
  m_saveButton->setEnabled( false );

  m_copyProfileButton = new QPushButton( "Copy" );
  m_copyProfileButton->setMaximumWidth( 60 );

  m_removeProfileButton = new QPushButton( "Remove" );
  m_removeProfileButton->setMaximumWidth( 70 );

  selectLayout->addWidget( selectLabel );
  selectLayout->addWidget( m_profileCombo, 1 );
  selectLayout->addWidget( m_applyButton );
  selectLayout->addWidget( m_saveButton );
  selectLayout->addWidget( m_copyProfileButton );
  selectLayout->addWidget( m_removeProfileButton );
  mainLayout->addLayout( selectLayout );

  // Add a separator line
  QFrame *separator = new QFrame();
  separator->setFrameShape( QFrame::HLine );
  separator->setStyleSheet( "color: #cccccc;" );
  mainLayout->addWidget( separator );

  // Now use grid layout for the details
  scrollLayout->setContentsMargins( 15, 10, 15, 10 );
  QGridLayout *detailsLayout = new QGridLayout();
  detailsLayout->setSpacing( 12 );
  detailsLayout->setColumnStretch( 0, 0 );  // Labels column - minimal width
  detailsLayout->setColumnStretch( 1, 1 );  // Controls column - expand

  int row = 0;

  // === DESCRIPTION ===
  QLabel *descLabel = new QLabel( "Description" );
  descLabel->setStyleSheet( "font-weight: bold;" );
  m_descriptionEdit = new QTextEdit();
  m_descriptionEdit->setPlainText( "Edit profile to change behaviour" );
  m_descriptionEdit->setMaximumHeight( 60 );
  detailsLayout->addWidget( descLabel, row, 0, Qt::AlignTop );
  detailsLayout->addWidget( m_descriptionEdit, row, 1 );
  row++;

  // === ACTIVATE PROFILE AUTOMATICALLY ON ===
  QLabel *autoActivateLabel = new QLabel( "Activate profile automatically on" );
  autoActivateLabel->setStyleSheet( "font-weight: bold;" );
  QHBoxLayout *buttonLayout = new QHBoxLayout();
  m_mainsButton = new QPushButton( "Mains" );
  m_mainsButton->setCheckable( true );
  m_batteryButton = new QPushButton( "Battery" );
  m_batteryButton->setCheckable( true );
  m_waterCoolerButton = new QPushButton( "Water Cooler" );
  m_waterCoolerButton->setCheckable( true );
  m_waterCoolerButton->setCheckable( true );
  m_mainsButton->setMaximumWidth( 100 );
  m_batteryButton->setMaximumWidth( 100 );
  m_waterCoolerButton->setMaximumWidth( 100 );
  buttonLayout->addWidget( m_mainsButton );
  buttonLayout->addWidget( m_batteryButton );
  buttonLayout->addWidget( m_waterCoolerButton );
  buttonLayout->addStretch();
  detailsLayout->addWidget( autoActivateLabel, row, 0, Qt::AlignTop );
  detailsLayout->addLayout( buttonLayout, row, 1 );
  row++;

  // Hide water cooler profile activation button if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    m_waterCoolerButton->setVisible( false );
  }

  // Add spacer/separator
  detailsLayout->addItem( new QSpacerItem( 0, 15 ), row, 0, 1, 2 );
  row++;

  // === CHARGING SECTION (visible only when hardware supports it) ===
  QLabel *chargingHeader = new QLabel( "Charging" );
  chargingHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( chargingHeader, row, 0, 1, 2 );
  row++;

  QLabel *chargingProfileLabel = new QLabel( "Charging profile" );
  m_profileChargingProfileCombo = new QComboBox();
  detailsLayout->addWidget( chargingProfileLabel, row, 0 );
  detailsLayout->addWidget( m_profileChargingProfileCombo, row, 1 );
  row++;

  QLabel *chargingPriorityLabel = new QLabel( "Charging priority" );
  m_profileChargingPriorityCombo = new QComboBox();
  detailsLayout->addWidget( chargingPriorityLabel, row, 0 );
  detailsLayout->addWidget( m_profileChargingPriorityCombo, row, 1 );
  row++;

  QLabel *chargeLimitLabel = new QLabel( "Charge limit" );
  m_profileChargeLimitCombo = new QComboBox();
  m_profileChargeLimitCombo->addItem( "Full Capacity (100%)", "full" );
  m_profileChargeLimitCombo->addItem( "Reduced (~90%)", "reduced" );
  m_profileChargeLimitCombo->addItem( "Stationary (~80%)", "stationary" );
  detailsLayout->addWidget( chargeLimitLabel, row, 0 );
  detailsLayout->addWidget( m_profileChargeLimitCombo, row, 1 );
  row++;

  auto setChargingSectionVisible = [chargingHeader, chargingProfileLabel,
                                    chargingPriorityLabel, chargeLimitLabel, this]( bool visible )
  {
    chargingHeader->setVisible( visible );
    chargingProfileLabel->setVisible( visible );
    m_profileChargingProfileCombo->setVisible( visible );

    // Charging priority is only visible if priorities are available
    bool hasPriorities = m_systemMonitor && !m_systemMonitor->chargingPrioritiesAvailable().isEmpty();
    chargingPriorityLabel->setVisible( visible && hasPriorities );
    m_profileChargingPriorityCombo->setVisible( visible && hasPriorities );

    // Charge limit is visible when thresholds are available
    bool hasThresholds = m_systemMonitor && m_systemMonitor->chargeThresholdsAvailable();
    chargeLimitLabel->setVisible( visible && hasThresholds );
    m_profileChargeLimitCombo->setVisible( visible && hasThresholds );
  };

  // Populate and show/hide based on hardware support
  if ( m_systemMonitor )
  {
    auto populateChargingProfiles = [this, setChargingSectionVisible]()
    {
      const QStringList profiles = m_systemMonitor->chargingProfilesAvailable();
      m_profileChargingProfileCombo->blockSignals( true );
      m_profileChargingProfileCombo->clear();

      static const QMap< QString, QString > displayNames = {
          { "high_capacity", "High Capacity (Full charge)" },
          { "balanced", "Balanced (~90%)" },
          { "stationary", "Stationary (~80%)" } };

      for ( const auto &p : profiles )
      {
        QString display = displayNames.value( p, p );
        m_profileChargingProfileCombo->addItem( display, p );
      }
      m_profileChargingProfileCombo->blockSignals( false );
      setChargingSectionVisible( !profiles.isEmpty() );
    };

    auto populateChargingPriorities = [this, setChargingSectionVisible]()
    {
      const QStringList priorities = m_systemMonitor->chargingPrioritiesAvailable();
      m_profileChargingPriorityCombo->blockSignals( true );
      m_profileChargingPriorityCombo->clear();

      static const QMap< QString, QString > priorityDisplayNames = {
          { "charge_battery", "Charge Battery" },
          { "performance", "Performance" } };

      for ( const auto &p : priorities )
      {
        QString display = priorityDisplayNames.value( p, p );
        m_profileChargingPriorityCombo->addItem( display, p );
      }
      m_profileChargingPriorityCombo->blockSignals( false );

      // Re-evaluate visibility since priority availability changed
      bool hasProfiles = !m_systemMonitor->chargingProfilesAvailable().isEmpty();
      setChargingSectionVisible( hasProfiles );
    };

    populateChargingProfiles();
    populateChargingPriorities();
    connect( m_systemMonitor.get(), &SystemMonitor::chargingProfilesAvailableChanged, this,
             populateChargingProfiles );
    connect( m_systemMonitor.get(), &SystemMonitor::chargingPrioritiesAvailableChanged, this,
             populateChargingPriorities );
    connect( m_systemMonitor.get(), &SystemMonitor::chargeThresholdsAvailableChanged, this,
             [this, setChargingSectionVisible]()
             {
               bool hasProfiles = !m_systemMonitor->chargingProfilesAvailable().isEmpty();
               setChargingSectionVisible( hasProfiles );
             } );
  }
  else
  {
    setChargingSectionVisible( false );
  }

  // Add spacer before Display section
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  // === DISPLAY SECTION ===
  QLabel *displayHeader = new QLabel( "Display and Keyboard" );
  displayHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( displayHeader, row, 0, 1, 2 );
  row++;

  QLabel *keyboardProfileLabel = new QLabel( "Keyboard profile" );
  m_profileKeyboardProfileCombo = new QComboBox();

  for ( const auto &v : m_profileManager->customKeyboardProfilesData() )
  {
    QJsonObject o = v.toObject();
    m_profileKeyboardProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
  }

  detailsLayout->addWidget( keyboardProfileLabel, row, 0 );
  detailsLayout->addWidget( m_profileKeyboardProfileCombo, row, 1 );
  row++;

  QLabel *backlightLabel = new QLabel( "Backlight brightness" );
  QHBoxLayout *backlightLayout = new QHBoxLayout();
  m_brightnessSlider = new QSlider( Qt::Horizontal );
  m_brightnessSlider->setMinimum( 0 );
  m_brightnessSlider->setMaximum( 100 );
  m_brightnessSlider->setValue( 100 );
  m_brightnessValueLabel = new QLabel( "100%" );
  m_brightnessValueLabel->setMinimumWidth( 40 );
  backlightLayout->addWidget( m_brightnessSlider, 1 );
  backlightLayout->addWidget( m_brightnessValueLabel );
  detailsLayout->addWidget( backlightLabel, row, 0 );
  detailsLayout->addLayout( backlightLayout, row, 1 );
  row++;

  QLabel *setBrightnessLabel = new QLabel( "Set brightness on profile activation" );
  m_setBrightnessCheckBox = new QCheckBox();
  m_setBrightnessCheckBox->setChecked( false );
  detailsLayout->addWidget( setBrightnessLabel, row, 0 );
  detailsLayout->addWidget( m_setBrightnessCheckBox, row, 1, Qt::AlignLeft );
  row++;

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  // === FAN CONTROL SECTION ===
  QLabel *fanHeader = new QLabel( "Fan control" );
  fanHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( fanHeader, row, 0, 1, 2 );
  row++;

  QLabel *fanProfileLabel = new QLabel( "Fan profile" );
  m_profileFanProfileCombo = new QComboBox();
  // Add built-in fan profiles from daemon (id + name)
  for ( const auto &v : m_profileManager->builtinFanProfilesData() )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      m_profileFanProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
    }
  }
  // Append persisted custom fan profiles loaded from settings
  for ( const auto &v : m_profileManager->customFanProfilesData() )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      QString name = o["name"].toString();
      if ( m_profileFanProfileCombo->findText( name ) == -1 )
        m_profileFanProfileCombo->addItem( name, o["id"].toString() );
    }
  }
  detailsLayout->addWidget( fanProfileLabel, row, 0 );
  detailsLayout->addWidget( m_profileFanProfileCombo, row, 1 );
  row++;

  QLabel *offsetFanLabel = new QLabel( "Offset fan speed" );
  QHBoxLayout *offsetFanLayout = new QHBoxLayout();
  m_offsetFanSpeedSlider = new QSlider( Qt::Horizontal );
  m_offsetFanSpeedSlider->setMinimum( -30 );
  m_offsetFanSpeedSlider->setMaximum( 30 );
  m_offsetFanSpeedSlider->setValue( -2 );
  m_offsetFanSpeedValue = new QLabel( "-2%" );
  m_offsetFanSpeedValue->setMinimumWidth( 40 );
  offsetFanLayout->addWidget( m_offsetFanSpeedSlider, 1 );
  offsetFanLayout->addWidget( m_offsetFanSpeedValue );
  detailsLayout->addWidget( offsetFanLabel, row, 0 );
  detailsLayout->addLayout( offsetFanLayout, row, 1 );
  row++;

  QLabel *sameSpeedLabel = new QLabel( "Same fan speed for all fans" );
  detailsLayout->addWidget( sameSpeedLabel, row, 0 );
  // Reuse the shared checkbox created in the dashboard (create if not present)
  if ( !m_sameFanSpeedCheckBox ) {
    m_sameFanSpeedCheckBox = new QCheckBox();
    m_sameFanSpeedCheckBox->setChecked( true );
  }
  detailsLayout->addWidget( m_sameFanSpeedCheckBox, row, 1, Qt::AlignLeft );
  row++;

  QLabel *autoWaterLabel = new QLabel( "Water cooler auto control" );
  m_autoWaterControlCheckBox = new QCheckBox();
  m_autoWaterControlCheckBox->setChecked( true );
  m_autoWaterControlCheckBox->setToolTip( tr( "When enabled the daemon will control the water cooler automatically" ) );
  detailsLayout->addWidget( autoWaterLabel, row, 0 );
  detailsLayout->addWidget( m_autoWaterControlCheckBox, row, 1, Qt::AlignLeft );
  row++;

  // Hide water cooler auto control if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    autoWaterLabel->setVisible( false );
    m_autoWaterControlCheckBox->setVisible( false );
  }

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  // === SYSTEM PERFORMANCE SECTION ===
  QLabel *sysHeader = new QLabel( "System performance" );
  sysHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( sysHeader, row, 0, 1, 2 );
  row++;

  QLabel *odmPowerHeader = new QLabel( "CPU power limit control" );
  detailsLayout->addWidget( odmPowerHeader, row, 0, 1, 2 );
  row++;

  // TDP Limit 1
  QLabel *tdp1Label = new QLabel( "Sustained TDP" );  // Sustained Power Limit
  QHBoxLayout *tdp1Layout = new QHBoxLayout();
  m_odmPowerLimit1Slider = new QSlider( Qt::Horizontal );
  m_odmPowerLimit1Slider->setMinimum( 0 );
  m_odmPowerLimit1Slider->setMaximum( 250 );  // Will be updated from hardware limits in loadProfileDetails
  m_odmPowerLimit1Slider->setValue( 0 );
  m_odmPowerLimit1Value = new QLabel( "0 W" );
  m_odmPowerLimit1Value->setMinimumWidth( 50 );
  tdp1Layout->addWidget( m_odmPowerLimit1Slider, 1 );
  tdp1Layout->addWidget( m_odmPowerLimit1Value );
  detailsLayout->addWidget( tdp1Label, row, 0 );
  detailsLayout->addLayout( tdp1Layout, row, 1 );
  row++;

  // TDP Limit 2
  QLabel *tdp2Label = new QLabel( "Boost TDP" );
  QHBoxLayout *tdp2Layout = new QHBoxLayout();
  m_odmPowerLimit2Slider = new QSlider( Qt::Horizontal );
  m_odmPowerLimit2Slider->setMinimum( 0 );
  m_odmPowerLimit2Slider->setMaximum( 250 );  // Will be updated from hardware limits in loadProfileDetails
  m_odmPowerLimit2Slider->setValue( 0 );
  m_odmPowerLimit2Value = new QLabel( "0 W" );
  m_odmPowerLimit2Value->setMinimumWidth( 50 );
  tdp2Layout->addWidget( m_odmPowerLimit2Slider, 1 );
  tdp2Layout->addWidget( m_odmPowerLimit2Value );
  detailsLayout->addWidget( tdp2Label, row, 0 );
  detailsLayout->addLayout( tdp2Layout, row, 1 );
  row++;

  // TDP Limit 3
  QLabel *tdp3Label = new QLabel( "Peak TDP" );
  QHBoxLayout *tdp3Layout = new QHBoxLayout();
  m_odmPowerLimit3Slider = new QSlider( Qt::Horizontal );
  m_odmPowerLimit3Slider->setMinimum( 0 );
  m_odmPowerLimit3Slider->setMaximum( 250 );  // Will be updated from hardware limits in loadProfileDetails
  m_odmPowerLimit3Slider->setValue( 0 );
  m_odmPowerLimit3Value = new QLabel( "0 W" );
  m_odmPowerLimit3Value->setMinimumWidth( 50 );
  tdp3Layout->addWidget( m_odmPowerLimit3Slider, 1 );
  tdp3Layout->addWidget( m_odmPowerLimit3Value );
  detailsLayout->addWidget( tdp3Label, row, 0 );
  detailsLayout->addLayout( tdp3Layout, row, 1 );
  row++;

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 5 ), row, 0, 1, 2 );
  row++;

  QLabel *cpuFreqHeader = new QLabel( "CPU frequency control" );
  detailsLayout->addWidget( cpuFreqHeader, row, 0, 1, 2 );
  row++;

  QLabel *coresLabel = new QLabel( "Number of logical cores" );
  QHBoxLayout *coresLayout = new QHBoxLayout();
  m_cpuCoresSlider = new QSlider( Qt::Horizontal );
  m_cpuCoresSlider->setMinimum( 1 );
  m_cpuCoresSlider->setMaximum( 32 );
  m_cpuCoresSlider->setValue( 32 );
  m_cpuCoresValue = new QLabel( "32" );
  m_cpuCoresValue->setMinimumWidth( 35 );
  coresLayout->addWidget( m_cpuCoresSlider, 1 );
  coresLayout->addWidget( m_cpuCoresValue );
  detailsLayout->addWidget( coresLabel, row, 0 );
  detailsLayout->addLayout( coresLayout, row, 1 );
  row++;

  QLabel *maxPerfLabel = new QLabel( "CPU Governor" );
  m_governorCombo = new QComboBox();
  m_governorCombo->addItem( "powersave", "powersave" );
  m_governorCombo->addItem( "performance", "performance" );
  detailsLayout->addWidget( maxPerfLabel, row, 0 );
  detailsLayout->addWidget( m_governorCombo, row, 1, Qt::AlignLeft );
  row++;

  QLabel *minFreqLabel = new QLabel( "Minimum frequency" );
  QHBoxLayout *minFreqLayout = new QHBoxLayout();
  m_minFrequencySlider = new QSlider( Qt::Horizontal );
  m_minFrequencySlider->setSingleStep( 100 ); // 100 MHz steps

  // Get hardware frequency limits and initialize slider with actual values
  int minFreqMHz = 400;  // fallback
  int maxFreqMHz = 6000; // fallback
  if ( auto limitsJson = m_UccdClient->getCpuFrequencyLimitsJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( limitsJson->c_str() );
    if ( doc.isObject() )
    {
      QJsonObject limitsObj = doc.object();
      int minFreqKHz = limitsObj["min"].toInt( 400000 );
      int maxFreqKHz = limitsObj["max"].toInt( 6000000 );
      minFreqMHz = minFreqKHz / 1000;
      maxFreqMHz = maxFreqKHz / 1000;
    }
  }
  m_minFrequencySlider->setMinimum( minFreqMHz );
  m_minFrequencySlider->setMaximum( maxFreqMHz );
  m_minFrequencySlider->setValue( minFreqMHz );

  m_minFrequencyValue = new QLabel();
  m_minFrequencyValue->setMinimumWidth( 60 );
  double freqGHz = minFreqMHz / 1000.0;
  m_minFrequencyValue->setText( QString::number( freqGHz, 'f', 2 ) + " GHz" );

  minFreqLayout->addWidget( m_minFrequencySlider, 1 );
  minFreqLayout->addWidget( m_minFrequencyValue );
  detailsLayout->addWidget( minFreqLabel, row, 0 );
  detailsLayout->addLayout( minFreqLayout, row, 1 );
  row++;

  QLabel *maxFreqLabel = new QLabel( "Maximum frequency" );
  QHBoxLayout *maxFreqLayout = new QHBoxLayout();
  m_maxFrequencySlider = new QSlider( Qt::Horizontal );
  m_maxFrequencySlider->setSingleStep( 100 ); // 100 MHz steps
  m_maxFrequencySlider->setMinimum( minFreqMHz );
  m_maxFrequencySlider->setMaximum( maxFreqMHz );
  m_maxFrequencySlider->setValue( maxFreqMHz );

  m_maxFrequencyValue = new QLabel();
  m_maxFrequencyValue->setMinimumWidth( 60 );
  freqGHz = maxFreqMHz / 1000.0;
  m_maxFrequencyValue->setText( QString::number( freqGHz, 'f', 2 ) + " GHz" );
  maxFreqLayout->addWidget( m_maxFrequencySlider, 1 );
  maxFreqLayout->addWidget( m_maxFrequencyValue );
  detailsLayout->addWidget( maxFreqLabel, row, 0 );
  detailsLayout->addLayout( maxFreqLayout, row, 1 );
  row++;

  // Add spacer
  detailsLayout->addItem( new QSpacerItem( 0, 10 ), row, 0, 1, 2 );
  row++;

  // === NVIDIA POWER CONTROL ===
  QLabel *nvidiaHeader = new QLabel( "NVIDIA power control" );
  nvidiaHeader->setStyleSheet( "font-weight: bold; font-size: 14px;" );
  detailsLayout->addWidget( nvidiaHeader, row, 0, 1, 2 );
  row++;

  QLabel *gpuPowerLabel = new QLabel( "Configurable graphics power (TGP)" );
  QHBoxLayout *gpuLayout = new QHBoxLayout();
  m_gpuPowerSlider = new QSlider( Qt::Horizontal );
  m_gpuPowerSlider->setMinimum( 40 );
  m_gpuPowerSlider->setMaximum( 175 );
  m_gpuPowerSlider->setValue( 175 );
  m_gpuPowerValue = new QLabel( "175 W" );
  m_gpuPowerValue->setMinimumWidth( 50 );
  gpuLayout->addWidget( m_gpuPowerSlider, 1 );
  gpuLayout->addWidget( m_gpuPowerValue );
  detailsLayout->addWidget( gpuPowerLabel, row, 0 );
  detailsLayout->addLayout( gpuLayout, row, 1 );
  row++;

  // Hide cTGP section if device does not support it
  if ( !m_cTGPAdjustmentSupported )
  {
    nvidiaHeader->setVisible( false );
    gpuPowerLabel->setVisible( false );
    m_gpuPowerSlider->setVisible( false );
    m_gpuPowerValue->setVisible( false );
  }

  detailsLayout->addItem( new QSpacerItem( 0, 20, QSizePolicy::Minimum, QSizePolicy::Expanding ), row, 0, 1, 2 );

  scrollLayout->addLayout( detailsLayout );

  scrollArea->setWidget( scrollWidget );
  mainLayout->addWidget( scrollArea );

  m_tabs->addTab( profilesWidget, "Profiles" );
}

void MainWindow::connectSignals()
{
  // Profile page connections

  connect( m_profileManager.get(), &ProfileManager::allProfilesChanged,
           this, &MainWindow::onAllProfilesChanged );

  connect( m_profileManager.get(), &ProfileManager::activeProfileIndexChanged,
           this, &MainWindow::onActiveProfileIndexChanged );

  connect( m_profileManager.get(), &ProfileManager::activeProfileChanged,
           this, [this]() {
    if ( m_initializing ) return;  // defer to onAllProfilesChanged after init
    qDebug() << "activeProfileChanged signal received, updating UI";
    loadProfileDetails( m_profileManager->activeProfileId() );
  } );

  connect( m_profileManager.get(), &ProfileManager::customKeyboardProfilesChanged,
           this, &MainWindow::onCustomKeyboardProfilesChanged );

  connect( m_profileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, &MainWindow::onProfileIndexChanged );

  // Display controls

  connect( m_brightnessSlider, &QSlider::valueChanged,
           this, &MainWindow::onBrightnessSliderChanged );

  // Fan controls

  connect( m_offsetFanSpeedSlider, &QSlider::valueChanged,
           this, &MainWindow::onOffsetFanSpeedChanged );

  // ODM Power Limit controls

  connect( m_odmPowerLimit1Slider, &QSlider::valueChanged,
           this, &MainWindow::onODMPowerLimit1Changed );

  connect( m_odmPowerLimit2Slider, &QSlider::valueChanged,
           this, &MainWindow::onODMPowerLimit2Changed );

  connect( m_odmPowerLimit3Slider, &QSlider::valueChanged,
           this, &MainWindow::onODMPowerLimit3Changed );

  // CPU frequency controls

  connect( m_cpuCoresSlider, &QSlider::valueChanged,
           this, &MainWindow::onCpuCoresChanged );

  connect( m_maxFrequencySlider, &QSlider::valueChanged,
           this, &MainWindow::onMaxFrequencyChanged );

  connect( m_minFrequencySlider, &QSlider::valueChanged,
           this, [this]( int value ) {
    double freqGHz = value / 1000.0;  // Convert MHz to GHz for display
    m_minFrequencyValue->setText( QString::number( freqGHz, 'f', 2 ) + " GHz" );
  } );

  // Snap to nearest available frequency when user releases slider
  connect( m_minFrequencySlider, &QSlider::sliderReleased,
           this, [this]() {
    int currentValue = m_minFrequencySlider->value();
    if ( int snapped = snapToAvailableFrequency( currentValue ); snapped != currentValue )
      m_minFrequencySlider->setValue( snapped );
  } );

  connect( m_maxFrequencySlider, &QSlider::sliderReleased,
           this, [this]() {
    int currentValue = m_maxFrequencySlider->value();
    if ( int snapped = snapToAvailableFrequency( currentValue ); snapped != currentValue )
      m_maxFrequencySlider->setValue( snapped );
  } );

  // Enforce min <= max for frequency sliders
  connect( m_minFrequencySlider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value > m_maxFrequencySlider->value() )
      m_maxFrequencySlider->setValue( value );
  } );

  connect( m_maxFrequencySlider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value < m_minFrequencySlider->value() )
      m_minFrequencySlider->setValue( value );
  } );

  // Enforce TDP ordering: sustained <= boost <= peak
  connect( m_odmPowerLimit1Slider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value > m_odmPowerLimit2Slider->value() )
      m_odmPowerLimit2Slider->setValue( value );
  } );

  connect( m_odmPowerLimit2Slider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value < m_odmPowerLimit1Slider->value() )
      m_odmPowerLimit1Slider->setValue( value );
    if ( value > m_odmPowerLimit3Slider->value() )
      m_odmPowerLimit3Slider->setValue( value );
  } );

  connect( m_odmPowerLimit3Slider, &QSlider::valueChanged,
           this, [this]( int value ) {
    if ( value < m_odmPowerLimit2Slider->value() )
      m_odmPowerLimit2Slider->setValue( value );
  } );

  // GPU power control

  connect( m_gpuPowerSlider, &QSlider::valueChanged,
           this, &MainWindow::onGpuPowerChanged );

  // Apply and Save buttons

  connect( m_applyButton, &QPushButton::clicked,
           this, &MainWindow::onApplyClicked );

  connect( m_saveButton, &QPushButton::clicked,
           this, &MainWindow::onSaveClicked );

  connect( m_copyProfileButton, &QPushButton::clicked,
           this, &MainWindow::onCopyProfileClicked );

  connect( m_removeProfileButton, &QPushButton::clicked,
           this, &MainWindow::onRemoveProfileClicked );

  // Connect all profile controls to mark changes

  connect( m_descriptionEdit, &QTextEdit::textChanged,
           this, &MainWindow::markChanged );

  // Profile combo rename handling
  connect( m_profileCombo->lineEdit(), &QLineEdit::editingFinished,
           this, &MainWindow::onProfileComboRenamed );

  connect( m_setBrightnessCheckBox, &QCheckBox::toggled,
           this, &MainWindow::markChanged );

  connect( m_brightnessSlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_profileFanProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, [this](int index) {
             markChanged();
             // Update fan profile tab to match profile tab selection
             m_fanControlTab->fanProfileCombo()->blockSignals(true);
             m_fanControlTab->fanProfileCombo()->setCurrentIndex(index);
             m_fanControlTab->fanProfileCombo()->blockSignals(false);
             // Load the fan curves for the new profile
             onFanProfileChanged(m_profileFanProfileCombo->currentData().toString());
           } );

  connect( m_offsetFanSpeedSlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_autoWaterControlCheckBox, &QCheckBox::toggled,
           this, &MainWindow::markChanged );

  connect( m_cpuCoresSlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_governorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
           this, &MainWindow::markChanged );

  connect( m_minFrequencySlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_maxFrequencySlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_odmPowerLimit1Slider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_odmPowerLimit2Slider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_odmPowerLimit3Slider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_gpuPowerSlider, &QSlider::valueChanged,
           this, [this]() { markChanged(); } );

  connect( m_profileKeyboardProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, &MainWindow::markChanged );

  if ( m_profileChargingProfileCombo )
  {
    connect( m_profileChargingProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &MainWindow::markChanged );
  }
  if ( m_profileChargingPriorityCombo )
  {
    connect( m_profileChargingPriorityCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &MainWindow::markChanged );
  }
  if ( m_profileChargeLimitCombo )
  {
    connect( m_profileChargeLimitCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
             this, &MainWindow::markChanged );
  }

  // Mains/Battery/Water Cooler activation buttons
  connect( m_mainsButton, &QPushButton::toggled,
           this, &MainWindow::markChanged );

  connect( m_batteryButton, &QPushButton::toggled,
           this, &MainWindow::markChanged );

  connect( m_waterCoolerButton, &QPushButton::toggled,
           this, &MainWindow::markChanged );

  // Error handling

  connect( m_profileManager.get(), QOverload< const QString & >::of( &ProfileManager::error ),
           this, [this]( const QString &msg ) {
    statusBar()->showMessage( "Error: " + msg );
    m_saveInProgress = false;
    updateButtonStates();
  } );

  // React to DBus client connection status so we can (re)load built-in fan profiles
  connect( m_UccdClient.get(), &UccdClient::connectionStatusChanged,
           this, &MainWindow::onUccdConnectionChanged );

  connectKeyboardBacklightPageWidgets();

  // Initial load of fan profiles (may be empty if service not yet available)
  reloadFanProfiles();

  // Populate governor combo
  populateGovernorCombo();
}

void MainWindow::populateGovernorCombo()
{
  if ( !m_governorCombo )
    return;

  m_governorCombo->clear();

  if ( auto governors = m_UccdClient->getAvailableCpuGovernors(); governors && !governors->empty() )
  {
    for ( const auto &gov : *governors )
      m_governorCombo->addItem( QString::fromStdString( gov ), QString::fromStdString( gov ) );
  }
}

void MainWindow::onTabChanged( int index )
{
  // Enable monitoring only when dashboard tab (index 0) is visible
  bool isDashboard = ( index == 0 );
  qDebug() << "Tab changed to" << index << "- Monitoring active:" << isDashboard;
  m_systemMonitor->setMonitoringActive( isDashboard );

  // Load current keyboard backlight states when keyboard tab (index 4) is activated
  if ( index == 4 )
  {
    if ( m_keyboardVisualizer )
    {
      if ( auto states = m_UccdClient->getKeyboardBacklightStates() )
      {
        m_keyboardVisualizer->loadCurrentStates( *states );
        qDebug() << "Loaded current keyboard backlight states";
      }

      // Apply current brightness to visualizer
      if ( m_keyboardBrightnessSlider )
      {
        m_keyboardVisualizer->setGlobalBrightness( m_keyboardBrightnessSlider->value() );
      }
    }

    // Reload keyboard profiles
    reloadKeyboardProfiles();

    // Auto-load the keyboard profile from the active profile's settings
    QString activeProfileId = m_profileManager->activeProfileId();
    if ( !activeProfileId.isEmpty() )
    {
      QString profileJson = m_profileManager->getProfileDetails( activeProfileId );
      if ( !profileJson.isEmpty() )
      {
        QJsonDocument doc = QJsonDocument::fromJson( profileJson.toUtf8() );
        if ( doc.isObject() )
        {
          QJsonObject obj = doc.object();
          // Check for embedded keyboard profile name
          if ( obj.contains( "selectedKeyboardProfile" ) )
          {
            QString keyboardProfileId = obj["selectedKeyboardProfile"].toString();
            // Find by ID in combo userData
            int kbIdx = -1;
            for ( int i = 0; i < m_keyboardProfileCombo->count(); ++i )
            {
              if ( m_keyboardProfileCombo->itemData( i ).toString() == keyboardProfileId )
              { kbIdx = i; break; }
            }
            // Fallback: try matching by name (legacy data)
            if ( kbIdx < 0 )
              kbIdx = m_keyboardProfileCombo->findText( keyboardProfileId );
            if ( kbIdx >= 0 )
            {
              m_keyboardProfileCombo->setCurrentIndex( kbIdx );
              // This triggers onKeyboardProfileChanged to load the profile data
            }
          }
          // Fallback: check for old format keyboard.profile field
          else if ( obj.contains( "keyboard" ) && obj["keyboard"].isObject() )
          {
            QJsonObject keyboardObj = obj["keyboard"].toObject();
            QString keyboardProfile = keyboardObj["profile"].toString();
            int kbIdx = -1;
            for ( int i = 0; i < m_keyboardProfileCombo->count(); ++i )
            {
              if ( m_keyboardProfileCombo->itemData( i ).toString() == keyboardProfile )
              { kbIdx = i; break; }
            }
            if ( kbIdx < 0 )
              kbIdx = m_keyboardProfileCombo->findText( keyboardProfile );
            if ( kbIdx >= 0 )
            {
              m_keyboardProfileCombo->setCurrentIndex( kbIdx );
              // This triggers onKeyboardProfileChanged to load the profile data
            }
          }
        }
      }
    }
  }
}

// Profile page slots
void MainWindow::onCustomKeyboardProfilesChanged()
{
  // Repopulate keyboard profile combos with ID userData
  m_profileKeyboardProfileCombo->clear();
  for ( const auto &v : m_profileManager->customKeyboardProfilesData() )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      m_profileKeyboardProfileCombo->addItem( o["name"].toString(), o["id"].toString() );
    }
  }

  reloadKeyboardProfiles();
}

void MainWindow::onProfileIndexChanged( int index )
{
  if ( index >= 0 )
  {
    QString profileName = m_profileCombo->currentText();
    QString profileId = m_profileCombo->currentData().toString();
    qDebug() << "Profile selected:" << profileName << "at index" << index;
    m_selectedProfileIndex = index;
    loadProfileDetails( profileId );
    m_removeProfileButton->setEnabled( m_profileManager->isCustomProfile( profileId ) );
    m_copyProfileButton->setEnabled( true );
    m_saveButton->setEnabled( true );
    statusBar()->showMessage( "Profile selected: " + profileName + " (click Apply to activate)" );
  }
}

void MainWindow::onAllProfilesChanged()
{
  // Block combo signals to prevent cascading loadProfileDetails calls
  // while we repopulate the list.
  m_profileCombo->blockSignals( true );
  m_profileCombo->clear();
  // Populate combo with name + ID userData
  const QStringList &names = m_profileManager->allProfiles();
  const QJsonArray &defaultData = m_profileManager->defaultProfilesData();
  const QJsonArray &customData = m_profileManager->customProfilesData();
  for ( const auto &p : defaultData )
  {
    if ( p.isObject() )
      m_profileCombo->addItem( p.toObject()["name"].toString(), p.toObject()["id"].toString() );
  }
  for ( const auto &p : customData )
  {
    if ( p.isObject() )
      m_profileCombo->addItem( p.toObject()["name"].toString(), p.toObject()["id"].toString() );
  }
  m_profileCombo->setCurrentIndex( m_profileManager->activeProfileIndex() );
  m_profileCombo->blockSignals( false );
  m_selectedProfileIndex = m_profileManager->activeProfileIndex();

  // Load the active profile details
  QString activeProfileId = m_profileManager->activeProfileId();
  if ( !activeProfileId.isEmpty() )
  {
    loadProfileDetails( activeProfileId );
  }
  // Custom profiles may have changed; reload fan profiles (adds custom entries to the fan combo)
  reloadFanProfiles();

  // If we were in the middle of saving, mark as complete
  if ( m_saveInProgress )
  {
    m_saveInProgress = false;
    m_profileChanged = false;
    statusBar()->showMessage( "Profile saved successfully" );
    updateButtonStates();
  }

  // Ensure buttons reflect current profile set (remove button availability etc.)
  updateButtonStates();
  m_saveButton->setEnabled( true );
}

void MainWindow::onActiveProfileIndexChanged()
{
  int activeIndex = m_profileManager->activeProfileIndex();
  if ( m_profileCombo->currentIndex() != activeIndex )
  {
    m_profileCombo->blockSignals( true );
    m_profileCombo->setCurrentIndex( activeIndex );
    m_profileCombo->blockSignals( false );
  }
  m_selectedProfileIndex = activeIndex;
}

// Profile detail control slot implementations
void MainWindow::onBrightnessSliderChanged( int value )
{
  m_brightnessValueLabel->setText( QString::number( value ) + "%" );
}

void MainWindow::onOffsetFanSpeedChanged( int value )
{
  m_offsetFanSpeedValue->setText( QString::number( value ) + "%" );
}

void MainWindow::onCpuCoresChanged( int value )
{
  m_cpuCoresValue->setText( QString::number( value ) );
}

void MainWindow::onMaxFrequencyChanged( int value )
{
  double freqGHz = value / 1000.0;  // Convert MHz to GHz for display
  m_maxFrequencyValue->setText( QString::number( freqGHz, 'f', 2 ) + " GHz" );
}

int MainWindow::snapToAvailableFrequency( int valueMHz ) const
{
  if ( m_availableFrequenciesMHz.isEmpty() )
    return valueMHz;

  int closestMHz = m_availableFrequenciesMHz.first();
  int minDiff = std::abs( valueMHz - closestMHz );

  for ( int availMHz : m_availableFrequenciesMHz )
  {
    int diff = std::abs( valueMHz - availMHz );
    if ( diff < minDiff )
    {
      minDiff = diff;
      closestMHz = availMHz;
    }
  }

  return closestMHz;
}

void MainWindow::onODMPowerLimit1Changed( int value )
{
  m_odmPowerLimit1Value->setText( QString::number( value ) + " W" );
}

void MainWindow::onODMPowerLimit2Changed( int value )
{
  m_odmPowerLimit2Value->setText( QString::number( value ) + " W" );
}

void MainWindow::onODMPowerLimit3Changed( int value )
{
  m_odmPowerLimit3Value->setText( QString::number( value ) + " W" );
}

void MainWindow::onGpuPowerChanged( int value )
{
  m_gpuPowerValue->setText( QString::number( value ) + " W" );
}

void MainWindow::loadProfileDetails( const QString &profileId )
{
  // Reset change flag when loading a new profile
  m_profileChanged = false;
  m_currentLoadedProfile = profileId;
  updateButtonStates();


  if ( profileId.isEmpty() )
  {
    return;
  }

  // Get the profile JSON from ProfileManager using the profile ID
  QString profileJson = m_profileManager->getProfileDetails( profileId );


  if ( profileJson.isEmpty() )
  {
    return;
  }

  QJsonDocument doc = QJsonDocument::fromJson( profileJson.toUtf8() );

  if ( !doc.isObject() )
  {
    return;
  }

  QJsonObject obj = doc.object();
  // Block signals while updating to avoid triggering slot updates
  m_brightnessSlider->blockSignals( true );
  m_setBrightnessCheckBox->blockSignals( true );
  m_profileFanProfileCombo->blockSignals( true );
  m_fanControlTab->fanProfileCombo()->blockSignals( true );
  m_offsetFanSpeedSlider->blockSignals( true );
  if ( m_autoWaterControlCheckBox ) m_autoWaterControlCheckBox->blockSignals( true );
  m_cpuCoresSlider->blockSignals( true );
  m_governorCombo->blockSignals( true );
  m_minFrequencySlider->blockSignals( true );
  m_maxFrequencySlider->blockSignals( true );
  m_odmPowerLimit1Slider->blockSignals( true );
  m_odmPowerLimit2Slider->blockSignals( true );
  m_odmPowerLimit3Slider->blockSignals( true );
  m_gpuPowerSlider->blockSignals( true );
  m_profileKeyboardProfileCombo->blockSignals( true );
  if ( m_profileChargingProfileCombo ) m_profileChargingProfileCombo->blockSignals( true );
  if ( m_profileChargingPriorityCombo ) m_profileChargingPriorityCombo->blockSignals( true );
  if ( m_profileChargeLimitCombo ) m_profileChargeLimitCombo->blockSignals( true );
  m_mainsButton->blockSignals( true );
  m_batteryButton->blockSignals( true );
  m_waterCoolerButton->blockSignals( true );

  // Load Display settings (nested in display object)

  if ( obj.contains( "display" ) && obj["display"].isObject() )
  {
    QJsonObject displayObj = obj["display"].toObject();


    if ( displayObj.contains( "brightness" ) )
    {
      int brightness = displayObj["brightness"].toInt( 100 );
      m_brightnessSlider->setValue( brightness );
    }


    if ( displayObj.contains( "useBrightness" ) )
    {
      bool useBrightness = displayObj["useBrightness"].toBool( false );
      m_setBrightnessCheckBox->setChecked( useBrightness );
    }
  }

  // Load Fan Control settings (nested in fan object)

  QString loadedFanProfile;
  if ( obj.contains( "fan" ) && obj["fan"].isObject() )
  {
    QJsonObject fanObj = obj["fan"].toObject();


    if ( fanObj.contains( "fanProfile" ) )
    {
      QString fanProfileRef = fanObj["fanProfile"].toString( "fan-balanced" );
      // Try finding by ID userData first (new format), then by text (legacy/name)
      int idx = -1;
      for ( int i = 0; i < m_profileFanProfileCombo->count(); ++i )
      {
        if ( m_profileFanProfileCombo->itemData( i ).toString() == fanProfileRef )
        {
          idx = i;
          break;
        }
      }
      if ( idx < 0 )
        idx = m_profileFanProfileCombo->findText( fanProfileRef );

      if ( idx >= 0 )
      {
        m_profileFanProfileCombo->setCurrentIndex( idx );
        m_fanControlTab->fanProfileCombo()->setCurrentIndex( idx );
        loadedFanProfile = m_profileFanProfileCombo->itemData( idx ).toString();
      }
    }

    if ( fanObj.contains( "offsetFanspeed" ) )
      m_offsetFanSpeedSlider->setValue( fanObj["offsetFanspeed"].toInt( 0 ) );

    // Load sameSpeed from profile (default true)
    if ( fanObj.contains( "sameSpeed" ) )
      m_sameFanSpeedCheckBox->setChecked( fanObj["sameSpeed"].toBool( true ) );
    else
      m_sameFanSpeedCheckBox->setChecked( true );

    if ( fanObj.contains( "autoControlWC" ) )
      m_autoWaterControlCheckBox->setChecked( fanObj["autoControlWC"].toBool( true ) );
    else
      m_autoWaterControlCheckBox->setChecked( true );

    // Load water-cooler scanning enable state (persisted per-profile)
    {
      bool wcEnable = fanObj.contains( "enableWaterCooler" )
                        ? fanObj["enableWaterCooler"].toBool( true )
                        : true;
      m_fanControlTab->setWaterCoolerEnabled( wcEnable );
      m_dashboardTab->setWaterCoolerEnabled( wcEnable );
    }
  }

  // Load CPU settings (nested in cpu object)

  if ( obj.contains( "cpu" ) && obj["cpu"].isObject() )
  {
    QJsonObject cpuObj = obj["cpu"].toObject();


    if ( cpuObj.contains( "onlineCores" ) )
      m_cpuCoresSlider->setValue( cpuObj["onlineCores"].toInt( 32 ) );

    if ( cpuObj.contains( "governor" ) )
    {
      QString governor = cpuObj["governor"].toString();
      int index = m_governorCombo->findData( governor );
      if ( index >= 0 )
        m_governorCombo->setCurrentIndex( index );
      else
        m_governorCombo->setCurrentIndex( 0 ); // default to first
    }

    // Get hardware frequency limits and set slider ranges
    if ( auto limitsJson = m_UccdClient->getCpuFrequencyLimitsJSON() )
    {
      QJsonDocument doc = QJsonDocument::fromJson( limitsJson->c_str() );
      if ( doc.isObject() )
      {
        QJsonObject limitsObj = doc.object();
        int minFreqKHz = limitsObj["min"].toInt( 400000 );   // hardware min frequency in kHz
        int maxFreqKHz = limitsObj["max"].toInt( 6000000 );  // hardware max frequency in kHz

        // Convert kHz to MHz for slider range
        int minFreqMHz = minFreqKHz / 1000;
        int maxFreqMHz = maxFreqKHz / 1000;

        m_minFrequencySlider->setMinimum( minFreqMHz );
        m_minFrequencySlider->setMaximum( maxFreqMHz );
        m_maxFrequencySlider->setMinimum( minFreqMHz );
        m_maxFrequencySlider->setMaximum( maxFreqMHz );

        // Parse and store available frequencies (kHz -> MHz)
        m_availableFrequenciesMHz.clear();
        if ( limitsObj.contains( "available" ) && limitsObj["available"].isArray() )
        {
          QJsonArray availableArray = limitsObj["available"].toArray();
          for ( const auto &freqValue : availableArray )
          {
            int freqKHz = freqValue.toInt();
            int freqMHz = freqKHz / 1000;
            if ( freqMHz >= minFreqMHz && freqMHz <= maxFreqMHz )
            {
              m_availableFrequenciesMHz.append( freqMHz );
            }
          }
        }
      }
    }

    // Load frequency values in MHz (convert from kHz stored in profile)
    // Snap to closest available frequency if we have the list
    if ( cpuObj.contains( "scalingMinFrequency" ) )
    {
      int requestedMHz = cpuObj["scalingMinFrequency"].toInt( 1000000 ) / 1000;
      m_minFrequencySlider->setValue( snapToAvailableFrequency( requestedMHz ) );
    }

    if ( cpuObj.contains( "scalingMaxFrequency" ) )
    {
      int requestedMHz = cpuObj["scalingMaxFrequency"].toInt( 5000000 ) / 1000;
      m_maxFrequencySlider->setValue( snapToAvailableFrequency( requestedMHz ) );
    }
  }
  else
  {
    // Profile loading failed, still try to set slider limits from hardware
    if ( auto limitsJson = m_UccdClient->getCpuFrequencyLimitsJSON() )
    {
      QJsonDocument doc = QJsonDocument::fromJson( limitsJson->c_str() );
      if ( doc.isObject() )
      {
        QJsonObject limitsObj = doc.object();
        int minFreqKHz = limitsObj["min"].toInt( 400000 );
        int maxFreqKHz = limitsObj["max"].toInt( 6000000 );
        int minFreqMHz = minFreqKHz / 1000;
        int maxFreqMHz = maxFreqKHz / 1000;
        m_minFrequencySlider->setMinimum( minFreqMHz );
        m_minFrequencySlider->setMaximum( maxFreqMHz );
        m_maxFrequencySlider->setMinimum( minFreqMHz );
        m_maxFrequencySlider->setMaximum( maxFreqMHz );
      }
    }
  }

  // Load ODM Power Limits (TDP) settings (nested in odmPowerLimits object)
  // First, set slider ranges from hardware limits
  std::vector< int > hardwareLimits = m_profileManager->getHardwarePowerLimits();
  if ( hardwareLimits.size() > 0 )
  {
    m_odmPowerLimit1Slider->setMaximum( hardwareLimits[0] );
  }

  if ( hardwareLimits.size() > 1 )
  {
    m_odmPowerLimit2Slider->setMaximum( hardwareLimits[1] );
  }

  if ( hardwareLimits.size() > 2 )
  {
    m_odmPowerLimit3Slider->setMaximum( hardwareLimits[2] );
  }

  // Set GPU power max from hardware
  if ( auto gpuMax = m_profileManager->getClient()->getNVIDIAPowerCTRLMaxPowerLimit() )
  {
    m_gpuPowerSlider->setMaximum( *gpuMax );
  }

  // Then, set slider values from profile

  if ( obj.contains( "odmPowerLimits" ) && obj["odmPowerLimits"].isObject() )
  {
    QJsonObject odmLimitsObj = obj["odmPowerLimits"].toObject();


    if ( odmLimitsObj.contains( "tdpValues" ) && odmLimitsObj["tdpValues"].isArray() )
    {
      QJsonArray tdpArray = odmLimitsObj["tdpValues"].toArray();

      // Load actual values from profile - these are the current settings

      if ( tdpArray.size() > 0 )
      {
        int val0 = tdpArray[0].toInt();
        m_odmPowerLimit1Slider->setValue( val0 );
      }


      if ( tdpArray.size() > 1 )
      {
        int val1 = tdpArray[1].toInt();
        m_odmPowerLimit2Slider->setValue( val1 );
      }


      if ( tdpArray.size() > 2 )
      {
        int val2 = tdpArray[2].toInt();
        m_odmPowerLimit3Slider->setValue( val2 );
      }
    }
  }
  // Load NVIDIA Power Control settings (nested in nvidiaPowerCTRLProfile object)

  if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj["nvidiaPowerCTRLProfile"].isObject() )
  {
    QJsonObject gpuObj = obj["nvidiaPowerCTRLProfile"].toObject();


    if ( gpuObj.contains( "cTGPOffset" ) )
      m_gpuPowerSlider->setValue( gpuObj["cTGPOffset"].toInt( 175 ) + 100 ); // Offset value, adjust as needed
  }

  // Load Charging profile setting
  if ( m_profileChargingProfileCombo && obj.contains( "chargingProfile" ) )
  {
    QString chargingProfile = obj["chargingProfile"].toString();
    int idx = m_profileChargingProfileCombo->findData( chargingProfile );
    if ( idx >= 0 )
      m_profileChargingProfileCombo->setCurrentIndex( idx );
    else
      m_profileChargingProfileCombo->setCurrentIndex( 0 );
  }
  else if ( m_profileChargingProfileCombo )
  {
    m_profileChargingProfileCombo->setCurrentIndex( 0 );
  }

  // Load Charging priority setting
  if ( m_profileChargingPriorityCombo && obj.contains( "chargingPriority" ) )
  {
    QString chargingPriority = obj["chargingPriority"].toString();
    int idx = m_profileChargingPriorityCombo->findData( chargingPriority );
    if ( idx >= 0 )
      m_profileChargingPriorityCombo->setCurrentIndex( idx );
    else
      m_profileChargingPriorityCombo->setCurrentIndex( 0 );
  }
  else if ( m_profileChargingPriorityCombo )
  {
    m_profileChargingPriorityCombo->setCurrentIndex( 0 );
  }

  // Load Charge limit setting (maps chargeType + thresholds to combo selection)
  if ( m_profileChargeLimitCombo )
  {
    QString chargeType = obj.value( "chargeType" ).toString();
    int startThr = obj.value( "chargeStartThreshold" ).toInt( -1 );
    int endThr = obj.value( "chargeEndThreshold" ).toInt( -1 );

    if ( chargeType == "Custom" && startThr == 60 && endThr == 90 )
      m_profileChargeLimitCombo->setCurrentIndex( m_profileChargeLimitCombo->findData( "reduced" ) );
    else if ( chargeType == "Custom" && startThr == 40 && endThr == 80 )
      m_profileChargeLimitCombo->setCurrentIndex( m_profileChargeLimitCombo->findData( "stationary" ) );
    else
      m_profileChargeLimitCombo->setCurrentIndex( m_profileChargeLimitCombo->findData( "full" ) );
  }

  // Load Keyboard settings - check for embedded keyboard profile name
  QString loadedKeyboardProfile;
  if ( obj.contains( "selectedKeyboardProfile" ) )
  {
    QString keyboardProfileId = obj["selectedKeyboardProfile"].toString();
    // Find by ID in combo userData
    int idx = -1;
    for ( int i = 0; i < m_profileKeyboardProfileCombo->count(); ++i )
    {
      if ( m_profileKeyboardProfileCombo->itemData( i ).toString() == keyboardProfileId )
      { idx = i; break; }
    }
    // Fallback: try matching by name (legacy data)
    if ( idx < 0 )
      idx = m_profileKeyboardProfileCombo->findText( keyboardProfileId );
    if ( idx >= 0 )
    {
      m_profileKeyboardProfileCombo->setCurrentIndex( idx );
      loadedKeyboardProfile = m_profileKeyboardProfileCombo->itemData( idx ).toString();
    }
  }
  // Fallback: check for old format keyboard.profile field
  else if ( obj.contains( "keyboard" ) && obj["keyboard"].isObject() )
  {
    QJsonObject keyboardObj = obj["keyboard"].toObject();
    if ( keyboardObj.contains( "profile" ) )
    {
      QString keyboardProfileId = keyboardObj["profile"].toString();
      int idx = -1;
      for ( int i = 0; i < m_profileKeyboardProfileCombo->count(); ++i )
      {
        if ( m_profileKeyboardProfileCombo->itemData( i ).toString() == keyboardProfileId )
        { idx = i; break; }
      }
      if ( idx < 0 )
        idx = m_profileKeyboardProfileCombo->findText( keyboardProfileId );
      if ( idx >= 0 )
      {
        m_profileKeyboardProfileCombo->setCurrentIndex( idx );
        loadedKeyboardProfile = m_profileKeyboardProfileCombo->itemData( idx ).toString();
      }
    }
  }

  // Load keyboard profile colors and brightness
  if ( obj.contains( "keyboard" ) && obj["keyboard"].isObject() )
  {
    QJsonObject keyboardObj = obj["keyboard"].toObject();

    // Load brightness if present
    if ( keyboardObj.contains( "brightness" ) && m_keyboardBrightnessSlider )
    {
      int brightness = keyboardObj["brightness"].toInt( m_keyboardBrightnessSlider->maximum() / 2 );
      m_keyboardBrightnessSlider->setValue( brightness );
    }

    // Load key colors if present
    if ( keyboardObj.contains( "states" ) && keyboardObj["states"].isArray() && m_keyboardVisualizer )
    {
      m_keyboardVisualizer->updateFromJSON( keyboardObj["states"].toArray() );
    }
  }

  // Load power state activation settings
  QString settingsJson = m_profileManager->getSettingsJSON();
  if ( !settingsJson.isEmpty() )
  {
    QJsonDocument settingsDoc = QJsonDocument::fromJson( settingsJson.toUtf8() );
    if ( settingsDoc.isObject() )
    {
      QJsonObject settingsObj = settingsDoc.object();
      if ( settingsObj.contains( "stateMap" ) && settingsObj["stateMap"].isObject() )
      {
        QJsonObject stateMap = settingsObj["stateMap"].toObject();
        QString mainsProfile = stateMap["power_ac"].toString();
        QString batteryProfile = stateMap["power_bat"].toString();
        QString wcProfile = stateMap["power_wc"].toString();

        m_mainsButton->setChecked( mainsProfile == profileId );
        m_batteryButton->setChecked( batteryProfile == profileId );
        m_waterCoolerButton->setChecked( wcProfile == profileId );

        // Store the loaded power state assignments
        m_loadedMainsAssignment = (mainsProfile == profileId);
        m_loadedBatteryAssignment = (batteryProfile == profileId);
        m_loadedWaterCoolerAssignment = (wcProfile == profileId);
      }
    }
  }

  // Unblock signals
  m_brightnessSlider->blockSignals( false );
  m_setBrightnessCheckBox->blockSignals( false );
  m_profileFanProfileCombo->blockSignals( false );
  m_fanControlTab->fanProfileCombo()->blockSignals( false );
  m_offsetFanSpeedSlider->blockSignals( false );
  if ( m_autoWaterControlCheckBox ) m_autoWaterControlCheckBox->blockSignals( false );
  m_cpuCoresSlider->blockSignals( false );
  m_governorCombo->blockSignals( false );
  m_minFrequencySlider->blockSignals( false );
  m_maxFrequencySlider->blockSignals( false );
  m_odmPowerLimit1Slider->blockSignals( false );
  m_odmPowerLimit2Slider->blockSignals( false );
  m_odmPowerLimit3Slider->blockSignals( false );
  m_gpuPowerSlider->blockSignals( false );
  m_profileKeyboardProfileCombo->blockSignals( false );
  if ( m_profileChargingProfileCombo ) m_profileChargingProfileCombo->blockSignals( false );
  if ( m_profileChargingPriorityCombo ) m_profileChargingPriorityCombo->blockSignals( false );
  if ( m_profileChargeLimitCombo ) m_profileChargeLimitCombo->blockSignals( false );
  m_mainsButton->blockSignals( false );
  m_batteryButton->blockSignals( false );
  m_waterCoolerButton->blockSignals( false );

  // Trigger label updates by calling the slots directly
  onBrightnessSliderChanged( m_brightnessSlider->value() );
  onOffsetFanSpeedChanged( m_offsetFanSpeedSlider->value() );
  onCpuCoresChanged( m_cpuCoresSlider->value() );
  onMaxFrequencyChanged( m_maxFrequencySlider->value() );
  onODMPowerLimit1Changed( m_odmPowerLimit1Slider->value() );
  onODMPowerLimit2Changed( m_odmPowerLimit2Slider->value() );
  onODMPowerLimit3Changed( m_odmPowerLimit3Slider->value() );
  onGpuPowerChanged( m_gpuPowerSlider->value() );

  // Trigger fan profile change if one was loaded (loads fan curve data for display only)
  if ( !loadedFanProfile.isEmpty() )
  {
    onFanProfileChanged( loadedFanProfile );
  }

  // Load keyboard profile for display only — block signals to prevent hardware writes
  // (brightness slider → setGlobalBrightness → colorsChanged → setKeyboardBacklight)
  if ( !loadedKeyboardProfile.isEmpty() )
  {
    if ( m_keyboardBrightnessSlider ) m_keyboardBrightnessSlider->blockSignals( true );
    if ( m_keyboardVisualizer ) m_keyboardVisualizer->blockSignals( true );

    onKeyboardProfileChanged( loadedKeyboardProfile );

    if ( m_keyboardBrightnessSlider ) m_keyboardBrightnessSlider->blockSignals( false );
    if ( m_keyboardVisualizer ) m_keyboardVisualizer->blockSignals( false );
  }


  // Enable/disable editing widgets based on whether profile is custom
  const bool isCustom = m_profileManager ? m_profileManager->isCustomProfile( profileId ) : false;
  updateProfileEditingWidgets( isCustom );

}

void MainWindow::updateProfileEditingWidgets( bool isCustom )
{
  // Enable/disable editing widgets based on whether profile is custom

  // Description edit
  if ( m_descriptionEdit ) {
    m_descriptionEdit->setEnabled( isCustom );
    m_descriptionEdit->setReadOnly( !isCustom );
  }

  // Profile combo - allow renaming only for custom profiles
  if ( m_profileCombo && m_profileCombo->lineEdit() ) {
    m_profileCombo->lineEdit()->setReadOnly( !isCustom );
  }

  // Auto-activate buttons (always enabled for power state assignment)
  if ( m_mainsButton ) m_mainsButton->setEnabled( true );
  if ( m_batteryButton ) m_batteryButton->setEnabled( true );

  // Display controls
  if ( m_setBrightnessCheckBox ) m_setBrightnessCheckBox->setEnabled( isCustom );
  if ( m_brightnessSlider ) m_brightnessSlider->setEnabled( isCustom );

  // Fan controls
  if ( m_profileFanProfileCombo ) m_profileFanProfileCombo->setEnabled( isCustom );
  if ( m_offsetFanSpeedSlider ) m_offsetFanSpeedSlider->setEnabled( isCustom );
  if ( m_sameFanSpeedCheckBox ) m_sameFanSpeedCheckBox->setEnabled( isCustom );
  if ( m_autoWaterControlCheckBox ) m_autoWaterControlCheckBox->setEnabled( isCustom );

  // CPU controls
  if ( m_cpuCoresSlider ) m_cpuCoresSlider->setEnabled( isCustom );
  if ( m_governorCombo ) m_governorCombo->setEnabled( isCustom );
  if ( m_minFrequencySlider ) m_minFrequencySlider->setEnabled( isCustom );
  if ( m_maxFrequencySlider ) m_maxFrequencySlider->setEnabled( isCustom );

  // ODM Power controls
  if ( m_odmPowerLimit1Slider ) m_odmPowerLimit1Slider->setEnabled( isCustom );
  if ( m_odmPowerLimit2Slider ) m_odmPowerLimit2Slider->setEnabled( isCustom );
  if ( m_odmPowerLimit3Slider ) m_odmPowerLimit3Slider->setEnabled( isCustom );

  // GPU controls
  if ( m_gpuPowerSlider ) m_gpuPowerSlider->setEnabled( isCustom );

  // Charging profile
  if ( m_profileChargingProfileCombo ) m_profileChargingProfileCombo->setEnabled( isCustom );
  if ( m_profileChargingPriorityCombo ) m_profileChargingPriorityCombo->setEnabled( isCustom );
  if ( m_profileChargeLimitCombo ) m_profileChargeLimitCombo->setEnabled( isCustom );
}

void MainWindow::markChanged()
{
  m_profileChanged = true;
  updateButtonStates();
}

void MainWindow::updateButtonStates( void)
{
  // Update profile page buttons if available
  if ( profileTopWidgetsAvailable() )
  {
    m_removeProfileButton->setEnabled( m_profileManager->isCustomProfile( m_profileCombo->currentData().toString() ) );
  }

  // Delegate fan profile button states to FanControlTab
  if ( m_fanControlTab )
    m_fanControlTab->updateButtonStates( m_UccdClient->isConnected() );
}

void MainWindow::onApplyClicked()
{
  if ( m_selectedProfileIndex >= 0 )
  {
    QString profileName = m_profileCombo->currentText();
    m_profileManager->setActiveProfileByIndex( m_selectedProfileIndex );
    statusBar()->showMessage( "Profile applied: " + profileName );
  }
  else
  {
    statusBar()->showMessage( "No profile selected" );
  }
}

void MainWindow::onSaveClicked()
{
  QString profileName = m_profileCombo->currentText();
  QString profileId = m_profileCombo->currentData().toString();
  const bool isCustom = m_profileManager->isCustomProfile( profileId );

  if ( isCustom )
  {
    // Save full profile changes for custom profiles
    // Build updated profile JSON
    QJsonObject profileObj;
    profileObj["id"] = profileId;
    profileObj["name"] = profileName;
    profileObj["description"] = m_descriptionEdit->toPlainText();

    // Brightness settings
    QJsonObject displayObj;
    if ( m_setBrightnessCheckBox->isChecked() )
    {
      displayObj["brightness"] = m_brightnessSlider->value();
    }
    profileObj["display"] = displayObj;

    // Fan settings - embed complete fan profile data
    QJsonObject fanObj;

    // Get the full fan profile JSON and embed tableCPU/tableGPU
    QString fanProfileId = m_profileFanProfileCombo->currentData().toString();
    QString fanProfileName = m_profileFanProfileCombo->currentText();
    QString fanProfileJSON = m_profileManager->getFanProfile(fanProfileId);

    if (!fanProfileJSON.isEmpty() && fanProfileJSON != "{}")
    {
      QJsonDocument fanDoc = QJsonDocument::fromJson(fanProfileJSON.toUtf8());
      if (fanDoc.isObject())
      {
        QJsonObject fanProfileObj = fanDoc.object();

        // Embed fan curve tables directly in the profile
        if (fanProfileObj.contains("tableCPU"))
          fanObj["tableCPU"] = fanProfileObj["tableCPU"];

        if (fanProfileObj.contains("tableGPU"))
          fanObj["tableGPU"] = fanProfileObj["tableGPU"];

        if (fanProfileObj.contains("tablePump"))
          fanObj["tablePump"] = fanProfileObj["tablePump"];

        if (fanProfileObj.contains("tableWaterCoolerFan"))
          fanObj["tableWaterCoolerFan"] = fanProfileObj["tableWaterCoolerFan"];
      }
    }

    // Store fan profile ID using the key the daemon expects
    fanObj["fanProfile"] = fanProfileId;
    fanObj["offsetFanspeed"] = m_offsetFanSpeedSlider->value();
    // Persist same-speed setting
    fanObj["sameSpeed"] = m_sameFanSpeedCheckBox ? m_sameFanSpeedCheckBox->isChecked() : true;
    fanObj["autoControlWC"] = m_autoWaterControlCheckBox ? m_autoWaterControlCheckBox->isChecked() : true;
    fanObj["enableWaterCooler"] = m_fanControlTab ? m_fanControlTab->isWaterCoolerEnabled() : true;

    profileObj["fan"] = fanObj;

    // CPU settings
    QJsonObject cpuObj;
    cpuObj["onlineCores"] = m_cpuCoresSlider->value();
    cpuObj["governor"] = m_governorCombo->currentData().toString();
    cpuObj["scalingMinFrequency"] = m_minFrequencySlider->value() * 1000;  // convert MHz to kHz
    cpuObj["scalingMaxFrequency"] = m_maxFrequencySlider->value() * 1000;  // convert MHz to kHz
    profileObj["cpu"] = cpuObj;

  // ODM Power Limit (TDP) settings
  QJsonObject odmObj;
  QJsonArray tdpArray;
  tdpArray.append( m_odmPowerLimit1Slider->value() );
  tdpArray.append( m_odmPowerLimit2Slider->value() );
  tdpArray.append( m_odmPowerLimit3Slider->value() );
  odmObj["tdpValues"] = tdpArray;
  profileObj["odmPowerLimits"] = odmObj;

  // GPU settings
  QJsonObject gpuObj;
  gpuObj["cTGPOffset"] = m_gpuPowerSlider->value() - 100; // Reverse the offset
  QJsonObject nvidiaPowerObj;
  nvidiaPowerObj["cTGPOffset"] = gpuObj["cTGPOffset"];
  profileObj["nvidiaPowerCTRLProfile"] = nvidiaPowerObj;

  // Keyboard settings - embed complete keyboard profile data
  QJsonObject keyboardObj;

  QString keyboardProfileId = m_profileKeyboardProfileCombo->currentData().toString();
  QString keyboardProfileName = m_profileKeyboardProfileCombo->currentText();

  // Get the selected keyboard profile data
  QString keyboardProfileJSON = m_profileManager->getKeyboardProfile(keyboardProfileId);
  if (!keyboardProfileJSON.isEmpty() && keyboardProfileJSON != "{}") {
    QJsonDocument kbDoc = QJsonDocument::fromJson(keyboardProfileJSON.toUtf8());
    if (kbDoc.isObject()) {
      keyboardObj = kbDoc.object();
    } else if (kbDoc.isArray()) {
      keyboardObj["states"] = kbDoc.array();
    }
  } else {
    // Fallback: get current keyboard backlight states from daemon
    if (auto keyboardStates = m_UccdClient->getKeyboardBacklightStates()) {
      keyboardObj["states"] = QJsonDocument::fromJson(QString::fromStdString(*keyboardStates).toUtf8()).array();
    }
  }

  // Store the name for reference/display
  keyboardObj["keyboardProfileName"] = keyboardProfileName;

  // Include keyboard brightness in the profile
  if ( m_keyboardBrightnessSlider )
  {
    keyboardObj["brightness"] = m_keyboardBrightnessSlider->value();
  }

  // Include key colors from visualizer if available
  if ( m_keyboardVisualizer )
  {
    keyboardObj["states"] = m_keyboardVisualizer->getJSONState();
  }

  profileObj["keyboard"] = keyboardObj;

  // Embed the selected keyboard profile ID at profile level
  profileObj["selectedKeyboardProfile"] = keyboardProfileId;

  // Charging profile setting
  if ( m_profileChargingProfileCombo )
  {
    QString chargingProfile = m_profileChargingProfileCombo->currentData().toString();
    if ( !chargingProfile.isEmpty() )
      profileObj["chargingProfile"] = chargingProfile;
  }

  // Charging priority setting
  if ( m_profileChargingPriorityCombo )
  {
    QString chargingPriority = m_profileChargingPriorityCombo->currentData().toString();
    if ( !chargingPriority.isEmpty() )
      profileObj["chargingPriority"] = chargingPriority;
  }

  // Charge limit setting (stored as chargeType + thresholds)
  if ( m_profileChargeLimitCombo )
  {
    QString limitPreset = m_profileChargeLimitCombo->currentData().toString();
    if ( limitPreset == "full" )
    {
      profileObj["chargeType"] = "Standard";
    }
    else if ( limitPreset == "reduced" )
    {
      profileObj["chargeType"] = "Custom";
      profileObj["chargeStartThreshold"] = 60;
      profileObj["chargeEndThreshold"] = 90;
    }
    else if ( limitPreset == "stationary" )
    {
      profileObj["chargeType"] = "Custom";
      profileObj["chargeStartThreshold"] = 40;
      profileObj["chargeEndThreshold"] = 80;
    }
  }

  // Convert to JSON string and save
  QJsonDocument doc( profileObj );
  QString profileJSON = QString::fromUtf8( doc.toJson() );

  m_profileManager->saveProfile( profileJSON );
  }

  // For both custom and built-in profiles, update stateMap based on mains/battery button states
  // Batch all stateMap changes into a single D-Bus call (single backup + write)
  std::map< QString, QString > stateMapUpdates;
  if ( m_mainsButton->isChecked() )
    stateMapUpdates["power_ac"] = profileId;
  if ( m_batteryButton->isChecked() )
    stateMapUpdates["power_bat"] = profileId;
  if ( m_waterCoolerButton->isChecked() )
    stateMapUpdates["power_wc"] = profileId;

  if ( !stateMapUpdates.empty() )
    m_profileManager->setBatchStateMap( stateMapUpdates );

  // Indicate saving; actual success will be reflected when ProfileManager signals
  m_saveInProgress = true;
  statusBar()->showMessage( "Saving profile..." );
  updateButtonStates();
}

void MainWindow::onSaveFanProfilesClicked()
{
  // Save fan profiles via DBus
  saveFanPoints();
  statusBar()->showMessage( "Fan profiles saved" );
}

void MainWindow::onAddProfileClicked()
{
  // Generate a unique profile name
  QString baseName = "New Profile";
  QString profileName;
  int counter = 1;

  do {
    profileName = QString("%1 %2").arg(baseName).arg(counter);
    counter++;
  } while (m_profileManager->allProfiles().contains(profileName));

  // Create profile from default
  QString profileJson = m_profileManager->createProfileFromDefault(profileName);
  if (!profileJson.isEmpty()) {
    statusBar()->showMessage( QString("Profile '%1' created successfully").arg(profileName) );

    // Switch to the newly created profile
    int newIndex = m_profileCombo->findText(profileName);
    if (newIndex != -1) {
      m_profileCombo->setCurrentIndex(newIndex);
    }
  }
  else
    QMessageBox::warning(this, "Error", "Failed to create new profile.");
}
void MainWindow::onCopyProfileClicked()
{
  QString current = m_profileCombo->currentText();

  // Allow copying any profile (built-in or custom)
  QString profileId = m_profileCombo->currentData().toString();
  QString json = m_profileManager->getProfileDetails(profileId);
  if (json.isEmpty()) {
    QMessageBox::warning(this, "Error", "Failed to get profile data.");
    return;
  }

  QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  if (!doc.isObject()) {
    QMessageBox::warning(this, "Error", "Invalid profile data.");
    return;
  }

  QJsonObject obj = doc.object();

  // Generate a new unique ID for the copied profile
  obj["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);

  // Generate new name by incrementing number
  QString baseName = current;
  int number = 0;
  int lastSpace = current.lastIndexOf(' ');
  if (lastSpace > 0) {
    QString after = current.mid(lastSpace + 1);
    bool ok;
    int num = after.toInt(&ok);
    if (ok) {
      baseName = current.left(lastSpace);
      number = num;
    }
  }
  QString newName;
  do {
    number++;
    newName = QString("%1 %2").arg(baseName).arg(number);
  } while (m_profileManager->allProfiles().contains(newName));

  // Set new name
  obj["name"] = newName;

  // Save
  QString newJson = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  m_profileManager->saveProfile(newJson);

  // Switch
  int newIndex = m_profileCombo->findText(newName);
  if (newIndex != -1) {
    m_profileCombo->setCurrentIndex(newIndex);
  }

  statusBar()->showMessage( QString("Profile '%1' copied to '%2'").arg(current).arg(newName) );
}

void MainWindow::onRemoveProfileClicked()
{
  QString currentProfile = m_profileCombo->currentText();
  QString currentProfileId = m_profileCombo->currentData().toString();

  // Check if it's a built-in profile
  if (!m_profileManager->isCustomProfile(currentProfileId)) {
    QMessageBox::information(this, "Cannot Remove",
                            "Built-in profiles cannot be removed.");
    return;
  }

  // Confirm deletion
  QMessageBox::StandardButton reply = QMessageBox::question(
    this, "Remove Profile",
    QString("Are you sure you want to remove the profile '%1'?").arg(currentProfile),
    QMessageBox::Yes | QMessageBox::No
  );

  if (reply == QMessageBox::Yes) {
    m_profileManager->deleteProfile(currentProfileId);
    statusBar()->showMessage( QString("Profile '%1' removed").arg(currentProfile) );
  }
}

void MainWindow::onProfileComboRenamed()
{
  if ( !m_profileCombo || !m_profileCombo->lineEdit() ) return;

  int idx = m_profileCombo->currentIndex();
  if ( idx < 0 ) return;

  QString oldName = m_profileCombo->itemText( idx );
  QString newName = m_profileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName ) {
    // Revert if empty or unchanged
    m_profileCombo->setEditText( oldName );
    return;
  }

  if ( !m_profileManager->isCustomProfile( m_profileCombo->itemData( idx ).toString() ) ) {
    // Cannot rename built-in profiles
    m_profileCombo->setEditText( oldName );
    return;
  }

  // Get profile data and update the name
  QString profileId = m_profileCombo->itemData( idx ).toString();
  QString json = m_profileManager->getProfileDetails( profileId );
  if ( json.isEmpty() ) {
    m_profileCombo->setEditText( oldName );
    return;
  }

  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( !doc.isObject() ) {
    m_profileCombo->setEditText( oldName );
    return;
  }

  QJsonObject obj = doc.object();
  obj["name"] = newName;

  // Save with new name (ProfileManager matches by ID and updates the name)
  QJsonDocument out( obj );
  m_profileManager->saveProfile( QString::fromUtf8( out.toJson( QJsonDocument::Compact ) ) );

  // Select the renamed profile after the combo gets repopulated
  int newIdx = m_profileCombo->findText( newName );
  if ( newIdx != -1 )
    m_profileCombo->setCurrentIndex( newIdx );

  statusBar()->showMessage( QString("Profile renamed from '%1' to '%2'").arg( oldName, newName ) );
}

void MainWindow::onKeyboardProfileComboRenamed()
{
  if ( !m_keyboardProfileCombo || !m_keyboardProfileCombo->lineEdit() ) return;

  int idx = m_keyboardProfileCombo->currentIndex();
  if ( idx < 0 ) return;

  QString oldName = m_keyboardProfileCombo->itemText( idx );
  QString newName = m_keyboardProfileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName ) {
    m_keyboardProfileCombo->setEditText( oldName );
    return;
  }

  // "Default" is built-in
  QString keyboardProfileId = m_keyboardProfileCombo->itemData( idx ).toString();
  if ( keyboardProfileId.isEmpty() || !m_profileManager->customKeyboardProfiles().contains( oldName ) ) {
    m_keyboardProfileCombo->setEditText( oldName );
    return;
  }

  if ( m_profileManager->renameKeyboardProfile( keyboardProfileId, newName ) ) {
    m_keyboardProfileCombo->setItemText( idx, newName );
    statusBar()->showMessage( QString("Keyboard profile renamed from '%1' to '%2'").arg( oldName, newName ) );
    updateKeyboardProfileButtonStates();
  } else {
    m_keyboardProfileCombo->setEditText( oldName );
    statusBar()->showMessage( "Failed to rename keyboard profile", 3000 );
  }
}

void MainWindow::onAddFanProfileClicked()
{
  // Generate a unique fan profile name
  QString baseName = "Custom Fan Profile";
  QString profileName;
  int counter = 1;

  do {
    profileName = QString("%1 %2").arg(baseName).arg(counter);
    counter++;
  } while (m_fanControlTab->fanProfileCombo()->findText(profileName) != -1);

  // Add to combo with generated ID
  QString newId = QUuid::createUuid().toString( QUuid::WithoutBraces );
  m_fanControlTab->fanProfileCombo()->addItem(profileName, newId);
  m_fanControlTab->fanProfileCombo()->setCurrentText(profileName);
  statusBar()->showMessage( QString("Fan profile '%1' created").arg(profileName) );
}

void MainWindow::onRemoveFanProfileClicked()
{
  QString currentProfile = m_fanControlTab->fanProfileCombo()->currentText();

  // Check if it's a built-in profile
  if ( m_fanControlTab->builtinFanProfiles().contains( currentProfile ) ) {
    QMessageBox::information(this, "Cannot Remove",
                            "Built-in fan profiles cannot be removed.");
    return;
  }

  // Confirm deletion
  QMessageBox::StandardButton reply = QMessageBox::question(
    this, "Remove Fan Profile",
    QString("Are you sure you want to remove the fan profile '%1'?").arg(currentProfile),
    QMessageBox::Yes | QMessageBox::No
  );

  if (reply == QMessageBox::Yes) {
    // Remove from persistent storage and UI
    QString fanProfileId = m_fanControlTab->fanProfileCombo()->currentData().toString();
    if ( m_profileManager->deleteFanProfile( fanProfileId ) ) {
      // Remove from both fan profile lists
      int idx = m_fanControlTab->fanProfileCombo()->currentIndex();
      if ( idx >= 0 ) m_fanControlTab->fanProfileCombo()->removeItem( idx );
      if ( m_profileFanProfileCombo ) {
        int idx2 = m_profileFanProfileCombo->findText( currentProfile );
        if ( idx2 != -1 ) m_profileFanProfileCombo->removeItem( idx2 );
      }
      statusBar()->showMessage( QString("Fan profile '%1' removed").arg(currentProfile) );
    } else {
      QMessageBox::warning(this, "Remove Failed", "Failed to remove custom fan profile.");
    }
  }
}

void MainWindow::onFanProfileChanged(const QString& fanProfileId)
{
  QString json = m_profileManager->getFanProfile(fanProfileId);
  QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  if (doc.isObject()) {
    QJsonObject obj = doc.object();

    // Load CPU points
    if (obj.contains("tableCPU")) {
      QJsonArray arr = obj["tableCPU"].toArray();
      QVector<FanCurveEditorWidget::Point> cpuPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        double speed = o["speed"].toDouble();
        cpuPoints.append({temp, speed});
      }
      if (m_fanControlTab->cpuEditor() && !cpuPoints.isEmpty()) {
        m_fanControlTab->cpuEditor()->setPoints(cpuPoints);
      }
    }

    // Load GPU points
    if (obj.contains("tableGPU")) {
      QJsonArray arr = obj["tableGPU"].toArray();
      QVector<FanCurveEditorWidget::Point> gpuPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        double speed = o["speed"].toDouble();
        gpuPoints.append({temp, speed});
      }
      if (m_fanControlTab->gpuEditor() && !gpuPoints.isEmpty()) {
        m_fanControlTab->gpuEditor()->setPoints(gpuPoints);
      }
    }

    // Load water cooler fan points
    if (obj.contains("tableWaterCoolerFan")) {
      QJsonArray arr = obj["tableWaterCoolerFan"].toArray();
      QVector<FanCurveEditorWidget::Point> wcFanPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        double speed = o["speed"].toDouble();
        wcFanPoints.append({temp, speed});
      }
      if (m_fanControlTab->wcFanEditor() && !wcFanPoints.isEmpty()) {
        m_fanControlTab->wcFanEditor()->setPoints(wcFanPoints);
      }
    }

    // Load pump threshold points
    if (obj.contains("tablePump")) {
      QJsonArray arr = obj["tablePump"].toArray();
      QVector<PumpCurveEditorWidget::Point> pumpPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        int speed = o["speed"].toInt();
        pumpPoints.append({temp, speed});
      }
      if (m_fanControlTab->pumpEditor() && !pumpPoints.isEmpty()) {
        m_fanControlTab->pumpEditor()->setPoints(pumpPoints);
      }
    }
  }

  // Update the current fan profile ID
  m_currentFanProfile = fanProfileId;

  // Synchronize profile tab fan profile combo with fan tab selection (match by ID userData)
  m_profileFanProfileCombo->blockSignals(true);
  int idx = -1;
  for ( int i = 0; i < m_profileFanProfileCombo->count(); ++i )
  {
    if ( m_profileFanProfileCombo->itemData( i ).toString() == fanProfileId )
    {
      idx = i;
      break;
    }
  }
  if (idx >= 0) {
    m_profileFanProfileCombo->setCurrentIndex(idx);
  }
  m_profileFanProfileCombo->blockSignals(false);

  // Set editors editable only for custom profiles (those not in built-ins)
  bool isEditable = !m_fanControlTab->builtinFanProfiles().contains( m_profileFanProfileCombo->currentText() );
  m_fanControlTab->setEditorsEditable( isEditable );

  // Update button states
  updateButtonStates();
}

void MainWindow::onCpuFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points)
{
  m_cpuFanPoints.clear();
  for (const auto& p : points) {
    m_cpuFanPoints.append({static_cast<int>(p.temp), static_cast<int>(p.duty)});
  }
}

void MainWindow::reloadFanProfiles()
{
  // Delegate to FanControlTab which owns the combo and builtin list
  m_fanControlTab->reloadFanProfiles();

  // Mirror into profile page combo if present
  if ( m_profileFanProfileCombo )
  {
    m_profileFanProfileCombo->blockSignals(true);
    m_profileFanProfileCombo->clear();
    for ( int i = 0; i < m_fanControlTab->fanProfileCombo()->count(); ++i )
      m_profileFanProfileCombo->addItem( m_fanControlTab->fanProfileCombo()->itemText(i),
                                         m_fanControlTab->fanProfileCombo()->itemData(i) );
    m_profileFanProfileCombo->blockSignals(false);
  }

  // Trigger change handler to update editors/buttons
  if ( m_fanControlTab->fanProfileCombo() && m_fanControlTab->fanProfileCombo()->count() > 0 )
    onFanProfileChanged( m_fanControlTab->fanProfileCombo()->currentData().toString() );
  else
    updateButtonStates();
}



void MainWindow::onUccdConnectionChanged( bool connected )
{
  qDebug() << "Uccd connection status changed:" << connected;
  if ( connected )
    reloadFanProfiles();
}

void MainWindow::onGpuFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points)
{
  m_gpuFanPoints.clear();
  for (const auto& p : points) {
    m_gpuFanPoints.append({static_cast<int>(p.temp), static_cast<int>(p.duty)});
  }
}

void MainWindow::onWaterCoolerFanPointsChanged(const QVector<FanCurveEditorWidget::Point>& points)
{
  m_waterCoolerFanPoints.clear();
  for (const auto& p : points) {
    m_waterCoolerFanPoints.append({static_cast<int>(p.temp), static_cast<int>(p.duty)});
  }
}

void MainWindow::onPumpPointsChanged(const QVector<PumpCurveEditorWidget::Point>& /*points*/)
{
  // Pump points changed – mark the fan profile as modified so it can be saved
  if ( m_fanControlTab )
  {
    m_fanControlTab->saveButton()->setEnabled( true );
    m_fanControlTab->applyButton()->setEnabled( true );
  }
}

void MainWindow::onCopyFanProfileClicked()
{
  QString currentProfileId = m_fanControlTab->fanProfileCombo()->currentData().toString();
  if ( currentProfileId.isEmpty() ) return;

  // Get the current profile data
  QString json = m_profileManager->getFanProfile( currentProfileId );
  if ( json.isEmpty() ) {
    QMessageBox::warning(this, "Error", "Failed to get fan profile data.");
    return;
  }

  // Generate a unique custom fan profile name
  QString baseName = "Custom Fan Profile";
  QString profileName;
  int counter = 1;
  do {
    profileName = QString("%1 %2").arg(baseName).arg(counter);
    counter++;
  } while ( m_fanControlTab->fanProfileCombo()->findText( profileName ) != -1 );

  // Save it under the new name with a new ID
  QString newId = QUuid::createUuid().toString( QUuid::WithoutBraces );
  if ( m_profileManager->setFanProfile( newId, profileName, json ) ) {
    m_fanControlTab->fanProfileCombo()->addItem( profileName, newId );
    if ( m_profileFanProfileCombo && m_profileFanProfileCombo->findText( profileName ) == -1 )
      m_profileFanProfileCombo->addItem( profileName, newId );
    m_fanControlTab->fanProfileCombo()->setCurrentText( profileName );
    statusBar()->showMessage( QString("Copied fan profile to '%1'").arg(profileName) );
  }
  else {
    QMessageBox::warning(this, "Error", "Failed to copy profile to new custom profile.");
  }
}

void MainWindow::onApplyFanProfilesClicked()
{
  if ( not m_fanControlTab->cpuEditor() and not m_fanControlTab->gpuEditor() )
  {
    QMessageBox::warning( this, "No Editors", "No fan curve editors available to apply fan profiles." );
    return;
  }

  if ( !m_UccdClient || !m_UccdClient->isConnected() )
  {
    QMessageBox::warning( this, "Not connected", "Not connected to system service; cannot apply fan profiles." );
    return;
  }

  const auto &cpuPoints = m_fanControlTab->cpuEditor()->points();
  const auto &gpuPoints = m_fanControlTab->gpuEditor()->points();
  QJsonObject root;
  QJsonArray cpuArr;
  QJsonArray gpuArr;

  for ( const auto &p : cpuPoints )
  {
    QJsonObject o;
    o["temp"] = p.temp;
    o["speed"] = p.duty;
    cpuArr.append( o );
  }

  for ( const auto &p : gpuPoints )
  {
    QJsonObject o;
    o["temp"] = p.temp;
    o["speed"] = p.duty;
    gpuArr.append( o );
  }

  // Water cooler fan points
  QJsonArray wcFanArr;
  if ( m_fanControlTab->wcFanEditor() )
  {
    const auto &wcFanPoints = m_fanControlTab->wcFanEditor()->points();
    for ( const auto &p : wcFanPoints )
    {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.duty;
      wcFanArr.append( o );
    }
  }

  // Pump threshold points
  QJsonArray pumpArr;
  if ( m_fanControlTab->pumpEditor() )
  {
    const auto &pumpPoints = m_fanControlTab->pumpEditor()->points();
    for ( const auto &p : pumpPoints )
    {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.level;
      pumpArr.append( o );
    }
  }

  root[ "cpu" ] = cpuArr;
  root[ "gpu" ] = gpuArr;
  root[ "waterCoolerFan" ] = wcFanArr;
  root[ "pump" ] = pumpArr;

  QJsonDocument doc( root );
  QString json = QString::fromUtf8( doc.toJson( QJsonDocument::Compact ) );

  if ( m_UccdClient->applyFanProfiles( json.toStdString() ) )
  {
    statusBar()->showMessage( "Temporary fan profiles applied" );

    // Keep an internal copy so UI actions like revert have the current values
    m_cpuFanPoints.clear();
    for ( const auto &p : m_fanControlTab->cpuEditor()->points() )
      m_cpuFanPoints.append( { static_cast< int >( p.temp ), static_cast< int >( p.duty ) } );

    m_gpuFanPoints.clear();
    for ( const auto &p : m_fanControlTab->gpuEditor()->points() )
      m_gpuFanPoints.append( { static_cast< int >( p.temp ), static_cast< int >( p.duty ) } );

    m_waterCoolerFanPoints.clear();
    if ( m_fanControlTab->wcFanEditor() )
      for ( const auto &p : m_fanControlTab->wcFanEditor()->points() )
        m_waterCoolerFanPoints.append( { static_cast< int >( p.temp ), static_cast< int >( p.duty ) } );
  }
  else
  {
    QMessageBox::warning( this, "Apply Failed", "Failed to apply fan profiles. Check service logs or connection." );
  }
}

void MainWindow::onRevertFanProfilesClicked()
{
  loadFanPoints();
}

void MainWindow::loadFanPoints()
{
  // Load fan profile JSON for the currently selected fan profile (if custom)
  if ( m_currentFanProfile.isEmpty() ) return;

  QString json = m_profileManager->getFanProfile( m_currentFanProfile );
  QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  if (doc.isObject()) {
    QJsonObject obj = doc.object();
    if (obj.contains("tableCPU")) {
      QJsonArray arr = obj["tableCPU"].toArray();
      m_cpuFanPoints.clear();
      QVector<FanCurveEditorWidget::Point> cpuPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        int temp = o["temp"].toInt();
        int speed = o["speed"].toInt();
        m_cpuFanPoints.append({temp, speed});
        cpuPoints.append({static_cast<double>(temp), static_cast<double>(speed)});
      }
      if (m_fanControlTab->cpuEditor()) {
        m_fanControlTab->cpuEditor()->setPoints(cpuPoints);
      }
    }
    if (obj.contains("tableGPU")) {
      QJsonArray arr = obj["tableGPU"].toArray();
      m_gpuFanPoints.clear();
      QVector<FanCurveEditorWidget::Point> gpuPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        int temp = o["temp"].toInt();
        int speed = o["speed"].toInt();
        m_gpuFanPoints.append({temp, speed});
        gpuPoints.append({static_cast<double>(temp), static_cast<double>(speed)});
      }
      if (m_fanControlTab->gpuEditor()) {
        m_fanControlTab->gpuEditor()->setPoints(gpuPoints);
      }
    }
    if (obj.contains("tableWaterCoolerFan")) {
      QJsonArray arr = obj["tableWaterCoolerFan"].toArray();
      m_waterCoolerFanPoints.clear();
      QVector<FanCurveEditorWidget::Point> wcFanPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        int temp = o["temp"].toInt();
        int speed = o["speed"].toInt();
        m_waterCoolerFanPoints.append({temp, speed});
        wcFanPoints.append({static_cast<double>(temp), static_cast<double>(speed)});
      }
      if (m_fanControlTab->wcFanEditor()) {
        m_fanControlTab->wcFanEditor()->setPoints(wcFanPoints);
      }
    }
    if (obj.contains("tablePump")) {
      QJsonArray arr = obj["tablePump"].toArray();
      QVector<PumpCurveEditorWidget::Point> pumpPoints;
      for (const QJsonValue &v : arr) {
        QJsonObject o = v.toObject();
        double temp = o["temp"].toDouble();
        int speed = o["speed"].toInt();
        pumpPoints.append({temp, speed});
      }
      if (m_fanControlTab->pumpEditor() && !pumpPoints.isEmpty()) {
        m_fanControlTab->pumpEditor()->setPoints(pumpPoints);
      }
    }
  }
}


void MainWindow::saveFanPoints()
{
  QJsonObject obj;

  // Get CPU points from the editor
  QJsonArray cpuArr;
  if (m_fanControlTab->cpuEditor()) {
    const auto& cpuPoints = m_fanControlTab->cpuEditor()->points();
    for (const auto &p : cpuPoints) {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.duty;
      cpuArr.append(o);
    }
  }
  obj["tableCPU"] = cpuArr;

  // Get GPU points from the editor
  QJsonArray gpuArr;
  if (m_fanControlTab->gpuEditor()) {
    const auto& gpuPoints = m_fanControlTab->gpuEditor()->points();
    for (const auto &p : gpuPoints) {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.duty;
      gpuArr.append(o);
    }
  }
  obj["tableGPU"] = gpuArr;

  // Get water cooler fan points from the editor
  QJsonArray wcFanArr;
  if (m_fanControlTab->wcFanEditor()) {
    const auto& wcFanPoints = m_fanControlTab->wcFanEditor()->points();
    for (const auto &p : wcFanPoints) {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.duty;
      wcFanArr.append(o);
    }
  }
  obj["tableWaterCoolerFan"] = wcFanArr;

  // Get pump points from the editor
  QJsonArray pumpArr;
  if (m_fanControlTab->pumpEditor()) {
    const auto& pumpPoints = m_fanControlTab->pumpEditor()->points();
    for (const auto &p : pumpPoints) {
      QJsonObject o;
      o["temp"] = p.temp;
      o["speed"] = p.level;
      pumpArr.append(o);
    }
  }
  obj["tablePump"] = pumpArr;

  QJsonDocument doc(obj);
  QString json = doc.toJson(QJsonDocument::Compact);

  const QString currentId = m_fanControlTab->fanProfileCombo() ? m_fanControlTab->fanProfileCombo()->currentData().toString() : QString();
  const QString currentName = m_fanControlTab->fanProfileCombo() ? m_fanControlTab->fanProfileCombo()->currentText() : QString();
  if ( currentId.isEmpty() ) {
    QMessageBox::warning(this, "Save Failed", "No fan profile selected to save to.");
    return;
  }

  if ( m_fanControlTab->builtinFanProfiles().contains( currentName ) ) {
    QMessageBox::warning(this, "Save Failed", "Cannot overwrite built-in fan profile. Copy it to a custom profile first.");
    return;
  }

  // Save into selected custom profile (by ID)
  m_profileManager->setFanProfile( currentId, currentName, json );
}










} // namespace ucc
