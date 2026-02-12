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

#include "UccDBusService.hpp"
#include "profiles/DefaultProfiles.hpp"
#include "profiles/FanProfile.hpp"
#include "StateUtils.hpp"
#include "Utils.hpp"
#include "SysfsNode.hpp"
#include <set>
#include <sstream>
#include <iomanip>
#include <map>
#include <thread>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <syslog.h>
#include <libudev.h>
#include <functional>
#include <algorithm>

namespace
{
}

// helper function to convert GPU info to JSON
std::string dgpuInfoToJSON( const DGpuInfo &info )
{
  std::ostringstream oss;
  oss << std::fixed << std::setprecision( 2 );
  oss << "{"
  << "\"temp\":" << info.m_temp << ","
      << "\"coreFrequency\":" << info.m_coreFrequency << ","
      << "\"maxCoreFrequency\":" << info.m_maxCoreFrequency << ","
      << "\"powerDraw\":" << info.m_powerDraw << ","
      << "\"maxPowerLimit\":" << info.m_maxPowerLimit << ","
      << "\"enforcedPowerLimit\":" << info.m_enforcedPowerLimit << ","
      << "\"d0MetricsUsage\":" << ( info.m_d0MetricsUsage ? "true" : "false" )
      << "}";
  return oss.str();
}

std::string igpuInfoToJSON( const IGpuInfo &info )
{
  std::ostringstream oss;
  oss << std::fixed << std::setprecision( 2 );
  oss << "{"
      << "\"temp\":" << info.m_temp << ","
      << "\"coreFrequency\":" << info.m_coreFrequency << ","
      << "\"maxCoreFrequency\":" << info.m_maxCoreFrequency << ","
      << "\"powerDraw\":" << info.m_powerDraw << ","
      << "\"vendor\":\"" << info.m_vendor << "\""
      << "}";
  return oss.str();
}

static std::string jsonEscape( const std::string &value )
{
  std::ostringstream oss;
  for ( const char c : value )
  {
    switch ( c )
    {
      case '"': oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default: oss << c; break;
    }
  }
  return oss.str();
}


static int32_t getDefaultOnlineCores()
{
  const auto cores = std::thread::hardware_concurrency();
  return cores > 0 ? static_cast< int32_t >( cores ) : -1;
}

static int32_t readSysFsInt( const std::string &path, int32_t defaultValue )
{
  std::ifstream file( path );
  if ( !file.is_open() )
    return defaultValue;
  
  int32_t value;
  if ( !( file >> value ) )
    return defaultValue;
  
  return value;
}

static int32_t getCpuMinFrequency()
{
  // Read from cpu0 cpuinfo_min_freq
  return readSysFsInt( "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", -1 );
}

static int32_t getCpuMaxFrequency()
{
  // Read from cpu0 cpuinfo_max_freq
  return readSysFsInt( "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", -1 );
}

static int32_t optionalValueOr( const std::optional< int32_t > &value, int32_t fallback )
{
  return value.has_value() ? value.value() : fallback;
}

static std::string profileToJSON( const UccProfile &profile,
                                  int32_t defaultOnlineCores,
                                  int32_t defaultScalingMin,
                                  int32_t defaultScalingMax )
{
  std::ostringstream oss;
  oss << "{"
      << "\"id\":\"" << jsonEscape( profile.id ) << "\" ,"
      << "\"name\":\"" << jsonEscape( profile.name ) << "\" ,"
      << "\"description\":\"" << jsonEscape( profile.description ) << "\" ,"
      << "\"display\":{"
      << "\"brightness\":" << profile.display.brightness << ","
      << "\"useBrightness\":" << ( profile.display.useBrightness ? "true" : "false" ) << ","
      << "\"refreshRate\":" << profile.display.refreshRate << ","
      << "\"useRefRate\":" << ( profile.display.useRefRate ? "true" : "false" ) << ","
      << "\"xResolution\":" << profile.display.xResolution << ","
      << "\"yResolution\":" << profile.display.yResolution << ","
      << "\"useResolution\":" << ( profile.display.useResolution ? "true" : "false" )
      << "},"
      << "\"cpu\":{"
      << "\"governor\":\"" << jsonEscape( profile.cpu.governor ) << "\" ,"
      << "\"energyPerformancePreference\":\"" << jsonEscape( profile.cpu.energyPerformancePreference ) << "\" ,"
      << "\"noTurbo\":" << ( profile.cpu.noTurbo ? "true" : "false" ) << ","
      << "\"onlineCores\":" << optionalValueOr( profile.cpu.onlineCores, defaultOnlineCores ) << ","
      << "\"scalingMinFrequency\":" << optionalValueOr( profile.cpu.scalingMinFrequency, defaultScalingMin ) << ","
      << "\"scalingMaxFrequency\":" << optionalValueOr( profile.cpu.scalingMaxFrequency, defaultScalingMax )
      << "},"
      << "\"webcam\":{"
      << "\"status\":" << ( profile.webcam.status ? "true" : "false" ) << ","
      << "\"useStatus\":" << ( profile.webcam.useStatus ? "true" : "false" )
      << "},"
      << "\"fan\":{"
      << "\"useControl\":" << ( profile.fan.useControl ? "true" : "false" ) << ","
      << "\"fanProfile\":\"" << jsonEscape( profile.fan.fanProfile ) << "\" ,"
      << "\"offsetFanspeed\":" << profile.fan.offsetFanspeed << ","
      << "\"sameSpeed\":" << ( profile.fan.sameSpeed ? "true" : "false" ) << ","
      << "\"autoControlWC\":" << ( profile.fan.autoControlWC ? "true" : "false" );

  // Embed fan tables if present
  if ( !profile.fan.tableCPU.empty() )
    oss << ",\"tableCPU\":" << ProfileManager::fanTableToJSON( profile.fan.tableCPU );
  if ( !profile.fan.tableGPU.empty() )
    oss << ",\"tableGPU\":" << ProfileManager::fanTableToJSON( profile.fan.tableGPU );
  if ( !profile.fan.tablePump.empty() )
    oss << ",\"tablePump\":" << ProfileManager::fanTableToJSON( profile.fan.tablePump );
  if ( !profile.fan.tableWaterCoolerFan.empty() )
    oss << ",\"tableWaterCoolerFan\":" << ProfileManager::fanTableToJSON( profile.fan.tableWaterCoolerFan );

  oss << "},"
      << "\"odmProfile\":{"
      << "\"name\":\"" << jsonEscape( profile.odmProfile.name.value_or( "" ) ) << "\""
      << "},"
      << "\"odmPowerLimits\":{"
      << "\"tdpValues\":[";

  for ( size_t i = 0; i < profile.odmPowerLimits.tdpValues.size(); ++i )
  {
    if ( i > 0 )
      oss << ",";
    oss << profile.odmPowerLimits.tdpValues[ i ];
  }

  oss << "]}";

  if ( profile.nvidiaPowerCTRLProfile.has_value() )
  {
    oss << ",\"nvidiaPowerCTRLProfile\":{"
        << "\"cTGPOffset\":" << profile.nvidiaPowerCTRLProfile->cTGPOffset
        << "}";
  }
  else
  {
    oss << ",\"nvidiaPowerCTRLProfile\":null";
  }

  // Keyboard section
  if ( !profile.keyboard.keyboardProfileData.empty() && profile.keyboard.keyboardProfileData != "{}" )
  {
    oss << ",\"keyboard\":" << profile.keyboard.keyboardProfileData;
  }
  else
  {
    oss << ",\"keyboard\":{}";
  }

  if ( !profile.keyboard.keyboardProfileName.empty() )
  {
    oss << ",\"selectedKeyboardProfile\":\"" << jsonEscape( profile.keyboard.keyboardProfileName ) << "\"";
  }

  // Charging profile (firmware-level mode, per-profile)
  if ( !profile.chargingProfile.empty() )
  {
    oss << ",\"chargingProfile\":\"" << jsonEscape( profile.chargingProfile ) << "\"";
  }
  if ( !profile.chargingPriority.empty() )
  {
    oss << ",\"chargingPriority\":\"" << jsonEscape( profile.chargingPriority ) << "\"";
  }
  if ( !profile.chargeType.empty() )
  {
    oss << ",\"chargeType\":\"" << jsonEscape( profile.chargeType ) << "\"";
  }
  if ( profile.chargeStartThreshold >= 0 )
  {
    oss << ",\"chargeStartThreshold\":" << profile.chargeStartThreshold;
  }
  if ( profile.chargeEndThreshold >= 0 )
  {
    oss << ",\"chargeEndThreshold\":" << profile.chargeEndThreshold;
  }

  oss << "}";

  return oss.str();
}


static std::string buildSettingsJSON( const std::string &keyboardBacklightStatesJSON,
                                      const std::string &chargingProfile,
                                      const TccSettings &settings )
{
  std::ostringstream oss;
  oss << "{"
      << "\"fahrenheit\":" << ( settings.fahrenheit ? "true" : "false" ) << ","
      << "\"stateMap\":{";
  
  // Serialize stateMap
  bool first = true;
  for ( const auto &[key, value] : settings.stateMap )
  {
    if ( !first )
      oss << ",";
    first = false;
    oss << "\"" << jsonEscape( key ) << "\":\"" << jsonEscape( value ) << "\"";
  }
  
  oss << "},"
      << "\"shutdownTime\":" << ( settings.shutdownTime.has_value() ? "\"" + jsonEscape( *settings.shutdownTime ) + "\"" : "null" ) << ","
      << "\"cpuSettingsEnabled\":" << ( settings.cpuSettingsEnabled ? "true" : "false" ) << ","
      << "\"fanControlEnabled\":" << ( settings.fanControlEnabled ? "true" : "false" ) << ","
      << "\"keyboardBacklightControlEnabled\":" << ( settings.keyboardBacklightControlEnabled ? "true" : "false" ) << ","
      << "\"ycbcr420Workaround\":[],"
      << "\"chargingProfile\":\"" << jsonEscape( chargingProfile ) << "\" ,"
      << "\"chargingPriority\":" << ( settings.chargingPriority.has_value() ? "\"" + jsonEscape( *settings.chargingPriority ) + "\"" : "null" ) << ","
      << "\"keyboardBacklightStates\":" << keyboardBacklightStatesJSON
      << "}";
  return oss.str();
}

// UccDBusInterfaceAdaptor implementation

UccDBusInterfaceAdaptor::UccDBusInterfaceAdaptor( QObject *parent,
                                                  UccDBusData &data,
                                                  UccDBusService *service )
  : QDBusAbstractAdaptor( parent ),
    m_data( data ),
    m_service( service ),
    m_lastDataCollectionAccess( std::chrono::steady_clock::now() )
{
  // Qt's MOC handles introspection and method dispatch automatically
  // via Q_CLASSINFO and public slots declarations
  setAutoRelaySignals( true );
  syslog( LOG_INFO, "UccDBusInterfaceAdaptor: registered interface %s", UccDBusInterfaceAdaptor::INTERFACE_NAME );
}


void UccDBusInterfaceAdaptor::resetDataCollectionTimeout()
{
  // Note: caller must hold m_data.dataMutex lock (for m_lastDataCollectionAccess)
  m_lastDataCollectionAccess = std::chrono::steady_clock::now();
  m_data.sensorDataCollectionStatus = true;
}

QVariantMap
UccDBusInterfaceAdaptor::exportFanData( const FanData &fanData )
{
  // Cast int64_t → qlonglong and int32_t → int so QtDBus can marshal them.
  // int64_t is 'long' on x86_64 Linux which QtDBus does not recognise,
  // whereas qlonglong ('long long') maps to D-Bus 'x' (INT64).
  QVariantMap speedData;
  speedData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( fanData.speed.timestamp ) );
  speedData[ "data" ] = QVariant::fromValue( static_cast< int >( fanData.speed.data ) );

  QVariantMap tempData;
  tempData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( fanData.temp.timestamp ) );
  tempData[ "data" ] = QVariant::fromValue( static_cast< int >( fanData.temp.data ) );

  QVariantMap result;
  result[ "speed" ] = QVariant::fromValue( speedData );
  result[ "temp" ] = QVariant::fromValue( tempData );
  return result;
}

// device and system information methods

QString UccDBusInterfaceAdaptor::GetDeviceName()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.device );
}

QString UccDBusInterfaceAdaptor::GetDisplayModesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.displayModes );
}

bool UccDBusInterfaceAdaptor::GetIsX11()
{
  return m_data.isX11;
}

bool UccDBusInterfaceAdaptor::TuxedoWmiAvailable()
{
  return m_data.tuxedoWmiAvailable;
}

bool UccDBusInterfaceAdaptor::FanHwmonAvailable()
{
  return m_data.fanHwmonAvailable;
}

QString UccDBusInterfaceAdaptor::UccdVersion()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.uccdVersion );
}

// fan data methods

QVariantMap
UccDBusInterfaceAdaptor::GetFanDataCPU()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  if ( m_data.fans.size() > 0 )
    return exportFanData( m_data.fans[ 0 ] );

  return {};
}

QVariantMap
UccDBusInterfaceAdaptor::GetFanDataGPU1()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  if ( m_data.fans.size() > 1 )
    return exportFanData( m_data.fans[ 1 ] );

  return {};
}

QVariantMap
UccDBusInterfaceAdaptor::GetFanDataGPU2()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  if ( m_data.fans.size() > 2 )
    return exportFanData( m_data.fans[ 2 ] );

  return {};
}

// webcam and display methods

bool UccDBusInterfaceAdaptor::WebcamSWAvailable()
{
  return m_data.webcamSwitchAvailable;
}

bool UccDBusInterfaceAdaptor::GetWebcamSWStatus()
{
  return m_data.webcamSwitchStatus;
}

bool UccDBusInterfaceAdaptor::GetForceYUV420OutputSwitchAvailable()
{
  return m_data.forceYUV420OutputSwitchAvailable;
}

bool UccDBusInterfaceAdaptor::SetDisplayRefreshRate( const QString &display, int refreshRate )
{
  // Note: display parameter is currently ignored - only works with primary display
  // TODO: Support multiple displays in the future
  (void)display;
  
  if ( m_service && m_service->m_displayWorker )
  {
    return m_service->m_displayWorker->setRefreshRate( refreshRate );
  }
  
  return false;
}

int UccDBusInterfaceAdaptor::GetDisplayBrightness()
{
  if ( m_service )
  {
    // return autosave-stored brightness (percent)
    return m_service->m_autosave.displayBrightness;
  }
  return -1;
}

bool UccDBusInterfaceAdaptor::SetDisplayBrightness( int brightness )
{
  if ( m_service )
  {
    // clamp
    if ( brightness < 0 ) brightness = 0;
    if ( brightness > 100 ) brightness = 100;

    // update autosave
    m_service->m_autosave.displayBrightness = brightness;

    // try to apply immediately via DisplayWorker if available
    if ( m_service->m_displayWorker )
    {
      return m_service->m_displayWorker->setBrightness( brightness );
    }

    // if no worker, still return true (autosave updated)
    return true;
  }
  return false;
}

// gpu information methods

QString UccDBusInterfaceAdaptor::GetDGpuInfoValuesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  resetDataCollectionTimeout();
  return QString::fromStdString( m_data.dGpuInfoValuesJSON );
}

QString UccDBusInterfaceAdaptor::GetIGpuInfoValuesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  resetDataCollectionTimeout();
  return QString::fromStdString( m_data.iGpuInfoValuesJSON );
}

QString UccDBusInterfaceAdaptor::GetCpuPowerValuesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  resetDataCollectionTimeout();
  return QString::fromStdString( m_data.cpuPowerValuesJSON );
}

// graphics methods

QString UccDBusInterfaceAdaptor::GetPrimeState()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.primeState );
}

bool UccDBusInterfaceAdaptor::ConsumeModeReapplyPending()
{
  return m_data.modeReapplyPending.exchange( false );
}

// profile methods

