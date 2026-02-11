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

#include "DashboardTab.hpp"
#include "SystemMonitor.hpp"
#include "ProfileManager.hpp"
#include <QDBusInterface>
#include <QDBusReply>
#include <QTimer>

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QPalette>
#include <QColor>

namespace
{

QString formatFanSpeed( const QString &fanSpeed )
{
  QString display = "---";

  if ( fanSpeed.endsWith( " %" ) || fanSpeed.endsWith( "%" ) )
  {
    QString num = fanSpeed;
    if ( fanSpeed.endsWith( " %" ) )
      num = fanSpeed.left( fanSpeed.size() - 2 ).trimmed();
    else
      num = fanSpeed.left( fanSpeed.size() - 1 ).trimmed();

    bool ok = false;
    int pct = num.toInt( &ok );
    if ( ok && pct >= 0 )
      display = QString::number( pct );
  }
  else if ( fanSpeed.endsWith( " RPM" ) )
  {
    QString rpmStr = fanSpeed.left( fanSpeed.size() - 4 ).trimmed();
    bool ok = false;
    int rpm = rpmStr.toInt( &ok );

    if ( ok && rpm > 0 )
    {
      int pct = rpm / 60;
      display = QString::number( pct );
    }
  }

  return display;
}

}


namespace ucc
{

DashboardTab::DashboardTab( SystemMonitor *systemMonitor, ProfileManager *profileManager, bool waterCoolerSupported, QWidget *parent )
  : QWidget( parent )
  , m_systemMonitor( systemMonitor )
  , m_profileManager( profileManager )
  , m_waterCoolerSupported( waterCoolerSupported )
{
  setupUI();
  connectSignals();
  
  // Initialize active profile label
  m_activeProfileLabel->setText( m_profileManager->activeProfile() );
  
  // Initialize water cooler status polling only if supported
  if ( m_waterCoolerSupported )
  {
    m_waterCoolerDbus = new QDBusInterface(QStringLiteral("com.uniwill.uccd"), QStringLiteral("/com/uniwill/uccd"), QStringLiteral("com.uniwill.uccd"), QDBusConnection::systemBus(), this);
    m_waterCoolerPollTimer = new QTimer(this);
    connect(m_waterCoolerPollTimer, &QTimer::timeout, this, &DashboardTab::updateWaterCoolerStatus);
    m_waterCoolerPollTimer->start(1000);
    updateWaterCoolerStatus();
  }
}

void DashboardTab::setupUI()
{
  // Do not force a static background or text color here; allow the application's
  // palette/theme to control colors so the UI remains readable in all themes.
  QVBoxLayout *layout = new QVBoxLayout( this );
  layout->setContentsMargins( 20, 20, 20, 20 );
  layout->setSpacing( 20 );

  // Resolve palette colors once and reuse as explicit hex values so the
  // applied styles have consistent contrast in both light and dark themes.
  QPalette pal = this->palette();
  const QString textHex = pal.color(QPalette::WindowText).name();
  const QString midHex = pal.color(QPalette::Mid).name();
  const QString highlightHex = pal.color(QPalette::Highlight).name();
  const QString linkHex = pal.color(QPalette::Link).name();
  const QColor windowBg = pal.color(QPalette::Window);
  // Choose a high-contrast inner text color based on window background
  const QString innerTextHex = (windowBg.value() < 128) ? QString("#ffffff") : QString("#000000");
  const QString ringColorHex = QString("#d32f2f");

  // Title
  QLabel *titleLabel = new QLabel( "System monitor" );
  titleLabel->setStyleSheet( QString("font-size: 22px; font-weight: bold;") );
  titleLabel->setAlignment( Qt::AlignCenter );
  layout->addWidget( titleLabel );

  // Active Profile and Water Cooler Status
  QHBoxLayout *statusLayout = new QHBoxLayout();
  
  // Active Profile section
  QLabel *activeLabel = new QLabel( "Active profile:" );
  activeLabel->setStyleSheet( "font-weight: bold;" );
  m_activeProfileLabel = new QLabel( "Loading..." );
  m_activeProfileLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(textHex) );
  
