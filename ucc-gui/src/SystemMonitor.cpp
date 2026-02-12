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

#include "SystemMonitor.hpp"
#include "CommonTypes.hpp"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <iostream>

namespace ucc
{

SystemMonitor::SystemMonitor( QObject *parent )
  : QObject( parent )
  , m_client( std::make_unique< UccdClient >( this ) )
  , m_updateTimer( new QTimer( this ) )
{
  // Update metrics every 500ms when active
  connect( m_updateTimer, &QTimer::timeout, this, &SystemMonitor::updateMetrics );
  m_updateTimer->setInterval( 500 );

  // Load charging capabilities (these don't change at runtime)
  initializeChargingState();

  // Timer will be started when monitoring becomes active
}

SystemMonitor::~SystemMonitor() = default;

void SystemMonitor::updateMetrics()
{
  qDebug() << "[SystemMonitor] updateMetrics() called";
  
  // Get CPU Temperature
  {
    QString cpuTemp = "--";

    if ( auto temp = m_client->getCpuTemperature() )
    {
      qDebug() << "[SystemMonitor] CPU temp:" << *temp;
      cpuTemp = QString::number( *temp ) + "°C";
    }
    else
    {
      qDebug() << "[SystemMonitor] Failed to get CPU temperature";
    }

    if ( m_cpuTemp != cpuTemp )
    {
      m_cpuTemp = cpuTemp;
      emit cpuTempChanged();
    }
  }

  // Get CPU Frequency
  {
    QString cpuFreq = "--";

    if ( auto freq = m_client->getCpuFrequency() )
    {
      cpuFreq = QString::number( *freq ) + " MHz";
    }

    if ( m_cpuFrequency != cpuFreq )
    {
      m_cpuFrequency = cpuFreq;
      emit cpuFrequencyChanged();
    }
  }

  // Get CPU Power
  {
    QString cpuPow = "--";

    if ( auto power = m_client->getCpuPower() )
    {
      cpuPow = QString::number( *power, 'f', 1 ) + " W";
    }

    if ( m_cpuPower != cpuPow )
    {
      m_cpuPower = cpuPow;
      emit cpuPowerChanged();
    }
  }

  // Get GPU Temperature
  {
    QString gpuTemp = "--";

    if ( auto temp = m_client->getGpuTemperature() )
    {
      qDebug() << "[SystemMonitor] GPU temp:" << *temp;
      gpuTemp = QString::number( *temp ) + "°C";
    }
    else
    {
      qDebug() << "[SystemMonitor] Failed to get GPU temperature";
    }
    if ( m_gpuTemp != gpuTemp )
    {
      m_gpuTemp = gpuTemp;
      emit gpuTempChanged();
    }
  }

  // Get GPU Frequency
  {
    QString gpuFreq = "--";

    if ( auto freq = m_client->getGpuFrequency() )
    {
      qDebug() << "[SystemMonitor] GPU freq:" << *freq;
      gpuFreq = QString::number( *freq ) + " MHz";
    }
    else
    {
      qDebug() << "[SystemMonitor] Failed to get GPU frequency";
    }

    if ( m_gpuFrequency != gpuFreq )
    {
      m_gpuFrequency = gpuFreq;
      emit gpuFrequencyChanged();
    }
  }

  // Get GPU Power
  {
    QString gpuPow = "--";

    if ( auto power = m_client->getGpuPower() )
    {
      gpuPow = QString::number( *power, 'f', 1 ) + " W";
    }
    if ( m_gpuPower != gpuPow )
    {
      m_gpuPower = gpuPow;
      emit gpuPowerChanged();
    }
  }

  // Get iGPU Frequency
  {
    QString iGpuFreq = "--";

    if ( auto freq = m_client->getIGpuFrequency(); freq && *freq > 0 )  
      iGpuFreq = QString::number( *freq ) + " MHz";

    if ( m_iGpuFrequency != iGpuFreq )
    {
      m_iGpuFrequency = iGpuFreq;
      emit iGpuFrequencyChanged();
    }
  }

  // Get iGPU Power
  {
    QString iGpuPow = "--";

    if ( auto power = m_client->getIGpuPower(); power && *power > 0.0 )
      iGpuPow = QString::number( *power, 'f', 1 ) + " W";

    if ( m_iGpuPower != iGpuPow )
    {
      m_iGpuPower = iGpuPow;
      emit iGpuPowerChanged();
    }
  }

  // Get iGPU Temperature
  {
    QString iGpuTmp = "--";

    if ( auto temp = m_client->getIGpuTemperature(); temp && *temp > 0 )
      iGpuTmp = QString::number( *temp ) + "°C";

    if ( m_iGpuTemp != iGpuTmp )
    {
      m_iGpuTemp = iGpuTmp;
      emit iGpuTempChanged();
    }
  }

  // Get Fan Speed (percentage)
  {
    QString fanSpd = "--";

    if ( auto pct = m_client->getFanSpeedPercent() )
    {
      qDebug() << "[SystemMonitor] Fan speed:" << *pct << "%";
      fanSpd = QString::number( *pct ) + " %";
    }
    else
    {
      qDebug() << "[SystemMonitor] Failed to get fan speed (percent)";
    }

    if ( m_fanSpeed != fanSpd )
    {
      m_fanSpeed = fanSpd;
      emit fanSpeedChanged();
    }
  }

  // Get GPU Fan Speed (percentage)
  {
    QString fanSpd = "--";

    if ( auto pct = m_client->getGpuFanSpeedPercent() )
    {
      qDebug() << "[SystemMonitor] GPU fan speed:" << *pct << "%";
      fanSpd = QString::number( *pct ) + " %";
    }
    else
    {
      qDebug() << "[SystemMonitor] Failed to get GPU fan speed (percent)";
    }
    if ( m_gpuFanSpeed != fanSpd )
    {
      m_gpuFanSpeed = fanSpd;
      emit gpuFanSpeedChanged();
    }
  }

  // Get water cooler fan speed (percentage) if available via uccd
  {
    QString wcFan = "--";
    if ( auto pct = m_client->getWaterCoolerFanSpeed() )
    {
      qDebug() << "[SystemMonitor] Water cooler fan speed:" << *pct << "%";
      wcFan = QString::number( *pct ) + " %";
    }
    else
    {
      qDebug() << "[SystemMonitor] Water cooler fan speed not available from uccd";
    }

    if ( m_waterCoolerFanSpeed != wcFan )
    {
      m_waterCoolerFanSpeed = wcFan;
      qDebug() << "[SystemMonitor] Emitting waterCoolerFanSpeedChanged():" << m_waterCoolerFanSpeed;
      emit waterCoolerFanSpeedChanged();
    }
  }

  // Get water cooler pump level/voltage if available via uccd
  {
    QString wcPump = "--";
    if ( auto level = m_client->getWaterCoolerPumpLevel() )
    {
      wcPump = *level == static_cast< int >( ucc::PumpVoltage::V7 )  ? "Low" :
               *level == static_cast< int >( ucc::PumpVoltage::V8 )  ? "Med" :
               *level == static_cast< int >( ucc::PumpVoltage::V11 ) ? "High" :
               *level == static_cast< int >( ucc::PumpVoltage::V12 ) ? "Max" :
               *level == static_cast< int >( ucc::PumpVoltage::Off ) ? "Off" : "--";
    }
    else
    {
      qDebug() << "[SystemMonitor] Water cooler pump level not available from uccd";
    }

    if ( m_waterCoolerPumpLevel != wcPump )
    {
      m_waterCoolerPumpLevel = wcPump;
      qDebug() << "[SystemMonitor] Emitting waterCoolerPumpLevelChanged():" << m_waterCoolerPumpLevel;
      emit waterCoolerPumpLevelChanged();
    }
  }

  // Get display brightness

  if ( auto brightness = m_client->getDisplayBrightness() )
  {

    if ( m_displayBrightness != *brightness )
    {
      m_displayBrightness = *brightness;
      emit displayBrightnessChanged();
    }
  }

  // Get webcam status
  if ( auto enabled = m_client->getWebcamEnabled() )
  {
    if ( m_webcamEnabled != *enabled )
    {
      m_webcamEnabled = *enabled;
      emit webcamEnabledChanged();
    }
  }

  // Get Fn lock status
  if ( auto fnLock = m_client->getFnLock() )
  {
    if ( m_fnLock != *fnLock )
    {
      m_fnLock = *fnLock;
      emit fnLockChanged();
    }
  }
}

void SystemMonitor::setDisplayBrightness( int brightness )
{
  if ( m_client->setDisplayBrightness( brightness ) )
  {
    m_displayBrightness = brightness;
    emit displayBrightnessChanged();
  }
}

void SystemMonitor::setWebcamEnabled( bool enabled )
{
  if ( m_client->setWebcamEnabled( enabled ) )
  {
    m_webcamEnabled = enabled;
    emit webcamEnabledChanged();
  }
}

void SystemMonitor::setFnLock( bool enabled )
{
  if ( m_client->setFnLock( enabled ) )
  {
    m_fnLock = enabled;
    emit fnLockChanged();
  }
}

void SystemMonitor::refreshAll()
{
  updateMetrics();
}

// =====================================================================
//  Charging — one-time initialization
// =====================================================================

void SystemMonitor::initializeChargingState()
{
  // Charging profiles (firmware-level modes)
  if ( auto json = m_client->getChargingProfilesAvailable() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isArray() )
    {
      QStringList profiles;
      for ( const auto &v : doc.array() )
        profiles << v.toString();
      if ( m_chargingProfilesAvailable != profiles )
      {
        m_chargingProfilesAvailable = profiles;
        emit chargingProfilesAvailableChanged();
      }
    }
  }