QString UccDBusInterfaceAdaptor::GetActiveProfileJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.activeProfileJSON );
}

bool UccDBusInterfaceAdaptor::SetFanProfileCPU( const QString &pointsJSON )
{
  if ( !m_service )
    return false;

  const std::string json = pointsJSON.toStdString();
  std::cerr << "[DBus] SetFanProfileCPU called with JSON: " << json << std::endl;

  try
  {
    auto table = ProfileManager::parseFanTableFromJSON( json );
    std::cerr << "[DBus] Parsed table size: " << table.size() << std::endl;
    if ( table.size() != 17 )
      return false;

    UccProfile profile = m_service->getCurrentProfile();
    std::cerr << "[DBus] Current profile ID: " << profile.id << std::endl;
    auto custom = m_service->getCustomProfiles();
    bool editable = false;
    for ( const auto &p : custom )
    {
      if ( p.id == profile.id ) { editable = true; break; }
    }
    std::cerr << "[DBus] Profile editable: " << editable << std::endl;
    if ( !editable )
      return false;

    // Apply as temporary table (do not persist in daemon profiles)
    if ( m_service->m_fanControlWorker )
    {
      m_service->m_fanControlWorker->applyTemporaryFanCurves( table, {} );
      return true;
    }
    return false;
  }
  catch ( ... )
  {
    return false;
  }
}

bool UccDBusInterfaceAdaptor::SetFanProfileDGPU( const QString &pointsJSON )
{
  if ( !m_service )
    return false;

  const std::string json = pointsJSON.toStdString();
  std::cerr << "[DBus] SetFanProfileDGPU called with JSON: " << json << std::endl;

  try
  {
    auto table = ProfileManager::parseFanTableFromJSON( json );
    std::cerr << "[DBus] Parsed table size: " << table.size() << std::endl;
    if ( table.size() != 17 )
      return false;

    UccProfile profile = m_service->getCurrentProfile();
    std::cerr << "[DBus] Current profile ID: " << profile.id << std::endl;
    auto custom = m_service->getCustomProfiles();
    bool editable = false;
    for ( const auto &p : custom )
    {
      if ( p.id == profile.id ) { editable = true; break; }
    }
    std::cerr << "[DBus] Profile editable: " << editable << std::endl;
    if ( !editable )
      return false;

    // Apply as temporary table (do not persist in daemon profiles)
    if ( m_service->m_fanControlWorker )
    {
      m_service->m_fanControlWorker->applyTemporaryFanCurves( {}, table );
      return true;
    }
    return false;
  }
  catch ( ... )
  {
    return false;
  }
}

bool UccDBusInterfaceAdaptor::ApplyFanProfiles( const QString &fanProfilesJSONq )
{
  if ( !m_service )
    return false;

  const std::string fanProfilesJSON = fanProfilesJSONq.toStdString();
  std::cerr << "[DBus] ApplyFanProfiles called with JSON: " << fanProfilesJSON << std::endl;

  try
  {
    // Helper: extract a JSON array by key name
    auto extractArray = [&]( const std::string &key ) -> std::string {
      std::string search = "\"" + key + "\":";
      size_t pos = fanProfilesJSON.find( search );
      if ( pos == std::string::npos ) return {};
      size_t bracketStart = fanProfilesJSON.find( '[', pos );
      if ( bracketStart == std::string::npos ) return {};
      int depth = 0;
      for ( size_t i = bracketStart; i < fanProfilesJSON.length(); ++i )
      {
        if ( fanProfilesJSON[i] == '[' ) ++depth;
        else if ( fanProfilesJSON[i] == ']' ) --depth;
        if ( depth == 0 )
          return fanProfilesJSON.substr( bracketStart, i - bracketStart + 1 );
      }
      return {};
    };

    auto parseTable = [&]( const std::string &key ) -> std::vector< FanTableEntry > {
      std::string json = extractArray( key );
      if ( json.empty() ) return {};
      auto table = ProfileManager::parseFanTableFromJSON( json );
      std::cerr << "[DBus] Parsed " << key << " table size: " << table.size() << std::endl;
      return table;
    };

    auto cpuTable            = parseTable( "cpu" );
    auto gpuTable            = parseTable( "gpu" );
    auto waterCoolerFanTable = parseTable( "waterCoolerFan" );
    auto pumpTable           = parseTable( "pump" );

    // Apply the temporary fan curves
    if ( m_service->m_fanControlWorker )
    {
      m_service->m_fanControlWorker->applyTemporaryFanCurves( cpuTable, gpuTable, waterCoolerFanTable, pumpTable );
      std::cerr << "[DBus] Applied temporary fan profiles" << std::endl;
    }
    
    return true;
  }
  catch ( ... )
  {
    return false;
  }
}

bool UccDBusInterfaceAdaptor::RevertFanProfiles()
{
  if ( !m_service )
    return false;

  std::cerr << "[DBus] RevertFanProfiles called" << std::endl;

  try
  {
    // Clear temporary fan curves by resetting the flag and reloading profile
    if ( m_service->m_fanControlWorker )
    {
      m_service->m_fanControlWorker->clearTemporaryCurves();
      std::cerr << "[DBus] Cleared temporary fan curves" << std::endl;
    }
    
    // Reload the current profile to reset fan logics
    auto profile = m_service->getCurrentProfile();
    // The onWork method will call updateFanLogicsFromProfile which will now use profile curves
    
    return true;
  }
  catch ( ... )
  {
    return false;
  }
}

bool UccDBusInterfaceAdaptor::SetTempProfile( const QString &profileName )
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  m_data.tempProfileName = profileName.toStdString();
  return true;
}

bool UccDBusInterfaceAdaptor::SetTempProfileById( const QString &id )
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  m_data.tempProfileId = id.toStdString();
  // trigger state check would be called here
  return true;
}

bool UccDBusInterfaceAdaptor::SetActiveProfile( const QString &id )
{
  // Immediately set the active profile
  return m_service->setCurrentProfileById( id.toStdString() );
}

bool UccDBusInterfaceAdaptor::ApplyProfile( const QString &profileJSON )
{
  // Apply the profile configuration sent by the GUI
  return m_service->applyProfileJSON( profileJSON.toStdString() );
}



QString UccDBusInterfaceAdaptor::GetProfilesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.profilesJSON );
}

QString UccDBusInterfaceAdaptor::GetCustomProfilesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  //std::cout << "[DBus] GetCustomProfilesJSON called, returning " 
  //          << m_data.customProfilesJSON.length() << " bytes" << std::endl;
  return QString::fromStdString( m_data.customProfilesJSON );
}

QString UccDBusInterfaceAdaptor::GetDefaultProfilesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.defaultProfilesJSON );
}

QString UccDBusInterfaceAdaptor::GetCpuFrequencyLimitsJSON()
{
  const int32_t minFreq = getCpuMinFrequency();
  const int32_t maxFreq = getCpuMaxFrequency();
  
  std::ostringstream json;
  json << "{\"min\":" << minFreq << ",\"max\":" << maxFreq << "}";
  return QString::fromStdString( json.str() );
}

QString UccDBusInterfaceAdaptor::GetDefaultValuesProfileJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.defaultValuesProfileJSON );
}

bool UccDBusInterfaceAdaptor::AddCustomProfile( const QString &profileJSON )
{
  if ( !m_service )
  {
    std::cerr << "[Profile] AddCustomProfile called but service not available" << std::endl;
    return false;
  }

  try
  {
    // Parse the profile JSON and add it
    auto profile = ProfileManager::parseProfileJSON( profileJSON.toStdString() );
    
    // Generate new ID if empty
    if ( profile.id.empty() )
    {
      profile.id = generateProfileId();
    }
    
    std::cout << "[Profile] Adding custom profile '" << profile.name 
              << "' (id: " << profile.id << ")" << std::endl;
    
    bool result = m_service->addCustomProfile( profile );

    if ( result )
    {
      std::cout << "[Profile] Successfully added profile '" << profile.name << "'" << std::endl;
    }
    else
    {
      std::cerr << "[Profile] Failed to add profile '" << profile.name << "'" << std::endl;
    }
    
    return result;
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[Profile] Exception in AddCustomProfile: " << e.what() << std::endl;
    return false;
  }
  catch ( ... )
  {
    std::cerr << "[Profile] Unknown exception in AddCustomProfile" << std::endl;
    return false;
  }
}

bool UccDBusInterfaceAdaptor::DeleteCustomProfile( const QString &profileId )
{
  if ( !m_service )
  {
    std::cerr << "[Profile] DeleteCustomProfile called but service not available" << std::endl;
    return false;
  }

  const std::string id = profileId.toStdString();
  std::cout << "[Profile] Deleting custom profile with id: " << id << std::endl;
  
  bool result = m_service->deleteCustomProfile( id );

  if ( result )
  {
    std::cout << "[Profile] Successfully deleted profile '" << id << "'" << std::endl;
  }
  else
  {
    std::cerr << "[Profile] Failed to delete profile '" << id << "' (not found or error)" << std::endl;
  }
  
  return result;
}

bool UccDBusInterfaceAdaptor::UpdateCustomProfile( const QString &profileJSON )
{
  if ( !m_service )
  {
    std::cerr << "[Profile] UpdateCustomProfile called but service not available" << std::endl;
    return false;
  }

  try
  {
    const std::string jsonStr = profileJSON.toStdString();
    std::cout << "[Profile] Received profile JSON (first 200 chars): " 
              << jsonStr.substr(0, 200) << "..." << std::endl;
    
    // Parse the profile JSON and update it
    auto profile = ProfileManager::parseProfileJSON( jsonStr );
    
    if ( profile.id.empty() )
    {
      std::cerr << "[Profile] UpdateCustomProfile called with empty profile ID" << std::endl;
      return false; // Must have an ID to update
    }
    
    std::cout << "[Profile] Updating custom profile '" << profile.name 
              << "' (id: " << profile.id << ")" << std::endl;
    std::cout << "[Profile]   Fan control: " << (profile.fan.useControl ? "enabled" : "disabled") << std::endl;
    std::cout << "[Profile]   Fan profile: " << profile.fan.fanProfile << std::endl;
    std::cout << "[Profile]   Auto control WC: " << (profile.fan.autoControlWC ? "enabled" : "disabled") << std::endl;
    std::cout << "[Profile]   Offset: " << profile.fan.offsetFanspeed << std::endl;
    std::cout << "[Profile]   Fan profile name: " << profile.fan.fanProfile << std::endl;
    
    bool result = m_service->updateCustomProfile( profile );
    
    if ( result )
    {
      std::cout << "[Profile] Successfully updated profile '" << profile.name << "'" << std::endl;
    }
    else
    {
      std::cerr << "[Profile] Failed to update profile '" << profile.name << "' (not found or error)" << std::endl;
    }
    
    return result;
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[Profile] Exception in UpdateCustomProfile: " << e.what() << std::endl;
    return false;
  }
  catch ( ... )
  {
    std::cerr << "[Profile] Unknown exception in UpdateCustomProfile" << std::endl;
    return false;
  }
}

bool UccDBusInterfaceAdaptor::SaveCustomProfile( const QString &profileJSON )
{
  if ( !m_service )
  {
    std::cerr << "[Profile] SaveCustomProfile called but service not available" << std::endl;
    return false;
  }

  try
  {
    const std::string jsonStr = profileJSON.toStdString();
    std::cout << "[Profile] Received SaveCustomProfile JSON (first 200 chars): "
              << jsonStr.substr(0, 200) << "..." << std::endl;

    // Parse the profile JSON
    auto profile = ProfileManager::parseProfileJSON( jsonStr );

    // Check if name collides with a built-in profile
    for ( const auto &builtIn : m_service->m_defaultProfiles )
    {
      if ( builtIn.name == profile.name )
      {
        std::cerr << "[Profile] SaveCustomProfile: name '" << profile.name
                  << "' collides with built-in profile, rejected" << std::endl;
        return false;
      }
    }

    // Check if a profile with the same name already exists in memory
    auto existingProfileIt = std::find_if( m_service->m_customProfiles.begin(), m_service->m_customProfiles.end(),
                                          [&profile]( const UccProfile &p ) { return p.name == profile.name; } );

    // Also check settings for profiles with the same name (in case parsing failed on load)
    std::string existingIdFromSettings;
    for ( const auto &[profileId, profileJson] : m_service->m_settings.profiles )
    {
      try
      {
        auto parsedProfile = ProfileManager::parseProfileJSON( profileJson );
        if ( parsedProfile.name == profile.name )
        {
          existingIdFromSettings = profileId;
          break;
        }
      }
      catch ( const std::exception &e )
      {
        // Skip malformed entries
        continue;
      }
    }

    bool result;
    if ( existingProfileIt != m_service->m_customProfiles.end() )
    {
      // Profile with same name exists in memory
      // Check if they have the SAME ID (genuine update) or DIFFERENT ID (name collision)
      if ( profile.id == existingProfileIt->id )
      {
        // Same profile, update it
        std::cout << "[Profile] SaveCustomProfile: updating existing profile '" << profile.name << "' (id: " << profile.id << ")" << std::endl;
        result = m_service->updateCustomProfile( profile );
      }
      else if ( !profile.id.empty() )
      {
        // Different ID but same name - GUI sent a new profile with same name
        // Respect the GUI's ID, don't overwrite it with the old one
        std::cout << "[Profile] SaveCustomProfile: received profile with new ID '" << profile.id << "' but same name as existing profile (id: " << existingProfileIt->id << ")" << std::endl;
        std::cout << "[Profile] Treating as NEW profile since IDs differ" << std::endl;
        result = m_service->addCustomProfile( profile );
      }
      else
      {
        // Received profile has no ID but same name exists - assign old ID
        profile.id = existingProfileIt->id;
        std::cout << "[Profile] SaveCustomProfile: received profile with no ID, using existing ID '" << profile.id << "'" << std::endl;
        result = m_service->updateCustomProfile( profile );
      }
    }
    else if ( !existingIdFromSettings.empty() )
    {
      // Profile with same name exists in settings but not in memory
      // Check if ID matches
      if ( profile.id == existingIdFromSettings )
      {
        // Same profile, reuse the ID
        std::cout << "[Profile] SaveCustomProfile: updating existing profile '" << profile.name << "' from settings (id: " << profile.id << ")" << std::endl;
        m_service->m_customProfiles.push_back( profile );
        result = true;
      }
      else if ( !profile.id.empty() )
      {
        // Different ID but same name - treat as new profile
        std::cout << "[Profile] SaveCustomProfile: received profile with new ID '" << profile.id << "' but same name in settings (id: " << existingIdFromSettings << ")" << std::endl;
        std::cout << "[Profile] Treating as NEW profile since IDs differ" << std::endl;
        result = m_service->addCustomProfile( profile );
      }
      else
      {
        // No ID provided, use the one from settings
        profile.id = existingIdFromSettings;
        std::cout << "[Profile] SaveCustomProfile: received profile with no ID, using existing ID from settings '" << profile.id << "'" << std::endl;
        m_service->m_customProfiles.push_back( profile );
        result = true;
      }
    }
    else
    {
      // No profile with this name exists, add as new
      if ( profile.id.empty() )
      {
        profile.id = generateProfileId();
        std::cout << "[Profile] SaveCustomProfile: adding new profile '" << profile.name << "' with generated id " << profile.id << std::endl;
      }
      else
      {
        std::cout << "[Profile] SaveCustomProfile: adding new profile '" << profile.name << "' with provided id " << profile.id << std::endl;
      }
      result = m_service->addCustomProfile( profile );
      if ( result )
      {
        std::cout << "[Profile] Successfully added profile '" << profile.name << "'" << std::endl;
      }
      else
      {
        std::cerr << "[Profile] Failed to add profile '" << profile.name << "'" << std::endl;
        return false;
      }
    }

    // Store the profile in settings for persistence using the corrected ID
    std::string storedJSON = ProfileManager::profileToJSON( profile );
    m_service->m_settings.profiles[profile.id] = storedJSON;
    
    // Clean up old profile entries with same name but different ID
    // This prevents accumulating duplicate profiles in settings
    std::vector<std::string> keysToDelete;
    for ( const auto &[mapKey, mapJson] : m_service->m_settings.profiles )
    {
      if ( mapKey != profile.id )  // Different key
      {
        try
        {
          auto parsedProfile = ProfileManager::parseProfileJSON( mapJson );
          if ( parsedProfile.name == profile.name )  // Same name
          {
            std::cout << "[Settings] Removing old profile entry with key '" << mapKey << "' (name: " << parsedProfile.name << ", same name as new id: " << profile.id << ")" << std::endl;
            keysToDelete.push_back( mapKey );
          }
        }
        catch ( const std::exception &e )
        {
          // Ignore parse errors
          continue;
        }
      }
    }
    
    // Delete the old entries
    for ( const auto &keyToDelete : keysToDelete )
    {
      m_service->m_settings.profiles.erase( keyToDelete );
    }
    
    // Also remove from m_customProfiles if it exists with old ID
    for ( auto &memProfile : m_service->m_customProfiles )
    {
      if ( memProfile.name == profile.name && memProfile.id != profile.id )
      {
        std::cout << "[Settings] Removing old profile from memory with id '" << memProfile.id << "' (name: " << memProfile.name << ")" << std::endl;
        // Mark for deletion by swapping with last element
        // Will be erased below
        memProfile.id = "";  // Mark for deletion
      }
    }
    
    // Remove marked entries
    auto it = std::remove_if( m_service->m_customProfiles.begin(), m_service->m_customProfiles.end(),
                             [](const UccProfile &p) { return p.id.empty(); } );
    if ( it != m_service->m_customProfiles.end() )
    {
      m_service->m_customProfiles.erase( it, m_service->m_customProfiles.end() );
    }
    
    // Always persist settings after saving a profile
    if ( m_service->m_settingsManager.writeSettings( m_service->m_settings ) )
    {
      std::cout << "[Settings] Settings persisted after saving profile" << std::endl;
    }
    else
    {
      std::cerr << "[Settings] Failed to persist settings after saving profile" << std::endl;
    }

    return result;
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[Profile] Exception in SaveCustomProfile: " << e.what() << std::endl;
    return false;
  }
  catch ( ... )
  {
    std::cerr << "[Profile] Unknown exception in SaveCustomProfile" << std::endl;
    return false;
  }
}