  // Water Cooler Status section
  QLabel *coolerLabel = new QLabel( "Water Cooler Status:" );
  coolerLabel->setStyleSheet( "font-weight: bold;" );
  m_waterCoolerStatusLabel = new QLabel( "Disconnected" );
  m_waterCoolerStatusLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(midHex) );
  
  // Water Cooler Enable checkbox (synced with FanControlTab)
  m_waterCoolerEnableCheckBox = new QCheckBox( "Enable Water Cooler" );
  m_waterCoolerEnableCheckBox->setChecked( true );
  m_waterCoolerEnableCheckBox->setToolTip( tr( "When enabled the daemon will scan for water cooler devices" ) );

  // Hide water cooler status bar items if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    coolerLabel->setVisible( false );
    m_waterCoolerStatusLabel->setVisible( false );
    m_waterCoolerEnableCheckBox->setVisible( false );
  }

  statusLayout->addStretch();
  statusLayout->addWidget( activeLabel );
  statusLayout->addSpacing( 6 );
  statusLayout->addWidget( m_activeProfileLabel );
  statusLayout->addSpacing( 20 );  // Space between sections
  statusLayout->addWidget( m_waterCoolerEnableCheckBox );
  statusLayout->addSpacing( 12 );
  statusLayout->addWidget( coolerLabel );
  statusLayout->addSpacing( 6 );
  statusLayout->addWidget( m_waterCoolerStatusLabel );
  statusLayout->addStretch();
  layout->addLayout( statusLayout );

  auto makeGauge = [&]( const QString &caption, const QString &unit, QLabel *&valueLabel ) -> QWidget * {
    QWidget *container = new QWidget();
    QVBoxLayout *outer = new QVBoxLayout( container );
    outer->setContentsMargins( 0, 0, 0, 0 );
    outer->setSpacing( 8 );
    outer->setAlignment( Qt::AlignHCenter );

    QFrame *ring = new QFrame();
    ring->setFixedSize( 140, 140 );
    // Thicker red ring for better visibility; use explicit red to ensure contrast
    ring->setStyleSheet( QString("QFrame { border: 12px solid %1; border-radius: 70px; background: transparent; }").arg(ringColorHex) );

    QVBoxLayout *ringLayout = new QVBoxLayout( ring );
    ringLayout->setAlignment( Qt::AlignCenter );
    ringLayout->setSpacing( 2 );

    valueLabel = new QLabel( "--" );
    valueLabel->setStyleSheet( QString("font-size: 28px; font-weight: bold; color: %1; background: transparent; border: none;").arg(innerTextHex) );
    valueLabel->setAlignment( Qt::AlignCenter );

    QLabel *unitLabel = new QLabel( unit );
    unitLabel->setStyleSheet( QString("font-size: 12px; color: %1; background: transparent; border: none;").arg(innerTextHex) );
    unitLabel->setAlignment( Qt::AlignCenter );

    ringLayout->addWidget( valueLabel );
    ringLayout->addWidget( unitLabel );

    QLabel *captionLabel = new QLabel( caption );
    // Caption should follow primary window text for reliable contrast in light themes
    captionLabel->setStyleSheet( QString("font-size: 12px; color: %1;").arg(textHex) );
    captionLabel->setAlignment( Qt::AlignCenter );

    outer->addWidget( ring );
    outer->addWidget( captionLabel );
    return container;
  };

  // CPU section
  QLabel *cpuHeader = new QLabel( "Main processor monitor" );
  cpuHeader->setStyleSheet( "font-size: 14px; font-weight: bold;" );
  cpuHeader->setAlignment( Qt::AlignCenter );
  layout->addWidget( cpuHeader );

  QGridLayout *cpuGrid = new QGridLayout();
  cpuGrid->setHorizontalSpacing( 24 );
  cpuGrid->setVerticalSpacing( 16 );
  cpuGrid->addWidget( makeGauge( "CPU - Temp", "°C", m_cpuTempLabel ), 0, 0 );
  cpuGrid->addWidget( makeGauge( "CPU - Frequency", "GHz", m_cpuFrequencyLabel ), 0, 1 );
  cpuGrid->addWidget( makeGauge( "CPU - Fan", "%", m_fanSpeedLabel ), 0, 2 );
  cpuGrid->addWidget( makeGauge( "CPU - Power", "W", m_cpuPowerLabel ), 0, 3 );
  layout->addLayout( cpuGrid );

  // GPU section
  QLabel *gpuHeader = new QLabel( "Graphics card monitor" );
  gpuHeader->setStyleSheet( "font-size: 14px; font-weight: bold;" );
  gpuHeader->setAlignment( Qt::AlignCenter );
  layout->addWidget( gpuHeader );

  QGridLayout *gpuGrid = new QGridLayout();
  gpuGrid->setHorizontalSpacing( 24 );
  gpuGrid->setVerticalSpacing( 16 );
  gpuGrid->addWidget( makeGauge( "dGPU - Temp", "°C", m_gpuTempLabel ), 0, 0 );
  gpuGrid->addWidget( makeGauge( "dGPU - Frequency", "GHz", m_gpuFrequencyLabel ), 0, 1 );
  gpuGrid->addWidget( makeGauge( "dGPU - Fan", "%", m_gpuFanSpeedLabel ), 0, 2 );
  gpuGrid->addWidget( makeGauge( "dGPU - Power", "W", m_gpuPowerLabel ), 0, 3 );
  layout->addLayout( gpuGrid );

  // Water cooler section
  m_waterCoolerHeader = new QLabel( "Water Cooler Monitor" );
  m_waterCoolerHeader->setStyleSheet( "font-size: 14px; font-weight: bold;" );
  m_waterCoolerHeader->setAlignment( Qt::AlignCenter );
  layout->addWidget( m_waterCoolerHeader );

  m_waterCoolerGrid = new QGridLayout();
  m_waterCoolerGrid->setHorizontalSpacing( 24 );
  m_waterCoolerGrid->setVerticalSpacing( 16 );
  m_waterCoolerGrid->addWidget( makeGauge( "Water Cooler - Fan", "%", m_waterCoolerFanSpeedLabel ), 0, 1 );
  m_waterCoolerGrid->addWidget( makeGauge( "Water Cooler - Pump", "Level", m_waterCoolerPumpLabel ), 0, 2 );
  layout->addLayout( m_waterCoolerGrid );

  // Hide water cooler monitor section if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    m_waterCoolerHeader->setVisible( false );
    // Hide all widgets in the water cooler grid
    for ( int i = 0; i < m_waterCoolerGrid->count(); ++i )
    {
      if ( auto *w = m_waterCoolerGrid->itemAt( i )->widget() )
        w->setVisible( false );
    }
  }

  layout->addStretch();
}

