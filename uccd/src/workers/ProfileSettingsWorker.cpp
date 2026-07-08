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

#include "workers/ProfileSettingsWorker.hpp"
#include "PowerSupplyController.hpp"

#include <cerrno>
#include <cstring>

// =====================================================================
//  Public methods
// =====================================================================

void ProfileSettingsWorker::start()
{
  detectPlatformProfileType();

  // Hardware capabilities (TDP limits, charging, YCbCr420, NVIDIA power limits)
  // are read directly by UccDBusService::readHardwareCapabilities() before this
  // worker is created. We only need to initialise charging internal state here
  // so that the getter methods (getCurrentChargingProfile etc.) work.
  initializeChargingSettings();
}

std::vector< TDPInfo > ProfileSettingsWorker::getTDPInfo()
{
  std::vector< TDPInfo > tdpInfo;
  const auto limits = ucc::uniwill::readCpuPowerLimits( m_sysfsRoot );

  for ( const auto &limit : limits )
  {
    TDPInfo info;
    info.current = limit.current;
    info.min = limit.min;
    info.max = limit.max;
    info.descriptor = limit.descriptor;

    tdpInfo.push_back( info );
  }

  return tdpInfo;
}

ProfileSettingsWorker::TDPWriteStatus ProfileSettingsWorker::setTDPValues(
  const std::vector< uint32_t > &values )
{
  const auto limits = ucc::uniwill::readCpuPowerLimits( m_sysfsRoot );
  if ( limits.empty() or values.empty() )
    return TDPWriteStatus::Unsupported;

  bool wroteAny = false;
  bool blocked = false;

  for ( size_t i = 0; i < values.size() and i < limits.size(); ++i )
  {
    SysfsNode< int64_t > node( limits[ i ].valuePath );
    const SysfsWriteResult result = node.writeDetailed( static_cast< int64_t >( values[ i ] ) );

    if ( result )
    {
      wroteAny = true;
      continue;
    }

    if ( result.error == EACCES )
    {
      blocked = true;
      continue;
    }

    if ( result.error == ENOENT )
      return TDPWriteStatus::Unsupported;

    return TDPWriteStatus::Failed;
  }

  if ( blocked )
    return TDPWriteStatus::BlockedByDriverPolicy;

  return wroteAny ? TDPWriteStatus::Applied : TDPWriteStatus::Unsupported;
}

bool ProfileSettingsWorker::applyChargingProfile( const std::string &profileDescriptor ) noexcept
{
  (void) profileDescriptor;
  return false;
}

bool ProfileSettingsWorker::applyChargingPriority( const std::string &priorityDescriptor ) noexcept
{
  if ( not hasChargingPriority() )
    return false;

  if ( not priorityDescriptor.empty() )
    m_currentChargingPriority = ucc::uniwill::translateUsbCPowerPriority( priorityDescriptor );

  try
  {
    const std::string prioToSet = m_currentChargingPriority;
    const auto priority = ucc::uniwill::readUsbCPowerPriority( m_sysfsRoot );
    const auto priosAvailable = priority.choices;

    if ( prioToSet.empty() or
         std::ranges::find( priosAvailable, prioToSet ) == priosAvailable.end() )
      return false;

    if ( prioToSet == priority.current )
      return true;

    if ( ucc::uniwill::writeUsbCPowerPriority( priority, prioToSet ) )
    {
      syslog( LOG_INFO, "Applied charging priority '%s'", prioToSet.c_str() );
      return true;
    }
  }
  catch ( ... )
  {
    syslog( LOG_WARNING, "Failed applying charging priority" );
  }

  return false;
}

bool ProfileSettingsWorker::setChargeStartThreshold( int value ) noexcept
{
  auto battery = PowerSupplyController::getFirstBattery();
  if ( not battery )
    return false;

  const SysfsWriteResult result = battery->setChargeControlStartThresholdDetailed( value );
  if ( result )
  {
    syslog( LOG_INFO, "Set charge start threshold to %d", value );
    return true;
  }

  syslog( LOG_WARNING, "Failed writing start threshold to %s: %s",
          battery->getBasePath().c_str(),
          std::strerror( result.error ) );
  return false;
}