QString UccDBusInterfaceAdaptor::GetFanProfile( const QString &name )
{
  return QString::fromStdString( getFanProfileJson(name.toStdString()) );
}

QString UccDBusInterfaceAdaptor::GetFanProfileNames()
{
  std::vector< std::string > names;
  for (const auto &p : defaultFanProfiles) {
    names.push_back(p.name);
  }
  
  // Return as JSON array
  std::string json = "[";
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) json += ",";
    json += "\"" + names[i] + "\"";
  }
  json += "]";
  return QString::fromStdString( json );
}

bool UccDBusInterfaceAdaptor::SetFanProfile( const QString &name, const QString &json )
{
  return setFanProfileJson(name.toStdString(), json.toStdString());
}

// settings methods

QString UccDBusInterfaceAdaptor::GetSettingsJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.settingsJSON );
}

QString UccDBusInterfaceAdaptor::GetPowerState()
{
  // Return the current power state string (e.g. "power_ac" or "power_bat")
  try {
    // m_service owns m_currentState
    if ( m_service )
    {
      return QString::fromStdString( profileStateToString( m_service->m_currentState ) );
    }
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[DBus] GetPowerState exception: " << e.what() << std::endl;
  }
  return QString("power_ac");
}

bool UccDBusInterfaceAdaptor::SetStateMap( const QString &state, const QString &profileId )
{
  if ( !m_service )
  {
    return false;
  }
  
  const std::string stateStr = state.toStdString();
  const std::string profileIdStr = profileId.toStdString();
  std::cout << "[DBus] SetStateMap: " << stateStr << " -> " << profileIdStr << std::endl;
  
  // Verify the profile exists before updating stateMap
  if ( stateStr == "power_ac" || stateStr == "power_bat" || stateStr == "power_wc" )
  {
    // Check if profile exists in:
    // 1. m_customProfiles (parsed objects)
    // 2. m_settings.profiles (authoritative source from file)
    // 3. m_defaultProfiles (built-in profiles)
    bool profileExists = false;
    
    for ( const auto &profile : m_service->m_customProfiles )
    {
      if ( profile.id == profileIdStr )
      {
        profileExists = true;
        break;
      }
    }
    
    if ( !profileExists && m_service->m_settings.profiles.find( profileIdStr ) != m_service->m_settings.profiles.end() )
    {
      profileExists = true;
    }
    
    if ( !profileExists )
    {
      for ( const auto &profile : m_service->m_defaultProfiles )
      {
        if ( profile.id == profileIdStr )
        {
          profileExists = true;
          break;
        }
      }
    }
    
    if ( !profileExists )
    {
      std::cerr << "[DBus] SetStateMap: Profile ID '" << profileIdStr << "' does not exist, rejecting" << std::endl;
      return false;
    }
    
    // Profile exists, safe to update
    m_service->m_settings.stateMap[stateStr] = profileIdStr;
    const bool wrote = m_service->m_settingsManager.writeSettings( m_service->m_settings );
    if ( !wrote )
      std::cerr << "[Settings] Failed to persist stateMap update" << std::endl;
    m_service->updateDBusSettingsData();
    return wrote;
  }
  
  return false;
}

// odm methods

QStringList UccDBusInterfaceAdaptor::ODMProfilesAvailable()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  QStringList result;
  for ( const auto &s : m_data.odmProfilesAvailable )
    result.append( QString::fromStdString( s ) );
  return result;
}

QString UccDBusInterfaceAdaptor::ODMPowerLimitsJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.odmPowerLimitsJSON );
}

// keyboard backlight methods

QString UccDBusInterfaceAdaptor::GetKeyboardBacklightCapabilitiesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.keyboardBacklightCapabilitiesJSON );
}

QString UccDBusInterfaceAdaptor::GetKeyboardBacklightStatesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.keyboardBacklightStatesJSON );
}

bool UccDBusInterfaceAdaptor::SetKeyboardBacklightStatesJSON( const QString &keyboardBacklightStatesJSON )
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  m_data.keyboardBacklightStatesNewJSON = keyboardBacklightStatesJSON.toStdString();
  return true;
}



// fan control methods

int UccDBusInterfaceAdaptor::GetFansMinSpeed()
{
  return m_data.fansMinSpeed;
}

bool UccDBusInterfaceAdaptor::GetFansOffAvailable()
{
  return m_data.fansOffAvailable;
}

// charging methods (stubs for now)

QString UccDBusInterfaceAdaptor::GetChargingProfilesAvailable()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargingProfilesAvailable );
}

QString UccDBusInterfaceAdaptor::GetCurrentChargingProfile()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.currentChargingProfile );
}

bool UccDBusInterfaceAdaptor::SetChargingProfile( const QString &profileDescriptor )
{
  bool result = m_service->m_profileSettingsWorker->applyChargingProfile( profileDescriptor.toStdString() );
  
  if ( result )
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.currentChargingProfile = m_service->m_profileSettingsWorker->getCurrentChargingProfile();
  }
  
  return result;
}

QString UccDBusInterfaceAdaptor::GetChargingPrioritiesAvailable()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargingPrioritiesAvailable );
}

QString UccDBusInterfaceAdaptor::GetCurrentChargingPriority()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.currentChargingPriority );
}

bool UccDBusInterfaceAdaptor::SetChargingPriority( const QString &priorityDescriptor )
{
  bool result = m_service->m_profileSettingsWorker->applyChargingPriority( priorityDescriptor.toStdString() );
  
  if ( result )
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.currentChargingPriority = m_service->m_profileSettingsWorker->getCurrentChargingPriority();
  }
  
  return result;
}

QString UccDBusInterfaceAdaptor::GetChargeStartAvailableThresholds()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargeStartAvailableThresholds );
}

QString UccDBusInterfaceAdaptor::GetChargeEndAvailableThresholds()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargeEndAvailableThresholds );
}

int UccDBusInterfaceAdaptor::GetChargeStartThreshold()
{
  // Read current value directly from hardware
  return m_service->m_profileSettingsWorker->getChargeStartThreshold();
}

int UccDBusInterfaceAdaptor::GetChargeEndThreshold()
{
  // Read current value directly from hardware
  return m_service->m_profileSettingsWorker->getChargeEndThreshold();
}

bool UccDBusInterfaceAdaptor::SetChargeStartThreshold( int value )
{
  bool result = m_service->m_profileSettingsWorker->setChargeStartThreshold( value );
  
  if ( result )
    m_data.chargeStartThreshold = value;
  
  return result;
}

bool UccDBusInterfaceAdaptor::SetChargeEndThreshold( int value )
{
  bool result = m_service->m_profileSettingsWorker->setChargeEndThreshold( value );
  
  if ( result )
    m_data.chargeEndThreshold = value;
  
  return result;
}

QString UccDBusInterfaceAdaptor::GetChargeType()
{
  // Read current value directly from hardware
  return QString::fromStdString( m_service->m_profileSettingsWorker->getChargeType() );
}

bool UccDBusInterfaceAdaptor::SetChargeType( const QString &type )
{
  bool result = m_service->m_profileSettingsWorker->setChargeType( type.toStdString() );
  
  if ( result )
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.chargeType = type.toStdString();
  }
  
  return result;
}

// fn lock methods (stubs for now)

bool UccDBusInterfaceAdaptor::GetFnLockSupported()
{
  return m_service->m_fnLockController.isSupported();
}

bool UccDBusInterfaceAdaptor::GetFnLockStatus()
{
  return m_service->m_fnLockController.getStatus();
}

void UccDBusInterfaceAdaptor::SetFnLockStatus( bool status )
{
  m_service->m_fnLockController.setStatus( status );
}

// sensor data collection methods

void UccDBusInterfaceAdaptor::SetSensorDataCollectionStatus( bool status )
{
  m_data.sensorDataCollectionStatus = status;
}

bool UccDBusInterfaceAdaptor::GetSensorDataCollectionStatus()
{
  return m_data.sensorDataCollectionStatus;
}

void UccDBusInterfaceAdaptor::SetDGpuD0Metrics( bool status )
{
  m_data.d0MetricsUsage = status;
}

// nvidia power control methods

int UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLDefaultPowerLimit()
{
  return m_data.nvidiaPowerCTRLDefaultPowerLimit;
}

int UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLMaxPowerLimit()
{
  return m_data.nvidiaPowerCTRLMaxPowerLimit;
}

bool UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLAvailable()
{
  return m_data.nvidiaPowerCTRLAvailable;
}

QString UccDBusInterfaceAdaptor::GetAvailableGovernors()
{
  if ( m_service && m_service->getCpuWorker() )
  {
    auto governors = m_service->getCpuWorker()->getAvailableGovernors();
    if ( governors )
    {
      std::string json = "[";
      for ( size_t i = 0; i < governors->size(); ++i )
      {
        if ( i > 0 ) json += ",";
        json += "\"" + (*governors)[i] + "\"";
      }
      json += "]";
      return QString::fromStdString( json );
    }
  }
  return QStringLiteral("[]");
}

// water cooler methods

bool UccDBusInterfaceAdaptor::GetWaterCoolerAvailable()
{
  return m_data.waterCoolerAvailable;
}

bool UccDBusInterfaceAdaptor::GetWaterCoolerConnected()
{
  return m_data.waterCoolerConnected;
}

int UccDBusInterfaceAdaptor::GetWaterCoolerFanSpeed()
{ return m_service ? static_cast< int >( m_service->m_waterCoolerWorker->getLastFanSpeed() ) : -1; }

int UccDBusInterfaceAdaptor::GetWaterCoolerPumpLevel()
{ return m_service ? static_cast< int >( m_service->m_waterCoolerWorker->getLastPumpVoltage() ) : -1; }