void DashboardTab::connectSignals()
{
  connect( m_systemMonitor, &SystemMonitor::cpuTempChanged,
           this, &DashboardTab::onCpuTempChanged );
  connect( m_systemMonitor, &SystemMonitor::cpuFrequencyChanged,
           this, &DashboardTab::onCpuFrequencyChanged );
  connect( m_systemMonitor, &SystemMonitor::cpuPowerChanged,
           this, &DashboardTab::onCpuPowerChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuTempChanged,
           this, &DashboardTab::onGpuTempChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuFrequencyChanged,
           this, &DashboardTab::onGpuFrequencyChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuPowerChanged,
           this, &DashboardTab::onGpuPowerChanged );
  connect( m_systemMonitor, &SystemMonitor::fanSpeedChanged,
           this, &DashboardTab::onFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuFanSpeedChanged,
           this, &DashboardTab::onGpuFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::waterCoolerFanSpeedChanged,
           this, &DashboardTab::onWaterCoolerFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::waterCoolerPumpLevelChanged,
           this, &DashboardTab::onWaterCoolerPumpLevelChanged );

  // Connect to profile manager for active profile changes
  connect( m_profileManager, &ProfileManager::activeProfileIndexChanged,
           this, [this]() {
             m_activeProfileLabel->setText( m_profileManager->activeProfile() );
           } );

  Q_UNUSED(m_waterCoolerDbus)

  // Water cooler enable checkbox → emit signal for cross-tab sync
  connect( m_waterCoolerEnableCheckBox, &QCheckBox::toggled,
           this, &DashboardTab::waterCoolerEnableChanged );
}

void DashboardTab::updateWaterCoolerStatus()
{
  if ( not m_waterCoolerDbus || not m_waterCoolerHeader )
    return;

  auto setWCStatus = [ this ]( const bool connected )
  {
    for ( int i = 0; i < m_waterCoolerGrid->count(); ++i )
    {
      if ( QWidget *w = m_waterCoolerGrid->itemAt( i )->widget() )
        w->setVisible( connected );
    }

    m_waterCoolerHeader->setVisible( connected );
  };

  QDBusReply<bool> available = m_waterCoolerDbus->call(QStringLiteral("GetWaterCoolerAvailable"));
  QDBusReply<bool> connected = m_waterCoolerDbus->call(QStringLiteral("GetWaterCoolerConnected"));
  // Prefer 'connected' state when available; compute explicit hex colors
  // from the current palette so styles are consistent.
  QPalette pal = this->palette();
  const QString textHex = pal.color(QPalette::WindowText).name();
  const QString midHex = pal.color(QPalette::Mid).name();
  const QString highlightHex = pal.color(QPalette::Highlight).name();
  const QString linkHex = pal.color(QPalette::Link).name();

  if ( connected.isValid() && connected.value() )
  {
    m_waterCoolerStatusLabel->setText( QStringLiteral("Connected") );
    m_waterCoolerStatusLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(highlightHex) );
    setWCStatus( true );
  }
  else if ( available.isValid() && available.value() )
  {
    m_waterCoolerStatusLabel->setText( QStringLiteral("Available") );
    m_waterCoolerStatusLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(linkHex) );
    setWCStatus( false );
  }
  else
  {
    m_waterCoolerStatusLabel->setText( QStringLiteral("Disconnected") );
    m_waterCoolerStatusLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(midHex) );
    setWCStatus( false );
  }
}