  if ( auto profile = m_client->getCurrentChargingProfile() )
  {
    QString p = QString::fromStdString( *profile );
    if ( m_currentChargingProfile != p )
    {
      m_currentChargingProfile = p;
      emit currentChargingProfileChanged();
    }
  }

  // Charging priority (USB-C PD)
  if ( auto json = m_client->getChargingPrioritiesAvailable() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isArray() )
    {
      QStringList priorities;
      for ( const auto &v : doc.array() )
        priorities << v.toString();
      if ( m_chargingPrioritiesAvailable != priorities )
      {
        m_chargingPrioritiesAvailable = priorities;
        emit chargingPrioritiesAvailableChanged();
      }
    }
  }

  if ( auto priority = m_client->getCurrentChargingPriority() )
  {
    QString p = QString::fromStdString( *priority );
    if ( m_currentChargingPriority != p )
    {
      m_currentChargingPriority = p;
      emit currentChargingPriorityChanged();
    }
  }

  // Battery charge thresholds
  bool thresholdsAvail = false;
  if ( auto json = m_client->getChargeEndAvailableThresholds() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isArray() and not doc.array().isEmpty() )
      thresholdsAvail = true;
  }
  if ( m_chargeThresholdsAvailable != thresholdsAvail )
  {
    m_chargeThresholdsAvailable = thresholdsAvail;
    emit chargeThresholdsAvailableChanged();
  }

  if ( m_chargeThresholdsAvailable )
  {
    if ( auto val = m_client->getChargeStartThreshold() )
    {
      if ( m_chargeStartThreshold != *val )
      {
        m_chargeStartThreshold = *val;
        emit chargeStartThresholdChanged();
      }
    }

    if ( auto val = m_client->getChargeEndThreshold() )
    {
      if ( m_chargeEndThreshold != *val )
      {
        m_chargeEndThreshold = *val;
        emit chargeEndThresholdChanged();
      }
    }

    if ( auto type = m_client->getChargeType() )
    {
      QString t = QString::fromStdString( *type );
      if ( m_chargeType != t )
      {
        m_chargeType = t;
        emit chargeTypeChanged();
      }
    }
  }
}