bool UccDBusInterfaceAdaptor::EnableWaterCooler( bool enable )
{
  // Update shared DBus flag and request service to perform actions when disabling
  m_data.waterCoolerScanningEnabled = enable;
  if ( !enable )
  {
    // When scanning is disabled, mark unavailable so clients don't expect discovery
    m_data.waterCoolerAvailable = false;
  }

  // Ask service to perform any active operations (disconnect/stop) if present
  if ( m_service )
  {
    m_service->setWaterCoolerScanningEnabled( enable );
  }

  return true;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerFanSpeed( int dutyCyclePercent )
{
  if ( m_service && m_service->m_waterCoolerWorker )
    return m_service->m_waterCoolerWorker->setFanSpeed( dutyCyclePercent );

  return false;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerPumpVoltage( int voltage )
{
  if ( m_service && m_service->m_waterCoolerWorker )
    return m_service->m_waterCoolerWorker->setPumpVoltage( voltage );

  return false;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerLEDColor( int red, int green, int blue, int mode )
{
  if ( m_service && m_service->m_waterCoolerWorker )
  {
    m_service->m_waterCoolerLedMode.store( mode );

    // Temperature mode: internally use Static, daemon auto-sets color from fan speed
    const int hwMode = ( mode == static_cast< int >( ucc::RGBState::Temperature ) )
                             ? static_cast< int >( ucc::RGBState::Static )
                             : mode;

    return m_service->m_waterCoolerWorker->setLEDColor( red, green, blue, hwMode );
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerLED()
{
  if ( m_service && m_service->m_waterCoolerWorker )
  {
    return m_service->m_waterCoolerWorker->turnOffLED();
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerFan()
{
  if ( m_service && m_service->m_waterCoolerWorker )
  {
    return m_service->m_waterCoolerWorker->turnOffFan();
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerPump()
{
  if ( m_service && m_service->m_waterCoolerWorker )
  {
    return m_service->m_waterCoolerWorker->turnOffPump();
  }
  return false;
}

bool UccDBusInterfaceAdaptor::IsWaterCoolerAutoControlEnabled()
{
  if ( m_service )
  {
    return m_service->m_activeProfile.fan.autoControlWC;
  }
  return false;
}

bool UccDBusInterfaceAdaptor::GetWaterCoolerSupported()
{
  return m_data.waterCoolerSupported;
}

bool UccDBusInterfaceAdaptor::GetCTGPAdjustmentSupported()
{
  return m_data.cTGPAdjustmentSupported;
}

// signal emitters
// These may be called from the DaemonWorker thread, but the adaptor lives in
// the main thread.  Use QMetaObject::invokeMethod with a queued connection so
// the actual emit happens in the object's owning thread.

void UccDBusInterfaceAdaptor::emitModeReapplyPendingChanged( bool pending )
{
  QMetaObject::invokeMethod( this, [this, pending]() {
    emit ModeReapplyPendingChanged( pending );
  }, Qt::QueuedConnection );
}

void UccDBusInterfaceAdaptor::emitProfileChanged( const std::string &profileId )
{
  QString id = QString::fromStdString( profileId );
  QMetaObject::invokeMethod( this, [this, id]() {
    emit ProfileChanged( id );
  }, Qt::QueuedConnection );
}

void UccDBusInterfaceAdaptor::emitPowerStateChanged( const std::string &state )
{
  QString s = QString::fromStdString( state );
  QMetaObject::invokeMethod( this, [this, s]() {
    emit PowerStateChanged( s );
  }, Qt::QueuedConnection );
}

void UccDBusInterfaceAdaptor::emitWaterCoolerStatusChanged( const std::string &status )
{
  QString s = QString::fromStdString( status );
  QMetaObject::invokeMethod( this, [this, s]() {
    emit WaterCoolerStatusChanged( s );
  }, Qt::QueuedConnection );
}

// UccDBusService implementation

UccDBusService::UccDBusService()
  : DaemonWorker( std::chrono::milliseconds( 1000 ), false ),
    m_dbusData(),
    m_io(),
    m_dbusObject( nullptr ),
    m_adaptor( nullptr ),
    m_started( false ),
    m_profileManager(),
    m_settingsManager(),
    m_settings(),
    m_activeProfile(),
    m_defaultProfiles(),
    m_customProfiles(),
    m_currentState( ProfileState::AC ),
    m_currentStateProfileId(),
    m_previousWaterCoolerConnected( false ),
    m_waterCoolerWorker( std::make_unique<LCTWaterCoolerWorker>( m_dbusData ) )
{
  // set daemon version
  m_dbusData.uccdVersion = "2.1.21";
  
  // identify and set device
  auto device = identifyDevice();
  m_deviceId = device;
  if ( device.has_value() )
  {
    m_dbusData.device = std::to_string( static_cast< int >( device.value() ) );
  }
  else
  {
    m_dbusData.device = "";
  }
  
  // compute device-specific feature flags (aquaris, cTGP)
  computeDeviceCapabilities();
  
  // detect display session type and initialize display modes
  initializeDisplayModes();
  
  // check tuxedo wmi availability
  m_dbusData.tuxedoWmiAvailable = m_io.wmiAvailable();

  // set default system JSON values (match TypeScript daemon structure)
  m_dbusData.primeState = "-1";
  m_dbusData.dGpuInfoValuesJSON = "{\"temp\":-1,\"powerDraw\":-1,\"maxPowerLimit\":-1,\"enforcedPowerLimit\":-1,\"coreFrequency\":-1,\"maxCoreFrequency\":-1}";
  m_dbusData.iGpuInfoValuesJSON = "{\"vendor\":\"unknown\",\"temp\":-1,\"coreFrequency\":-1,\"maxCoreFrequency\":-1,\"powerDraw\":-1}";
  // cpuPowerValuesJSON is set by CpuPowerWorker
  m_dbusData.nvidiaPowerCTRLAvailable = true;
  m_dbusData.nvidiaPowerCTRLDefaultPowerLimit = 95;
  m_dbusData.nvidiaPowerCTRLMaxPowerLimit = 175;
  m_dbusData.odmPowerLimitsJSON = "[{\"min\":25,\"max\":162,\"current\":162,\"descriptor\":\"pl1\"},{\"min\":25,\"max\":162,\"current\":162,\"descriptor\":\"pl2\"},{\"min\":25,\"max\":195,\"current\":195,\"descriptor\":\"pl4\"}]";
  m_dbusData.chargingProfilesAvailable = "[\"high_capacity\",\"balanced\",\"stationary\"]";
  m_dbusData.currentChargingProfile = "balanced";
  m_dbusData.chargingPrioritiesAvailable = "[]";
  m_dbusData.currentChargingPriority = "";
  m_dbusData.chargeStartAvailableThresholds = "[]";
  m_dbusData.chargeEndAvailableThresholds = "[]";
  m_dbusData.chargeStartThreshold = -1;
  m_dbusData.chargeEndThreshold = -1;
  m_dbusData.chargeType = "Unknown";
  m_dbusData.fnLockSupported = true;
  m_dbusData.fnLockStatus = false;

  // Keyboard backlight will be detected and initialized by KeyboardBacklightListener
  m_dbusData.keyboardBacklightCapabilitiesJSON = "null";
  m_dbusData.keyboardBacklightStatesJSON = "[]";
  m_dbusData.keyboardBacklightStatesNewJSON = "";
  
  // initialize profiles first (safer, doesn't start threads)
  initializeProfiles();
  
  // Load settings (creates defaults if needed)
  loadSettings();
  
  // Now build settings JSON with actual stateMap
  m_dbusData.settingsJSON = buildSettingsJSON( m_dbusData.keyboardBacklightStatesJSON,
                                               m_dbusData.currentChargingProfile,
                                               m_settings );
  
  // Load autosave
  loadAutosave();
  
  // Initialize display worker (merged backlight + refresh rate)
  m_displayWorker = std::make_unique< DisplayWorker >(
    m_autosaveManager.getAutosavePath(),
    [this]() { return m_activeProfile; },
    [this]() { return m_autosave.displayBrightness; },
    [this]( int32_t brightness ) { m_autosave.displayBrightness = brightness; },
    [this]() -> bool { return m_dbusData.isX11; },
    [this]( const std::string &json ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.displayModes = json;
    },
    [this]( bool isX11 ) { m_dbusData.isX11 = isX11; }
  );
  
  // initialize cpu worker
  m_cpuWorker = std::make_unique< CpuWorker >(
    [this]() { return m_activeProfile; },
    [this]() { return m_settings.cpuSettingsEnabled; },
    [this]( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); }
  );
  
  // initialize profile settings worker (replaces ODMPowerLimitWorker, ODMProfileWorker, ChargingWorker, YCbCr420WorkaroundWorker)
  m_profileSettingsWorker = std::make_unique< ProfileSettingsWorker >(
    m_io,
    [this]() -> UccProfile { return m_activeProfile; },
    [this]( const std::vector< std::string > &profiles ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.odmProfilesAvailable = profiles;
    },
    [this]( const std::string &json ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.odmPowerLimitsJSON = json;
    },
    [this]( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); },
    m_settings,
    m_dbusData.modeReapplyPending,
    m_dbusData.nvidiaPowerCTRLDefaultPowerLimit,
    m_dbusData.nvidiaPowerCTRLMaxPowerLimit,
    m_dbusData.nvidiaPowerCTRLAvailable,
    m_dbusData.cTGPAdjustmentSupported
  );
  
  // initialize hardware monitor worker (merged GPU info + CPU power + Prime)
  m_hardwareMonitorWorker = std::make_unique< HardwareMonitorWorker >(
    [this]( const std::string &json ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.cpuPowerValuesJSON = json;
    },
    [this]() { return m_dbusData.sensorDataCollectionStatus.load(); },
    [this]( const std::string &primeState ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.primeState = primeState;
    }
  );

  // webcam monitoring via HardwareMonitorWorker (replaces former WebcamWorker)
  m_hardwareMonitorWorker->setWebcamCallbacks(
    [this]() -> std::pair< bool, bool > {
      bool status = false;
      bool available = m_io.getWebcam( status );
      return { available, status };
    },
    [this]( bool available, bool status ) {
      m_dbusData.webcamSwitchAvailable = available;
      m_dbusData.webcamSwitchStatus = status;
    }
  );
  
  // initialize fan control worker
  m_fanControlWorker = std::make_unique< FanControlWorker >(
    m_io,
    [this]() { return m_activeProfile; },
    [this]() { return m_settings.fanControlEnabled; },
    [this]( size_t fanIndex, int64_t timestamp, int speed ) 
    {
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );

        if ( fanIndex < m_dbusData.fans.size() )
          m_dbusData.fans[fanIndex].speed.set( timestamp, speed );
      }
    },
    [this]( size_t fanIndex, int64_t timestamp, int temp )
    {
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
        if ( fanIndex < m_dbusData.fans.size() )
          m_dbusData.fans[ fanIndex ].temp.set( timestamp, temp );
      }

      // Auto-control water cooler fan and pump voltage based on CPU temperature
      if ( m_dbusData.waterCoolerConnected.load() && m_activeProfile.fan.autoControlWC && fanIndex == 0 )
      {
        try
        {
          const std::string &fpName = m_activeProfile.fan.fanProfile;
          FanProfile fp = getDefaultFanProfileByName( fpName );

          // Overlay water cooler fan table from profile or temporary curves
          if ( m_fanControlWorker && m_fanControlWorker->hasTemporaryCurves() )
          {
            const auto &wcTable = m_fanControlWorker->tempWaterCoolerFanTable();
            if ( !wcTable.empty() )
            {
              fp.tableWaterCoolerFan = wcTable;
            }
          }
          else if ( !m_activeProfile.fan.tableWaterCoolerFan.empty() )
          {
            fp.tableWaterCoolerFan = m_activeProfile.fan.tableWaterCoolerFan;
          }

          // Overlay pump table from temporary curves or from the active profile
          if ( m_fanControlWorker && m_fanControlWorker->hasTemporaryCurves() )
          {
            const auto &pTable = m_fanControlWorker->tempPumpTable();
            if ( !pTable.empty() )
            {
              fp.tablePump = pTable;
            }
          }
          else if ( !m_activeProfile.fan.tablePump.empty() )
          {
            fp.tablePump = m_activeProfile.fan.tablePump;
          }

          int32_t wcFanSpeed = fp.getWaterCoolerFanSpeedForTemp( temp );
          int bucket = wcFanSpeed <= 0 ? 0 : wcFanSpeed >= 100 ? 9 : wcFanSpeed / 10;
          m_waterCoolerWorker->setFanSpeed( bucket * 10 + 5 );

          // Temperature LED mode: compute gradient color from fan speed
          if ( m_waterCoolerLedMode.load() == static_cast< int32_t >( ucc::RGBState::Temperature ) )
          {
            const float t = static_cast< float >( std::clamp( wcFanSpeed, 0, 100 ) ) / 100.0f;
            const int ledR = static_cast< int >( t * 255.0f );
            const int ledG = 0;
            const int ledB = static_cast< int >( ( 1.0f - t ) * 255.0f );
            m_waterCoolerWorker->setLEDColor( ledR, ledG, ledB,
              static_cast< int >( ucc::RGBState::Static ) );
          }

          // Auto-control pump voltage
          const ucc::PumpVoltage pumpSpeedValue = fp.getPumpSpeedForTemp( temp );
          m_waterCoolerWorker->setPumpVoltage( static_cast<int>(pumpSpeedValue) );

          //std::cout << "[Auto WC] Temp: " << temp << "°C, Fan: " << wcFanSpeed
          //          << "%, Pump Voltage: " << static_cast<int>(pumpSpeedValue) << std::endl;
        }
        catch ( ... ) { /* ignore errors in water cooler auto-control */ }
      }
    }
  );
  
  // Update DBus availability flag from profile settings worker
  m_dbusData.forceYUV420OutputSwitchAvailable = m_profileSettingsWorker->isYCbCr420Available();
  
  // Initialize NVIDIA Power Control listener on first profile set
  // Removed from constructor to prevent automatic initialization

  // Initialize Keyboard Backlight listener
  m_keyboardBacklightListener = std::make_unique< KeyboardBacklightListener >(
    [this]( const std::string &json ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.keyboardBacklightCapabilitiesJSON = json;
    },
    [this]( const std::string &json ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.keyboardBacklightStatesJSON = json;
    },
    [this]() -> std::string {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      return m_dbusData.keyboardBacklightStatesNewJSON;
    },
    [this]( const std::string & ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.keyboardBacklightStatesNewJSON.clear();
    },
    [this]() {
      return m_settings.keyboardBacklightControlEnabled;
    }
  );
  
  // Update DBus data with charging initial states from profile settings worker
  {
    std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
    m_dbusData.chargingProfilesAvailable = m_profileSettingsWorker->getChargingProfilesAvailableJSON();
    m_dbusData.currentChargingProfile = m_profileSettingsWorker->getCurrentChargingProfile();
    m_dbusData.chargingPrioritiesAvailable = m_profileSettingsWorker->getChargingPrioritiesAvailableJSON();
    m_dbusData.currentChargingPriority = m_profileSettingsWorker->getCurrentChargingPriority();
    m_dbusData.chargeStartAvailableThresholds = m_profileSettingsWorker->getChargeStartAvailableThresholdsJSON();
    m_dbusData.chargeEndAvailableThresholds = m_profileSettingsWorker->getChargeEndAvailableThresholdsJSON();
    m_dbusData.chargeStartThreshold = m_profileSettingsWorker->getChargeStartThreshold();
    m_dbusData.chargeEndThreshold = m_profileSettingsWorker->getChargeEndThreshold();
    m_dbusData.chargeType = m_profileSettingsWorker->getChargeType();
  }
  
  // then setup gpu callback before worker starts processing
  setupGpuDataCallback();

  // fill device-specific defaults BEFORE starting workers
  fillDeviceSpecificDefaults( m_defaultProfiles );
  fillDeviceSpecificDefaults( m_customProfiles );
  serializeProfilesJSON();

  // start worker threads after all callbacks and data are ready
  m_hardwareMonitorWorker->start();
  m_displayWorker->start();
  m_cpuWorker->start();
  m_profileSettingsWorker->start();  // replaces 4 former workers (runs on main thread)
  m_fanControlWorker->start();
  m_keyboardBacklightListener->start();

  // Only start water cooler worker if device supports Aquaris
  if ( m_dbusData.waterCoolerSupported )
  {
    m_waterCoolerWorker->start();
  }
  else
  {
    syslog( LOG_INFO, "Aquaris not supported for this device - water cooler worker disabled" );
  }

  // Apply startup profile based on current power state and stateMap
  applyStartupProfile();

  // start main DBus worker loop after construction completes
  start();
}

void UccDBusService::setupGpuDataCallback()
{
  // Set up callback to update DBus data when GPU info is collected
  m_hardwareMonitorWorker->setGpuDataCallback(
    [this]( const IGpuInfo &iGpuInfo, const DGpuInfo &dGpuInfo )
    {
      // safety check - ensure we're not being called during destruction
      if ( not m_started )
        return;

      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );

      // Convert GPU structures to JSON and update DBus data
      m_dbusData.iGpuInfoValuesJSON = igpuInfoToJSON( iGpuInfo );
      m_dbusData.dGpuInfoValuesJSON = dgpuInfoToJSON( dGpuInfo );

      // Expose dGPU temperature through fan data for UI compatibility
      if ( dGpuInfo.m_temp > -1.0 and m_dbusData.fans.size() > 1 )
      {
        const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
          std::chrono::system_clock::now().time_since_epoch() ).count();

        m_dbusData.fans[ 1 ].temp.set(
          static_cast< int64_t >( now ),
          static_cast< int32_t >( std::lround( dGpuInfo.m_temp ) ) );
      }
    }
  );
}

void UccDBusService::updateFanData()
{
  int numberFans = 0;
  bool fansAvailable = m_io.getNumberFans( numberFans ) && numberFans > 0;
  
  // If getNumberFans fails, try to detect fans by reading temperature from fan 0
  if ( !fansAvailable )
  {
    int temp = -1;
    if ( m_io.getFanTemperature( 0, temp ) && temp >= 0 )
    {
      // We can read from at least fan 0, assume fans are available
      fansAvailable = true;
      numberFans = 2; // Assume CPU and GPU fans
      syslog( LOG_INFO, "UccDBusService: Detected fans by temperature reading (getNumberFans failed)" );
    }
  }

  int minSpeed = 0;
  bool fansOffAvailable = false;
  ( void ) m_io.getFansMinSpeed( minSpeed );
  ( void ) m_io.getFansOffAvailable( fansOffAvailable );

  const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
    std::chrono::system_clock::now().time_since_epoch() ).count();

  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.fanHwmonAvailable = fansAvailable;
  m_dbusData.fansMinSpeed = minSpeed;
  m_dbusData.fansOffAvailable = fansOffAvailable;

  if ( not fansAvailable )
    return;

  const int maxFans = std::min( numberFans, static_cast< int >( m_dbusData.fans.size() ) );
  for ( int fanIndex = 0; fanIndex < maxFans; ++fanIndex )
  {
    int speedPercent = -1;
    int tempCelsius = -1;

    if ( m_io.getFanSpeedPercent( fanIndex, speedPercent ) )
    {
      m_dbusData.fans[ fanIndex ].speed.set( static_cast< int64_t >( now ), speedPercent );
    }

    if ( m_io.getFanTemperature( fanIndex, tempCelsius ) )
    {
      m_dbusData.fans[ fanIndex ].temp.set( static_cast< int64_t >( now ), tempCelsius );
    }
  }
}

bool UccDBusService::initDBus()
{
  // Must be called from the main thread (before start()) so that
  // m_dbusObject lives in the main thread's event loop and
  // QDBusConnection::registerObject() can create child QObjects there.
  try
  {
    QDBusConnection bus = QDBusConnection::systemBus();
    if ( !bus.isConnected() )
    {
      syslog( LOG_ERR, "Failed to connect to system D-Bus" );
      return false;
    }

    // Create the D-Bus object in the main thread
    m_dbusObject = std::make_unique< QObject >();

    // Create the adaptor (attaches to m_dbusObject automatically)
    m_adaptor = std::make_unique< UccDBusInterfaceAdaptor >( m_dbusObject.get(), m_dbusData, this );

    // Register the object on the bus
    if ( !bus.registerObject( OBJECT_PATH, m_dbusObject.get() ) )
    {
      syslog( LOG_ERR, "Failed to register D-Bus object at %s: %s",
              OBJECT_PATH, qPrintable( bus.lastError().message() ) );
      m_adaptor.reset();
      m_dbusObject.reset();
      return false;
    }

    // Request the service name
    if ( !bus.registerService( SERVICE_NAME ) )
    {
      syslog( LOG_ERR, "Failed to register D-Bus service name %s: %s",
              SERVICE_NAME, qPrintable( bus.lastError().message() ) );
      bus.unregisterObject( OBJECT_PATH );
      m_adaptor.reset();
      m_dbusObject.reset();
      return false;
    }

    syslog( LOG_INFO, "DBus service registered on %s (Qt D-Bus)", SERVICE_NAME );
    return true;
  }
  catch ( const std::exception &e )
  {
    syslog( LOG_ERR, "DBus service error: %s", e.what() );
    return false;
  }
}

void UccDBusService::onStart()
{
  m_started = true;
}

void UccDBusService::onWork()
{
  if ( not m_started )
    return;

  // update tuxedo wmi availability (matches typescript implementation)
  m_dbusData.tuxedoWmiAvailable = m_io.wmiAvailable();

  // Periodic NVIDIA cTGP offset validation (every 5 ticks = 5 s)
  if ( m_profileSettingsWorker && m_profileSettingsWorker->isNVIDIAPowerCTRLAvailable() )
  {
    ++m_nvidiaValidationCounter;
    if ( m_nvidiaValidationCounter >= 5 )
    {
      m_nvidiaValidationCounter = 0;
      m_profileSettingsWorker->validateNVIDIACTGPOffset();
    }
  }

  // Fan data is now updated by FanControlWorker

  // check sensor data collection timeout
  auto now = std::chrono::steady_clock::now();
  if ( m_adaptor )
  {
    std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
    auto elapsed = std::chrono::duration_cast< std::chrono::milliseconds >(
      now - m_adaptor->m_lastDataCollectionAccess ).count();

    if ( elapsed > 10000 )
      m_dbusData.sensorDataCollectionStatus = false;
  }

  // STATE-BASED PROFILE SWITCHING (like TypeScript StateSwitcherWorker)
  // Disabled: uccd no longer saves or monitors settings file
  // UCC handles all profile decisions
  
  // Monitor power state and emit signals for UCC to handle
  // Skip AC/BAT changes when water cooler is connected (power_wc takes priority)
  if ( m_currentState != ProfileState::WC )
  {
    const ProfileState newState = determineState();
    const std::string stateKey = profileStateToString( newState );

    if ( newState != m_currentState )
    {
      m_currentState = newState;
      m_currentStateProfileId = m_settings.stateMap[stateKey];
      
      std::cout << "[State] Power state changed to " << stateKey << std::endl;
      
      // Emit signal for UCC to handle profile switching
      m_adaptor->emitPowerStateChanged( stateKey );
    }
  }
  
  // Check for temp profile requests
  const std::string oldActiveProfileId = m_activeProfile.id;
  const std::string oldActiveProfileName = m_activeProfile.name;
  
  // Check if a temp profile by ID was requested
  std::string profileId;
  {
    std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
    if ( m_dbusData.tempProfileId.empty() || m_dbusData.tempProfileId == oldActiveProfileId )
    {
      profileId.clear();
    }
    else
    {
      profileId = m_dbusData.tempProfileId;
      m_dbusData.tempProfileId.clear(); // Clear before applying
    }
  }

  if ( !profileId.empty() )
  {
    std::cout << "[Profile] Applying temp profile by ID: " << profileId << std::endl;
    if ( setCurrentProfileById( profileId ) )
    {
      std::cout << "[Profile] Successfully switched to profile ID: " << profileId << std::endl;
    }
    else
    {
      std::cerr << "[Profile] Failed to switch to profile ID: " << profileId << std::endl;
    }
    
    return; // Process one change per cycle
  }
  
  // Check if a temp profile by name was requested
  std::string profileName;
  {
    std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
    if ( m_dbusData.tempProfileName.empty() || m_dbusData.tempProfileName == oldActiveProfileName )
    {
      profileName.clear();
    }
    else
    {
      profileName = m_dbusData.tempProfileName;
      m_dbusData.tempProfileName.clear(); // Clear before applying
    }
  }

  if ( !profileName.empty() )
  {
    std::cout << "[Profile] Applying temp profile by name: " << profileName << std::endl;
    if ( setCurrentProfileByName( profileName ) )
    {
      std::cout << "[Profile] Successfully switched to profile: " << profileName << std::endl;
    }
    else
    {
      std::cerr << "[Profile] Failed to switch to profile: " << profileName << std::endl;
    }
    
    return; // Process one change per cycle
  }

  // emit signal if mode reapply is pending
  if ( m_dbusData.modeReapplyPending and m_adaptor )
    m_adaptor->emitModeReapplyPendingChanged( true );

  // Check water cooler connection state changes and switch power state.
  // Debounce: BLE connections are inherently unstable – the water cooler
  // may briefly disconnect (UART error) and reconnect within seconds.
  // We only act on a state change once it has been stable for a
  // configurable number of seconds (shorter for connect, longer for
  // disconnect so a quick reconnect does not trigger a power-state flip).
  bool wcConnected = m_dbusData.waterCoolerConnected;

  if ( wcConnected != m_previousWaterCoolerConnected )
  {
    // Raw flag differs from the last accepted state.
    if ( !m_wcDebouncePending || m_wcDebouncedTarget != wcConnected )
    {
      // First time we see this new value (or direction changed) – start timer.
      m_wcDebouncePending  = true;
      m_wcDebouncedTarget  = wcConnected;
      m_wcDebounceStart    = std::chrono::steady_clock::now();
    }
    else
    {
      // Still waiting for the same direction – check elapsed time.
      const int requiredSeconds = wcConnected ? WC_CONNECT_DEBOUNCE_S
                                              : WC_DISCONNECT_DEBOUNCE_S;
      auto elapsed = std::chrono::steady_clock::now() - m_wcDebounceStart;
      if ( std::chrono::duration_cast< std::chrono::seconds >( elapsed ).count()
           >= requiredSeconds )
      {
        // Stable long enough – accept the change.
        m_wcDebouncePending = false;
        m_previousWaterCoolerConnected = wcConnected;

        const std::string status = wcConnected ? "connected" : "disconnected";
        syslog( LOG_INFO, "Water cooler status changed to: %s (debounced)", status.c_str() );

        // Emit signal for applications to handle water cooler status changes
        m_adaptor->emitWaterCoolerStatusChanged( status );

        // Switch power state based on water cooler connection and apply the
        // corresponding profile so the system actually transitions.
        if ( wcConnected )
        {
          m_currentState = ProfileState::WC;
          const std::string stateKey = "power_wc";
          std::cout << "[State] Water cooler connected, switching to " << stateKey << std::endl;
          m_adaptor->emitPowerStateChanged( stateKey );
        }
        else
        {
          m_currentState = determineState();
          const std::string stateKey = profileStateToString( m_currentState );
          std::cout << "[State] Water cooler disconnected, reverting to " << stateKey << std::endl;
          m_adaptor->emitPowerStateChanged( stateKey );
        }

        applyProfileForCurrentState();
      }
    }
  }
  else
  {
    // Raw flag matches accepted state – cancel any pending debounce.
    m_wcDebouncePending = false;
  }
}

void UccDBusService::setWaterCoolerScanningEnabled( bool enable )
{
  // Caller may hold no locks; update dbus data and request worker actions.
  m_dbusData.waterCoolerScanningEnabled = enable;
  if ( !enable ) {
    m_dbusData.waterCoolerAvailable = false;
  }

  if ( !m_waterCoolerWorker )
    return;

  if ( enable ) {
    // Start scanning immediately
    m_waterCoolerWorker->startScanning();
  } else {
    // Request a graceful disconnect and stop discovery
    m_waterCoolerWorker->disconnectFromDevice();
    m_waterCoolerWorker->stopScanning();
  }
}

void UccDBusService::onExit()
{
  // Save autosave data
  saveAutosave();
  
  if ( m_started )
  {
    try
    {
      QDBusConnection bus = QDBusConnection::systemBus();
      bus.unregisterService( SERVICE_NAME );
      bus.unregisterObject( OBJECT_PATH );
      std::cout << "dbus service stopped" << std::endl;
    }
    catch ( const std::exception &e )
    {
      syslog( LOG_ERR, "DBus service error on exit: %s", e.what() );
    }
  }

  m_adaptor.reset();
  m_dbusObject.reset();
  m_started = false;
}

// profile management implementation

void UccDBusService::loadProfiles()
{
  std::cout << "[ProfileManager] Loading profiles..." << std::endl;
  
  // identify device for device-specific profiles
  auto device = identifyDevice();
  
  // load default profiles
  m_defaultProfiles = m_profileManager.getDefaultProfiles( device );
  std::cout << "[ProfileManager] Loaded " << m_defaultProfiles.size() << " default profiles" << std::endl;

  // Fill device-specific defaults (TDP values, etc.) after loading profiles
  fillDeviceSpecificDefaults( m_defaultProfiles );
  
  // Debug: Verify TDP values were filled
  std::cout << "[loadProfiles] After fillDeviceSpecificDefaults, checking TDP values:" << std::endl;
  for ( size_t i = 0; i < m_defaultProfiles.size() && i < 3; ++i )
  {
    std::cout << "[loadProfiles]   Default profile " << i << " (" << m_defaultProfiles[i].id 
              << ") has " << m_defaultProfiles[i].odmPowerLimits.tdpValues.size() << " TDP values";
    if ( !m_defaultProfiles[i].odmPowerLimits.tdpValues.empty() )
    {
      std::cout << ": [";
      for ( size_t j = 0; j < m_defaultProfiles[i].odmPowerLimits.tdpValues.size(); ++j )
      {
        if ( j > 0 ) std::cout << ", ";
        std::cout << m_defaultProfiles[i].odmPowerLimits.tdpValues[j];
      }
      std::cout << "]";
    }
    std::cout << std::endl;
  }
}

void UccDBusService::initializeProfiles()
{
  loadProfiles();

  // Don't set any active profile on startup - let UCC handle this
  // Only refresh if we already have an active profile (from autosave)
  if ( !m_activeProfile.id.empty() )
  {
    // Refresh the active profile from the reloaded profiles
    // in case it was modified
    std::string currentId = m_activeProfile.id;
    if ( !setCurrentProfileById( currentId ) )
    {
      // Profile no longer exists, clear it
      m_activeProfile = UccProfile();
    }
  }

  // update dbus data with profile JSONs
  updateDBusActiveProfileData();

  const int32_t defaultOnlineCores = getDefaultOnlineCores();
  const int32_t defaultScalingMin = getCpuMinFrequency();
  const int32_t defaultScalingMax = getCpuMaxFrequency();
  
  UccProfile baseCustomProfile = m_profileManager.getDefaultCustomProfiles()[0];
  
  // serialize all profiles to JSON
  std::ostringstream allProfilesJSON;
  allProfilesJSON << "[";
  
  auto allProfiles = getAllProfiles();
  for ( size_t i = 0; i < allProfiles.size(); ++i )
  {
    if ( i > 0 )
      allProfilesJSON << ",";
    
    allProfilesJSON << profileToJSON( allProfiles[ i ],
                      defaultOnlineCores,
                      defaultScalingMin,
                      defaultScalingMax );
  }
  allProfilesJSON << "]";

  std::ostringstream defaultProfilesJSON;
  defaultProfilesJSON << "[";
  for ( size_t i = 0; i < m_defaultProfiles.size(); ++i )
  {
    if ( i > 0 )
      defaultProfilesJSON << ",";
    
    defaultProfilesJSON << profileToJSON( m_defaultProfiles[ i ],
                        defaultOnlineCores,
                        defaultScalingMin,
                        defaultScalingMax );
  }
  defaultProfilesJSON << "]";

  std::ostringstream customProfilesJSON;
  customProfilesJSON << "[";
  for ( size_t i = 0; i < m_customProfiles.size(); ++i )
  {
    if ( i > 0 )
      customProfilesJSON << ",";
    
    customProfilesJSON << profileToJSON( m_customProfiles[ i ],
                       defaultOnlineCores,
                       defaultScalingMin,
                       defaultScalingMax );
  }
  customProfilesJSON << "]";

  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.profilesJSON = defaultProfilesJSON.str();  // Only default profiles now
  m_dbusData.defaultProfilesJSON = defaultProfilesJSON.str();
  m_dbusData.customProfilesJSON = "[]";  // Empty array since custom profiles are local
  m_dbusData.defaultValuesProfileJSON = profileToJSON( baseCustomProfile,
                                                       defaultOnlineCores,
                                                       defaultScalingMin,
                                                       defaultScalingMax );
  
  std::cout << "[DBus] Updated profile JSONs:" << std::endl;
  std::cout << "[DBus]   customProfilesJSON: " << m_dbusData.customProfilesJSON.length() << " bytes, " 
            << m_customProfiles.size() << " profiles" << std::endl;
  std::cout << "[DBus]   defaultProfilesJSON: " << m_dbusData.defaultProfilesJSON.length() << " bytes, " 
            << m_defaultProfiles.size() << " profiles" << std::endl;
  
}

UccProfile UccDBusService::getCurrentProfile() const
{
  return m_activeProfile;
}

bool UccDBusService::setCurrentProfileByName( const std::string &profileName )
{
  auto allProfiles = getAllProfiles();
  
  for ( const auto &profile : allProfiles )
  {
    if ( profile.name == profileName )
    {
      m_activeProfile = profile;
      updateDBusActiveProfileData();
      return true;
    }
  }

  // fallback to default profile
  m_activeProfile = getDefaultProfile();
  updateDBusActiveProfileData();
  return false;
}

bool UccDBusService::setCurrentProfileById( const std::string &id )
{
  auto allProfiles = getAllProfiles();
  
  for ( const auto &profile : allProfiles )
  {
    if ( profile.id == id )
    {
      std::cout << "[Profile] Switching to profile: " << profile.name << " (ID: " << id << ")" << std::endl;
      m_activeProfile = profile;
      updateDBusActiveProfileData();
      
      // apply fan curves and pump auto-control
      applyFanAndPumpSettings( profile );
      
      // apply new profile to workers
      if ( m_cpuWorker )
      {
        std::cout << "[Profile] Applying CPU settings from profile" << std::endl;
        m_cpuWorker->reapplyProfile();
      }
      if ( m_profileSettingsWorker )
      {
        std::cout << "[Profile] Applying TDP settings from profile" << std::endl;
        m_profileSettingsWorker->reapplyProfile();

        // Apply charging profile if the profile specifies one
        if ( !profile.chargingProfile.empty() && m_profileSettingsWorker->hasChargingProfile() )
        {
          std::cout << "[Profile] Applying charging profile '" << profile.chargingProfile << "'" << std::endl;
          m_profileSettingsWorker->applyChargingProfile( profile.chargingProfile );
          std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
          m_dbusData.currentChargingProfile = m_profileSettingsWorker->getCurrentChargingProfile();
        }

        // Apply charging priority if the profile specifies one
        if ( !profile.chargingPriority.empty() && m_profileSettingsWorker->hasChargingPriority() )
        {
          std::cout << "[Profile] Applying charging priority '" << profile.chargingPriority << "'" << std::endl;
          m_profileSettingsWorker->applyChargingPriority( profile.chargingPriority );
        }

        // Apply charge type and thresholds if the profile specifies them
        if ( !profile.chargeType.empty() )
        {
          std::cout << "[Profile] Applying charge type '" << profile.chargeType << "'" << std::endl;
          m_profileSettingsWorker->setChargeType( profile.chargeType );
        }
        if ( profile.chargeStartThreshold >= 0 )
        {
          std::cout << "[Profile] Applying charge start threshold " << profile.chargeStartThreshold << std::endl;
          m_profileSettingsWorker->setChargeStartThreshold( profile.chargeStartThreshold );
        }
        if ( profile.chargeEndThreshold >= 0 )
        {
          std::cout << "[Profile] Applying charge end threshold " << profile.chargeEndThreshold << std::endl;
          m_profileSettingsWorker->setChargeEndThreshold( profile.chargeEndThreshold );
        }
      }
      if ( m_profileSettingsWorker && m_profileSettingsWorker->isNVIDIAPowerCTRLAvailable() )
      {
        std::cout << "[Profile] Notifying NVIDIA power control" << std::endl;
        m_profileSettingsWorker->onNVIDIAPowerProfileChanged();
      }
      
      // Emit ProfileChanged signal for DBus clients
      if ( m_adaptor )
      {
        m_adaptor->emitProfileChanged( id );
      }
      
      return true;
    }
  }

  // fallback to default profile
  std::cout << "[Profile] Profile ID not found: " << id << ", using default" << std::endl;
  m_activeProfile = getDefaultProfile();
  updateDBusActiveProfileData();
  
  // Emit ProfileChanged signal for DBus clients
  if ( m_adaptor )
  {
    m_adaptor->emitProfileChanged( m_activeProfile.id );
  }
  
  return false;
}

bool UccDBusService::applyProfileJSON( const std::string &profileJSON )
{
  try
  {
    // Parse the profile JSON
    auto profile = m_profileManager.parseProfileJSON( profileJSON );
    
    std::cout << "[Profile] Applying profile from GUI: " << profile.name << std::endl;
    
    // Set as active profile
    m_activeProfile = profile;
    updateDBusActiveProfileData();

    // If the profile explicitly sets sameSpeed, apply it to fan worker immediately
    try
    {
      if ( m_fanControlWorker )
      {
        bool same = m_activeProfile.fan.sameSpeed;
        m_fanControlWorker->setSameSpeed( same );
        syslog( LOG_INFO, "UccDBusService: applied sameSpeed=%d from profile", same ? 1 : 0 );
      }
    }
    catch ( ... ) { /* ignore */ }

    // Try to resolve and apply fan curves: prefer embedded tables, fallback to named fan profile
    try
    {
      std::vector< FanTableEntry > cpuTable;
      std::vector< FanTableEntry > gpuTable;
      std::vector< FanTableEntry > wcFanTable;
      std::vector< FanTableEntry > pumpTable;

      // First, try embedded tables from the profile itself
      if ( profile.fan.hasEmbeddedTables() )
      {
        cpuTable = profile.fan.tableCPU;
        gpuTable = profile.fan.tableGPU;
        wcFanTable = profile.fan.tableWaterCoolerFan;
        pumpTable = profile.fan.tablePump;
        std::cout << "[Profile] Using embedded fan tables from profile" << std::endl;
      }
      else
      {
        // Fallback: resolve from named fan profile preset
        const std::string fpName = profile.fan.fanProfile;
        if ( !fpName.empty() )
        {
          FanProfile fp = getDefaultFanProfileByName( fpName );
          if ( fp.isValid() )
          {
            cpuTable = fp.tableCPU;
            gpuTable = fp.tableGPU;
            wcFanTable = fp.tableWaterCoolerFan;
            pumpTable = fp.tablePump;
            std::cout << "[Profile] Using fan tables from named profile '" << fpName << "'" << std::endl;
          }
        }
      }

      if ( m_fanControlWorker && !cpuTable.empty() )
      {
        m_fanControlWorker->applyTemporaryFanCurves( cpuTable, gpuTable, wcFanTable, pumpTable );
        std::cout << "[Profile] Applied fan curves (CPU=" << cpuTable.size()
                  << " GPU=" << gpuTable.size()
                  << " WCFan=" << wcFanTable.size()
                  << " Pump=" << pumpTable.size() << ")" << std::endl;
      }

      // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
      if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
           && !pumpTable.empty() )
      {
        int maxTemp = 0;
        {
          std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
          for ( const auto &fan : m_dbusData.fans )
            maxTemp = std::max( maxTemp, fan.temp.data );
        }
        FanProfile tempFp;
        tempFp.tablePump = pumpTable;
        m_waterCoolerWorker->setPumpVoltage( static_cast<int>( tempFp.getPumpSpeedForTemp( maxTemp ) ) );
        std::cout << "[Profile] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
      }
    }
    catch ( ... ) { /* ignore parse errors and continue applying other profile settings */ }

    // Apply to workers
    if ( m_cpuWorker )
    {
      std::cout << "[Profile] Applying CPU settings from profile" << std::endl;
      m_cpuWorker->reapplyProfile();
    }
    if ( m_profileSettingsWorker )
    {
      std::cout << "[Profile] Applying TDP settings from profile" << std::endl;
      m_profileSettingsWorker->reapplyProfile();

      // Apply charging profile if the profile specifies one
      if ( !profile.chargingProfile.empty() && m_profileSettingsWorker->hasChargingProfile() )
      {
        std::cout << "[Profile] Applying charging profile '" << profile.chargingProfile << "'" << std::endl;
        m_profileSettingsWorker->applyChargingProfile( profile.chargingProfile );
        std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
        m_dbusData.currentChargingProfile = m_profileSettingsWorker->getCurrentChargingProfile();
      }

      // Apply charging priority if the profile specifies one
      if ( !profile.chargingPriority.empty() && m_profileSettingsWorker->hasChargingPriority() )
      {
        std::cout << "[Profile] Applying charging priority '" << profile.chargingPriority << "'" << std::endl;
        m_profileSettingsWorker->applyChargingPriority( profile.chargingPriority );
      }

      // Apply charge type and thresholds if the profile specifies them
      if ( !profile.chargeType.empty() )
      {
        std::cout << "[Profile] Applying charge type '" << profile.chargeType << "'" << std::endl;
        m_profileSettingsWorker->setChargeType( profile.chargeType );
      }
      if ( profile.chargeStartThreshold >= 0 )
      {
        std::cout << "[Profile] Applying charge start threshold " << profile.chargeStartThreshold << std::endl;
        m_profileSettingsWorker->setChargeStartThreshold( profile.chargeStartThreshold );
      }
      if ( profile.chargeEndThreshold >= 0 )
      {
        std::cout << "[Profile] Applying charge end threshold " << profile.chargeEndThreshold << std::endl;
        m_profileSettingsWorker->setChargeEndThreshold( profile.chargeEndThreshold );
      }
    }
    
    // Apply keyboard backlight settings from profile
    if ( m_keyboardBacklightListener && !profile.keyboard.keyboardProfileData.empty() && profile.keyboard.keyboardProfileData != "{}" )
    {
      std::cout << "[Profile] Applying keyboard backlight settings from profile" << std::endl;
      m_keyboardBacklightListener->applyProfileKeyboardStates( profile.keyboard.keyboardProfileData );
    }
    
    // Emit ProfileChanged signal for DBus clients
    if ( m_adaptor )
    {
      m_adaptor->emitProfileChanged( profile.id );
    }
    
    return true;
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[Profile] Failed to apply profile JSON: " << e.what() << std::endl;
    return false;
  }
}