bool ProfileSettingsWorker::setChargeEndThreshold( int value ) noexcept
{
  auto battery = PowerSupplyController::getFirstBattery();
  if ( not battery )
    return false;

  const SysfsWriteResult result = battery->setChargeControlEndThresholdDetailed( value );
  if ( result )
  {
    syslog( LOG_INFO, "Set charge end threshold to %d", value );
    return true;
  }

  if ( battery->hasChargeControlEndThreshold() && !battery->isChargeControlEndThresholdWritable() )
  {
    syslog( LOG_WARNING,
            "Charge end threshold is read-only at %s; load uniwill-laptop with allow_charge_limit=1",
            battery->getBasePath().c_str() );
  }
  else
  {
    syslog( LOG_WARNING, "Failed writing end threshold to %s: %s",
            battery->getBasePath().c_str(),
            std::strerror( result.error ) );
  }

  return false;
}

bool ProfileSettingsWorker::setChargeType( const std::string &type ) noexcept
{
  auto battery = PowerSupplyController::getFirstBatteryWithChargeType();
  if ( not battery )
    return false;

  if ( SysfsNode< std::string >( battery->getBasePath() + "/charge_type" ).write( type ) )
  {
    syslog( LOG_INFO, "Set charge type to %s", type.c_str() );
    return true;
  }

  syslog( LOG_WARNING, "Failed writing charge type" );
  return false;
}

void ProfileSettingsWorker::validateNVIDIACTGPOffset()
{
  if ( !m_nvidiaPowerCTRLAvailable )
    return;

  const auto ctgpInfo = ucc::uniwill::readCtgpInfo( m_sysfsRoot );
  if ( ctgpInfo.isAvailable() )
  {
    int32_t expectedOffset = m_lastAppliedNVIDIAOffset;

    if ( ctgpInfo.currentOffset != expectedOffset )
    {
      std::cout << "[NVIDIAPowerCTRL] External change detected (current: " << ctgpInfo.currentOffset
                << ", expected: " << expectedOffset << "), re-applying profile" << std::endl;
      applyNVIDIACTGPOffset( expectedOffset );
    }
  }
}

// =====================================================================
//  Private methods - Platform Profile
// =====================================================================

void ProfileSettingsWorker::detectPlatformProfileType()
{
  m_platformProfile = ucc::uniwill::discover( m_sysfsRoot ).platformProfile;

  if ( m_platformProfile.isAvailable() )
  {
    if ( m_skipAcpiPlatformProfile &&
         m_platformProfile.description.find( "ACPI platform_profile" ) != std::string::npos )
    {
      m_setPlatformProfilesAvailable( {} );
      m_platformProfileType = PlatformProfileType::None;
      syslog( LOG_INFO, "ProfileSettingsWorker: Skipping ACPI platform_profile for this device" );
      return;
    }

    m_platformProfileType = PlatformProfileType::UniwillPlatformProfile;
    m_setPlatformProfilesAvailable( readPlatformProfileChoices( m_platformProfile.choicesPath ) );
    syslog( LOG_INFO, "ProfileSettingsWorker: Using %s",
            m_platformProfile.description.c_str() );
    return;
  }

  m_platformProfileType = PlatformProfileType::None;
  syslog( LOG_INFO, "ProfileSettingsWorker: No uniwill platform_profile support available" );
}

std::vector< std::string > ProfileSettingsWorker::readPlatformProfileChoices(
  const std::string &path )
{
  std::vector< std::string > profiles;

  std::ifstream file( path );
  if ( not file.is_open() )
    return profiles;

  std::string line;
  if ( std::getline( file, line ) )
  {
    std::istringstream iss( line );
    std::string profile;
    while ( iss >> profile )
    {
      profiles.push_back( profile );
    }
  }

  return profiles;
}

void ProfileSettingsWorker::applyConfiguredPlatformProfile()
{
  const UccProfile profile = m_getActiveProfile();

  switch ( m_platformProfileType )
  {
    case PlatformProfileType::UniwillPlatformProfile:
    {
      const std::vector< std::string > availableProfiles =
        readPlatformProfileChoices( m_platformProfile.choicesPath );
      m_setPlatformProfilesAvailable( availableProfiles );
      applyPlatformProfile( m_platformProfile, profile.platformProfile );
      break;
    }

    case PlatformProfileType::None:
      m_setPlatformProfilesAvailable( {} );
      break;
  }
}