// Dashboard slots
void DashboardTab::onCpuTempChanged()
{
  QString temp = m_systemMonitor->cpuTemp().replace( "°C", "" ).trimmed();
  bool ok = false;
  int tempValue = temp.toInt( &ok );

  if ( ok && tempValue > 0 )
  {
    m_cpuTempLabel->setText( temp );
  }
  else
  {
    m_cpuTempLabel->setText( "---" );
  }
}

void DashboardTab::onCpuFrequencyChanged()
{
  QString freq = m_systemMonitor->cpuFrequency();

  if ( freq.endsWith( " MHz" ) )
  {
    bool ok = false;
    double mhz = freq.left( freq.size() - 4 ).trimmed().toDouble( &ok );

    if ( ok )
    {
      if ( mhz > 0.0 )
      {
        double ghz = mhz / 1000.0;
        m_cpuFrequencyLabel->setText( QString::number( ghz, 'f', 1 ) );
        return;
      }
      m_cpuFrequencyLabel->setText( "--" );
      return;
    }
  }
  m_cpuFrequencyLabel->setText( freq.isEmpty() ? "--" : freq );
}

void DashboardTab::onCpuPowerChanged()
{
  QString power = m_systemMonitor->cpuPower();
  QString trimmed = power.replace( " W", "" ).trimmed();
  bool ok = false;
  double watts = trimmed.toDouble( &ok );

  if ( ok && watts > 0.0 )
  {
    m_cpuPowerLabel->setText( QString::number( watts, 'f', 1 ) );
    return;
  }
  m_cpuPowerLabel->setText( "--" );
}

void DashboardTab::onGpuTempChanged()
{
  QString temp = m_systemMonitor->gpuTemp().replace( "°C", "" ).trimmed();
  bool ok = false;
  int tempValue = temp.toInt( &ok );

  if ( ok && tempValue > 0 )
  {
    m_gpuTempLabel->setText( temp );
  }
  else
  {
    m_gpuTempLabel->setText( "---" );
  }
}

void DashboardTab::onGpuFrequencyChanged()
{
  QString freq = m_systemMonitor->gpuFrequency();

  if ( freq.endsWith( " MHz" ) )
  {
    bool ok = false;
    double mhz = freq.left( freq.size() - 4 ).trimmed().toDouble( &ok );

    if ( ok )
    {
      if ( mhz > 0.0 )
      {
        double ghz = mhz / 1000.0;
        m_gpuFrequencyLabel->setText( QString::number( ghz, 'f', 1 ) );
        return;
      }
      m_gpuFrequencyLabel->setText( "--" );
      return;
    }
  }
  m_gpuFrequencyLabel->setText( freq.isEmpty() ? "--" : freq );
}

void DashboardTab::onGpuPowerChanged()
{
  QString power = m_systemMonitor->gpuPower();
  QString trimmed = power.replace( " W", "" ).trimmed();
  bool ok = false;
  double watts = trimmed.toDouble( &ok );

  if ( ok && watts > 0.0 )
  {
    m_gpuPowerLabel->setText( QString::number( watts, 'f', 1 ) );
    return;
  }
  m_gpuPowerLabel->setText( "--" );
}

// Water cooler status slots
void DashboardTab::onWaterCoolerConnected()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDisconnected()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDiscoveryStarted()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDiscoveryFinished()
{
  // If scanning finished, refresh status
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerConnectionError( const QString &error )
{
  Q_UNUSED( error );
  updateWaterCoolerStatus();
}

// Note: status is determined by daemon; this function queries it and updates label/color
// (The DBus-backed implementation is the single source of truth.)

void DashboardTab::onFanSpeedChanged()
{
  m_fanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->cpuFanSpeed() ) );
}

void DashboardTab::onGpuFanSpeedChanged()
{
  m_gpuFanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->gpuFanSpeed() ) );
}

void DashboardTab::onWaterCoolerFanSpeedChanged()
{
  if ( m_waterCoolerFanSpeedLabel )
    m_waterCoolerFanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->waterCoolerFanSpeed() ) );
}

void DashboardTab::onWaterCoolerPumpLevelChanged()
{
  if ( m_waterCoolerPumpLabel )
  {
    QString val = m_systemMonitor->waterCoolerPumpLevel();
    if ( val.isEmpty() )
      val = "--";
    m_waterCoolerPumpLabel->setText( val );
  }
}

void DashboardTab::setWaterCoolerEnabled( bool enabled )
{
  m_waterCoolerEnableCheckBox->blockSignals( true );
  m_waterCoolerEnableCheckBox->setChecked( enabled );
  m_waterCoolerEnableCheckBox->blockSignals( false );
}

}