std::vector< UccProfile > UccDBusService::getAllProfiles() const
{
  std::vector< UccProfile > allProfiles;
  allProfiles.reserve( m_defaultProfiles.size() + m_customProfiles.size() );
  
  allProfiles.insert( allProfiles.end(), m_defaultProfiles.begin(), m_defaultProfiles.end() );
  allProfiles.insert( allProfiles.end(), m_customProfiles.begin(), m_customProfiles.end() );
  
  return allProfiles;
}

std::vector< UccProfile > UccDBusService::getDefaultProfiles() const
{
  return m_defaultProfiles;
}

std::vector< UccProfile > UccDBusService::getCustomProfiles() const
{
  return m_customProfiles;
}

UccProfile UccDBusService::getDefaultProfile() const
{
  if ( not m_defaultProfiles.empty() )
    return m_defaultProfiles[0];
  
  if ( not m_customProfiles.empty() )
    return m_customProfiles[0];
  
  // ultimate fallback
  return defaultCustomProfile;
}

void UccDBusService::updateDBusActiveProfileData()
{
  const int32_t defaultOnlineCores = getDefaultOnlineCores();
  const int32_t defaultScalingMin = -1;
  const int32_t defaultScalingMax = -1;

  std::string profileJSON = profileToJSON( m_activeProfile,
                                           defaultOnlineCores,
                                           defaultScalingMin,
                                           defaultScalingMax );
  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.activeProfileJSON = profileJSON;
}