void ProfileSettingsWorker::applyPlatformProfile(
  const ucc::uniwill::PlatformProfileSink &sink,
  const std::string &chosenProfileName )
{
  std::vector< std::string > availableProfiles = readPlatformProfileChoices( sink.choicesPath );

  m_setPlatformProfilesAvailable( availableProfiles );

  if ( chosenProfileName.empty() )
  {
    syslog( LOG_INFO, "ProfileSettingsWorker: No profile name specified in active profile" );
    return;
  }

  const std::string profileToApply = chosenProfileName;

  if ( auto it = std::ranges::find( availableProfiles, profileToApply );
       it == availableProfiles.end() )
  {
    syslog( LOG_WARNING, "ProfileSettingsWorker: Profile '%s' not available",
            profileToApply.c_str() );
    return;
  }

  SysfsNode< std::string > profileNode( sink.profilePath );
  if ( profileNode.write( profileToApply ) )
  {
    syslog( LOG_INFO, "ProfileSettingsWorker: Set platform profile to '%s'",
            profileToApply.c_str() );
  }
  else
  {
    syslog( LOG_WARNING, "ProfileSettingsWorker: Failed to set platform profile to '%s'",
            profileToApply.c_str() );
  }
}

// =====================================================================
//  Private methods - ODM Power Limits
// =====================================================================

void ProfileSettingsWorker::logLine( const std::string &message )
{
  if ( m_logFunction )
  {
    m_logFunction( message );
  }
  else
  {
    syslog( LOG_INFO, "%s", message.c_str() );
  }
}

void ProfileSettingsWorker::publishODMPowerLimitsJSON( const std::vector< TDPInfo > &tdpInfo )
{
  std::ostringstream jsonStream;
  jsonStream << "[";

  for ( size_t i = 0; i < tdpInfo.size(); ++i )
  {
    if ( i > 0 )
      jsonStream << ",";

    jsonStream << "{"
               << "\"current\":" << tdpInfo[ i ].current << ","
               << "\"min\":" << tdpInfo[ i ].min << ","
               << "\"max\":" << tdpInfo[ i ].max
               << "}";
  }

  jsonStream << "]";

  m_setOdmPowerLimitsJSON( jsonStream.str() );
}

void ProfileSettingsWorker::applyODMPowerLimits()
{
  logLine( "ProfileSettingsWorker: applyODMPowerLimits() called" );

  const UccProfile profile = m_getActiveProfile();
  const auto &odmPowerLimits = profile.odmPowerLimits;

  auto tdpInfo = getTDPInfo();

  if ( tdpInfo.empty() )
  {
    logLine( "ProfileSettingsWorker: No TDP hardware available" );
    m_setOdmPowerLimitsJSON( "[]" );
    return;
  }

  logLine( "ProfileSettingsWorker: Found " + std::to_string( tdpInfo.size() ) +
           " TDP descriptors" );

  if ( m_platformProfile.isAvailable() )
  {
    const std::string currentProfile =
      SysfsNode< std::string >( m_platformProfile.profilePath ).read().value_or( "" );

    if ( currentProfile != "performance" )
    {
      logLine( "ProfileSettingsWorker: CPU power limit writes disabled; current platform profile is '" +
               ( currentProfile.empty() ? std::string( "unknown" ) : currentProfile ) + "'" );
      publishODMPowerLimitsJSON( tdpInfo );
      return;
    }
  }

  std::vector< uint32_t > newTDPValues;

  if ( not odmPowerLimits.tdpValues.empty() )
  {
    for ( int val : odmPowerLimits.tdpValues )
      newTDPValues.push_back( static_cast< uint32_t >( val ) );
  }

  if ( newTDPValues.empty() )
  {
    for ( const auto &tdp : tdpInfo )
    {
      newTDPValues.push_back( tdp.max );
    }
  }

  for ( size_t i = 0; i < newTDPValues.size() && i < tdpInfo.size(); ++i )
  {
    const uint32_t minValue = tdpInfo[ i ].min;
    const uint32_t maxValue = tdpInfo[ i ].max;

    if ( minValue > maxValue )
      continue;

    if ( newTDPValues[ i ] < minValue )
    {
      logLine( "ProfileSettingsWorker: TDP[" + std::to_string( i ) + "] " +
               std::to_string( newTDPValues[ i ] ) + " W is below current minimum " +
               std::to_string( minValue ) + " W; clamping" );
      newTDPValues[ i ] = minValue;
    }
    else if ( newTDPValues[ i ] > maxValue )
    {
      logLine( "ProfileSettingsWorker: TDP[" + std::to_string( i ) + "] " +
               std::to_string( newTDPValues[ i ] ) + " W is above current profile maximum " +
               std::to_string( maxValue ) + " W; clamping" );
      newTDPValues[ i ] = maxValue;
    }
  }

  std::ostringstream logMessage;
  logMessage << "ProfileSettingsWorker: Set ODM TDPs [";

  for ( size_t i = 0; i < newTDPValues.size(); ++i )
  {
    if ( i > 0 )
      logMessage << ", ";

    logMessage << newTDPValues[ i ] << " W";
  }

  logMessage << "]";
  logLine( logMessage.str() );

  const TDPWriteStatus writeStatus = setTDPValues( newTDPValues );

  if ( writeStatus == TDPWriteStatus::Applied )
  {
    for ( size_t i = 0; i < tdpInfo.size() and i < newTDPValues.size(); ++i )
    {
      tdpInfo[ i ].current = newTDPValues[ i ];
    }
  }
  else if ( writeStatus == TDPWriteStatus::BlockedByDriverPolicy )
  {
    logLine( "ProfileSettingsWorker: Driver blocked manual TDP writes; using profile-managed PL values" );
  }
  else if ( writeStatus == TDPWriteStatus::Unsupported )
  {
    logLine( "ProfileSettingsWorker: Manual TDP writes unsupported" );
  }
  else
  {
    logLine( "ProfileSettingsWorker: Failed to write TDP values" );
  }

  publishODMPowerLimitsJSON( tdpInfo );
}