// =====================================================================
//  Charging — setters
// =====================================================================

void SystemMonitor::setCurrentChargingProfile( const QString &profile )
{
  if ( m_client->setChargingProfile( profile.toStdString() ) )
  {
    m_currentChargingProfile = profile;
    emit currentChargingProfileChanged();
  }
}

void SystemMonitor::setCurrentChargingPriority( const QString &priority )
{
  if ( m_client->setChargingPriority( priority.toStdString() ) )
  {
    m_currentChargingPriority = priority;
    emit currentChargingPriorityChanged();
  }
}

void SystemMonitor::setChargeStartThreshold( int value )
{
  if ( m_client->setChargeStartThreshold( value ) )
  {
    m_chargeStartThreshold = value;
    emit chargeStartThresholdChanged();
  }
}

void SystemMonitor::setChargeEndThreshold( int value )
{
  if ( m_client->setChargeEndThreshold( value ) )
  {
    m_chargeEndThreshold = value;
    emit chargeEndThresholdChanged();
  }
}

void SystemMonitor::setChargeType( const QString &type )
{
  if ( m_client->setChargeType( type.toStdString() ) )
  {
    m_chargeType = type;
    emit chargeTypeChanged();
  }
}

void SystemMonitor::setMonitoringActive( bool active )
{
  if ( m_monitoringActive != active )
  {
    m_monitoringActive = active;
    emit monitoringActiveChanged();

    if ( active )
    {
      // Start monitoring - do immediate update and start timer
      qDebug() << "[SystemMonitor] Starting monitoring with 500ms interval";
      
      // Force refresh all values by clearing them first
      m_cpuTemp = "";
      m_cpuFrequency = "";
      m_cpuPower = "";
      m_gpuTemp = "";
      m_gpuFrequency = "";
      m_gpuPower = "";
      m_fanSpeed = "";
      m_gpuFanSpeed = "";
      m_waterCoolerFanSpeed = "";
      m_waterCoolerPumpLevel = "";
      
      // Start timer first - this gives uccd time to collect initial sensor data
      // The first update will happen after 500ms
      m_updateTimer->start();
      qDebug() << "[SystemMonitor] Timer started, first update in 500ms. Timer active:" << m_updateTimer->isActive();
    }
    else
    {
      // Stop monitoring
      qDebug() << "[SystemMonitor] Stopping monitoring";
      m_updateTimer->stop();
    }
  }
}

} // namespace ucc