void UccDBusService::updateDBusSettingsData()
{
  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.settingsJSON = buildSettingsJSON( m_dbusData.keyboardBacklightStatesJSON,
                                               m_dbusData.currentChargingProfile,
                                               m_settings );
}

bool UccDBusService::addCustomProfile( const UccProfile &profile )
{
  std::cout << "[ProfileManager] Adding profile '" << profile.name << "' to memory" << std::endl;
  
  // Add to in-memory profiles
  m_customProfiles.push_back( profile );
  
  // Update DBus data
  updateDBusActiveProfileData();
  
  std::cout << "[ProfileManager] Profile added successfully" << std::endl;
  return true;
}

bool UccDBusService::deleteCustomProfile( const std::string &profileId )
{
  std::cout << "[ProfileManager] Deleting profile '" << profileId << "' from memory" << std::endl;
  
  // Remove from in-memory profiles
  auto it = std::remove_if( m_customProfiles.begin(), m_customProfiles.end(),
                           [&profileId]( const UccProfile &p ) { return p.id == profileId; } );
  
  if ( it != m_customProfiles.end() )
  {
    m_customProfiles.erase( it, m_customProfiles.end() );
    
    // Update DBus data
    updateDBusActiveProfileData();
    
    std::cout << "[ProfileManager] Profile deleted successfully" << std::endl;
    return true;
  }
  
  std::cerr << "[ProfileManager] Profile not found" << std::endl;
  return false;
}

bool UccDBusService::updateCustomProfile( const UccProfile &profile )
{
  std::cout << "[ProfileManager] Updating profile '" << profile.name << "' in memory" << std::endl;
  
  // Check if this is a default (hardcoded) profile
  bool isDefaultProfile = false;
  for ( const auto &defaultProf : m_defaultProfiles )
  {
    if ( defaultProf.id == profile.id )
    {
      isDefaultProfile = true;
      break;
    }
  }
  
  if ( isDefaultProfile )
  {
    std::cout << "[ProfileManager] Cannot update hardcoded default profile '" << profile.id << "'" << std::endl;
    std::cout << "[ProfileManager] Default profiles are read-only." << std::endl;
    std::cerr << "[ProfileManager] ERROR: Attempt to modify read-only default profile rejected!" << std::endl;
    return false;
  }
  
  // Update in-memory profile
  auto it = std::find_if( m_customProfiles.begin(), m_customProfiles.end(),
                         [&profile]( const UccProfile &p ) { return p.id == profile.id; } );
  
  if ( it != m_customProfiles.end() )
  {
    *it = profile;
    
    // Update DBus data
    updateDBusActiveProfileData();
    
    // Update active profile if it was the one modified
    if ( m_activeProfile.id == profile.id )
    {
      std::cout << "[ProfileManager] Updated profile is active, reapplying to system" << std::endl;
      m_activeProfile = profile;
      // Reapply the profile to actually update the hardware/system settings
      if ( setCurrentProfileById( profile.id ) )
      {
        std::cout << "[ProfileManager] Successfully reapplied updated profile to system" << std::endl;
      }
      else
      {
        std::cerr << "[ProfileManager] Failed to reapply updated profile!" << std::endl;
      }
    }
    
    std::cout << "[ProfileManager] Profile updated successfully" << std::endl;
    return true;
  }
  
  std::cerr << "[ProfileManager] Profile not found for update" << std::endl;
  return false;
}

void UccDBusService::initializeDisplayModes()
{
  // detect session type (x11 or wayland)
  std::string sessionType = TccUtils::executeCommand( 
    "cat $(printf \"/proc/%s/environ \" $(pgrep -vu root | tail -n 20)) 2>/dev/null | "
    "tr '\\0' '\\n' | grep -m1 '^XDG_SESSION_TYPE=' | cut -d= -f2" 
  );
  
  // trim whitespace
  while ( not sessionType.empty() and 
          ( sessionType.back() == '\n' or sessionType.back() == '\r' or 
            sessionType.back() == ' ' or sessionType.back() == '\t' ) )
  {
    sessionType.pop_back();
  }
  
  m_dbusData.isX11 = ( sessionType == "x11" );
  
  // initialize display modes as empty array - will be populated by display worker if implemented
  // must be valid JSON (empty array, not empty string) for GUI to parse correctly
  m_dbusData.displayModes = "[]";
}