// =====================================================================
//  Private methods - Charging
// =====================================================================

void ProfileSettingsWorker::initializeChargingSettings() noexcept
{
  if ( const auto priority = ucc::uniwill::readUsbCPowerPriority( m_sysfsRoot );
       priority.isAvailable() and not priority.current.empty() )
  {
    m_currentChargingPriority = priority.current;
    syslog( LOG_INFO, "Initialized USB-C power priority: %s", m_currentChargingPriority.c_str() );
  }
}

// =====================================================================
//  Private methods - YCbCr 4:2:0
// =====================================================================

void ProfileSettingsWorker::checkYCbCr420Availability()
{
  m_ycbcr420Available = false;

  if ( m_settings.ycbcr420Workaround.empty() )
  {
    return;
  }

  for ( const auto &cardEntry : m_settings.ycbcr420Workaround )
  {
    int card = cardEntry.card;
    for ( const auto &portEntry : cardEntry.ports )
    {
      std::string port = portEntry.port;
      std::string path = "/sys/kernel/debug/dri/" + std::to_string( card ) + "/" + port +
                         "/force_yuv420_output";

      if ( fileExists( path ) )
      {
        m_ycbcr420Available = true;
        return;
      }
    }
  }
}

void ProfileSettingsWorker::applyYCbCr420Workaround()
{
  bool settings_changed = false;

  for ( const auto &cardEntry : m_settings.ycbcr420Workaround )
  {
    int card = cardEntry.card;
    for ( const auto &portEntry : cardEntry.ports )
    {
      std::string port = portEntry.port;
      bool enableYuv = portEntry.enabled;

      // Validate port to prevent path traversal (check for .. and / sequences)
      if ( port.find( ".." ) != std::string::npos || port.find( "/" ) != std::string::npos )
      {
        syslog( LOG_WARNING, "Invalid port name: %s (contains traversal sequences)", port.c_str() );
        continue;
      }

      std::string path = "/sys/kernel/debug/dri/" + std::to_string( card ) + "/" + port +
                         "/force_yuv420_output";

      if ( fileExists( path ) )
      {
        std::ifstream file( path );
        if ( file.is_open() )
        {
          char currentValue;
          file.get( currentValue );
          file.close();

          bool oldValue = ( currentValue == '1' );
          if ( oldValue != enableYuv )
          {
            std::ofstream outFile( path );
            if ( outFile.is_open() )
            {
              outFile << ( enableYuv ? "1" : "0" );
              outFile.close();
              settings_changed = true;
              std::cout << "[YCbCr420] Set " << path << " to " << ( enableYuv ? "1" : "0" )
                        << std::endl;
            }
            else
            {
              std::cerr << "[YCbCr420] Failed to write to " << path << std::endl;
            }
          }
        }
      }
    }
  }

  if ( settings_changed )
  {
    m_modeReapplyPending = true;
    std::cout << "[YCbCr420] Mode reapply pending due to YUV420 changes" << std::endl;
  }
}

// =====================================================================
//  Private methods - NVIDIA Power Control
// =====================================================================