std::optional< UniwillDeviceID > UccDBusService::identifyDevice()
{
  // read dmi information from sysfs
  const std::string dmiBasePath = "/sys/class/dmi/id";
  const std::string productSKU = SysfsNode< std::string >( dmiBasePath + "/product_sku" ).read().value_or( "" );
  const std::string boardName = SysfsNode< std::string >( dmiBasePath + "/board_name" ).read().value_or( "" );

  // get module info from tuxedo_io
  std::string deviceModelId;
  m_io.deviceModelIdStr( deviceModelId );

  // create dmi sku to device map (matches typescript version)
  std::map< std::string, UniwillDeviceID > dmiSKUDeviceMap;
  dmiSKUDeviceMap[ "IBS1706" ] = UniwillDeviceID::IBP17G6;
  dmiSKUDeviceMap[ "IBP1XI08MK1" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "IBP1XI08MK2" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "IBP14I08MK2" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "IBP16I08MK2" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "OMNIA08IMK2" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "IBP14A10MK1 / IBP15A10MK1" ] = UniwillDeviceID::IBPG10AMD;
  dmiSKUDeviceMap[ "IIBP14A10MK1 / IBP15A10MK1" ] = UniwillDeviceID::IBPG10AMD;
  dmiSKUDeviceMap[ "POLARIS1XA02" ] = UniwillDeviceID::POLARIS1XA02;
  dmiSKUDeviceMap[ "POLARIS1XI02" ] = UniwillDeviceID::POLARIS1XI02;
  dmiSKUDeviceMap[ "POLARIS1XA03" ] = UniwillDeviceID::POLARIS1XA03;
  dmiSKUDeviceMap[ "POLARIS1XI03" ] = UniwillDeviceID::POLARIS1XI03;
  dmiSKUDeviceMap[ "STELLARIS1XA03" ] = UniwillDeviceID::STELLARIS1XA03;
  dmiSKUDeviceMap[ "STEPOL1XA04" ] = UniwillDeviceID::STEPOL1XA04;
  dmiSKUDeviceMap[ "STELLARIS1XI03" ] = UniwillDeviceID::STELLARIS1XI03;
  dmiSKUDeviceMap[ "STELLARIS1XI04" ] = UniwillDeviceID::STELLARIS1XI04;
  dmiSKUDeviceMap[ "PULSE1502" ] = UniwillDeviceID::PULSE1502;
  dmiSKUDeviceMap[ "PULSE1403" ] = UniwillDeviceID::PULSE1403;
  dmiSKUDeviceMap[ "PULSE1404" ] = UniwillDeviceID::PULSE1404;
  dmiSKUDeviceMap[ "STELLARIS1XI05" ] = UniwillDeviceID::STELLARIS1XI05;
  dmiSKUDeviceMap[ "POLARIS1XA05" ] = UniwillDeviceID::POLARIS1XA05;
  dmiSKUDeviceMap[ "STELLARIS1XA05" ] = UniwillDeviceID::STELLARIS1XA05;
  dmiSKUDeviceMap[ "STELLARIS16I06" ] = UniwillDeviceID::STELLARIS16I06;
  dmiSKUDeviceMap[ "STELLARIS17I06" ] = UniwillDeviceID::STELLARIS17I06;
  dmiSKUDeviceMap[ "STELLSL15A06" ] = UniwillDeviceID::STELLSL15A06;
  dmiSKUDeviceMap[ "STELLSL15I06" ] = UniwillDeviceID::STELLSL15I06;
  dmiSKUDeviceMap[ "AURA14GEN3" ] = UniwillDeviceID::AURA14G3;
  dmiSKUDeviceMap[ "AURA15GEN3" ] = UniwillDeviceID::AURA15G3;
  dmiSKUDeviceMap[ "STELLARIS16A07" ] = UniwillDeviceID::STELLARIS16A07;
  dmiSKUDeviceMap[ "STELLARIS16I07" ] = UniwillDeviceID::STELLARIS16I07;
  dmiSKUDeviceMap[ "XNE16A25" ] = UniwillDeviceID::XNE16A25;
  dmiSKUDeviceMap[ "XNE16E25" ] = UniwillDeviceID::XNE16E25;
  dmiSKUDeviceMap[ "SIRIUS1601" ] = UniwillDeviceID::SIRIUS1601;
  dmiSKUDeviceMap[ "SIRIUS1602" ] = UniwillDeviceID::SIRIUS1602;

  // check for sku match
  auto skuIt = dmiSKUDeviceMap.find( productSKU );
  if ( skuIt != dmiSKUDeviceMap.end() )
  {
    return skuIt->second;
  }

  // check uwid (univ wmi interface) device mapping
  std::map< int, UniwillDeviceID > uwidDeviceMap;
  uwidDeviceMap[ 0x13 ] = UniwillDeviceID::IBP14G6_TUX;
  uwidDeviceMap[ 0x12 ] = UniwillDeviceID::IBP14G6_TRX;
  uwidDeviceMap[ 0x14 ] = UniwillDeviceID::IBP14G6_TQF;
  uwidDeviceMap[ 0x17 ] = UniwillDeviceID::IBP14G7_AQF_ARX;

  int modelId = 0;
  try
  {
    modelId = std::stoi( deviceModelId );
  }
  catch ( ... )
  {
    // ignore parse errors
  }

  auto uwidIt = uwidDeviceMap.find( modelId );
  if ( uwidIt != uwidDeviceMap.end() )
  {
    return uwidIt->second;
  }

  // no device match found
  return std::nullopt;
}

void UccDBusService::computeDeviceCapabilities()
{
  // Aquaris (LCT water cooler) is supported only on specific devices
  static const std::set< UniwillDeviceID > waterCoolerDevices =
  {
    UniwillDeviceID::STELLARIS1XI04,
    UniwillDeviceID::STEPOL1XA04,
    UniwillDeviceID::STELLARIS1XI05,
    UniwillDeviceID::STELLARIS16I06,
    UniwillDeviceID::STELLARIS17I06,
    UniwillDeviceID::STELLARIS16A07,
    UniwillDeviceID::XNE16A25,
    UniwillDeviceID::XNE16E25,
    UniwillDeviceID::STELLARIS16I07,
  };

  // cTGP adjustment is hidden for the IBP series (undefined behaviour despite nvidia-smi reporting support)
  static const std::set< UniwillDeviceID > cTGPHiddenDevices =
  {
    UniwillDeviceID::IBP14G6_TUX,
    UniwillDeviceID::IBP14G6_TRX,
    UniwillDeviceID::IBP14G6_TQF,
    UniwillDeviceID::IBP14G7_AQF_ARX,
    UniwillDeviceID::IBPG8,
    UniwillDeviceID::IBPG10AMD,
  };

  if ( m_deviceId.has_value() )
  {
    m_dbusData.waterCoolerSupported = waterCoolerDevices.count( m_deviceId.value() ) > 0;
    m_dbusData.cTGPAdjustmentSupported = cTGPHiddenDevices.count( m_deviceId.value() ) == 0;
  }
  else
  {
    // Unknown device: water cooler not available, cTGP defers to hardware detection
    m_dbusData.waterCoolerSupported = false;
    m_dbusData.cTGPAdjustmentSupported = false;
  }

  syslog( LOG_INFO, "Device capabilities: aquaris=%s, cTGP=%s",
          m_dbusData.waterCoolerSupported.load() ? "supported" : "not supported",
          m_dbusData.cTGPAdjustmentSupported.load() ? "supported" : "hidden" );
}

void UccDBusService::loadSettings()
{
  auto loadedSettings = m_settingsManager.readSettings();
  
  if ( loadedSettings.has_value() )
  {
    m_settings = *loadedSettings;
    std::cout << "[Settings] Loaded existing settings" << std::endl;
  }
  else
  {
    // Use defaults - DON'T WRITE FILE YET
    // FIX #1 (CRITICAL): On fresh install, keep settings in memory only.
    // Let them be created when the first profile is actually saved.
    // This prevents the empty file overwrite bug on daemon restart.
    m_settings = TccSettings();
    
    // Set both AC, battery and water cooler to the default custom profile (in memory only)
    auto allProfiles = getAllProfiles();
    if ( !allProfiles.empty() )
    {
      m_settings.stateMap["power_ac"] = allProfiles[0].id;
      m_settings.stateMap["power_bat"] = allProfiles[0].id;
      m_settings.stateMap["power_wc"] = allProfiles[0].id;
    }
    
    // DO NOT call writeSettings() here - wait until profiles are actually saved
    // This prevents creating an empty file on first start
    std::cout << "[Settings] Using in-memory defaults (settings file will be created on first save)" << std::endl;
    updateDBusSettingsData();
  }
  
  // Load custom profiles from settings BEFORE validating stateMap
  for ( const auto &[profileId, profileJson] : m_settings.profiles )
  {
    try
    {
      auto profile = m_profileManager.parseProfileJSON( profileJson );
      m_customProfiles.push_back( profile );
      std::cout << "[Settings] Loaded profile '" << profile.name << "' (ID: " << profile.id << ") from settings" << std::endl;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[Settings] Failed to parse profile '" << profileId << "' from settings: " << e.what() << std::endl;
    }
  }
  
  // IMPORTANT: Do NOT resync/clear m_settings.profiles!
  // Reason: m_settings.profiles is the authoritative source from the file.
  // Resyncing can change keys or representation, breaking stateMap lookups.
  // Keep m_settings.profiles exactly as loaded from file.
  // Only modify it when profiles are explicitly added/edited via API.
  
  // validate and fix state map if needed
  auto allProfiles = getAllProfiles();
  bool settingsChanged = false;
  
  for ( const auto &stateKey : { "power_ac", "power_bat", "power_wc" } )
  {
    // check if state key exists in map
    if ( m_settings.stateMap.find( stateKey ) == m_settings.stateMap.end() )
    {
      std::cout << "[Settings] Missing state id assignment for '" 
                << stateKey << "' default to first profile" << std::endl;

      if ( not allProfiles.empty() )
      {
        m_settings.stateMap[stateKey] = allProfiles[0].id;
        settingsChanged = true;
      }

      continue;
    }

    auto &profileId = m_settings.stateMap[stateKey];
    
    // check if assigned profile exists (either in m_customProfiles OR in m_settings.profiles)
    bool profileExists = false;

    // First check if it exists as a loaded custom profile object
    for ( const auto &profile : m_customProfiles )
    {
      if ( profile.id == profileId )
      {
        profileExists = true;
        break;
      }
    }
    
    // If not found in objects, check if it exists as a key in m_settings.profiles (JSON map)
    // This is important because a profile might be in the file but not yet parsed into an object
    if ( !profileExists )
    {
      profileExists = ( m_settings.profiles.find( profileId ) != m_settings.profiles.end() );
    }
    
    // Also check default profiles
    if ( !profileExists )
    {
      for ( const auto &profile : m_defaultProfiles )
      {
        if ( profile.id == profileId )
        {
          profileExists = true;
          break;
        }
      }
    }
    
    if ( not profileExists )
    {
      std::cout << "[Settings] Profile ID '" << profileId << "' for state '" 
                << stateKey << "' not found, resetting to default" << std::endl;
      
      if ( not allProfiles.empty() )
      {
        profileId = allProfiles[0].id;
        settingsChanged = true;
      }
    }
  }
  
  if ( settingsChanged )
  {
    if ( m_settingsManager.writeSettings( m_settings ) )
    {
      std::cout << "[Settings] Saved updated settings" << std::endl;
      updateDBusSettingsData();
    }
    else
    {
      std::cerr << "[Settings] Failed to update settings!" << std::endl;
    }
  }
  
  // Sync ycbcr420Workaround with detected display ports
  if ( syncOutputPortsSetting() )
  {
    if ( m_settingsManager.writeSettings( m_settings ) )
    {
      std::cout << "[Settings] Synced ycbcr420Workaround settings" << std::endl;
      updateDBusSettingsData();
    }
  }
}

void UccDBusService::applyStartupProfile()
{
  // Determine current power state
  m_currentState = determineState();
  const std::string stateKey = profileStateToString( m_currentState );
  
  std::cout << "[Startup] Current power state: " << stateKey << std::endl;
  
  // Look up the profile assigned to this state
  auto stateMapIt = m_settings.stateMap.find( stateKey );
  if ( stateMapIt == m_settings.stateMap.end() )
  {
    std::cout << "[Startup] No profile assigned to state '" << stateKey << "'" << std::endl;
    return;
  }
  
  const std::string &profileId = stateMapIt->second;
  m_currentStateProfileId = profileId;
  
  std::cout << "[Startup] Applying profile assigned to state '" << stateKey << "': " << profileId << std::endl;
  
  // First try to find the profile in settings (persistent profiles)
  auto profileIt = m_settings.profiles.find( profileId );
  if ( profileIt != m_settings.profiles.end() )
  {
    try
    {
      auto profile = m_profileManager.parseProfileJSON( profileIt->second );
      m_activeProfile = profile;
      updateDBusActiveProfileData();
      
      std::cout << "[Startup] Applied profile from settings: " << profile.name << " (ID: " << profile.id << ")" << std::endl;
      
      // Apply fan curves and pump auto-control
      applyFanAndPumpSettings( profile );
      
      // Apply to workers
      if ( m_cpuWorker )
      {
        std::cout << "[Startup] Triggering CPU settings reapply" << std::endl;
        m_cpuWorker->reapplyProfile();
      }
      
      if ( m_profileSettingsWorker )
      {
        std::cout << "[Startup] Triggering TDP settings reapply" << std::endl;
        m_profileSettingsWorker->reapplyProfile();
      }
      
      // Apply keyboard backlight settings from profile
      if ( m_keyboardBacklightListener && !profile.keyboard.keyboardProfileData.empty() && profile.keyboard.keyboardProfileData != "{}" )
      {
        std::cout << "[Startup] Applying keyboard backlight settings from profile" << std::endl;
        m_keyboardBacklightListener->applyProfileKeyboardStates( profile.keyboard.keyboardProfileData );
      }
      
      return;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[Startup] Failed to parse profile '" << profileId << "' from settings: " << e.what() << std::endl;
    }
  }
  
  // Fall back to finding profile in allProfiles (for built-in profiles)
  auto allProfiles = getAllProfiles();
  bool profileFound = false;
  
  for ( const auto &profile : allProfiles )
  {
    if ( profile.id == profileId )
    {
      m_activeProfile = profile;
      updateDBusActiveProfileData();
      profileFound = true;
      
      std::cout << "[Startup] Applied built-in profile: " << profile.name << " (ID: " << profile.id << ")" << std::endl;
      
      // Apply fan curves and pump auto-control
      applyFanAndPumpSettings( profile );
      
      // Apply to workers (they should pick up the active profile automatically)
      // But we can trigger a reapply to be sure
      if ( m_cpuWorker )
      {
        std::cout << "[Startup] Triggering CPU settings reapply" << std::endl;
        m_cpuWorker->reapplyProfile();
      }
      
      if ( m_profileSettingsWorker )
      {
        std::cout << "[Startup] Triggering TDP settings reapply" << std::endl;
        m_profileSettingsWorker->reapplyProfile();
      }
      
      // Apply keyboard backlight settings from profile
      if ( m_keyboardBacklightListener && !profile.keyboard.keyboardProfileData.empty() && profile.keyboard.keyboardProfileData != "{}" )
      {
        std::cout << "[Startup] Applying keyboard backlight settings from profile" << std::endl;
        m_keyboardBacklightListener->applyProfileKeyboardStates( profile.keyboard.keyboardProfileData );
      }
      
      break;
    }
  }
  
  if ( !profileFound )
  {
    std::cerr << "[Startup] WARNING: Profile ID '" << profileId << "' not found!" << std::endl;
  }
}

void UccDBusService::applyFanAndPumpSettings( const UccProfile &profile )
{
  // Apply sameSpeed setting to fan worker
  if ( m_fanControlWorker )
    m_fanControlWorker->setSameSpeed( profile.fan.sameSpeed );

  // Resolve and apply fan curves: prefer embedded tables, fallback to named profile
  try
  {
    std::vector< FanTableEntry > cpuTable;
    std::vector< FanTableEntry > gpuTable;
    std::vector< FanTableEntry > wcFanTable;
    std::vector< FanTableEntry > pumpTable;

    if ( profile.fan.hasEmbeddedTables() )
    {
      cpuTable = profile.fan.tableCPU;
      gpuTable = profile.fan.tableGPU;
      wcFanTable = profile.fan.tableWaterCoolerFan;
      pumpTable = profile.fan.tablePump;
      std::cout << "[FanPump] Using embedded fan tables from profile" << std::endl;
    }
    else
    {
      const std::string &fpName = profile.fan.fanProfile;
      if ( !fpName.empty() )
      {
        FanProfile fp = getDefaultFanProfileByName( fpName );
        if ( fp.isValid() )
        {
          cpuTable = fp.tableCPU;
          gpuTable = fp.tableGPU;
          wcFanTable = fp.tableWaterCoolerFan;
          pumpTable = fp.tablePump;
          std::cout << "[FanPump] Using fan tables from named profile '" << fpName << "'" << std::endl;
        }
      }
    }

    if ( m_fanControlWorker && !cpuTable.empty() )
    {
      m_fanControlWorker->applyTemporaryFanCurves( cpuTable, gpuTable, wcFanTable, pumpTable );
      std::cout << "[FanPump] Applied fan curves (CPU=" << cpuTable.size()
                << " GPU=" << gpuTable.size()
                << " WCFan=" << wcFanTable.size()
                << " Pump=" << pumpTable.size() << ")" << std::endl;
    }

    // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
    if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
         && !pumpTable.empty() )
    {
      int maxTemp = 0;
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
        for ( const auto &fan : m_dbusData.fans )
          maxTemp = std::max( maxTemp, fan.temp.data );
      }
      FanProfile tempFp;
      tempFp.tablePump = pumpTable;
      m_waterCoolerWorker->setPumpVoltage( static_cast<int>( tempFp.getPumpSpeedForTemp( maxTemp ) ) );
      std::cout << "[FanPump] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
    }
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[FanPump] Failed to apply fan/pump settings: " << e.what() << std::endl;
  }
}