void ProfileSettingsWorker::initNVIDIAPowerCTRL()
{
  m_nvidiaPowerCTRLAvailable = checkNVIDIAAvailability();

  if ( m_nvidiaPowerCTRLAvailable )
  {
    // Always query hardware power limits so the GUI has real values
    queryNVIDIAPowerLimits();
  }
}

bool ProfileSettingsWorker::applyNVIDIAPowerOffset( int32_t offset )
{
  if ( !m_nvidiaPowerCTRLAvailable )
    return false;

  return applyNVIDIACTGPOffset( offset );
}

bool ProfileSettingsWorker::applyNVIDIACTGPOffset( int32_t ctgpOffset )
{
  if ( !m_cTGPAdjustmentSupported )
  {
    std::cout << "[NVIDIAPowerCTRL] cTGP adjustment not supported for this device, skipping" << std::endl;
    return false;
  }

  const auto ctgpInfo = ucc::uniwill::readCtgpInfo( m_sysfsRoot );
  if ( !ctgpInfo.isAvailable() )
  {
    std::cout << "[NVIDIAPowerCTRL] cTGP sysfs node not available" << std::endl;
    return false;
  }

  const int32_t maxAdjustment = ctgpInfo.maxOffset > 0
                                  ? ctgpInfo.maxOffset
                                  : std::max< int32_t >( 0, m_nvidiaPowerCTRLMaxPowerLimit -
                                                            m_nvidiaPowerCTRLDefaultPowerLimit );
  ctgpOffset = std::clamp( ctgpOffset, 0, maxAdjustment );

  SysfsNode< int64_t > node( ctgpInfo.offsetPath );
  const SysfsWriteResult writeResult = node.writeDetailed( static_cast< int64_t >( ctgpOffset ) );

  if ( !writeResult )
  {
    std::cerr << "[NVIDIAPowerCTRL] Failed to write cTGP offset to " << ctgpInfo.offsetPath
              << " (errno " << writeResult.error << ")" << std::endl;
    return false;
  }

  // Verify the write by reading back
  const auto verifiedInfo = ucc::uniwill::readCtgpInfo( m_sysfsRoot );
  if ( verifiedInfo.isAvailable() )
  {
    const int32_t verifiedValue = verifiedInfo.currentOffset;

    // The kernel module may clamp or round the offset to hardware-supported
    // steps, so the readback can legitimately differ from what we wrote.
    // Always track the value the hardware actually accepted so that the
    // periodic validator does not fight the hardware.
    m_lastAppliedNVIDIAOffset = verifiedValue;

    if ( verifiedValue == ctgpOffset )
    {
      std::cout << "[NVIDIAPowerCTRL] Applied cTGP offset: " << ctgpOffset << std::endl;
    }
    else
    {
      std::cout << "[NVIDIAPowerCTRL] Applied cTGP offset (rounded by hardware): wrote "
                << ctgpOffset << ", hardware accepted " << verifiedValue << std::endl;
    }
    return true;
  }

  return false;
}

void ProfileSettingsWorker::queryNVIDIAPowerLimits()
{
  const auto ctgpInfo = ucc::uniwill::readCtgpInfo( m_sysfsRoot );
  if ( ctgpInfo.isAvailable() and ctgpInfo.tgpBase > 0 )
  {
    m_nvidiaPowerCTRLDefaultPowerLimit = ctgpInfo.tgpBase;
    m_nvidiaPowerCTRLMaxPowerLimit = ctgpInfo.tgpBase + std::max< int32_t >( 0, ctgpInfo.maxOffset );
  }

  if ( m_nvml && m_nvml->isAvailable() && m_nvml->deviceCount() > 0 )
  {
    if ( m_nvidiaPowerCTRLDefaultPowerLimit <= 0 )
    {
      if ( auto v = m_nvml->getPowerDefaultLimitW( 0 ) )
        m_nvidiaPowerCTRLDefaultPowerLimit = static_cast< int32_t >( *v );
    }

    if ( m_nvidiaPowerCTRLMaxPowerLimit <= 0 )
    {
      if ( auto v = m_nvml->getPowerMaxLimitW( 0 ) )
        m_nvidiaPowerCTRLMaxPowerLimit = static_cast< int32_t >( *v );
    }
  }

  std::cout << "[NVIDIAPowerCTRL] NVIDIA GPU power limits - Default: "
            << m_nvidiaPowerCTRLDefaultPowerLimit << "W, Max: " << m_nvidiaPowerCTRLMaxPowerLimit
            << "W" << std::endl;
}