void UccDBusService::applyProfileForCurrentState()
{
  const std::string stateKey = profileStateToString( m_currentState );
  auto stateMapIt = m_settings.stateMap.find( stateKey );
  if ( stateMapIt == m_settings.stateMap.end() )
  {
    std::cerr << "[State] No profile assigned to state '" << stateKey << "'" << std::endl;
    return;
  }

  const std::string &profileId = stateMapIt->second;
  m_currentStateProfileId = profileId;

  std::cout << "[State] Applying profile for state '" << stateKey << "': " << profileId << std::endl;

  // Lambda to apply all profile settings (fan curves, sameSpeed, CPU, ODM, keyboard, pump auto-control)
  auto applyFullProfile = [this]( const UccProfile &profile )
  {
    m_activeProfile = profile;
    updateDBusActiveProfileData();

    // Apply sameSpeed setting to fan worker
    if ( m_fanControlWorker )
    {
      m_fanControlWorker->setSameSpeed( profile.fan.sameSpeed );
    }

    // Resolve and apply fan curves: prefer embedded tables, fallback to named profile
    try
    {
      std::vector< FanTableEntry > cpuTable;
      std::vector< FanTableEntry > gpuTable;
      std::vector< FanTableEntry > wcFanTable;
      std::vector< FanTableEntry > pumpTable;

      if ( profile.fan.hasEmbeddedTables() )
      {
        cpuTable = profile.fan.tableCPU;
        gpuTable = profile.fan.tableGPU;
        wcFanTable = profile.fan.tableWaterCoolerFan;
        pumpTable = profile.fan.tablePump;
        std::cout << "[State] Using embedded fan tables from profile" << std::endl;
      }
      else
      {
        const std::string &fpName = profile.fan.fanProfile;
        if ( !fpName.empty() )
        {
          FanProfile fp = getDefaultFanProfileByName( fpName );
          if ( fp.isValid() )
          {
            cpuTable = fp.tableCPU;
            gpuTable = fp.tableGPU;
            wcFanTable = fp.tableWaterCoolerFan;
            pumpTable = fp.tablePump;
            std::cout << "[State] Using fan tables from named profile '" << fpName << "'" << std::endl;
          }
        }
      }

      if ( m_fanControlWorker && !cpuTable.empty() )
      {
        m_fanControlWorker->applyTemporaryFanCurves( cpuTable, gpuTable, wcFanTable, pumpTable );
        std::cout << "[State] Applied fan curves (CPU=" << cpuTable.size()
                  << " GPU=" << gpuTable.size()
                  << " WCFan=" << wcFanTable.size()
                  << " Pump=" << pumpTable.size() << ")" << std::endl;
      }

      // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
      if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
           && !pumpTable.empty() )
      {
        int maxTemp = 0;
        {
          std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
          for ( const auto &fan : m_dbusData.fans )
            maxTemp = std::max( maxTemp, fan.temp.data );
        }
        FanProfile tempFp;
        tempFp.tablePump = pumpTable;
        m_waterCoolerWorker->setPumpVoltage( static_cast<int>( tempFp.getPumpSpeedForTemp( maxTemp ) ) );
        std::cout << "[State] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
      }
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[State] Failed to apply fan curves: " << e.what() << std::endl;
    }

    // Apply CPU/ODM/keyboard workers
    if ( m_cpuWorker )
      m_cpuWorker->reapplyProfile();
    if ( m_profileSettingsWorker )
      m_profileSettingsWorker->reapplyProfile();
    if ( m_keyboardBacklightListener
         && !profile.keyboard.keyboardProfileData.empty()
         && profile.keyboard.keyboardProfileData != "{}" )
      m_keyboardBacklightListener->applyProfileKeyboardStates( profile.keyboard.keyboardProfileData );

    // Emit ProfileChanged signal for DBus clients
    if ( m_adaptor )
      m_adaptor->emitProfileChanged( profile.id );
  };

  // Try persistent (custom) profiles first
  auto profileIt = m_settings.profiles.find( profileId );
  if ( profileIt != m_settings.profiles.end() )
  {
    try
    {
      auto profile = m_profileManager.parseProfileJSON( profileIt->second );
      std::cout << "[State] Applied profile from settings: " << profile.name
                << " (ID: " << profile.id << ")" << std::endl;
      applyFullProfile( profile );
      return;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[State] Failed to parse profile '" << profileId << "': " << e.what() << std::endl;
    }
  }

  // Fall back to built-in profiles
  for ( const auto &profile : getAllProfiles() )
  {
    if ( profile.id == profileId )
    {
      std::cout << "[State] Applied built-in profile: " << profile.name
                << " (ID: " << profile.id << ")" << std::endl;
      applyFullProfile( profile );
      return;
    }
  }

  std::cerr << "[State] WARNING: Profile ID '" << profileId << "' not found for state '" << stateKey << "'" << std::endl;
}

void UccDBusService::serializeProfilesJSON()
{
  std::cout << "[serializeProfilesJSON] Starting profile serialization" << std::endl;
  std::cout << "[serializeProfilesJSON] Default profiles count: " << m_defaultProfiles.size() << std::endl;
  
  // Debug: Check TDP values before serialization
  for ( size_t i = 0; i < m_defaultProfiles.size() && i < 3; ++i )
  {
    std::cout << "[serializeProfilesJSON]   Profile " << i << " (" << m_defaultProfiles[i].id 
              << ") has " << m_defaultProfiles[i].odmPowerLimits.tdpValues.size() << " TDP values" << std::endl;
    if ( !m_defaultProfiles[i].odmPowerLimits.tdpValues.empty() )
    {
      std::cout << "[serializeProfilesJSON]     TDP values: [";
      for ( size_t j = 0; j < m_defaultProfiles[i].odmPowerLimits.tdpValues.size(); ++j )
      {
        if ( j > 0 ) std::cout << ", ";
        std::cout << m_defaultProfiles[i].odmPowerLimits.tdpValues[j];
      }
      std::cout << "]" << std::endl;
    }
  }
  
  const int32_t defaultOnlineCores = getDefaultOnlineCores();
  const int32_t defaultScalingMin = getCpuMinFrequency();
  const int32_t defaultScalingMax = getCpuMaxFrequency();
  
  UccProfile defaultProfile = m_profileManager.getDefaultCustomProfiles()[0];
  
  // serialize all profiles to JSON
  std::ostringstream allProfilesJSON;
  allProfilesJSON << "[";
  
  auto allProfiles = getAllProfiles();
  for ( size_t i = 0; i < allProfiles.size(); ++i )
  {
    if ( i > 0 )
      allProfilesJSON << ",";
    
    allProfilesJSON << profileToJSON( allProfiles[ i ],
                      defaultOnlineCores,
                      defaultScalingMin,
                      defaultScalingMax );
  }
  allProfilesJSON << "]";

  std::ostringstream defaultProfilesJSON;
  defaultProfilesJSON << "[";
  for ( size_t i = 0; i < m_defaultProfiles.size(); ++i )
  {
    if ( i > 0 )
      defaultProfilesJSON << ",";
    
    defaultProfilesJSON << profileToJSON( m_defaultProfiles[ i ],
                        defaultOnlineCores,
                        defaultScalingMin,
                        defaultScalingMax );
  }
  defaultProfilesJSON << "]";

  std::ostringstream customProfilesJSON;
  customProfilesJSON << "[";
  for ( size_t i = 0; i < m_customProfiles.size(); ++i )
  {
    if ( i > 0 )
      customProfilesJSON << ",";
    
    customProfilesJSON << profileToJSON( m_customProfiles[ i ],
                       defaultOnlineCores,
                       defaultScalingMin,
                       defaultScalingMax );
  }
  customProfilesJSON << "]";

  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.profilesJSON = defaultProfilesJSON.str();  // Only default profiles now
  m_dbusData.defaultProfilesJSON = defaultProfilesJSON.str();
  m_dbusData.customProfilesJSON = "[]";  // Empty array since custom profiles are local
  m_dbusData.defaultValuesProfileJSON = profileToJSON( defaultProfile,
                                                       defaultOnlineCores,
                                                       defaultScalingMin,
                                                       defaultScalingMax );
  
  std::cout << "[DBus] Re-serialized profile JSONs" << std::endl;
}

void UccDBusService::fillDeviceSpecificDefaults( std::vector< UccProfile > &profiles )
{
  const int32_t cpuMinFreq = getCpuMinFrequency();
  const int32_t cpuMaxFreq = getCpuMaxFrequency();
  
  // Get TDP info from hardware
  std::vector< TDPInfo > tdpInfo;
  if ( m_profileSettingsWorker )
  {
    tdpInfo = m_profileSettingsWorker->getTDPInfo();
    std::cout << "[fillDeviceSpecificDefaults] TDP info available: " << tdpInfo.size() << " entries" << std::endl;
    for ( size_t i = 0; i < tdpInfo.size(); ++i )
    {
      std::cout << "[fillDeviceSpecificDefaults]   TDP[" << i << "]: min=" << tdpInfo[i].min 
                << ", max=" << tdpInfo[i].max << ", current=" << tdpInfo[i].current << std::endl;
    }
  }
  else
  {
    std::cout << "[fillDeviceSpecificDefaults] No ODM power limit worker available" << std::endl;
  }
  
  for ( auto &profile : profiles )
  {
    std::cout << "[fillDeviceSpecificDefaults] Filling profile: " << profile.id 
              << ", current TDP values: " << profile.odmPowerLimits.tdpValues.size() << std::endl;
    
    // Fill CPU frequency defaults
    if ( !profile.cpu.scalingMinFrequency.has_value() || profile.cpu.scalingMinFrequency.value() < cpuMinFreq )
    {
      profile.cpu.scalingMinFrequency = cpuMinFreq;
    }
    
    if ( !profile.cpu.scalingMaxFrequency.has_value() )
    {
      profile.cpu.scalingMaxFrequency = cpuMaxFreq;
    }
    else if ( profile.cpu.scalingMaxFrequency.value() < profile.cpu.scalingMinFrequency.value() )
    {
      profile.cpu.scalingMaxFrequency = profile.cpu.scalingMinFrequency;
    }
    else if ( profile.cpu.scalingMaxFrequency.value() > cpuMaxFreq )
    {
      profile.cpu.scalingMaxFrequency = cpuMaxFreq;
    }
    
    // Fill TDP values if missing and hardware TDP info is available
    if ( !tdpInfo.empty() && tdpInfo.size() > profile.odmPowerLimits.tdpValues.size() )
    {
      const size_t nrMissingValues = tdpInfo.size() - profile.odmPowerLimits.tdpValues.size();
      std::cout << "[fillDeviceSpecificDefaults]   Adding " << nrMissingValues << " TDP values" << std::endl;
      // Add missing TDP values with max values from hardware
      for ( size_t i = profile.odmPowerLimits.tdpValues.size(); i < tdpInfo.size(); ++i )
      {
        profile.odmPowerLimits.tdpValues.push_back( tdpInfo[i].max );
        std::cout << "[fillDeviceSpecificDefaults]     Added TDP[" << i << "] = " << tdpInfo[i].max << std::endl;
      }
    }
    
    std::cout << "[fillDeviceSpecificDefaults]   Final TDP values: " << profile.odmPowerLimits.tdpValues.size() << std::endl;
  }
}

void UccDBusService::loadAutosave()
{
  m_autosave = m_autosaveManager.readAutosave();
  std::cout << "[Autosave] Loaded autosave (displayBrightness: " 
            << m_autosave.displayBrightness << "%)" << std::endl;
}

void UccDBusService::saveAutosave()
{
  if ( m_autosaveManager.writeAutosave( m_autosave ) )
  {
    std::cout << "[Autosave] Saved autosave" << std::endl;
  }
  else
  {
    std::cerr << "[Autosave] Failed to save autosave!" << std::endl;
  }
}

std::vector< std::vector< std::string > > UccDBusService::getOutputPorts()
{
  std::vector< std::vector< std::string > > result;
  
  struct udev *udev_context = udev_new();
  if ( !udev_context )
  {
    std::cerr << "[OutputPorts] Failed to create udev context" << std::endl;
    return result;
  }
  
  struct udev_enumerate *drm_devices = udev_enumerate_new( udev_context );
  if ( !drm_devices )
  {
    std::cerr << "[OutputPorts] Failed to enumerate devices" << std::endl;
    udev_unref( udev_context );
    return result;
  }
  
  if ( udev_enumerate_add_match_subsystem( drm_devices, "drm" ) < 0 ||
       udev_enumerate_add_match_sysname( drm_devices, "card*-*-*" ) < 0 ||
       udev_enumerate_scan_devices( drm_devices ) < 0 )
  {
    std::cerr << "[OutputPorts] Failed to scan devices" << std::endl;
    udev_enumerate_unref( drm_devices );
    udev_unref( udev_context );
    return result;
  }
  
  struct udev_list_entry *drm_devices_iterator = udev_enumerate_get_list_entry( drm_devices );
  if ( !drm_devices_iterator )
  {
    udev_enumerate_unref( drm_devices );
    udev_unref( udev_context );
    return result;
  }
  
  struct udev_list_entry *drm_devices_entry;
  udev_list_entry_foreach( drm_devices_entry, drm_devices_iterator )
  {
    std::string path = udev_list_entry_get_name( drm_devices_entry );
    std::string name = path.substr( path.rfind( "/" ) + 1 );
    
    // Extract card number (e.g., "card0" -> 0)
    size_t cardPos = name.find( "card" );
    size_t dashPos = name.find( "-", cardPos );
    if ( cardPos == std::string::npos || dashPos == std::string::npos )
      continue;
    
    int cardNumber = std::stoi( name.substr( cardPos + 4, dashPos - cardPos - 4 ) );
    
    // Ensure result vector is large enough
    if ( static_cast< size_t >( cardNumber + 1 ) > result.size() )
    {
      result.resize( cardNumber + 1 );
    }
    
    // Extract port name (everything after "card0-")
    std::string portName = name.substr( dashPos + 1 );
    result[cardNumber].push_back( portName );
  }
  
  udev_enumerate_unref( drm_devices );
  udev_unref( udev_context );
  
  return result;
}

bool UccDBusService::syncOutputPortsSetting()
{
  bool settingsChanged = false;
  
  auto outputPorts = getOutputPorts();
  
  // Delete additional cards from settings
  if ( m_settings.ycbcr420Workaround.size() > outputPorts.size() )
  {
    m_settings.ycbcr420Workaround.resize( outputPorts.size() );
    settingsChanged = true;
  }
  
  for ( size_t card = 0; card < outputPorts.size(); ++card )
  {
    // Add card to settings if missing
    if ( m_settings.ycbcr420Workaround.size() <= card )
    {
      YCbCr420Card newCard;
      newCard.card = static_cast< int >( card );
      m_settings.ycbcr420Workaround.push_back( newCard );
      settingsChanged = true;
    }
    
    // Get reference to card settings
    auto &cardSettings = m_settings.ycbcr420Workaround[card];
    
    // Delete ports that no longer exist
    std::vector< std::string > portsToRemove;
    
    for ( const auto &portEntry : cardSettings.ports )
    {
      bool stillAvailable = false;
      for ( const auto &port : outputPorts[card] )
      {
        if ( portEntry.port == port )
        {
          stillAvailable = true;
          break;
        }
      }
      
      if ( !stillAvailable )
      {
        portsToRemove.push_back( portEntry.port );
      }
    }
    
    // Remove ports that are no longer available
    for ( const auto &port : portsToRemove )
    {
      cardSettings.ports.erase(
        std::remove_if( cardSettings.ports.begin(), cardSettings.ports.end(),
                       [&port]( const YCbCr420Port &p ) { return p.port == port; } ),
        cardSettings.ports.end()
      );
      settingsChanged = true;
    }
    
    // Add missing ports to settings
    for ( const auto &port : outputPorts[card] )
    {
      bool found = false;
      for ( const auto &portEntry : cardSettings.ports )
      {
        if ( portEntry.port == port )
        {
          found = true;
          break;
        }
      }
      
      if ( !found )
      {
        YCbCr420Port newPort;
        newPort.port = port;
        newPort.enabled = false;
        cardSettings.ports.push_back( newPort );
        settingsChanged = true;
      }
    }
  }
  
  return settingsChanged;
}
