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
#include "CommonTypes.hpp"
#include "NvmlWrapper.hpp"
#include "SmbiosMemoryDecoder.hpp"
#include "profiles/BuiltinSubProfiles.hpp"
#include "profiles/DefaultProfiles.hpp"
#include "profiles/FanProfile.hpp"
#include "PolkitAuthority.hpp"
#include "StateUtils.hpp"
#include "Utils.hpp"
#include "SysfsNode.hpp"
#include "platform/native/HwmonFanProvider.hpp"
#include "platform/native/HwmonTempProvider.hpp"
#include "platform/native/GenericProfileProvider.hpp"
#include "platform/gpu/nvidia/NvidiaGpuPowerProvider.hpp"
#include "platform/gpu/nvidia/NvmlTempProvider.hpp"
#include "platform/cpu/amd/AmdCpuPlatformProvider.hpp"
#include "platform/uniwill/TuxedoIOProviders.hpp"
#include "platform/uniwill/UniwillProfileProvider.hpp"
#include <sstream>
#include <iomanip>
#include <map>
#include <set>
#include <thread>
#include <cmath>
#include <climits>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include <syslog.h>
#include <libudev.h>
#include <algorithm>
#include <cctype>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <nlohmann/json.hpp>

static std::string jsonEscape( const std::string &value );

// Water cooler zone IDs — these are the zone IDs created by the BLE water cooler subsystem
static constexpr const char *kWCFanZoneId  = "wc-fan";
static constexpr const char *kWCPumpZoneId = "wc-pump";

static std::string fanProfileToJSON( const FanProfile &fp,
                                     const std::unordered_map< std::string, std::string > &hwDeviceTypes,
                                     const std::unordered_map< std::string, std::string > &zoneNames )
{
  auto zoneDisplayName = [&zoneNames]( const std::string &zid ) -> std::string {
    if ( auto it = zoneNames.find( zid ); it != zoneNames.end() && !it->second.empty() )
      return it->second;
    return zid;
  };

  auto zoneDeviceTypeStr = [&hwDeviceTypes]( const std::string &zid ) -> std::string {
    if ( auto it = hwDeviceTypes.find( zid ); it != hwDeviceTypes.end() )
      return it->second;
    return "fan";
  };

  std::string json = "{";
  json += "\"id\":\"" + jsonEscape( fp.id ) + "\",";
  json += "\"name\":\"" + jsonEscape( fp.name ) + "\",";
  json += "\"zones\":[";

  bool firstZone = true;
  for ( const auto &zc : fp.zoneCurves )
  {
    if ( !hwDeviceTypes.empty() && hwDeviceTypes.find( zc.zoneId ) == hwDeviceTypes.end() )
      continue;

    if ( !firstZone )
      json += ',';
    firstZone = false;

    json += "{\"id\":\"" + jsonEscape( zc.zoneId ) + "\"";
    json += ",\"name\":\"" + jsonEscape( zoneDisplayName( zc.zoneId ) ) + "\"";
    json += ",\"deviceType\":\"" + jsonEscape( zoneDeviceTypeStr( zc.zoneId ) ) + "\"";
    json += ",\"hysteresisDeg\":" + std::to_string( zc.hysteresisDeg );
    json += ",\"enabled\":" + std::string( zc.enabled ? "true" : "false" );
    if ( !zc.thermalSourceId.empty() )
      json += ",\"thermalSourceId\":\"" + jsonEscape( zc.thermalSourceId ) + "\"";

    // Serialize fan assignments if present (topology from user profile)
    if ( !zc.fanIds.empty() )
    {
      json += ",\"fanIds\":[";
      for ( size_t f = 0; f < zc.fanIds.size(); ++f )
      {
        if ( f > 0 ) json += ',';
        json += "\"" + jsonEscape( zc.fanIds[f] ) + "\"";
      }
      json += "]";
    }

    json += ",\"curve\":[";
    for ( size_t i = 0; i < zc.curve.size(); ++i )
    {
      if ( i > 0 )
        json += ',';
      json += "{\"temp\":" + std::to_string( zc.curve[i].temp )
           + ",\"speed\":" + std::to_string( zc.curve[i].speed ) + "}";
    }
    json += "]}";
  }
  json += "]";

  // Serialize custom thermal sources
  if ( !fp.thermalSources.empty() )
  {
    json += ",\"thermalSources\":[";
    bool firstTs = true;
    for ( const auto &ts : fp.thermalSources )
    {
      if ( !firstTs ) json += ',';
      firstTs = false;
      json += "{\"id\":\"" + jsonEscape( ts.id ) + "\"";
      json += ",\"label\":\"" + jsonEscape( ts.label ) + "\"";
      json += ",\"strategy\":\"" + jsonEscape( ucc::hal::thermalStrategyToString( ts.strategy ) ) + "\"";
      json += ",\"sensorIds\":[";
      for ( size_t s = 0; s < ts.sensorIds.size(); ++s )
      {
        if ( s > 0 ) json += ',';
        json += "\"" + jsonEscape( ts.sensorIds[s] ) + "\"";
      }
      json += "],\"weights\":[";
      for ( size_t w = 0; w < ts.weights.size(); ++w )
      {
        if ( w > 0 ) json += ',';
        json += std::to_string( ts.weights[w] );
      }
      json += "]}";
    }
    json += "]";
  }

  json += "}";
  return json;
}

// helper function to convert GPU info to JSON
std::string dgpuInfoToJSON( const DGpuInfo &info )
{
  std::ostringstream oss;
  oss << std::fixed << std::setprecision( 2 );
  oss << "{"
  << "\"temp\":" << info.m_temp << ","
      << "\"coreFrequency\":" << info.m_coreFrequency << ","
      << "\"vramFrequency\":" << info.m_vramFrequency << ","
      << "\"maxCoreFrequency\":" << info.m_maxCoreFrequency << ","
      << "\"powerDraw\":" << info.m_powerDraw << ","
      << "\"maxPowerLimit\":" << info.m_maxPowerLimit << ","
      << "\"enforcedPowerLimit\":" << info.m_enforcedPowerLimit << ","
      << "\"computeUtilPct\":" << info.m_computeUtilPct << ","
      << "\"memoryUtilPct\":" << info.m_memoryUtilPct << ","
      << "\"vramUsedMiB\":" << info.m_vramUsedMiB << ","
      << "\"vramTotalMiB\":" << info.m_vramTotalMiB << ","
      << "\"perfLimitReason\":\"" << jsonEscape( info.m_perfLimitReason ) << "\","
      << "\"encoderUtilPct\":" << info.m_encoderUtilPct << ","
      << "\"decoderUtilPct\":" << info.m_decoderUtilPct << ","
      << "\"currentPstate\":" << info.m_currentPstate << ","
      << "\"grClockOffsetMHz\":" << ( info.m_grClockOffsetMHz == INT_MIN ? -999 : info.m_grClockOffsetMHz ) << ","
      << "\"memClockOffsetMHz\":" << ( info.m_memClockOffsetMHz == INT_MIN ? -999 : info.m_memClockOffsetMHz ) << ","
      << "\"coreVoltageMv\":" << info.m_coreVoltageMv << ","
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
                                  int32_t defaultScalingMax,
                                  bool editable = false )
{
  std::ostringstream oss;
  oss << "{"
      << "\"id\":\"" << jsonEscape( profile.id ) << "\" ,"
      << "\"name\":\"" << jsonEscape( profile.name ) << "\" ,"
      << "\"editable\":" << ( editable ? "true" : "false" ) << ","
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
      << "\"autoControlWC\":" << ( profile.fan.autoControlWC ? "true" : "false" ) << ","
      << "\"enableWaterCooler\":" << ( profile.fan.enableWaterCooler ? "true" : "false" )
      << "},"
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

  // GPU OC profile — ID reference only
  if ( !profile.gpuProfileId.empty() )
  {
    oss << ",\"gpuProfileId\":\"" << jsonEscape( profile.gpuProfileId ) << "\"";
  }

  // Keyboard — ID reference only
  {
    const std::string &kbRef = profile.keyboard.keyboardProfileId;
    if ( !kbRef.empty() )
      oss << ",\"selectedKeyboardProfile\":\"" << jsonEscape( kbRef ) << "\"";
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

  oss << "},\"appGpuProfileMap\":{";

  first = true;
  for ( const auto &[appKey, profileId] : settings.appGpuProfileMap )
  {
    if ( !first )
      oss << ",";
    first = false;
    oss << "\"" << jsonEscape( appKey ) << "\":\"" << jsonEscape( profileId ) << "\"";
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

  // Create a fast timer to poll the FPS socket and push MetricId::Fps
  // to the daemon's metric history store when sensor collection is active.
  m_fpsPollTimer = new QTimer( this );
  m_fpsPollTimer->setInterval( 250 );
  m_fpsPollTimer->setSingleShot( false );
  QObject::connect( m_fpsPollTimer, &QTimer::timeout, this, &UccDBusInterfaceAdaptor::onFpsPollTimeout );

  // FPS collection must remain available unconditionally for AutoOC.
  if ( !m_fpsServer.start() )
    syslog( LOG_WARNING, "FpsServer: failed to start always-on socket server" );
  if ( m_fpsPollTimer )
    m_fpsPollTimer->start();
}

bool UccDBusInterfaceAdaptor::checkAuth( const char *actionId ) noexcept
{
  auto *dbusObj = qobject_cast< UccDBusObject * >( parent() );
  if ( not dbusObj )
  {
    syslog( LOG_ERR, "PolkitAuth: parent is not UccDBusObject" );
    return false;
  }
  const QString methodName = dbusObj->message().member();
  std::cerr << "[PolkitAuth] method='" << methodName.toStdString()
            << "' action='" << actionId << "'\n";
  return PolkitAuthority::checkAuthorization(
      dbusObj->connection(), dbusObj->message(), actionId );
}

void UccDBusInterfaceAdaptor::onFpsPollTimeout()
{
  // Auto-recovery: if the socket file was removed while collection is
  // active (e.g. /tmp cleanup), recreate it transparently.
  if ( m_fpsServer.isRunning() )
  {
    struct stat st;
    if ( ::stat( m_fpsServer.socketPath().c_str(), &st ) != 0 )
    {
      syslog( LOG_WARNING, "FpsServer: socket file disappeared — recreating" );
      m_fpsServer.rebind();
    }
  }

  m_fpsServer.poll();
  const std::string appName = m_fpsServer.clientAppName();
  const pid_t clientPid = m_fpsServer.clientPid();

  // Auto-apply app-bound GPU profile on FPS client connect/switch.
  if ( m_service )
    autoApplyGpuProfileForApp( appName, clientPid );

  const double fps = m_fpsServer.currentFps();
  if ( fps < 0.0 || !m_service )
    return;

  if ( !appName.empty() )
    m_seenFpsApps.insert( appName );

  // FPS ingestion policy:
  // 1) NVIDIA must be present.
  // 2) Optional manual source-app filter.
  bool allow = m_service->m_nvml && m_service->m_nvml->isAvailable()
               && m_service->m_nvml->deviceCount() > 0;

  if ( allow && !m_selectedFpsApp.empty() && m_selectedFpsApp != "auto" )
  {
    auto toLower = []( const std::string &s ) {
      std::string out = s;
      std::transform( out.begin(), out.end(), out.begin(),
                      []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
      return out;
    };
    allow = !appName.empty() && toLower( appName ) == toLower( m_selectedFpsApp );
  }

  if ( allow )
    m_service->m_metricsStore.push( "fps", fps );
}

void UccDBusInterfaceAdaptor::autoApplyGpuProfileForApp(
    const std::string &appName, pid_t clientPid )
{
  const bool autoUvRunning = m_service->m_autoUndervoltWorker
                          && m_service->m_autoUndervoltWorker->isRunning();

  if ( autoUvRunning )
    return;

  if ( !appName.empty() )
  {
    auto it = m_service->m_settings.appGpuProfileMap.find( appName );
    if ( it != m_service->m_settings.appGpuProfileMap.end() )
    {
      applyMappedGpuProfile( appName, clientPid, it->second );
      return;
    }

    // No mapping for this app — restore active profile's GPU profile
    restoreFallbackGpuProfile( appName, clientPid );
    return;
  }

  // No 3D app active — restore active profile's GPU profile
  const std::string &fallbackGpuProfileId = m_service->m_activeProfile.gpuProfileId;
  if ( !fallbackGpuProfileId.empty() && fallbackGpuProfileId != m_lastAutoAppliedGpuProfileId )
  {
    m_service->applyGpuOCFromProfile( m_service->m_activeProfile );

    if ( m_service->m_adaptor )
    {
      m_service->m_adaptor->emitProfileChanged( m_service->m_activeProfile.id,
                                                m_service->m_activeProfile.keyboard.keyboardProfileId,
                                                m_service->m_activeProfile.fan.fanProfile,
                                                fallbackGpuProfileId );
    }

    syslog( LOG_INFO, "[AutoUV] No active 3D app; restored active profile GPU profile '%s'",
            fallbackGpuProfileId.c_str() );
  }

  // Clear app/pid so the next 3D app launch always re-evaluates mapping.
  m_lastAutoAppliedApp.clear();
  m_lastAutoAppliedPid = 0;
  m_lastAutoAppliedGpuProfileId = fallbackGpuProfileId;
}

void UccDBusInterfaceAdaptor::applyMappedGpuProfile(
    const std::string &appName, pid_t clientPid, const std::string &mappedGpuProfileId )
{
  const bool needsApply = ( appName != m_lastAutoAppliedApp )
                       || ( clientPid != m_lastAutoAppliedPid )
                       || ( mappedGpuProfileId != m_lastAutoAppliedGpuProfileId );
  if ( !needsApply )
    return;

  const std::string gpuJson = m_service->resolveGpuProfileJSON( mappedGpuProfileId );
  if ( gpuJson.empty() || gpuJson == "{}" )
  {
    syslog( LOG_WARNING, "[AutoUV] Mapped GPU profile '%s' for app '%s' not found",
            mappedGpuProfileId.c_str(), appName.c_str() );
    return;
  }

  UccProfile profile;
  profile.name = "AutoUV runtime " + appName;
  profile.gpuProfileId = mappedGpuProfileId;
  m_service->applyGpuOCFromProfile( profile );

  if ( m_service->m_adaptor )
  {
    m_service->m_adaptor->emitProfileChanged( m_service->m_activeProfile.id,
                                              m_service->m_activeProfile.keyboard.keyboardProfileId,
                                              m_service->m_activeProfile.fan.fanProfile,
                                              mappedGpuProfileId );
  }

  m_lastAutoAppliedApp = appName;
  m_lastAutoAppliedPid = clientPid;
  m_lastAutoAppliedGpuProfileId = mappedGpuProfileId;

  syslog( LOG_INFO, "[AutoUV] Auto-applied GPU profile '%s' for app '%s' (pid=%d)",
          mappedGpuProfileId.c_str(), appName.c_str(), static_cast< int >( clientPid ) );
}

void UccDBusInterfaceAdaptor::restoreFallbackGpuProfile(
    const std::string &appName, pid_t clientPid )
{
  const std::string &fallbackGpuProfileId = m_service->m_activeProfile.gpuProfileId;
  const bool needsFallback = !fallbackGpuProfileId.empty()
                          && ( fallbackGpuProfileId != m_lastAutoAppliedGpuProfileId
                            || appName != m_lastAutoAppliedApp
                            || clientPid != m_lastAutoAppliedPid );
  if ( !needsFallback )
    return;

  m_service->applyGpuOCFromProfile( m_service->m_activeProfile );

  if ( m_service->m_adaptor )
  {
    m_service->m_adaptor->emitProfileChanged( m_service->m_activeProfile.id,
                                              m_service->m_activeProfile.keyboard.keyboardProfileId,
                                              m_service->m_activeProfile.fan.fanProfile,
                                              fallbackGpuProfileId );
  }

  m_lastAutoAppliedApp = appName;
  m_lastAutoAppliedPid = clientPid;
  m_lastAutoAppliedGpuProfileId = fallbackGpuProfileId;
  syslog( LOG_INFO, "[AutoUV] No app GPU mapping for '%s'; restored active profile GPU profile '%s'",
          appName.c_str(), fallbackGpuProfileId.c_str() );
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

QVariantMap
UccDBusInterfaceAdaptor::exportZoneTelemetry( const UccDBusData::ZoneTelemetry &zt )
{
  QVariantMap speedData;
  speedData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( zt.timestamp ) );
  speedData[ "data" ] = QVariant::fromValue( static_cast< int >( zt.duty ) );

  QVariantMap tempData;
  tempData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( zt.timestamp ) );
  tempData[ "data" ] = QVariant::fromValue( static_cast< int >( zt.temp ) );

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

QString UccDBusInterfaceAdaptor::GetSystemInfoJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.systemInfoJSON );
}

bool UccDBusInterfaceAdaptor::IsDeviceSupported()
{
  return m_data.deviceSupported.load();
}

QString UccDBusInterfaceAdaptor::GetCapabilitiesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.capabilitiesJSON );
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

  const int64_t now = std::chrono::duration_cast< std::chrono::milliseconds >(
    std::chrono::system_clock::now().time_since_epoch() ).count();

  // Read CPU temperature directly from the hardware sensor
  int cpuTemp = -1;
  if ( m_service )
  {
    const auto *sensor = m_service->m_hw.findCpuTempSensor();
    if ( sensor )
    {
      auto val = m_service->m_hw.readTemp( *sensor );
      if ( val.has_value() )
        cpuTemp = static_cast< int >( std::round( val.value() ) );
    }
  }

  QVariantMap tempData;
  tempData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( now ) );
  tempData[ "data" ] = QVariant::fromValue( cpuTemp );

  // Fan speed from the first fan (if available)
  QVariantMap speedData;
  if ( m_data.fans.size() > 0 )
  {
    speedData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( m_data.fans[0].speed.timestamp ) );
    speedData[ "data" ] = QVariant::fromValue( static_cast< int >( m_data.fans[0].speed.data ) );
  }
  else
  {
    speedData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( now ) );
    speedData[ "data" ] = QVariant::fromValue( -1 );
  }

  QVariantMap result;
  result[ "speed" ] = QVariant::fromValue( speedData );
  result[ "temp" ] = QVariant::fromValue( tempData );
  return result;
}

QVariantMap
UccDBusInterfaceAdaptor::GetFanDataGPU1()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );

  const int64_t now = std::chrono::duration_cast< std::chrono::milliseconds >(
    std::chrono::system_clock::now().time_since_epoch() ).count();

  // Read GPU temperature directly from NVML
  int gpuTemp = -1;
  if ( m_service && m_service->m_nvml && m_service->m_nvml->isAvailable() )
  {
    auto val = m_service->m_nvml->getTemperatureDegC( 0 );
    if ( val.has_value() )
      gpuTemp = static_cast< int >( *val );
  }

  if ( gpuTemp < 0 )
    return {};

  QVariantMap tempData;
  tempData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( now ) );
  tempData[ "data" ] = QVariant::fromValue( gpuTemp );

  QVariantMap speedData;
  speedData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( now ) );
  speedData[ "data" ] = QVariant::fromValue( -1 );

  QVariantMap result;
  result[ "speed" ] = QVariant::fromValue( speedData );
  result[ "temp" ] = QVariant::fromValue( tempData );
  return result;
}

QVariantMap
UccDBusInterfaceAdaptor::GetFanDataGPU2()
{
  // Only populated if a second GPU zone exists; currently not mapped
  return {};
}

QString
UccDBusInterfaceAdaptor::GetFanZoneTelemetryJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  QJsonObject root;
  for ( const auto &[zoneId, zt] : m_data.zoneTelemetry )
  {
    QJsonObject obj;
    obj["temp"] = zt.temp;
    obj["duty"] = zt.duty;
    if ( zt.rpm >= 0 )
      obj["rpm"] = zt.rpm;
    root[QString::fromStdString( zoneId )] = obj;
  }
  return QString::fromUtf8( QJsonDocument( root ).toJson( QJsonDocument::Compact ) );
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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
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

QString UccDBusInterfaceAdaptor::GetAppliedProfilesJSON()
{
  QJsonObject root;

  if ( !m_service )
    return QString::fromUtf8( QJsonDocument( root ).toJson( QJsonDocument::Compact ) );

  // Applied profile view for live UI consumers (tray).
  // Keep GetActiveProfileJSON() as persisted profile source of truth.
  root[ "profileId" ] = QString::fromStdString( m_service->m_activeProfile.id );
  root[ "profileName" ] = QString::fromStdString( m_service->m_activeProfile.name );
  root[ "fanProfileId" ] = QString::fromStdString( m_service->m_activeProfile.fan.fanProfile );
  root[ "wcAutoControl" ] = m_service->m_activeProfile.fan.autoControlWC;
  root[ "keyboardProfileId" ] = QString::fromStdString( m_service->m_activeProfile.keyboard.keyboardProfileId );
  root[ "savedGpuProfileId" ] = QString::fromStdString( m_service->m_activeProfile.gpuProfileId );

  std::string appliedGpuProfileId = m_service->m_activeProfile.gpuProfileId;
  if ( !m_lastAutoAppliedGpuProfileId.empty() )
    appliedGpuProfileId = m_lastAutoAppliedGpuProfileId;

  root[ "appliedGpuProfileId" ] = QString::fromStdString( appliedGpuProfileId );
  root[ "appliedByApp" ] = QString::fromStdString( m_lastAutoAppliedApp );
  root[ "appliedByPid" ] = static_cast< qlonglong >( m_lastAutoAppliedPid );

  return QString::fromUtf8( QJsonDocument( root ).toJson( QJsonDocument::Compact ) );
}

bool UccDBusInterfaceAdaptor::ApplyFanProfiles( const QString &fanProfilesJSONq )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( not m_service )
    return false;

  const std::string fanProfilesJSON = fanProfilesJSONq.toStdString();

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

    // Build zone curves map from the JSON
    std::map< std::string, std::vector< ucc::hal::FanCurvePoint > > zoneCurves;

    // Register custom thermal sources from the profile before zone processing
    {
      auto tsJson = extractArray( "thermalSources" );
      if ( !tsJson.empty() )
      {
        auto tsProfile = ProfileManager::parseFanProfileJSON(
          "{\"thermalSources\":" + tsJson + "}" );
        if ( !tsProfile.thermalSources.empty() )
        {
          m_service->m_hw.addThermalSources( tsProfile.thermalSources );
          std::cerr << "[DBus] Registered " << tsProfile.thermalSources.size()
                    << " custom thermal sources" << std::endl;
        }
      }
    }

    // Parse "zones" array
    auto zonesJson = extractArray( "zones" );
    if ( !zonesJson.empty() )
    {
      auto zoneProfile = ProfileManager::parseFanProfileJSON(
        "{\"zones\":" + zonesJson + "}" );

      // If the zones carry topology (fanIds), rebuild the zone model
      bool hasTopology = false;
      for ( const auto &zc : zoneProfile.zoneCurves )
      {
        if ( zc.hasTopology() )
        {
          hasTopology = true;
          break;
        }
      }

      if ( hasTopology && m_service )
      {
        FanProfile topologyProfile;
        topologyProfile.zoneCurves = zoneProfile.zoneCurves;
        m_service->rebuildFanZonesFromProfile( topologyProfile );

        // Restart worker to pick up new zone topology
        if ( m_service->m_fanControlWorker )
        {
          m_service->m_fanControlWorker->stop();
          m_service->m_fanControlWorker->start();
          std::cerr << "[DBus] Restarted FanControlWorker with new zone topology" << std::endl;
        }
      }

      for ( const auto &zc : zoneProfile.zoneCurves )
      {
        if ( !zc.curve.empty() )
          zoneCurves[zc.zoneId] = zc.curve;
      }

      // Apply per-zone thermal source overrides
      if ( m_service && m_service->m_fanControlWorker )
      {
        std::map< std::string, std::string > thermalSources;
        for ( const auto &zc : zoneProfile.zoneCurves )
        {
          std::cerr << "[DBus] Zone '" << zc.zoneId << "' thermalSourceId='" << zc.thermalSourceId << "'" << std::endl;
          if ( !zc.thermalSourceId.empty() )
            thermalSources[zc.zoneId] = zc.thermalSourceId;
        }
        if ( !thermalSources.empty() )
        {
          m_service->m_fanControlWorker->applyZoneThermalSources( thermalSources );
          std::cerr << "[DBus] Applied thermal sources for " << thermalSources.size() << " zones" << std::endl;
        }
        else
        {
          std::cerr << "[DBus] WARNING: No thermal source overrides found in zone data!" << std::endl;
        }
      }
    }

    // Apply the temporary zone curves
    if ( m_service->m_fanControlWorker && !zoneCurves.empty() )
    {
      m_service->m_fanControlWorker->applyTemporaryZoneCurves( zoneCurves );
      std::cerr << "[DBus] Applied temporary zone curves (" << zoneCurves.size() << " zones)" << std::endl;
    }

    // If the caller provided a fan profile ID, update the active profile
    // reference and notify all clients so they stay in sync.
    {
      auto extractStr = []( const std::string &json, const std::string &key ) -> std::string {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find( search );
        if ( pos == std::string::npos ) return {};
        pos += search.length();
        size_t end = json.find( '"', pos );
        if ( end == std::string::npos ) return {};
        return json.substr( pos, end - pos );
      };
      std::string fanProfileId = extractStr( fanProfilesJSON, "fanProfileId" );
      if ( !fanProfileId.empty() )
      {
        m_service->m_activeProfile.fan.fanProfile = fanProfileId;
        m_service->updateDBusActiveProfileData();
        if ( m_service->m_adaptor )
          emitProfileChanged( m_service->m_activeProfile.id,
                              m_service->m_activeProfile.keyboard.keyboardProfileId,
                              fanProfileId );
      }
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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  m_data.tempProfileName = profileName.toStdString();
  return true;
}

bool UccDBusInterfaceAdaptor::SetActiveProfile( const QString &id )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // Immediately set the active profile
  return m_service->setCurrentProfileById( id.toStdString() );
}

bool UccDBusInterfaceAdaptor::ApplyProfile( const QString &profileJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // Apply the profile configuration sent by the GUI
  return m_service->applyProfileJSON( profileJSON.toStdString() );
}



QString UccDBusInterfaceAdaptor::GetProfilesJSON()
{
  // Returns ALL profiles (built-in + custom) with "editable" flag
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.profilesJSON );
}

QString UccDBusInterfaceAdaptor::GetCustomProfilesJSON()
{
  // Backward-compat: returns only editable (custom) profiles
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.customProfilesJSON );
}

QString UccDBusInterfaceAdaptor::GetDefaultProfilesJSON()
{
  // Backward-compat: returns only built-in profiles
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

// ---------------------------------------------------------------------------
// Unified profile save/delete (new API)
// ---------------------------------------------------------------------------

bool UccDBusInterfaceAdaptor::SaveProfile( const QString &profileJSON )
{
  // Unified save: handles both new and existing custom profiles.
  // Forwards to the existing SaveCustomProfile logic.
  return SaveCustomProfile( profileJSON );
}

bool UccDBusInterfaceAdaptor::DeleteProfile( const QString &profileId )
{
  // Unified delete: forwards to existing logic.
  return DeleteCustomProfile( profileId );
}

// ---------------------------------------------------------------------------
// Backward-compatible aliases (deprecated — forward to unified methods)
// ---------------------------------------------------------------------------

bool UccDBusInterfaceAdaptor::AddCustomProfile( const QString &profileJSON )
{
  return SaveProfile( profileJSON );
}

bool UccDBusInterfaceAdaptor::DeleteCustomProfile( const QString &profileId )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
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
  return SaveProfile( profileJSON );
}

bool UccDBusInterfaceAdaptor::SaveCustomProfile( const QString &profileJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
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
    auto existingProfileIt = std::ranges::find_if( m_service->m_customProfiles,
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

    bool result = false;
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
    if ( auto it = std::remove_if( m_service->m_customProfiles.begin(), m_service->m_customProfiles.end(),
                             [](const UccProfile &p) { return p.id.empty(); } );
         it != m_service->m_customProfiles.end() )
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

// ---------------------------------------------------------------------------
// Unified fan profile methods (new API)
// ---------------------------------------------------------------------------

QString UccDBusInterfaceAdaptor::GetFanProfilesJSON()
{
  // Returns all fan profiles: built-in (editable=false) + custom (editable=true)
  std::string json = "[";
  size_t idx = 0;

  if ( m_service )
  {
    for ( const auto &fp : m_service->m_builtinFanProfiles )
    {
      if ( idx > 0 ) json += ",";
      json += "{\"id\":\"" + fp.id + "\","
              "\"name\":\"" + fp.name + "\","
              "\"editable\":false}";
      ++idx;
    }
  }

  if ( m_service )
  {
    for ( const auto &fp : m_service->m_customFanProfiles )
    {
      if ( idx > 0 ) json += ",";
      json += "{\"id\":\"" + fp.id + "\","
              "\"name\":\"" + fp.name + "\","
              "\"editable\":true}";
      ++idx;
    }
  }

  json += "]";
  return QString::fromStdString( json );
}

QString UccDBusInterfaceAdaptor::GetThermalSourcesJSON()
{
  if ( !m_service )
    return QStringLiteral( "[]" );

  const auto &sources = m_service->m_hw.thermalSources();
  std::string json = "[";
  for ( size_t i = 0; i < sources.size(); ++i )
  {
    const auto &ts = sources[i];
    if ( i > 0 ) json += ',';
    json += "{\"id\":\"" + ts.id + "\"";
    json += ",\"label\":\"" + ts.label + "\"";
    json += ",\"strategy\":\"" + ucc::hal::thermalStrategyToString( ts.strategy ) + "\"";
    json += ",\"sensorIds\":[";
    for ( size_t s = 0; s < ts.sensorIds.size(); ++s )
    {
      if ( s > 0 ) json += ',';
      json += "\"" + ts.sensorIds[s] + "\"";
    }
    json += "]";
    json += ",\"weights\":[";
    for ( size_t w = 0; w < ts.weights.size(); ++w )
    {
      if ( w > 0 ) json += ',';
      json += std::to_string( ts.weights[w] );
    }
    json += "]";
    json += "}";
  }
  json += "]";
  return QString::fromStdString( json );
}

QString UccDBusInterfaceAdaptor::GetSensorReadingsJSON()
{
  if ( !m_service )
    return QStringLiteral( "{}" );

  QJsonObject root;

  // Individual sensor readings
  for ( const auto &sensor : m_service->m_hw.tempSensors() )
  {
    auto val = m_service->m_hw.readTemp( sensor );
    if ( val.has_value() )
      root[QString::fromStdString( sensor.id )] = static_cast< int >( std::round( val.value() ) );
  }

  // Resolved thermal source readings (prefixed with "_source:")
  for ( const auto &ts : m_service->m_hw.thermalSources() )
  {
    auto val = m_service->m_hw.readThermalSource( ts );
    if ( val.has_value() )
    {
      root[QStringLiteral( "_source:" ) + QString::fromStdString( ts.id )] =
        static_cast< int >( std::round( val.value() ) );
    }
  }

  // Fan RPM readings (prefixed with "fan:")
  if ( auto *fp = m_service->m_hw.fanProvider() )
  {
    for ( const auto &fan : m_service->m_hw.fans() )
    {
      if ( fan.canRead )
      {
        auto rpm = fp->getFanRPM( fan );
        if ( rpm.has_value() )
          root[QStringLiteral( "fan:" ) + QString::fromStdString( fan.id )] =
            static_cast< int >( *rpm );
      }
    }
  }

  return QString::fromUtf8( QJsonDocument( root ).toJson( QJsonDocument::Compact ) );
}

QString UccDBusInterfaceAdaptor::GetHardwareFanDevicesJSON()
{
  if ( !m_service )
    return QStringLiteral( "[]" );

  const auto &fans = m_service->m_hw.fans();
  std::string json = "[";
  for ( size_t i = 0; i < fans.size(); ++i )
  {
    const auto &f = fans[i];
    if ( i > 0 ) json += ',';
    json += "{\"id\":\"" + f.id + "\"";
    json += ",\"label\":\"" + f.label + "\"";
    json += ",\"hwmonPath\":\"" + f.hwmonPath + "\"";
    json += ",\"index\":" + std::to_string( f.index );
    json += ",\"canRead\":" + std::string( f.canRead ? "true" : "false" );
    json += ",\"canControl\":" + std::string( f.canControl ? "true" : "false" );
    json += ",\"deviceType\":\"" + ucc::hal::fanDeviceTypeToString( f.deviceType ) + "\"";
    json += "}";
  }
  json += "]";
  return QString::fromStdString( json );
}

QString UccDBusInterfaceAdaptor::GetHardwareSensorsJSON()
{
  if ( !m_service )
    return QStringLiteral( "[]" );

  auto trimCopy = []( const std::string &in ) -> std::string {
    size_t b = 0;
    while ( b < in.size() && std::isspace( static_cast< unsigned char >( in[b] ) ) )
      ++b;
    size_t e = in.size();
    while ( e > b && std::isspace( static_cast< unsigned char >( in[e - 1] ) ) )
      --e;
    return in.substr( b, e - b );
  };

  auto readFirstLine = [&]( const std::string &path ) -> std::string {
    std::ifstream f( path );
    if ( !f.is_open() )
      return {};
    std::string line;
    std::getline( f, line );
    return trimCopy( line );
  };

  auto resolveNetworkSourceDisplay = [&]( const std::string &source ) -> std::string {
    if ( source.empty() )
      return {};

    const std::filesystem::path netPath = std::filesystem::path( "/sys/class/net" ) / source;
    const std::filesystem::path devPath = netPath / "device";
    if ( !std::filesystem::exists( netPath ) || !std::filesystem::exists( devPath ) )
      return {};

    std::string model;
    std::string vendor;
    std::string busId;
    std::string driver;

    struct udev *udevCtx = udev_new();
    if ( !udevCtx )
      return {};

    struct udev_device *netDev = udev_device_new_from_subsystem_sysname( udevCtx, "net", source.c_str() );
    if ( !netDev )
    {
      udev_unref( udevCtx );
      return {};
    }

    const char *driverProp = udev_device_get_property_value( netDev, "ID_NET_DRIVER" );
    if ( driverProp )
      driver = driverProp;

    struct udev_device *parent = udev_device_get_parent_with_subsystem_devtype( netDev, "pci", nullptr );
    if ( !parent )
      parent = udev_device_get_parent_with_subsystem_devtype( netDev, "usb", nullptr );
    if ( !parent )
      parent = udev_device_get_parent_with_subsystem_devtype( netDev, "platform", nullptr );

    if ( parent )
    {
      if ( const char *v = udev_device_get_property_value( parent, "ID_MODEL_FROM_DATABASE" ); v )
        model = v;
      if ( const char *v = udev_device_get_property_value( parent, "ID_MODEL" ); v && model.empty() )
        model = v;
      if ( const char *v = udev_device_get_property_value( parent, "ID_VENDOR_FROM_DATABASE" ); v )
        vendor = v;
      if ( const char *v = udev_device_get_sysname( parent ); v )
        busId = v;
    }

    udev_device_unref( netDev );
    udev_unref( udevCtx );

    std::string resolved;
    if ( !vendor.empty() && !model.empty() )
    {
      const std::string lVendor = QString::fromStdString( vendor ).toLower().toStdString();
      const std::string lModel = QString::fromStdString( model ).toLower().toStdString();
      if ( lModel.find( lVendor ) != std::string::npos )
        resolved = model;
      else
        resolved = vendor + " " + model;
    }
    else if ( !model.empty() )
      resolved = model;
    else if ( !vendor.empty() )
      resolved = vendor;

    if ( resolved.empty() )
      return {};

    if ( !driver.empty() )
      resolved += " [" + driver + "]";
    if ( !busId.empty() )
      resolved += " (" + busId + ")";

    return resolved;
  };

  const std::vector< MemoryModuleInfo > dmiMemDevices = detectMemoryModulesFromSmbios();

  auto findNvmeDeviceDir = []( const std::string &hwmonPath ) -> std::filesystem::path {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::path( hwmonPath );
    if ( p.empty() )
      return {};

    p = fs::weakly_canonical( p, ec );
    if ( ec )
      p = fs::path( hwmonPath );

    for ( fs::path cur = p; !cur.empty(); cur = cur.parent_path() )
    {
      const std::string name = cur.filename().string();
      if ( name.rfind( "nvme", 0 ) == 0 )
        return cur;
      if ( cur == cur.parent_path() )
        break;
    }
    return {};
  };

  auto extractPciAddress = []( const std::filesystem::path &start ) -> std::string {
    auto isHex = []( char c ) -> bool {
      return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
    };
    auto looksLikePciBdf = [&]( const std::string &s ) -> bool {
      if ( s.size() != 12 )
        return false;
      return isHex( s[0] ) && isHex( s[1] ) && isHex( s[2] ) && isHex( s[3] )
          && s[4] == ':'
          && isHex( s[5] ) && isHex( s[6] )
          && s[7] == ':'
          && isHex( s[8] ) && isHex( s[9] )
          && s[10] == '.'
          && ( s[11] >= '0' && s[11] <= '7' );
    };

    for ( std::filesystem::path cur = start; !cur.empty(); cur = cur.parent_path() )
    {
      const std::string n = cur.filename().string();
      if ( looksLikePciBdf( n ) )
        return n;
      if ( cur == cur.parent_path() )
        break;
    }
    return {};
  };

  auto buildNvmeDisplayLabel = [&]( const ucc::hal::TempSensorInfo &s ) -> std::string {
    const std::filesystem::path nvmeDir = findNvmeDeviceDir( s.hwmonPath );
    if ( nvmeDir.empty() )
      return {};

    const std::string model = readFirstLine( ( nvmeDir / "model" ).string() );
    const std::string serial = readFirstLine( ( nvmeDir / "serial" ).string() );
    const std::string fw = readFirstLine( ( nvmeDir / "firmware_rev" ).string() );
    const std::string bdf = extractPciAddress( nvmeDir );

    std::string label = model.empty() ? std::string( "NVMe" ) : model;
    std::vector< std::string > meta;
    if ( !serial.empty() ) meta.push_back( serial );
    if ( !fw.empty() ) meta.push_back( "FW " + fw );
    if ( !bdf.empty() ) meta.push_back( bdf );

    if ( !meta.empty() )
    {
      label += " (";
      for ( size_t i = 0; i < meta.size(); ++i )
      {
        if ( i > 0 ) label += ", ";
        label += meta[i];
      }
      label += ")";
    }

    return label;
  };

  auto parseDdrSlotFromPath = []( const std::string &hwmonPath ) -> int {
    const size_t dash = hwmonPath.rfind( '-' );
    if ( dash == std::string::npos || dash + 5 > hwmonPath.size() )
      return 0;
    const std::string tail = hwmonPath.substr( dash + 1, 4 );
    if ( tail.size() != 4 )
      return 0;

    char *end = nullptr;
    const long addr = std::strtol( tail.c_str(), &end, 16 );
    if ( end == nullptr || *end != '\0' )
      return 0;

    if ( addr >= 0x50 && addr <= 0x57 )
      return static_cast< int >( addr - 0x50 + 1 );
    return 0;
  };

  auto parseSlotFromText = [&]( const std::string &text ) -> int {
    if ( text.empty() )
      return 0;

    const std::string t = QString::fromStdString( text ).toLower().toStdString();

    // First, try explicit "slot N" form.
    const size_t slotPos = t.find( "slot" );
    if ( slotPos != std::string::npos )
    {
      for ( size_t i = slotPos + 4; i < t.size(); ++i )
      {
        if ( std::isdigit( static_cast< unsigned char >( t[i] ) ) )
        {
          int n = 0;
          while ( i < t.size() && std::isdigit( static_cast< unsigned char >( t[i] ) ) )
          {
            n = ( n * 10 ) + ( t[i] - '0' );
            ++i;
          }
          return n > 0 ? n : 0;
        }
      }
    }

    // Then try DIMM-style labels like A1/B2/... and map to a linear slot index.
    for ( size_t i = 0; i + 1 < t.size(); ++i )
    {
      const char a = t[i];
      const char b = t[i + 1];
      if ( a >= 'a' && a <= 'h' && b >= '1' && b <= '8' )
      {
        const int channel = ( a - 'a' );
        const int pos = ( b - '0' );
        return channel * 2 + pos;
      }
    }

    return 0;
  };

  const auto &sensors = m_service->m_hw.tempSensors();

  std::map< int, const MemoryModuleInfo * > modulesBySlot;
  for ( const auto &md : dmiMemDevices )
  {
    int slot = parseSlotFromText( md.locator );
    if ( slot <= 0 )
      slot = parseSlotFromText( md.bankLocator );
    if ( slot > 0 )
      modulesBySlot[slot] = &md;
  }

  // If locator parsing is incomplete, map remaining modules to observed DDR slots in order.
  std::set< int > observedDdrSlots;
  for ( const auto &s : sensors )
  {
    const QString src = QString::fromStdString( s.source ).toLower();
    if ( !src.contains( QStringLiteral( "spd" ) ) )
      continue;
    const int slot = parseDdrSlotFromPath( s.hwmonPath );
    if ( slot > 0 )
      observedDdrSlots.insert( slot );
  }

  if ( !observedDdrSlots.empty() )
  {
    std::vector< const MemoryModuleInfo * > unassignedModules;
    for ( const auto &md : dmiMemDevices )
    {
      bool alreadyAssigned = false;
      for ( const auto &[slot, ptr] : modulesBySlot )
      {
        if ( ptr == &md )
        {
          alreadyAssigned = true;
          break;
        }
      }
      if ( !alreadyAssigned )
        unassignedModules.push_back( &md );
    }

    std::vector< int > unassignedSlots;
    for ( const int slot : observedDdrSlots )
    {
      if ( modulesBySlot.find( slot ) == modulesBySlot.end() )
        unassignedSlots.push_back( slot );
    }

    const size_t n = std::min( unassignedSlots.size(), unassignedModules.size() );
    for ( size_t i = 0; i < n; ++i )
      modulesBySlot[unassignedSlots[i]] = unassignedModules[i];
  }

  auto buildDdrDisplayLabel = [&]( const ucc::hal::TempSensorInfo &s ) -> std::string {
    const int slot = parseDdrSlotFromPath( s.hwmonPath );
    const MemoryModuleInfo *md = nullptr;
    if ( slot > 0 )
    {
      if ( auto it = modulesBySlot.find( slot ); it != modulesBySlot.end() )
        md = it->second;
    }

    if ( !md )
    {
      if ( slot > 0 )
        return "DDR5 module, SLOT " + std::to_string( slot );
      return {};
    }

    std::string label;
    if ( md->sizeMiB > 0 )
    {
      std::ostringstream ss;
      ss << std::fixed << std::setprecision( 1 )
         << ( static_cast< double >( md->sizeMiB ) / 1024.0 ) << " GiB";
      label += ss.str();
    }

    const int speedMTs = md->configuredSpeedMTs > 0 ? md->configuredSpeedMTs : md->maxSpeedMTs;
    if ( speedMTs > 0 )
    {
      if ( !label.empty() ) label += " @ ";
      label += std::to_string( speedMTs ) + " MT/s";
    }

    if ( md->configuredVoltageMv > 0 )
    {
      std::ostringstream vs;
      vs << std::fixed << std::setprecision( 3 )
         << ( static_cast< double >( md->configuredVoltageMv ) / 1000.0 ) << " V";
      if ( !label.empty() ) label += " ";
      label += vs.str();
    }

    if ( slot > 0 )
    {
      if ( !label.empty() ) label += ", ";
      label += "SLOT " + std::to_string( slot );
    }
    else if ( !md->locator.empty() )
    {
      if ( !label.empty() ) label += ", ";
      label += md->locator;
    }

    return label;
  };

  auto classifySensorCategory = []( const std::string &source,
                                    const std::string &label,
                                    const std::string &hwmonPath ) -> std::string {
    const QString s = ( QString::fromStdString( source ) + " "
                        + QString::fromStdString( label ) + " "
                        + QString::fromStdString( hwmonPath ) ).toLower();

    if ( s.contains( QStringLiteral( "gpu" ) )
         || s.contains( QStringLiteral( "nvidia" ) )
         || s.contains( QStringLiteral( "amdgpu" ) )
         || s.contains( QStringLiteral( "radeon" ) ) )
      return "gpu";

    if ( s.contains( QStringLiteral( "k10temp" ) )
         || s.contains( QStringLiteral( "coretemp" ) )
         || s.contains( QStringLiteral( "cpu" ) ) )
      return "cpu";

    if ( s.contains( QStringLiteral( "nvme" ) ) )
      return "nvme";

    if ( s.contains( QStringLiteral( "spd5118" ) )
         || s.contains( QStringLiteral( "spd" ) ) )
      return "ddr5";

    if ( s.contains( QStringLiteral( "nct6799" ) )
         || s.contains( QStringLiteral( "nct" ) )
         || s.contains( QStringLiteral( "it87" ) )
         || s.contains( QStringLiteral( "w836" ) )
         || s.contains( QStringLiteral( "f718" ) )
         || s.contains( QStringLiteral( "acpi" ) )
         || s.contains( QStringLiteral( "ec" ) )
         || s.contains( QStringLiteral( "board" ) )
         || s.contains( QStringLiteral( "pch" ) )
         || s.contains( QStringLiteral( "chipset" ) ) )
      return "board";

    return "other";
  };

  std::string json = "[";
  bool first = true;

  auto appendObj = [&]( const std::string &obj ) {
    if ( !first ) json += ',';
    json += obj;
    first = false;
  };

  for ( size_t i = 0; i < sensors.size(); ++i )
  {
    const auto &s = sensors[i];
    const std::string category = classifySensorCategory( s.source, s.label, s.hwmonPath );
    std::string displayLabel;
    std::string sourceDisplay;

    sourceDisplay = resolveNetworkSourceDisplay( s.source );
    if ( category == "nvme" )
      displayLabel = buildNvmeDisplayLabel( s );
    else if ( category == "ddr5" )
      displayLabel = buildDdrDisplayLabel( s );

    std::string obj = "{\"id\":\"" + s.id + "\"";
    obj += ",\"label\":\"" + jsonEscape( s.label ) + "\"";
    if ( !displayLabel.empty() )
      obj += ",\"displayLabel\":\"" + jsonEscape( displayLabel ) + "\"";
    obj += ",\"source\":\"" + jsonEscape( s.source ) + "\"";
    if ( !sourceDisplay.empty() )
      obj += ",\"sourceDisplay\":\"" + jsonEscape( sourceDisplay ) + "\"";
    obj += ",\"category\":\"" + category + "\"";
    obj += ",\"hwmonPath\":\"" + jsonEscape( s.hwmonPath ) + "\"";
    obj += ",\"index\":" + std::to_string( s.index );
    obj += "}";
    appendObj( obj );
  }

  // Add iGPU virtual sensor for systems where hwmon doesn't cover integrated GPU.
  // dGPU temp is provided by NvmlTempProvider as sensor \"gpu-dgpu-temp\".
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );

    const QString iGpuModel = QString::fromStdString( m_service->m_systemInfo.iGpuModel ).trimmed();

    auto igpuDoc = QJsonDocument::fromJson( QByteArray::fromStdString( m_data.iGpuInfoValuesJSON ) );
    if ( !iGpuModel.isEmpty() )
    {
      const QJsonObject o = igpuDoc.isObject() ? igpuDoc.object() : QJsonObject();
      const double temp = o.value( QStringLiteral( "temp" ) ).toDouble( -1.0 );

      std::string obj = "{\"id\":\"gpu-igpu-temp\"";
      obj += ",\"label\":\"Temperature\"";
      obj += ",\"source\":\"" + jsonEscape( iGpuModel.toStdString() ) + "\"";
      obj += ",\"category\":\"gpu\"";
      obj += ",\"hwmonPath\":\"\"";
      obj += ",\"index\":1";
      if ( temp >= 0.0 )
        obj += ",\"currentTempC\":" + std::to_string( static_cast< int >( std::lround( temp ) ) );
      obj += "}";
      appendObj( obj );
    }
  }

  json += "]";
  return QString::fromStdString( json );
}

QString UccDBusInterfaceAdaptor::GetFanZonesJSON()
{
  if ( !m_service )
    return QStringLiteral( "[]" );

  const auto &zones = m_service->m_hw.defaultFanZones();
  std::string json = "[";
  for ( size_t i = 0; i < zones.size(); ++i )
  {
    const auto &z = zones[i];
    if ( i > 0 ) json += ',';
    json += "{\"id\":\"" + z.id + "\"";
    json += ",\"name\":\"" + z.name + "\"";
    json += ",\"deviceType\":\"" + ucc::hal::fanDeviceTypeToString( z.defaultType ) + "\"";
    json += ",\"thermalSourceId\":\"" + z.thermalSourceId + "\"";
    json += ",\"fanIds\":[";
    for ( size_t f = 0; f < z.fanIds.size(); ++f )
    {
      if ( f > 0 ) json += ',';
      json += "\"" + z.fanIds[f] + "\"";
    }
    json += "]";
    json += "}";
  }
  json += "]";
  return QString::fromStdString( json );
}

QString UccDBusInterfaceAdaptor::GetFanProfileJSON( const QString &id )
{
  const std::string requestedId = id.toStdString();

  // Check custom fan profiles first (daemon-side)
  if ( m_service )
  {
    for ( const auto &fp : m_service->m_customFanProfiles )
    {
      if ( fp.id == requestedId )
        return QString::fromStdString( fp.json );
    }

    for ( const auto &fp : m_service->m_builtinFanProfiles )
    {
      if ( fp.id == requestedId )
        return QString::fromStdString( fp.json );
    }
  }

  return QStringLiteral( "{}" );
}

bool UccDBusInterfaceAdaptor::SaveFanProfile( const QString &id, const QString &name, const QString &json )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service ) return false;

  const std::string sid = id.toStdString();
  const std::string sname = name.toStdString();
  const std::string sjson = json.toStdString();

  // Check this doesn't collide with a built-in fan profile
  for ( const auto &fp : m_service->m_builtinFanProfiles )
  {
    if ( fp.id == sid )
    {
      std::cerr << "[FanProfile] Cannot overwrite built-in fan profile '" << sid << "'" << std::endl;
      return false;
    }
  }

  // Update or add
  bool found = false;
  for ( auto &fp : m_service->m_customFanProfiles )
  {
    if ( fp.id == sid )
    {
      fp.name = sname;
      fp.json = sjson;
      found = true;
      break;
    }
  }
  if ( !found )
  {
    m_service->m_customFanProfiles.push_back( { sid, sname, sjson } );
  }

  // Persist to settings
  // Wrap the raw fan JSON inside an object with "name" so loadSubProfiles can extract it
  nlohmann::json wrapper;
  try { wrapper = nlohmann::json::parse( sjson ); } catch ( ... ) { wrapper = nlohmann::json::object(); }
  wrapper["name"] = sname;
  m_service->m_settings.fanProfiles[sid] = wrapper.dump();

  if ( m_service->m_settingsManager.writeSettings( m_service->m_settings ) )
    std::cout << "[FanProfile] Saved fan profile '" << sname << "' (ID: " << sid << ")" << std::endl;
  else
    std::cerr << "[FanProfile] Failed to persist fan profile '" << sname << "'" << std::endl;

  return true;
}

bool UccDBusInterfaceAdaptor::DeleteFanProfile( const QString &id )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service ) return false;

  const std::string sid = id.toStdString();

  // Cannot delete built-in
  for ( const auto &fp : m_service->m_builtinFanProfiles )
  {
    if ( fp.id == sid ) return false;
  }

  auto &vec = m_service->m_customFanProfiles;
  auto it = std::remove_if( vec.begin(), vec.end(),
                            [&sid]( const UccDBusService::SubProfile &p ) { return p.id == sid; } );
  if ( it == vec.end() ) return false;

  vec.erase( it, vec.end() );
  m_service->m_settings.fanProfiles.erase( sid );
  (void) m_service->m_settingsManager.writeSettings( m_service->m_settings );
  std::cout << "[FanProfile] Deleted fan profile '" << sid << "'" << std::endl;
  return true;
}

// ---------------------------------------------------------------------------
// Unified GPU profile methods (new API)
// ---------------------------------------------------------------------------

QString UccDBusInterfaceAdaptor::GetGpuProfilesJSON()
{
  if ( !m_service ) return QStringLiteral( "[]" );

  QJsonArray arr;

  // Built-in GPU profiles (editable=false)
  for ( const auto &profile : m_service->m_builtinGpuProfiles )
  {
    QJsonObject obj;
    obj[ "id" ] = QString::fromStdString( profile.id );
    obj[ "name" ] = QString::fromStdString( profile.name );
    obj[ "editable" ] = false;
    arr.append( obj );
  }

  // Custom GPU profiles (editable=true)
  for ( const auto &profile : m_service->m_customGpuProfiles )
  {
    QJsonObject obj;
    obj[ "id" ] = QString::fromStdString( profile.id );
    obj[ "name" ] = QString::fromStdString( profile.name );
    obj[ "editable" ] = true;
    arr.append( obj );
  }

  return QString::fromUtf8( QJsonDocument( arr ).toJson( QJsonDocument::Compact ) );
}

QString UccDBusInterfaceAdaptor::GetGpuProfileJSON( const QString &id )
{
  if ( !m_service ) return QStringLiteral( "{}" );

  const std::string requestedId = id.toStdString();

  // Check custom GPU profiles first
  for ( const auto &profile : m_service->m_customGpuProfiles )
  {
    if ( profile.id == requestedId )
      return QString::fromStdString( profile.json );
  }

  // Fall back to built-in GPU profiles
  auto it = std::find_if( m_service->m_builtinGpuProfiles.begin(),
                          m_service->m_builtinGpuProfiles.end(),
                          [&requestedId]( const UccDBusService::SubProfile &profile ) {
                            return profile.id == requestedId;
                          } );
  if ( it != m_service->m_builtinGpuProfiles.end() )
    return QString::fromStdString( it->json );

  return QStringLiteral( "{}" );
}

bool UccDBusInterfaceAdaptor::SaveGpuProfile( const QString &id, const QString &name, const QString &json )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service ) return false;

  const std::string sid = id.toStdString();
  const std::string sname = name.toStdString();
  const std::string sjson = json.toStdString();

  // Cannot overwrite built-in
  for ( const auto &gp : m_service->m_builtinGpuProfiles )
  {
    if ( gp.id == sid )
    {
      std::cerr << "[GpuProfile] Cannot overwrite built-in GPU profile '" << sid << "'" << std::endl;
      return false;
    }
  }

  bool found = false;
  for ( auto &gp : m_service->m_customGpuProfiles )
  {
    if ( gp.id == sid )
    {
      gp.name = sname;
      gp.json = sjson;
      found = true;
      break;
    }
  }
  if ( !found )
    m_service->m_customGpuProfiles.push_back( { sid, sname, sjson } );

  nlohmann::json wrapper;
  try { wrapper = nlohmann::json::parse( sjson ); } catch ( ... ) { wrapper = nlohmann::json::object(); }
  wrapper["name"] = sname;
  m_service->m_settings.gpuProfiles[sid] = wrapper.dump();
  (void) m_service->m_settingsManager.writeSettings( m_service->m_settings );
  std::cout << "[GpuProfile] Saved GPU profile '" << sname << "' (ID: " << sid << ")" << std::endl;
  return true;
}

bool UccDBusInterfaceAdaptor::DeleteGpuProfile( const QString &id )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service ) return false;

  const std::string sid = id.toStdString();

  for ( const auto &gp : m_service->m_builtinGpuProfiles )
  {
    if ( gp.id == sid ) return false;
  }

  auto &vec = m_service->m_customGpuProfiles;
  auto it = std::remove_if( vec.begin(), vec.end(),
                            [&sid]( const UccDBusService::SubProfile &p ) { return p.id == sid; } );
  if ( it == vec.end() ) return false;

  vec.erase( it, vec.end() );
  m_service->m_settings.gpuProfiles.erase( sid );
  (void) m_service->m_settingsManager.writeSettings( m_service->m_settings );
  std::cout << "[GpuProfile] Deleted GPU profile '" << sid << "'" << std::endl;
  return true;
}

// ---------------------------------------------------------------------------
// Unified keyboard profile methods (new API)
// ---------------------------------------------------------------------------

QString UccDBusInterfaceAdaptor::GetKeyboardProfilesJSON()
{
  if ( !m_service ) return QStringLiteral( "[]" );

  QJsonArray arr;

  // Built-in keyboard profiles (editable=false)
  for ( const auto &profile : m_service->m_builtinKeyboardProfiles )
  {
    QJsonObject obj;
    obj[ "id" ] = QString::fromStdString( profile.id );
    obj[ "name" ] = QString::fromStdString( profile.name );
    obj[ "editable" ] = false;
    arr.append( obj );
  }

  // Custom keyboard profiles (editable=true)
  for ( const auto &profile : m_service->m_customKeyboardProfiles )
  {
    QJsonObject obj;
    obj[ "id" ] = QString::fromStdString( profile.id );
    obj[ "name" ] = QString::fromStdString( profile.name );
    obj[ "editable" ] = true;
    arr.append( obj );
  }
  return QString::fromUtf8( QJsonDocument( arr ).toJson( QJsonDocument::Compact ) );
}

QString UccDBusInterfaceAdaptor::GetKeyboardProfileJSON( const QString &id )
{
  if ( !m_service ) return QStringLiteral( "{}" );

  const std::string requestedId = id.toStdString();

  // Check custom keyboard profiles first
  for ( const auto &profile : m_service->m_customKeyboardProfiles )
  {
    if ( profile.id == requestedId )
      return QString::fromStdString( profile.json );
  }

  // Fall back to built-in keyboard profiles
  for ( const auto &profile : m_service->m_builtinKeyboardProfiles )
  {
    if ( profile.id == requestedId )
      return QString::fromStdString( profile.json );
  }

  return QStringLiteral( "{}" );
}

bool UccDBusInterfaceAdaptor::SaveKeyboardProfile( const QString &id, const QString &name, const QString &json )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service ) return false;

  const std::string sid = id.toStdString();
  const std::string sname = name.toStdString();
  const std::string sjson = json.toStdString();

  // Cannot overwrite built-in
  for ( const auto &kp : m_service->m_builtinKeyboardProfiles )
  {
    if ( kp.id == sid )
    {
      std::cerr << "[KbProfile] Cannot overwrite built-in keyboard profile '" << sid << "'" << std::endl;
      return false;
    }
  }

  bool found = false;
  for ( auto &kp : m_service->m_customKeyboardProfiles )
  {
    if ( kp.id == sid )
    {
      kp.name = sname;
      kp.json = sjson;
      found = true;
      break;
    }
  }
  if ( !found )
    m_service->m_customKeyboardProfiles.push_back( { sid, sname, sjson } );

  nlohmann::json wrapper;
  try { wrapper = nlohmann::json::parse( sjson ); } catch ( ... ) { wrapper = nlohmann::json::object(); }
  wrapper["name"] = sname;
  m_service->m_settings.keyboardProfiles[sid] = wrapper.dump();
  (void) m_service->m_settingsManager.writeSettings( m_service->m_settings );
  std::cout << "[KbProfile] Saved keyboard profile '" << sname << "' (ID: " << sid << ")" << std::endl;
  return true;
}

bool UccDBusInterfaceAdaptor::DeleteKeyboardProfile( const QString &id )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service ) return false;

  const std::string sid = id.toStdString();

  // Cannot delete built-in
  for ( const auto &kp : m_service->m_builtinKeyboardProfiles )
  {
    if ( kp.id == sid ) return false;
  }

  auto &vec = m_service->m_customKeyboardProfiles;
  auto it = std::remove_if( vec.begin(), vec.end(),
                            [&sid]( const UccDBusService::SubProfile &p ) { return p.id == sid; } );
  if ( it == vec.end() ) return false;

  vec.erase( it, vec.end() );
  m_service->m_settings.keyboardProfiles.erase( sid );
  (void) m_service->m_settingsManager.writeSettings( m_service->m_settings );
  std::cout << "[KbProfile] Deleted keyboard profile '" << sid << "'" << std::endl;
  return true;
}

// ---------------------------------------------------------------------------
// Backward-compatible fan/GPU aliases (deprecated)
// ---------------------------------------------------------------------------

QString UccDBusInterfaceAdaptor::GetFanProfile( const QString &name )
{
  return GetFanProfileJSON( name );
}

QString UccDBusInterfaceAdaptor::GetFanProfileNames()
{
  return GetFanProfilesJSON();
}

QString UccDBusInterfaceAdaptor::GetGpuProfile( const QString &id )
{
  return GetGpuProfileJSON( id );
}

QString UccDBusInterfaceAdaptor::GetGpuProfileNames()
{
  return GetGpuProfilesJSON();
}

bool UccDBusInterfaceAdaptor::SetFanProfile( const QString & /*name*/, const QString & /*json*/ )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // Legacy D-Bus method — fan profiles are now managed through ApplyFanProfiles
  return false;
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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
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

bool UccDBusInterfaceAdaptor::SetBatchStateMap( const QString &stateMapJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service )
    return false;

  QJsonDocument doc = QJsonDocument::fromJson( stateMapJSON.toUtf8() );
  if ( !doc.isObject() )
  {
    std::cerr << "[DBus] SetBatchStateMap: Invalid JSON" << std::endl;
    return false;
  }

  QJsonObject map = doc.object();
  static const QStringList validStates = { "power_ac", "power_bat", "power_wc" };
  bool anyChanged = false;

  for ( auto it = map.constBegin(); it != map.constEnd(); ++it )
  {
    const std::string stateStr = it.key().toStdString();
    const std::string profileIdStr = it.value().toString().toStdString();

    if ( !validStates.contains( it.key() ) )
    {
      std::cerr << "[DBus] SetBatchStateMap: Invalid state '" << stateStr << "', skipping" << std::endl;
      continue;
    }

    // Verify the profile exists (same checks as SetStateMap)
    bool profileExists = false;
    for ( const auto &profile : m_service->m_customProfiles )
    {
      if ( profile.id == profileIdStr ) { profileExists = true; break; }
    }
    if ( !profileExists && m_service->m_settings.profiles.find( profileIdStr ) != m_service->m_settings.profiles.end() )
      profileExists = true;
    if ( !profileExists )
    {
      for ( const auto &profile : m_service->m_defaultProfiles )
      {
        if ( profile.id == profileIdStr ) { profileExists = true; break; }
      }
    }
    if ( !profileExists )
    {
      std::cerr << "[DBus] SetBatchStateMap: Profile ID '" << profileIdStr << "' does not exist, skipping" << std::endl;
      continue;
    }

    std::cout << "[DBus] SetBatchStateMap: " << stateStr << " -> " << profileIdStr << std::endl;
    m_service->m_settings.stateMap[stateStr] = profileIdStr;
    anyChanged = true;
  }

  if ( !anyChanged )
    return false;

  // Write settings only once for the entire batch
  const bool wrote = m_service->m_settingsManager.writeSettings( m_service->m_settings );
  if ( !wrote )
    std::cerr << "[Settings] Failed to persist batch stateMap update" << std::endl;
  m_service->updateDBusSettingsData();

  // If the current power state was among the changed entries, apply the new profile immediately
  const std::string currentStateKey = profileStateToString( m_service->m_currentState );
  if ( map.contains( QString::fromStdString( currentStateKey ) ) )
  {
    std::cout << "[DBus] SetBatchStateMap: Current state '" << currentStateKey
              << "' was updated, applying profile" << std::endl;
    m_service->applyProfileForCurrentState();
  }

  return wrote;
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
  if ( !m_service )
    return QStringLiteral( "[]" );

  auto *platform = m_service->m_hw.tdpProvider();
  if ( !platform )
    return QStringLiteral( "[]" );

  const int nrTDPs = platform->getNumberTDPs();
  if ( nrTDPs <= 0 )
    return QStringLiteral( "[]" );

  std::ostringstream jsonStream;
  jsonStream << "[";

  for ( int i = 0; i < nrTDPs; ++i )
  {
    const int current = platform->getTDP( i ).value_or( 0 );
    const int min     = platform->getTDPMin( i ).value_or( 0 );
    const int max     = platform->getTDPMax( i ).value_or( 0 );

    if ( i > 0 )
      jsonStream << ",";

    jsonStream << "{"
               << "\"current\":" << current << ","
               << "\"min\":" << min << ","
               << "\"max\":" << max
               << "}";
  }

  jsonStream << "]";
  return QString::fromStdString( jsonStream.str() );
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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service->m_settings.keyboardBacklightControlEnabled ) return false;

  auto inputJSON = keyboardBacklightStatesJSON.toStdString();

  // The caller sends a JSON object:
  //   { "keyboardProfileId": "...", "states": [...] }
  // Extract optional metadata before applying to hardware.
  auto extractStr = []( const std::string &json, const std::string &key ) -> std::string {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find( search );
    if ( pos == std::string::npos ) return {};
    pos += search.length();
    size_t end = json.find( '"', pos );
    if ( end == std::string::npos ) return {};
    return json.substr( pos, end - pos );
  };
  std::string keyboardProfileId = extractStr( inputJSON, "keyboardProfileId" );

  // Apply the states array to hardware (extracts "states" from the object)
  if ( !m_service->m_keyboardBacklightController.applyProfileKeyboardStates( inputJSON ) )
    return false;

  // Update the D-Bus readable state with the states *array* so
  // GetKeyboardBacklightStatesJSON returns a clean array.
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.keyboardBacklightStatesJSON =
      m_service->m_keyboardBacklightController.currentStatesJSON();
  }

  // If the caller provided a keyboard profile ID, update the active profile
  // reference and notify all clients so they stay in sync.
  if ( !keyboardProfileId.empty() )
  {
    m_service->m_activeProfile.keyboard.keyboardProfileId = keyboardProfileId;
    m_service->updateDBusActiveProfileData();
    if ( m_service->m_adaptor )
      emitProfileChanged( m_service->m_activeProfile.id,
                          keyboardProfileId,
                          m_service->m_activeProfile.fan.fanProfile );
  }

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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  bool result = m_service->m_profileSettingsWorker->applyChargingProfile( profileDescriptor.toStdString() );

  if ( result )
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.currentChargingProfile = profileDescriptor.toStdString();
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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  bool result = m_service->m_profileSettingsWorker->applyChargingPriority( priorityDescriptor.toStdString() );

  if ( result )
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.currentChargingPriority = priorityDescriptor.toStdString();
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
  return m_data.chargeStartThreshold;
}

int UccDBusInterfaceAdaptor::GetChargeEndThreshold()
{
  return m_data.chargeEndThreshold;
}

bool UccDBusInterfaceAdaptor::SetChargeStartThreshold( int value )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( value < 0 || value > 100 )
    return false;
  bool result = m_service->m_profileSettingsWorker->setChargeStartThreshold( value );

  if ( result )
    m_data.chargeStartThreshold = value;

  return result;
}

bool UccDBusInterfaceAdaptor::SetChargeEndThreshold( int value )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( value < 0 || value > 100 )
    return false;
  bool result = m_service->m_profileSettingsWorker->setChargeEndThreshold( value );

  if ( result )
    m_data.chargeEndThreshold = value;

  return result;
}

QString UccDBusInterfaceAdaptor::GetChargeType()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargeType );
}

bool UccDBusInterfaceAdaptor::SetChargeType( const QString &type )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
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
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return;
  m_service->m_fnLockController.setStatus( status );
}

// sensor data collection methods

void UccDBusInterfaceAdaptor::SetSensorDataCollectionStatus( bool status )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return;
  m_data.sensorDataCollectionStatus = status;

  // FPS collection stays always-on; this flag only controls sensor data policy.
}

bool UccDBusInterfaceAdaptor::GetSensorDataCollectionStatus()
{
  return m_data.sensorDataCollectionStatus;
}

void UccDBusInterfaceAdaptor::SetDGpuD0Metrics( bool status )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return;
  m_data.d0MetricsUsage = status;
}

// nvidia power control methods

int UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLDefaultPowerLimit()
{
  if ( m_service )
  {
    const ucc::hal::NvidiaGpuPowerProvider powerProvider( m_service->m_nvml.get() );
    if ( auto v = powerProvider.getDefaultLimitW( 0 ) )
    {
      m_data.nvidiaPowerCTRLDefaultPowerLimit = static_cast< int32_t >( *v );
      return *v;
    }
  }
  return m_data.nvidiaPowerCTRLDefaultPowerLimit;
}

int UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLMaxPowerLimit()
{
  if ( m_service )
  {
    const ucc::hal::NvidiaGpuPowerProvider powerProvider( m_service->m_nvml.get() );
    if ( auto v = powerProvider.getMaxLimitW( 0 ) )
    {
      m_data.nvidiaPowerCTRLMaxPowerLimit = static_cast< int32_t >( *v );
      return *v;
    }
  }
  return m_data.nvidiaPowerCTRLMaxPowerLimit;
}

int UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLMinPowerLimit()
{
  if ( m_service )
  {
    const ucc::hal::NvidiaGpuPowerProvider powerProvider( m_service->m_nvml.get() );
    if ( auto v = powerProvider.getMinLimitW( 0 ) )
    {
      m_data.nvidiaPowerCTRLMinPowerLimit = static_cast< int32_t >( *v );
      return *v;
    }
  }
  return m_data.nvidiaPowerCTRLMinPowerLimit;
}

bool UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLAvailable()
{
  static const std::string NVIDIA_CTGP_OFFSET =
      "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";

  std::error_code ec;
  const bool ctgpAvailable = std::filesystem::exists( NVIDIA_CTGP_OFFSET, ec )
                          && std::filesystem::is_regular_file( NVIDIA_CTGP_OFFSET, ec );

  bool genericNvmlAvailable = false;
  if ( m_service )
  {
    const ucc::hal::NvidiaGpuPowerProvider powerProvider( m_service->m_nvml.get() );
    genericNvmlAvailable = powerProvider.isAvailable( 0 );

    if ( auto v = powerProvider.getDefaultLimitW( 0 ) )
      m_data.nvidiaPowerCTRLDefaultPowerLimit = static_cast< int32_t >( *v );
    if ( auto v = powerProvider.getMinLimitW( 0 ) )
      m_data.nvidiaPowerCTRLMinPowerLimit = static_cast< int32_t >( *v );
    if ( auto v = powerProvider.getMaxLimitW( 0 ) )
      m_data.nvidiaPowerCTRLMaxPowerLimit = static_cast< int32_t >( *v );
  }

  // Keep DBusData flag semantics: this one tracks cTGP sysfs availability only.
  m_data.nvidiaPowerCTRLAvailable = ctgpAvailable;
  return ctgpAvailable || genericNvmlAvailable;
}

int UccDBusInterfaceAdaptor::GetNVIDIAPowerOffset()
{
  if ( !m_service || !m_service->m_profileSettingsWorker )
    return 0;
  if ( !m_data.cTGPAdjustmentSupported.load() || !m_data.nvidiaPowerCTRLAvailable.load() )
    return 0;

  // Read the actual current cTGP offset from sysfs so callers always see
  // the hardware-accepted value, not a potentially stale profile value.
  static const std::string NVIDIA_CTGP_OFFSET =
      "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";
  std::ifstream file( NVIDIA_CTGP_OFFSET );
  if ( file.is_open() )
  {
    int value = 0;
    file >> value;
    return value;
  }
  return 0;
}

bool UccDBusInterfaceAdaptor::SetNVIDIAPowerOffset( int offset )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_profileSettingsWorker ) return false;
  if ( !m_data.cTGPAdjustmentSupported.load() || !m_data.nvidiaPowerCTRLAvailable.load() )
    return false;

  return m_service->m_profileSettingsWorker->applyNVIDIAPowerOffset( offset );
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

QString UccDBusInterfaceAdaptor::GetAvailableEPPs()
{
  if ( m_service && m_service->getCpuWorker() )
  {
    auto epps = m_service->getCpuWorker()->getAvailableEPPs();
    if ( epps )
    {
      std::string json = "[";
      for ( size_t i = 0; i < epps->size(); ++i )
      {
        if ( i > 0 ) json += ",";
        json += "\"" + (*epps)[i] + "\"";
      }
      json += "]";
      return QString::fromStdString( json );
    }
  }
  return QStringLiteral("[]");
}

int UccDBusInterfaceAdaptor::GetCpuCoreCount()
{
  if ( m_service && m_service->getCpuWorker() )
    return m_service->getCpuWorker()->getCoreCount();
  return -1;
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
{
  if ( !m_service ) return -1;
  auto *wc = m_service->m_waterCoolerWorker.get();
  return wc ? static_cast< int >( wc->getLastFanSpeed() ) : -1;
}

int UccDBusInterfaceAdaptor::GetWaterCoolerPumpLevel()
{
  if ( !m_service ) return -1;
  auto *wc = m_service->m_waterCoolerWorker.get();
  return wc ? static_cast< int >( wc->getLastPumpVoltage() ) : -1;
}

bool UccDBusInterfaceAdaptor::EnableWaterCooler( bool enable )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // Update shared DBus flag and request service to perform actions when disabling
  m_data.waterCoolerScanningEnabled = enable;

  // When scanning is disabled, mark unavailable and disconnected so clients
  // immediately see the device as gone
  if ( not enable )
  {
    m_data.waterCoolerAvailable = false;
    m_data.waterCoolerConnected = false;
  }

  // Update the active profile so that subsequent profile re-applications
  // (e.g. resume from suspend, autosave reload) don't override the runtime state.
  if ( m_service )
  {
    m_service->m_activeProfile.fan.enableWaterCooler = enable;
    m_service->setWaterCoolerScanningEnabled( enable );
  }

  return true;
}

bool UccDBusInterfaceAdaptor::IsWaterCoolerEnabled()
{
  if ( m_service )
    return m_service->m_activeProfile.fan.enableWaterCooler;
  return false;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerFanSpeed( int dutyCyclePercent )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( dutyCyclePercent < 0 || dutyCyclePercent > 100 )
    return false;
  if ( m_service )
  {
    auto *wc = m_service->m_waterCoolerWorker.get();
    if ( wc )
      return wc->setFanSpeed( dutyCyclePercent );
  }

  return false;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerPumpVoltage( int voltage )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // V12(1) is reserved and V1bis excluded. Valid: {0, 2, 3, 4}
  if ( voltage != 0 && voltage != 2 && voltage != 3 && voltage != 4 )
    return false;
  if ( m_service )
  {
    auto *wc = m_service->m_waterCoolerWorker.get();
    if ( wc )
      return wc->setPumpVoltage( voltage );
  }

  return false;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerLEDColor( int red, int green, int blue, int mode )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service )
  {
    m_service->m_waterCoolerLedMode.store( mode );

    // Temperature mode: internally use Static, daemon auto-sets color from fan speed
    const int hwMode = ( mode == static_cast< int >( ucc::RGBState::Temperature ) )
                             ? static_cast< int >( ucc::RGBState::Static )
                             : mode;

    auto *wc = m_service->m_waterCoolerWorker.get();
    if ( wc )
      return wc->setLEDColor( red, green, blue, hwMode );
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerLED()
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service )
  {
    auto *wc = m_service->m_waterCoolerWorker.get();
    if ( wc )
      return wc->turnOffLED();
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerFan()
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service )
  {
    auto *wc = m_service->m_waterCoolerWorker.get();
    if ( wc )
      return wc->turnOffFan();
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerPump()
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service )
  {
    auto *wc = m_service->m_waterCoolerWorker.get();
    if ( wc )
      return wc->turnOffPump();
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

// --- Monitoring history D-Bus methods ---

QByteArray UccDBusInterfaceAdaptor::GetMonitorDataSince( qlonglong sinceTimestampMs )
{
  if ( !m_service )
    return QByteArray{};
  const auto raw = m_service->m_metricsStore.querySinceBinary( sinceTimestampMs );
  return QByteArray( reinterpret_cast< const char * >( raw.data() ),
                     static_cast< qsizetype >( raw.size() ) );
}

void UccDBusInterfaceAdaptor::SetMonitorHistoryHorizon( int seconds )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return;
  if ( m_service )
    m_service->m_metricsStore.setHorizon( seconds );
}

int UccDBusInterfaceAdaptor::GetMonitorHistoryHorizon()
{
  return m_service ? m_service->m_metricsStore.horizonSeconds() : 0;
}

QString UccDBusInterfaceAdaptor::GetMonitorSourcesJSON()
{
  if ( !m_service )
    return QStringLiteral( "[]" );

  // Build a JSON array of all available monitoring sources.
  // Each entry: { "key": "<store-key>", "label": "<human>", "group": "<type>", "unit": "<unit>" }
  std::string json = "[";
  bool first = true;

  auto append = [&]( const std::string &key, const std::string &label,
                     const char *group, const char *unit )
  {
    if ( !first ) json += ',';
    first = false;
    json += "{\"key\":\"" + key + "\""
          + ",\"label\":\"" + label + "\""
          + ",\"group\":\"" + group + "\""
          + ",\"unit\":\"" + unit + "\"}";
  };

  // Temperature sensors
  for ( const auto &s : m_service->m_hw.tempSensors() )
    append( "sensor:" + s.id, s.label.empty() ? s.id : s.label, "sensor", "°C" );

  // Thermal sources
  for ( const auto &ts : m_service->m_hw.thermalSources() )
    append( "tsrc:" + ts.id, ts.label.empty() ? ts.id : ts.label, "thermal", "°C" );

  // Fan / pump RPMs
  for ( const auto &f : m_service->m_hw.fans() )
  {
    if ( f.canRead )
      append( "fan:" + f.id, f.label.empty() ? f.id : f.label, "fan", "RPM" );
  }

  // Hwmon voltage sensors
  for ( const auto &vs : m_service->m_voltageSensors )
    append( "voltage:" + vs.id, vs.label, "voltage", "mV" );

  // Per-core CPU frequency
  if ( !m_service->m_cpuFreqCores.empty() )
  {
    append( "cpufreq:avg", "CPU Frequency (avg)", "cpufreq", "MHz" );
    for ( const auto &core : m_service->m_cpuFreqCores )
      append( "cpufreq:" + std::to_string( core.coreIndex ),
              "CPU Core " + std::to_string( core.coreIndex ) + " Freq",
              "cpufreq", "MHz" );
  }

  // Legacy aggregate metrics (GPU info, CPU power/freq, FPS)
  append( "cpuTemp",          "CPU Temp",          "legacy", "°C" );
  append( "cpuFanDuty",       "CPU Fan Duty",      "legacy", "%" );
  append( "cpuPower",         "CPU Power",         "legacy", "W" );
  append( "cpuFrequency",     "CPU Frequency",     "legacy", "MHz" );
  append( "gpuTemp",          "dGPU Temp",         "legacy", "°C" );
  append( "gpuFanDuty",       "dGPU Fan Duty",     "legacy", "%" );
  append( "gpuPower",         "dGPU Power",        "legacy", "W" );
  append( "gpuFrequency",     "dGPU Frequency",    "legacy", "MHz" );
  append( "gpuVramFrequency", "dGPU VRAM Freq",    "legacy", "MHz" );
  append( "gpuCoreVoltage",   "dGPU Core Voltage", "legacy", "mV" );
  append( "fps",              "FPS",               "legacy", "fps" );

  json += "]";
  return QString::fromStdString( json );
}

int UccDBusInterfaceAdaptor::GetCpuFrequencyMHz()
{
  return m_data.cpuFrequencyMHz.load();
}

QString UccDBusInterfaceAdaptor::GetFpsSourcesJSON()
{
  QJsonObject root;
  root[ "selectedApp" ] = QString::fromStdString( m_selectedFpsApp.empty() ? std::string( "auto" ) : m_selectedFpsApp );
  root[ "currentApp" ] = QString::fromStdString( m_fpsServer.clientAppName() );
  root[ "currentPid" ] = static_cast< qlonglong >( m_fpsServer.clientPid() );

  QJsonArray apps;
  apps.append( QStringLiteral( "auto" ) );
  for ( const auto &app : m_seenFpsApps )
    apps.append( QString::fromStdString( app ) );
  root[ "apps" ] = apps;

  return QString::fromUtf8( QJsonDocument( root ).toJson( QJsonDocument::Compact ) );
}

QString UccDBusInterfaceAdaptor::GetAutoUvAutoApplyStatusJSON()
{
  QJsonObject root;
  root[ "lastApp" ] = QString::fromStdString( m_lastAutoAppliedApp );
  root[ "lastPid" ] = static_cast< qlonglong >( m_lastAutoAppliedPid );
  root[ "lastGpuProfileId" ] = QString::fromStdString( m_lastAutoAppliedGpuProfileId );

  const std::string currentApp = m_fpsServer.clientAppName();
  root[ "currentApp" ] = QString::fromStdString( currentApp );
  root[ "currentPid" ] = static_cast< qlonglong >( m_fpsServer.clientPid() );

  std::string mapped;
  if ( m_service && !currentApp.empty() )
  {
    auto it = m_service->m_settings.appGpuProfileMap.find( currentApp );
    if ( it != m_service->m_settings.appGpuProfileMap.end() )
      mapped = it->second;
  }
  root[ "mappedGpuProfileId" ] = QString::fromStdString( mapped );

  return QString::fromUtf8( QJsonDocument( root ).toJson( QJsonDocument::Compact ) );
}

bool UccDBusInterfaceAdaptor::SetFpsSourceApp( const QString &appName )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  const QString trimmed = appName.trimmed();
  if ( trimmed.isEmpty() || trimmed.compare( "auto", Qt::CaseInsensitive ) == 0 )
  {
    m_selectedFpsApp = "auto";
    return true;
  }

  m_selectedFpsApp = trimmed.toStdString();
  m_seenFpsApps.insert( m_selectedFpsApp );
  return true;
}

QString UccDBusInterfaceAdaptor::GetFpsSourceApp()
{
  return QString::fromStdString( m_selectedFpsApp.empty() ? std::string( "auto" ) : m_selectedFpsApp );
}

// ---------------------------------------------------------------------------
// NVIDIA GPU OC methods
// ---------------------------------------------------------------------------

bool UccDBusInterfaceAdaptor::GetNvidiaOCAvailable()
{
  return m_service && m_service->m_nvidiaOCWorker && m_service->m_nvidiaOCWorker->isAvailable();
}

QString UccDBusInterfaceAdaptor::GetNvidiaOCState( int deviceIndex )
{
  if ( !m_service || !m_service->m_nvidiaOCWorker )
    return QStringLiteral( "{}" );
  return QString::fromStdString(
      m_service->m_nvidiaOCWorker->getOCStateJSON( static_cast< unsigned int >( deviceIndex ) ) );
}

bool UccDBusInterfaceAdaptor::SetNvidiaClockOffset( int deviceIndex, int clockType, int pstate, int offsetMHz )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->setClockOffset(
      static_cast< unsigned int >( deviceIndex ),
      static_cast< unsigned int >( clockType ),
      static_cast< unsigned int >( pstate ),
      offsetMHz );
}

bool UccDBusInterfaceAdaptor::SetNvidiaGpuLockedClocks( int deviceIndex, int minMHz, int maxMHz )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->setGpuLockedClocks(
      static_cast< unsigned int >( deviceIndex ),
      static_cast< unsigned int >( minMHz ),
      static_cast< unsigned int >( maxMHz ) );
}

bool UccDBusInterfaceAdaptor::SetNvidiaVramLockedClocks( int deviceIndex, int minMHz, int maxMHz )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->setVramLockedClocks(
      static_cast< unsigned int >( deviceIndex ),
      static_cast< unsigned int >( minMHz ),
      static_cast< unsigned int >( maxMHz ) );
}

bool UccDBusInterfaceAdaptor::ResetNvidiaGpuLockedClocks( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetGpuLockedClocks( static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::ResetNvidiaVramLockedClocks( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetVramLockedClocks( static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::ResetNvidiaAllClockOffsets( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetAllClockOffsets( static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::SetNvidiaGpuPowerLimit( int deviceIndex, double watts )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->setPowerLimit( static_cast< unsigned int >( deviceIndex ), watts );
}

bool UccDBusInterfaceAdaptor::ResetNvidiaGpuPowerLimit( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetPowerLimit( static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::ApplyNvidiaGpuOCProfile( const QString &profileJSON, int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;

  const std::string profileJsonStd = profileJSON.toStdString();

  // When the Uniwill/Tuxedo cTGP sysfs interface is available, power limiting
  // is handled via that path (below).  Strip the NVML powerLimitW field so
  // applyGpuOCProfile() does not also call nvmlDeviceSetPowerManagementLimit,
  // which would conflict.  On desktop GPUs without cTGP, the NVML path is the
  // only option — leave powerLimitW in place.
  std::string filteredJson = profileJsonStd;
  const bool ctgpAvailable = m_service->m_dbusData.cTGPAdjustmentSupported.load()
                          && m_service->m_dbusData.nvidiaPowerCTRLAvailable.load();
  if ( ctgpAvailable )
  {
    QJsonDocument tmpDoc = QJsonDocument::fromJson( QByteArray::fromStdString( profileJsonStd ) );
    if ( tmpDoc.isObject() )
    {
      QJsonObject tmpObj = tmpDoc.object();
      tmpObj.remove( "powerLimitW" );
      filteredJson = QJsonDocument( tmpObj ).toJson( QJsonDocument::Compact ).toStdString();
    }
  }

  const bool result = m_service->m_nvidiaOCWorker->applyGpuOCProfile(
      filteredJson, static_cast< unsigned int >( deviceIndex ) );

  if ( !result )
    return false;

  // Apply cTGP offset from GPU profile payload (GPU-profile path only)
  if ( m_service->m_profileSettingsWorker && ctgpAvailable )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( profileJsonStd ) );
    if ( doc.isObject() )
    {
      QJsonObject obj = doc.object();
      if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj[ "nvidiaPowerCTRLProfile" ].isObject() )
      {
        QJsonObject nvidiaObj = obj[ "nvidiaPowerCTRLProfile" ].toObject();
        int ctgpOffset = nvidiaObj.value( "cTGPOffset" ).toInt( 0 );
        m_service->m_profileSettingsWorker->applyNVIDIAPowerOffset( ctgpOffset );
      }

      m_service->updateDBusActiveProfileData();
    }
  }

  auto extractStr = []( const std::string &json, const std::string &key ) -> std::string {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find( search );
    if ( pos == std::string::npos ) return {};
    pos += search.length();
    size_t end = json.find( '"', pos );
    if ( end == std::string::npos ) return {};
    return json.substr( pos, end - pos );
  };

  const std::string gpuProfileId = extractStr( profileJsonStd, "gpuProfileId" );
  if ( !gpuProfileId.empty() )
  {
    m_service->m_activeProfile.gpuProfileId = gpuProfileId;
    m_service->updateDBusActiveProfileData();

    if ( m_service->m_adaptor )
      emitProfileChanged( m_service->m_activeProfile.id,
                          m_service->m_activeProfile.keyboard.keyboardProfileId,
                          m_service->m_activeProfile.fan.fanProfile,
                          gpuProfileId );
  }

  return true;
}

bool UccDBusInterfaceAdaptor::ResetNvidiaGpuOCAll( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker || !m_service->m_profileSettingsWorker ) return false;

  const bool ocResetOk = m_service->m_nvidiaOCWorker->resetAll( static_cast< unsigned int >( deviceIndex ) );
  bool ctgpResetOk = true;
  if ( m_service->m_dbusData.cTGPAdjustmentSupported.load()
       && m_service->m_dbusData.nvidiaPowerCTRLAvailable.load() )
  {
    ctgpResetOk = m_service->m_profileSettingsWorker->applyNVIDIAPowerOffset( 0 );
    if ( !ctgpResetOk )
      syslog( LOG_WARNING, "[GPU-RESET] Failed to reset NVIDIA cTGP offset to 0" );
  }

  return ocResetOk && ctgpResetOk;
}

// ── Auto-OC ─────────────────────────────────────────────────────────────────

bool UccDBusInterfaceAdaptor::StartAutoOC( const QString &component, int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_autoOCWorker ) return false;

  AutoOCComponent comp = AutoOCComponent::Both;
  if ( component == "core" ) comp = AutoOCComponent::Core;
  else if ( component == "vram" ) comp = AutoOCComponent::Vram;

  AutoOCConfig config;
  config.mode = AutoOCMode::MaxOffset;

  return m_service->m_autoOCWorker->start( comp, static_cast< unsigned int >( deviceIndex ), config );
}

bool UccDBusInterfaceAdaptor::StopAutoOC()
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_autoOCWorker ) return false;
  m_service->m_autoOCWorker->stop();
  return true;
}

bool UccDBusInterfaceAdaptor::GetAutoOCRunning()
{
  if ( !m_service || !m_service->m_autoOCWorker ) return false;
  return m_service->m_autoOCWorker->isRunning();
}

QString UccDBusInterfaceAdaptor::GetAutoOCProgress()
{
  // Progress is pushed via the AutoOCProgressChanged signal; this method
  // returns the current running state as a simple JSON for polling clients.
  if ( !m_service || !m_service->m_autoOCWorker )
    return QStringLiteral( "{\"running\":false}" );

  bool running = m_service->m_autoOCWorker->isRunning();
  return running ? QStringLiteral( "{\"running\":true}" )
                 : QStringLiteral( "{\"running\":false}" );
}

// ─── Auto-Undervolt D-Bus methods ───────────────────────────────────────────

bool UccDBusInterfaceAdaptor::StartAutoUndervolt( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_autoUndervoltWorker ) return false;

  return m_service->m_autoUndervoltWorker->start(
    static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::StopAutoUndervolt()
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_autoUndervoltWorker ) return false;
  m_service->m_autoUndervoltWorker->stop();
  return true;
}

bool UccDBusInterfaceAdaptor::GetAutoUndervoltRunning()
{
  if ( !m_service || !m_service->m_autoUndervoltWorker ) return false;
  return m_service->m_autoUndervoltWorker->isRunning();
}

QString UccDBusInterfaceAdaptor::GetAutoUndervoltProgress()
{
  if ( !m_service || !m_service->m_autoUndervoltWorker )
    return QStringLiteral( "{\"running\":false}" );

  bool running = m_service->m_autoUndervoltWorker->isRunning();
  return running ? QStringLiteral( "{\"running\":true}" )
                 : QStringLiteral( "{\"running\":false}" );
}

QString UccDBusInterfaceAdaptor::GetAutoUndervoltProfiles()
{
  if ( !m_service || !m_service->m_autoUndervoltWorker )
    return QStringLiteral( "[]" );

  QString json = QStringLiteral( "[" );
  bool first = true;
  for ( const auto &[name, p] : m_service->m_autoUndervoltWorker->profiles() )
  {
    if ( !first ) json += ',';
    first = false;
    json += QStringLiteral( "{\"app\":\"%1\",\"capMHz\":%2,\"baselineClk\":%3,"
                            "\"baselineFps\":%4,\"achievedFps\":%5}" )
      .arg( QString::fromStdString( name ) )
      .arg( p.gpuFreqCapMHz )
      .arg( p.baselineClkMHz )
      .arg( p.baselineFps, 0, 'f', 1 )
      .arg( p.achievedFps, 0, 'f', 1 );
  }
  json += ']';
  return json;
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

void UccDBusInterfaceAdaptor::emitProfileChanged( const std::string &profileId,
                                                  const std::string &keyboardProfileId,
                                                  const std::string &fanProfileId,
                                                  const std::string &gpuProfileId )
{
  QString id = QString::fromStdString( profileId );
  QString kbId = QString::fromStdString( keyboardProfileId );
  QString fpId = QString::fromStdString( fanProfileId );

  std::string effectiveGpu = gpuProfileId;
  if ( effectiveGpu.empty() && m_service )
    effectiveGpu = m_service->m_activeProfile.gpuProfileId;
  QString gpId = QString::fromStdString( effectiveGpu );

  QMetaObject::invokeMethod( this, [this, id, kbId, fpId, gpId]() {
    emit ProfileChanged( id, kbId, fpId, gpId );
  }, Qt::QueuedConnection );
}

void UccDBusInterfaceAdaptor::emitProfilesListChanged()
{
  QMetaObject::invokeMethod( this, [this]() {
    emit ProfilesListChanged();
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

  // Early device identification for pre-HAL code that needs it.
  // After m_hw.detect(), m_deviceId is updated from the UniwillProfileProvider.
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

  // detect system hardware info (CPU, GPU, laptop model)
  m_systemInfo = detectSystemInfo( m_deviceId );
  m_dbusData.systemInfoJSON = m_systemInfo.toJSON();

  // Hardware support is determined dynamically by the HAL provider registry.
  // After m_hw.detect() completes, m_hw.capabilities() tells us exactly what
  // this machine can do.  Clients query GetCapabilitiesJSON() to adapt their
  // UI.  The old SKU-whitelist gate is gone — we always continue startup.

  // detect display session type and initialize display modes
  initializeDisplayModes();

  // set default system JSON values (sentinels for GPU/CPU monitoring data)
  m_dbusData.primeState = "-1";
  m_dbusData.dGpuInfoValuesJSON = "{\"temp\":-1,\"powerDraw\":-1,\"maxPowerLimit\":-1,\"enforcedPowerLimit\":-1,\"coreFrequency\":-1,\"vramFrequency\":-1,\"maxCoreFrequency\":-1,\"computeUtilPct\":-1,\"memoryUtilPct\":-1,\"vramUsedMiB\":-1,\"vramTotalMiB\":-1,\"perfLimitReason\":\"\",\"encoderUtilPct\":-1,\"decoderUtilPct\":-1,\"currentPstate\":-1,\"grClockOffsetMHz\":-999,\"memClockOffsetMHz\":-999,\"coreVoltageMv\":-1}";
  m_dbusData.iGpuInfoValuesJSON = "{\"vendor\":\"unknown\",\"temp\":-1,\"coreFrequency\":-1,\"maxCoreFrequency\":-1,\"powerDraw\":-1}";

  // Keyboard backlight will be detected during worker initialization
  m_dbusData.keyboardBacklightCapabilitiesJSON = "null";
  m_dbusData.keyboardBacklightStatesJSON = "[]";

  // Read all hardware capabilities directly using m_io / sysfs BEFORE any
  // workers or profiles are created.  This populates TDP limits, NVIDIA
  // power limits, charging profiles, and YCbCr420 availability with real
  // hardware values so the D-Bus data is never populated with fake defaults.
  //
  // Create the single shared NvmlWrapper instance first — it is reused by
  // readHardwareCapabilities(), HardwareMonitorWorker, NvidiaOCWorker, and
  // ProfileSettingsWorker so that NVML is initialised exactly once.
  m_nvml = std::make_shared< NvmlWrapper >();

  // --- HAL: register and detect hardware providers ---
  // TuxedoIO-based providers (OEM, high priority; only available on Clevo/Uniwill)
  m_hw.addFanProvider( std::make_unique< ucc::hal::TuxedoIOFanProvider >( m_io ) );
  m_hw.addTempProvider( std::make_unique< ucc::hal::TuxedoIOTempProvider >( m_io ) );
  m_hw.addPlatformProvider( std::make_unique< ucc::hal::TuxedoIOPlatformProvider >( m_io ) );
  // Generic hwmon providers (work on any Linux machine)
  m_hw.addFanProvider( std::make_unique< ucc::hal::HwmonFanProvider >() );
  m_hw.addTempProvider( std::make_unique< ucc::hal::HwmonTempProvider >() );
  // NVML-based GPU temperature (covers systems without nvidia hwmon, e.g. RTX 5090)
  m_hw.addTempProvider( std::make_unique< ucc::hal::NvmlTempProvider >( m_nvml.get() ) );
  // CPU-specific platform providers (AMD via RyzenAdj/SMU, Intel TBD)
  m_hw.addPlatformProvider( std::make_unique< ucc::hal::AmdCpuPlatformProvider >() );
  // Profile providers — Uniwill-specific (high priority) and generic fallback
  m_hw.addProfileProvider( std::make_unique< ucc::hal::UniwillProfileProvider >( m_io ) );
  m_hw.addProfileProvider( std::make_unique< ucc::hal::GenericProfileProvider >() );
  // Probe all providers and select the best for each subsystem
  m_hw.detect();

  // Build initial one-zone-per-fan defaults (profile zones override this later)
  buildInitialFanZones();

  // Wire the HAL profile provider into the ProfileManager
  m_profileManager.setProfileProvider( m_hw.profileProvider() );

  // Update device ID from the UniwillProfileProvider if it detected a device
  if ( auto *uwProvider = dynamic_cast< ucc::hal::UniwillProfileProvider * >( m_hw.profileProvider() ) )
  {
    m_deviceId = uwProvider->deviceId();
    if ( m_deviceId.has_value() )
      m_dbusData.device = std::to_string( static_cast< int >( *m_deviceId ) );
  }

  // Compute device-specific feature flags (aquaris, cTGP) — needs HAL profile provider
  computeDeviceCapabilities();

  // Read hardware capabilities using the HAL (TDP, NVIDIA, charging, etc.)
  readHardwareCapabilities();

  // Publish capabilities for D-Bus clients
  m_dbusData.capabilitiesJSON = ucc::hal::capabilitiesToJSON( m_hw.capabilities() );
  m_dbusData.deviceSupported = ( m_hw.capabilities() != ucc::hal::HwCapability::None );
  m_dbusData.tuxedoWmiAvailable = ( m_hw.platformProvider() != nullptr );

  // Resize fan data vector based on actual detected fans
  {
    const size_t nFans = m_hw.fans().size();
    if ( nFans > 0 && nFans != m_dbusData.fans.size() )
    {
      m_dbusData.fans.resize( std::max( nFans, static_cast< size_t >( 3 ) ) );
      syslog( LOG_INFO, "[uccd] HAL detected %zu fans, resized D-Bus fan data", nFans );
    }
    m_dbusData.fanHwmonAvailable = m_hw.hasFanControl();
  }

  // Discover additional monitoring sources (voltage sensors, per-core CPU freq)
  discoverVoltageSensors();
  discoverCpuFreqCores();

  // Build built-in fan profiles based on platform.
  rebuildBuiltinFanProfiles();

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
    []( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); }
  );

  // initialize profile settings worker (replaces ODMPowerLimitWorker, ODMProfileWorker, ChargingWorker, YCbCr420WorkaroundWorker)
  // Quirk: some devices (e.g. IBP Gen10 AMD) have a non-functional ACPI platform_profile —
  // skip it and fall through to the Tuxedo IO API instead (synced from TCC f71acd86, a1b2f6b4).
  const bool skipAcpiPlatformProfile =
    m_deviceId.has_value() and
    ( m_deviceId.value() == UniwillDeviceID::IBPG10AMD or
      m_deviceId.value() == UniwillDeviceID::IBM15A10 );

  m_profileSettingsWorker = std::make_unique< ProfileSettingsWorker >(
    m_hw,
    m_nvml,
    [this]() -> UccProfile { return m_activeProfile; },
    [this]( const std::vector< std::string > &profiles ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.odmProfilesAvailable = profiles;
    },
    []( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); },
    m_settings,
    m_dbusData.modeReapplyPending,
    m_dbusData.nvidiaPowerCTRLDefaultPowerLimit,
    m_dbusData.nvidiaPowerCTRLMaxPowerLimit,
    m_dbusData.nvidiaPowerCTRLAvailable,
    m_dbusData.cTGPAdjustmentSupported,
    skipAcpiPlatformProfile
  );

  // initialize hardware monitor worker (merged GPU info + CPU power + Prime)
  // Quirk: IBM15A10 has a display mux — prime-select is only supported when
  // the eDP display is NOT wired to the NVIDIA GPU (synced from TCC PrimeWorker).
  const bool isDisplayMuxDevice =
    m_deviceId.has_value() and m_deviceId.value() == UniwillDeviceID::IBM15A10;

  m_hardwareMonitorWorker = std::make_unique< HardwareMonitorWorker >(
    m_nvml,
    [this]( const std::string &json, double cpuPowerWatts ) {
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
        m_dbusData.cpuPowerValuesJSON = json;
      }
      // Push CPU power to history store
      if ( cpuPowerWatts > -1.0 )
        m_metricsStore.push( "cpuPower", cpuPowerWatts );
    },
    [this]() { return m_dbusData.sensorDataCollectionStatus.load(); },
    [this]( const std::string &primeState ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.primeState = primeState;
    },
    isDisplayMuxDevice
  );

  // webcam monitoring via HardwareMonitorWorker (replaces former WebcamWorker)
  m_hardwareMonitorWorker->setWebcamCallbacks(
    [this]() -> std::pair< bool, bool > {
      auto *platform = m_hw.platformProvider();
      if ( platform )
      {
        auto webcamOpt = platform->getWebcam();
        if ( webcamOpt.has_value() )
          return { true, webcamOpt.value() };
      }
      return { false, false };
    },
    [this]( bool available, bool status ) {
      m_dbusData.webcamSwitchAvailable = available;
      m_dbusData.webcamSwitchStatus = status;
    }
  );

  // CPU frequency monitoring via HardwareMonitorWorker (every cycle ≈ 800ms)
  m_hardwareMonitorWorker->setCpuFrequencyCallback(
    [this]( int frequencyMHz ) {
      m_dbusData.cpuFrequencyMHz = frequencyMHz;
      if ( frequencyMHz > 0 )
        m_metricsStore.push( "cpuFrequency", static_cast< double >( frequencyMHz ) );
    }
  );

  // initialize fan control worker
  m_fanControlWorker = std::make_unique< FanControlWorker >(
    m_hw,
    [this]() { return m_activeProfile; },
    [this]() { return m_settings.fanControlEnabled; },
    [this]( size_t fanIndex, int64_t timestamp, int speed )
    {
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );

        if ( fanIndex < m_dbusData.fans.size() )
          m_dbusData.fans[fanIndex].speed.set( timestamp, speed );
      }

      // Push fan duty to history store for legacy compatibility
      if ( fanIndex == 0 )
        m_metricsStore.push( "cpuFanDuty", timestamp, speed );
      else if ( fanIndex == 1 )
        m_metricsStore.push( "gpuFanDuty", timestamp, speed );
    },
    [this]( size_t fanIndex, int64_t timestamp, int temp )
    {
      onFanTemperatureUpdate( fanIndex, timestamp, temp );
    },
    [this]( const std::string &fpId ) -> FanProfile
    {
      return resolveFanProfile( fpId );
    },
    [this]( const std::string &zoneId, int64_t timestamp, int temp, int duty, int rpm )
    {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.zoneTelemetry[zoneId] = { timestamp, temp, duty, rpm };
    }
  );

  // Initialize keyboard backlight controller (synchronous — no worker thread)
  {
    std::string capsJSON = m_keyboardBacklightController.init();
    m_dbusData.keyboardBacklightCapabilitiesJSON = capsJSON;

    if ( m_keyboardBacklightController.isAvailable() )
    {
      std::string defaultStates = m_keyboardBacklightController.buildDefaultStatesJSON();
      m_dbusData.keyboardBacklightStatesJSON = defaultStates;

      if ( m_settings.keyboardBacklightControlEnabled )
        m_keyboardBacklightController.applyStatesFromJSON( defaultStates );
    }

    rebuildBuiltinKeyboardProfile();
  }

  // then setup gpu callback before worker starts processing
  setupGpuDataCallback();

  // fill device-specific defaults BEFORE starting workers
  fillDeviceSpecificDefaults( m_defaultProfiles );
  fillDeviceSpecificDefaults( m_customProfiles );
  serializeProfilesJSON();

  // start worker threads after all callbacks and data are ready
  m_profileSettingsWorker->start();  // synchronous: detects ODM profile type + inits charging state
  m_hardwareMonitorWorker->start();
  m_displayWorker->start();
  m_cpuWorker->start();
  m_fanControlWorker->start();

  // Initialize NVIDIA OC worker (non-threaded, on-demand calls via D-Bus)
  m_nvidiaOCWorker = std::make_unique< NvidiaOCWorker >(
    m_nvml,
    []( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); }
  );

  // Initialize Auto-OC worker (QTimer-based, lives on main thread)
  m_autoOCWorker = std::make_unique< AutoOCWorker >(
    m_nvml,
    []( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); }
  );

  // Initialize Auto-Undervolt worker (QTimer-based, lives on main thread)
  m_autoUndervoltWorker = std::make_unique< AutoUndervoltWorker >(
    m_nvml,
    []( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); }
  );

  // Restore persisted auto-undervolt app profiles from app->GPU-profile map.
  if ( m_autoUndervoltWorker )
  {
    std::map< std::string, AppUndervoltProfile > loadedProfiles;
    for ( const auto &[appKey, profileId] : m_settings.appGpuProfileMap )
    {
      auto gpIt = m_settings.gpuProfiles.find( profileId );
      if ( gpIt == m_settings.gpuProfiles.end() )
        continue;

      try
      {
        auto j = nlohmann::json::parse( gpIt->second );
        AppUndervoltProfile p;
        p.appName = appKey;

        if ( j.contains( "gpuLockedClocks" ) && j["gpuLockedClocks"].is_object() )
        {
          const auto &glc = j["gpuLockedClocks"];
          const bool enabled = glc.value( "enabled", false );
          if ( enabled )
          {
            const int minClk = glc.value( "min", 0 );
            const int maxClk = glc.value( "max", 0 );
            if ( minClk > 0 && maxClk > 0 )
              p.gpuFreqCapMHz = std::min( minClk, maxClk );
          }
        }

        if ( j.contains( "offsets" ) && j["offsets"].is_array() )
        {
          for ( const auto &entry : j["offsets"] )
          {
            if ( !entry.is_object() ) continue;
            const int pstate = entry.value( "pstate", -1 );
            if ( pstate == 0 )
            {
              p.coreOffsetMHz = std::max( 0, entry.value( "gpuOffsetMHz", 0 ) );
              break;
            }
          }
        }

        if ( j.contains( "meta" ) && j["meta"].is_object() )
        {
          const auto &meta = j["meta"];
          if ( meta.contains( "autoUndervolt" ) && meta["autoUndervolt"].is_object() )
          {
            const auto &uv = meta["autoUndervolt"];
            p.baselineClkMHz = uv.value( "baselineClkMHz", 0 );
            p.baselineFps = uv.value( "baselineFps", 0.0 );
            p.achievedFps = uv.value( "achievedFps", 0.0 );
            p.achievedPowerW = uv.value( "achievedPowerW", 0.0 );
            p.achievedVoltageMv = uv.value( "achievedVoltageMv", 0.0 );

            const auto lastUsedEpoch = uv.value( "lastUsedEpochSec", 0LL );
            if ( lastUsedEpoch > 0 )
              p.lastUsed = std::chrono::system_clock::time_point{ std::chrono::seconds( lastUsedEpoch ) };
          }
        }

        const auto lastUsedEpoch = j.value( "lastUsedEpochSec", 0LL );
        if ( lastUsedEpoch > 0 )
          p.lastUsed = std::chrono::system_clock::time_point{ std::chrono::seconds( lastUsedEpoch ) };
        else
          p.lastUsed = std::chrono::system_clock::now();

        if ( !p.appName.empty() && p.gpuFreqCapMHz > 0 )
          loadedProfiles[ p.appName ] = p;
      }
      catch ( const std::exception &e )
      {
        syslog( LOG_WARNING, "[AutoUV] Failed to parse mapped GPU profile '%s' for app '%s': %s",
                profileId.c_str(), appKey.c_str(), e.what() );
      }
    }

    m_autoUndervoltWorker->loadProfiles( loadedProfiles );
    if ( !loadedProfiles.empty() )
      syslog( LOG_INFO, "[AutoUV] Restored %zu persisted app profiles", loadedProfiles.size() );
  }

  // NOTE: AutoOC and AutoUndervolt signal forwarding to D-Bus is set up in
  // initDBus() because m_adaptor is not yet created at this point.

  rebuildBuiltinGpuProfiles();
}

int UccDBusService::readCurrentCTGPOffset() const
{
  static const std::string NVIDIA_CTGP_OFFSET =
    "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";

  if ( !m_dbusData.cTGPAdjustmentSupported )
    return 0;

  try
  {
    if ( auto value = SysfsNode< int >( NVIDIA_CTGP_OFFSET ).read(); value.has_value() )
      return value.value();
    return 0;
  }
  catch ( ... )
  {
    return 0;
  }
}

void UccDBusService::rebuildBuiltinFanProfiles()
{
  m_builtinFanProfiles.clear();

  std::unordered_map< std::string, std::string > hwTypes;
  std::unordered_map< std::string, std::string > zoneNames;
  std::vector< ucc::hal::FanZone > zones;
  for ( const auto &zone : m_hw.defaultFanZones() )
  {
    zones.push_back( zone );
    hwTypes[zone.id] = ucc::hal::fanDeviceTypeToString( zone.defaultType );
    zoneNames[zone.id] = zone.name;
  }

  auto *provider = m_hw.profileProvider();
  if ( !provider )
    return;

  for ( const auto &fp : provider->getDefaultFanProfiles( zones ) )
  {
    m_builtinFanProfiles.push_back( { fp.id,
                                      fp.name,
                                      fanProfileToJSON( fp, hwTypes, zoneNames ) } );
  }
}

void UccDBusService::buildInitialFanZones()
{
  // Build one zone per detected fan — the simplest neutral default.
  // No grouping or classification.  Profile zones override this when loaded.
  static const std::vector< ucc::hal::FanCurvePoint > defaultCurve = {
    { 30, 25 }, { 45, 30 }, { 55, 40 }, { 65, 55 },
    { 75, 70 }, { 80, 85 }, { 90, 100 }
  };

  static const std::vector< ucc::hal::FanCurvePoint > defaultPumpCurve = {
    { 30, 30 }, { 50, 35 }, { 65, 45 }, { 75, 60 },
    { 85, 80 }, { 90, 100 }
  };

  const auto &fans = m_hw.fans();
  std::vector< ucc::hal::FanZone > zones;
  zones.reserve( fans.size() );

  // No hardcoded thermal source assignment — zones start without a source.
  // The user assigns sources through the Zone Setup UI.
  const std::string fallbackTs;

  for ( size_t i = 0; i < fans.size(); ++i )
  {
    const auto &fan = fans[i];
    bool isPump = ( fan.deviceType == ucc::hal::FanDeviceType::Pump
                 || fan.deviceType == ucc::hal::FanDeviceType::StagedPump );

    zones.push_back( ucc::hal::FanZone{
      .id = QUuid::createUuid().toString( QUuid::WithoutBraces ).toStdString(),
      .name = fan.label.empty() ? ( "Fan " + std::to_string( i + 1 ) ) : fan.label,
      .fanIds = { fan.id },
      .thermalSourceId = fallbackTs,
      .defaultType = fan.deviceType,
      .curve = isPump ? defaultPumpCurve : defaultCurve,
      .hysteresisDeg = 3,
      .enabled = true } );
  }

  m_hw.setFanZones( std::move( zones ) );
  syslog( LOG_INFO, "[uccd] Built %zu initial per-fan zones", m_hw.defaultFanZones().size() );
}

void UccDBusService::rebuildFanZonesFromProfile( const FanProfile &fp )
{
  // Check if the profile carries zone topology (fanIds)
  bool hasTopology = false;
  for ( const auto &zc : fp.zoneCurves )
  {
    if ( zc.hasTopology() )
    {
      hasTopology = true;
      break;
    }
  }

  if ( !hasTopology )
    return; // Profile only has curves — keep existing zones

  // Build FanZone objects from the profile's zone topology
  std::vector< ucc::hal::FanZone > zones;
  zones.reserve( fp.zoneCurves.size() );

  for ( const auto &zc : fp.zoneCurves )
  {
    if ( zc.zoneId.empty() )
      continue;

    std::string tsId = zc.thermalSourceId;
    // If the referenced source no longer exists, clear it rather than
    // silently substituting a different source.
    if ( !tsId.empty() && !m_hw.findThermalSource( tsId ) )
      tsId.clear();

    zones.push_back( ucc::hal::FanZone{
      .id = zc.zoneId,
      .name = zc.name.empty() ? zc.zoneId : zc.name,
      .fanIds = zc.fanIds,
      .thermalSourceId = tsId,
      .defaultType = zc.deviceType,
      .curve = zc.curve,
      .hysteresisDeg = zc.hysteresisDeg,
      .enabled = zc.enabled } );
  }

  if ( !zones.empty() )
  {
    m_hw.setFanZones( std::move( zones ) );
    syslog( LOG_INFO, "[uccd] Rebuilt %zu fan zones from profile '%s'",
            m_hw.defaultFanZones().size(), fp.name.c_str() );
  }
}

void UccDBusService::rebuildBuiltinGpuProfiles()
{
  m_builtinGpuProfiles.clear();

  if ( !m_nvidiaOCWorker || !m_nvidiaOCWorker->isAvailable() )
    return;

  const std::string ocStateJson = m_nvidiaOCWorker->getOCStateJSON( 0 );
  QJsonDocument stateDoc = QJsonDocument::fromJson( QByteArray::fromStdString( ocStateJson ) );
  if ( !stateDoc.isObject() )
    return;

  const QJsonObject state = stateDoc.object();
  QJsonObject profile;

  QJsonArray offsets;
  const QJsonArray pstates = state.value( "pstates" ).toArray();
  for ( const QJsonValue &entry : pstates )
  {
    const QJsonObject pstate = entry.toObject();
    QJsonObject offset;
    offset[ "pstate" ] = pstate.value( "pstate" ).toInt();
    offset[ "gpuOffsetMHz" ] = pstate.value( "gpu" ).toObject().value( "currentOffset" ).toInt( 0 );
    offset[ "vramOffsetMHz" ] = pstate.value( "vram" ).toObject().value( "currentOffset" ).toInt( 0 );
    offsets.append( offset );
  }
  profile[ "offsets" ] = offsets;

  QJsonObject gpuLocked;
  const QJsonObject gpuLockedState = state.value( "gpuLockedClocks" ).toObject();
  const QJsonObject gpuRange = state.value( "gpuClockRange" ).toObject();
  const bool gpuLockedEnabled = !gpuLockedState.isEmpty();
  gpuLocked[ "enabled" ] = gpuLockedEnabled;
  gpuLocked[ "min" ] = gpuLockedEnabled ? gpuLockedState.value( "min" ).toInt() : gpuRange.value( "min" ).toInt( 0 );
  gpuLocked[ "max" ] = gpuLockedEnabled ? gpuLockedState.value( "max" ).toInt() : gpuRange.value( "max" ).toInt( 0 );
  profile[ "gpuLockedClocks" ] = gpuLocked;

  QJsonObject vramLocked;
  const QJsonObject vramLockedState = state.value( "vramLockedClocks" ).toObject();
  const QJsonObject vramRange = state.value( "vramClockRange" ).toObject();
  const bool vramLockedEnabled = !vramLockedState.isEmpty();
  vramLocked[ "enabled" ] = vramLockedEnabled;
  vramLocked[ "min" ] = vramLockedEnabled ? vramLockedState.value( "min" ).toInt() : vramRange.value( "min" ).toInt( 0 );
  vramLocked[ "max" ] = vramLockedEnabled ? vramLockedState.value( "max" ).toInt() : vramRange.value( "max" ).toInt( 0 );
  profile[ "vramLockedClocks" ] = vramLocked;

  profile[ "powerLimitW" ] = state.value( "powerLimitW" ).toDouble( 0.0 );

  QJsonObject nvidiaPowerCtrl;
  nvidiaPowerCtrl[ "cTGPOffset" ] = readCurrentCTGPOffset();
  profile[ "nvidiaPowerCTRLProfile" ] = nvidiaPowerCtrl;

  SubProfile builtin;
  builtin.id = BUILTIN_GPU_PROFILE_ID;
  builtin.name = BUILTIN_GPU_PROFILE_NAME;
  builtin.json = QJsonDocument( profile ).toJson( QJsonDocument::Compact ).toStdString();
  m_builtinGpuProfiles.push_back( builtin );
}

void UccDBusService::rebuildBuiltinKeyboardProfile()
{
  m_builtinKeyboardProfiles.clear();

  if ( !m_keyboardBacklightController.isAvailable() )
    return;

  SubProfile builtin;
  builtin.id = BUILTIN_KEYBOARD_PROFILE_ID;
  builtin.name = BUILTIN_KEYBOARD_PROFILE_NAME;
  builtin.json = BUILTIN_KEYBOARD_PROFILE_JSON;
  m_builtinKeyboardProfiles.push_back( builtin );
}

void UccDBusService::readHardwareCapabilities()
{
  syslog( LOG_INFO, "[uccd] Reading hardware capabilities directly" );

  // ---- ODM Power Limits (TDP) ----
  // Read from HAL platform provider
  {
    auto *platform = m_hw.tdpProvider();
    const int nrTDPs = platform ? platform->getNumberTDPs() : 0;
    if ( nrTDPs > 0 )
    {
      for ( int i = 0; i < nrTDPs; ++i )
      {
        const int current = platform->getTDP( i ).value_or( 0 );
        const int min     = platform->getTDPMin( i ).value_or( 0 );
        const int max     = platform->getTDPMax( i ).value_or( 0 );

        syslog( LOG_INFO, "[uccd] TDP[%d]: min=%d, max=%d, current=%d", i, min, max, current );
      }
    }
    else
    {
      syslog( LOG_INFO, "[uccd] No TDP hardware available" );
    }
  }

  // ---- NVIDIA Power Control ----
  {
    static const std::string NVIDIA_CTGP_OFFSET =
      "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";

    const ucc::hal::NvidiaGpuPowerProvider powerProvider( m_nvml.get() );

    std::error_code ec;
    const bool ctgpAvailable = std::filesystem::exists( NVIDIA_CTGP_OFFSET, ec )
                            && std::filesystem::is_regular_file( NVIDIA_CTGP_OFFSET, ec );
    m_dbusData.nvidiaPowerCTRLAvailable = ctgpAvailable;

    if ( !m_nvidiaPowerLimitsInitialized )
    {
      // Query power limits once per daemon startup via generic NVML provider.
      if ( auto v = powerProvider.getDefaultLimitW( 0 ) )
        m_dbusData.nvidiaPowerCTRLDefaultPowerLimit = static_cast< int32_t >( *v );
      if ( auto v = powerProvider.getMaxLimitW( 0 ) )
        m_dbusData.nvidiaPowerCTRLMaxPowerLimit = static_cast< int32_t >( *v );

      m_nvidiaPowerLimitsInitialized = true;
    }

    if ( powerProvider.isAvailable( 0 ) )
    {
      syslog( LOG_INFO, "[uccd] NVIDIA NVML power limits — Default: %dW, Max: %dW",
              m_dbusData.nvidiaPowerCTRLDefaultPowerLimit.load(),
              m_dbusData.nvidiaPowerCTRLMaxPowerLimit.load() );
    }

    if ( ctgpAvailable )
    {
      syslog( LOG_INFO, "[uccd] NVIDIA cTGP control available" );
    }
    else
    {
      syslog( LOG_INFO, "[uccd] NVIDIA cTGP control not available" );
    }
  }

  // ---- Charging ----
  {
    static const std::string CHARGING_PROFILE_PATH =
      "/sys/devices/platform/tuxedo_keyboard/charging_profile/charging_profile";
    static const std::string CHARGING_PROFILES_AVAILABLE_PATH =
      "/sys/devices/platform/tuxedo_keyboard/charging_profile/charging_profiles_available";
    static const std::string CHARGING_PRIORITY_PATH =
      "/sys/devices/platform/tuxedo_keyboard/charging_priority/charging_prio";
    static const std::string CHARGING_PRIORITIES_AVAILABLE_PATH =
      "/sys/devices/platform/tuxedo_keyboard/charging_priority/charging_prios_available";

    // Charging profiles
    if ( SysfsNode< std::string >( CHARGING_PROFILE_PATH ).isAvailable() and
         SysfsNode< std::string >( CHARGING_PROFILES_AVAILABLE_PATH ).isAvailable() )
    {
      auto profiles = SysfsNode< std::vector< std::string > >( CHARGING_PROFILES_AVAILABLE_PATH, " " ).read();
      if ( profiles.has_value() and not profiles->empty() )
      {
        std::ostringstream oss;
        oss << "[";
        for ( size_t i = 0; i < profiles->size(); ++i )
        {
          if ( i > 0 ) oss << ",";
          oss << "\"" << ( *profiles )[ i ] << "\"";
        }
        oss << "]";
        m_dbusData.chargingProfilesAvailable = oss.str();

        auto current = SysfsNode< std::string >( CHARGING_PROFILE_PATH ).read();
        if ( current.has_value() )
          m_dbusData.currentChargingProfile = *current;

        syslog( LOG_INFO, "[uccd] Charging profiles: %s, current: %s",
                m_dbusData.chargingProfilesAvailable.c_str(),
                m_dbusData.currentChargingProfile.c_str() );
      }
    }

    // Charging priorities
    if ( SysfsNode< std::string >( CHARGING_PRIORITY_PATH ).isAvailable() and
         SysfsNode< std::string >( CHARGING_PRIORITIES_AVAILABLE_PATH ).isAvailable() )
    {
      auto prios = SysfsNode< std::vector< std::string > >( CHARGING_PRIORITIES_AVAILABLE_PATH, " " ).read();
      if ( prios.has_value() and not prios->empty() )
      {
        std::ostringstream oss;
        oss << "[";
        for ( size_t i = 0; i < prios->size(); ++i )
        {
          if ( i > 0 ) oss << ",";
          oss << "\"" << ( *prios )[ i ] << "\"";
        }
        oss << "]";
        m_dbusData.chargingPrioritiesAvailable = oss.str();

        auto current = SysfsNode< std::string >( CHARGING_PRIORITY_PATH ).read();
        if ( current.has_value() )
          m_dbusData.currentChargingPriority = *current;
      }
    }

    // Charge thresholds
    auto battery = PowerSupplyController::getFirstBattery();
    if ( battery )
    {
      m_dbusData.chargeStartThreshold = battery->getChargeControlStartThreshold();
      m_dbusData.chargeEndThreshold = battery->getChargeControlEndThreshold();

      // Map charge type
      auto chargeType = battery->getChargeType();
      switch ( chargeType )
      {
        case ChargeType::Trickle:      m_dbusData.chargeType = "Trickle"; break;
        case ChargeType::Fast:         m_dbusData.chargeType = "Fast"; break;
        case ChargeType::Standard:     m_dbusData.chargeType = "Standard"; break;
        case ChargeType::Adaptive:     m_dbusData.chargeType = "Adaptive"; break;
        case ChargeType::Custom:       m_dbusData.chargeType = "Custom"; break;
        case ChargeType::LongLife:     m_dbusData.chargeType = "LongLife"; break;
        case ChargeType::Bypass:       m_dbusData.chargeType = "Bypass"; break;
        case ChargeType::NotAvailable: m_dbusData.chargeType = "N/A"; break;
        default:                       m_dbusData.chargeType = "Unknown"; break;
      }

      // Threshold ranges
      auto startThresholds = battery->getChargeControlStartAvailableThresholds();
      auto endThresholds = battery->getChargeControlEndAvailableThresholds();

      if ( !startThresholds.empty() )
      {
        std::ostringstream oss;
        oss << "[";
        for ( size_t i = 0; i < startThresholds.size(); ++i )
        {
          if ( i > 0 ) oss << ",";
          oss << startThresholds[ i ];
        }
        oss << "]";
        m_dbusData.chargeStartAvailableThresholds = oss.str();
      }

      if ( !endThresholds.empty() )
      {
        std::ostringstream oss;
        oss << "[";
        for ( size_t i = 0; i < endThresholds.size(); ++i )
        {
          if ( i > 0 ) oss << ",";
          oss << endThresholds[ i ];
        }
        oss << "]";
        m_dbusData.chargeEndAvailableThresholds = oss.str();
      }
    }
  }

  // ---- YCbCr 4:2:0 ----
  {
    m_dbusData.forceYUV420OutputSwitchAvailable = false;

    for ( const auto &cardEntry : m_settings.ycbcr420Workaround )
    {
      int card = cardEntry.card;
      for ( const auto &portEntry : cardEntry.ports )
      {
        std::string path = "/sys/kernel/debug/dri/" + std::to_string( card ) + "/" +
                           portEntry.port + "/force_yuv420_output";

        std::error_code ec;
        if ( std::filesystem::exists( path, ec ) && std::filesystem::is_regular_file( path, ec ) )
        {
          m_dbusData.forceYUV420OutputSwitchAvailable = true;
          syslog( LOG_INFO, "[uccd] YCbCr 4:2:0 output available at %s", path.c_str() );
          break;
        }
      }
      if ( m_dbusData.forceYUV420OutputSwitchAvailable.load() )
        break;
    }
  }

  // Rebuild settings JSON with real charging data
  m_dbusData.settingsJSON = buildSettingsJSON( m_dbusData.keyboardBacklightStatesJSON,
                                               m_dbusData.currentChargingProfile,
                                               m_settings );
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

      // Push GPU metrics to history store (outside dataMutex — store has its own lock)
      const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
        std::chrono::system_clock::now().time_since_epoch() ).count();

      if ( dGpuInfo.m_temp > -1.0 )
        m_metricsStore.push( "gpuTemp", now, dGpuInfo.m_temp );
      if ( dGpuInfo.m_coreFrequency > -1.0 )
        m_metricsStore.push( "gpuFrequency", now, dGpuInfo.m_coreFrequency );
      if ( dGpuInfo.m_powerDraw > -1.0 )
        m_metricsStore.push( "gpuPower", now, dGpuInfo.m_powerDraw );
      if ( dGpuInfo.m_vramFrequency > -1.0 )
        m_metricsStore.push( "gpuVramFrequency", now, dGpuInfo.m_vramFrequency );
      if ( dGpuInfo.m_coreVoltageMv > -1 )
        m_metricsStore.push( "gpuCoreVoltage", now,
                             static_cast< double >( dGpuInfo.m_coreVoltageMv ) );

      // Expose dGPU temperature through fan data for UI compatibility
      if ( dGpuInfo.m_temp > -1.0 and m_dbusData.fans.size() > 1 )
      {
        m_dbusData.fans[ 1 ].temp.set(
          static_cast< int64_t >( now ),
          static_cast< int32_t >( std::lround( dGpuInfo.m_temp ) ) );
      }
    }
  );
}

void UccDBusService::updateFanData()
{
  auto *fanProvider = m_hw.fanProvider();
  const auto &fans = m_hw.fans();
  const bool fansAvailable = ( fanProvider != nullptr && !fans.empty() );

  int minSpeed = 0;
  bool fansOffAvailable = false;

  if ( fanProvider && !fans.empty() )
  {
    minSpeed = fanProvider->getMinSpeedPercent( fans[0] );
    fansOffAvailable = fanProvider->canTurnOff( fans[0] );
  }

  const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
    std::chrono::system_clock::now().time_since_epoch() ).count();

  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.fanHwmonAvailable = fansAvailable;
  m_dbusData.fansMinSpeed = minSpeed;
  m_dbusData.fansOffAvailable = fansOffAvailable;

  if ( not fansAvailable )
    return;

  const size_t maxFans = std::min( fans.size(), m_dbusData.fans.size() );
  for ( size_t fanIndex = 0; fanIndex < maxFans; ++fanIndex )
  {
    auto speedOpt = fanProvider->getFanSpeedPercent( fans[fanIndex] );
    if ( speedOpt.has_value() )
    {
      m_dbusData.fans[fanIndex].speed.set( static_cast< int64_t >( now ), speedOpt.value() );
    }

    // For temperature, use the HardwareManager's temp sensor mapping
    const ucc::hal::TempSensorInfo *sensor = ( fanIndex == 0 )
      ? m_hw.findCpuTempSensor() : m_hw.findGpuTempSensor();
    if ( sensor )
    {
      auto tempOpt = m_hw.readTemp( *sensor );
      if ( tempOpt.has_value() )
      {
        m_dbusData.fans[fanIndex].temp.set(
          static_cast< int64_t >( now ),
          static_cast< int32_t >( std::lround( tempOpt.value() ) ) );
      }
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

    // Create the D-Bus object in the main thread (with QDBusContext for Polkit)
    m_dbusObject = std::make_unique< UccDBusObject >();

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

    // Forward AutoOC worker signals to D-Bus adaptor signals.
    // This must happen here (after m_adaptor is created) rather than in the
    // constructor where m_autoOCWorker is created, since m_adaptor is null
    // during construction.
    if ( m_autoOCWorker && m_adaptor )
    {
      // Share the adaptor's FPS server with the AutoOC worker so they use
      // the same socket (which is already open when monitoring is active).
      m_autoOCWorker->setFpsServer( &m_adaptor->m_fpsServer );

      QObject::connect( m_autoOCWorker.get(), &AutoOCWorker::progress,
        m_adaptor.get(), [this]( const AutoOCProgress &prog )
      {
        QString json = QStringLiteral(
          "{\"phase\":\"%1\",\"component\":\"%2\",\"iteration\":%3,"
          "\"maxIterations\":%4,\"currentOffset\":%5,\"bestStable\":%6,"
          "\"temp\":%7,\"gpuClock\":%8,\"vramClock\":%9,"
          "\"gpuUtil\":%10,\"fps\":%11,\"message\":\"%12\"}" )
          .arg( prog.phase == AutoOCPhase::Baseline    ? "baseline"
                : prog.phase == AutoOCPhase::Searching  ? "searching"
                : prog.phase == AutoOCPhase::Validating ? "validating"
                : prog.phase == AutoOCPhase::Done       ? "done"
                : "idle" )
          .arg( prog.component == AutoOCComponent::Vram ? "vram" : "core" )
          .arg( prog.iteration )
          .arg( prog.maxIterations )
          .arg( prog.currentOffsetMHz )
          .arg( prog.bestStableMHz )
          .arg( prog.tempC )
          .arg( prog.gpuClockMHz )
          .arg( prog.vramClockMHz )
          .arg( prog.gpuUtilPct )
          .arg( prog.fps, 0, 'f', 1 )
          .arg( QString::fromStdString( prog.message ) );

        emit m_adaptor->AutoOCProgressChanged( json );
      } );

      QObject::connect( m_autoOCWorker.get(), &AutoOCWorker::finished,
        m_adaptor.get(), [this]( const AutoOCResult &result )
      {
        emit m_adaptor->AutoOCFinished(
          result.coreOffsetMHz,
          result.vramOffsetMHz,
          result.success,
          QString::fromStdString( result.message ) );
      } );
    }

    // Forward AutoUndervolt worker signals to D-Bus adaptor signals.
    if ( m_autoUndervoltWorker && m_adaptor )
    {
      m_autoUndervoltWorker->setFpsServer( &m_adaptor->m_fpsServer );

      QObject::connect( m_autoUndervoltWorker.get(), &AutoUndervoltWorker::progress,
        m_adaptor.get(), [this]( const UndervoltProgress &prog )
      {
        QString json = QStringLiteral(
          "{\"phase\":\"%1\",\"iteration\":%2,\"maxIterations\":%3,"
            "\"currentCapMHz\":%4,\"bestCapMHz\":%5,"
            "\"currentOffsetMHz\":%6,\"bestOffsetMHz\":%7,"
            "\"temp\":%8,\"gpuClock\":%9,\"powerDraw\":%10,"
            "\"gpuUtil\":%11,\"fps\":%12,\"baselineFps\":%13,"
            "\"app\":\"%14\",\"message\":\"%15\"}" )
          .arg( prog.phase == UVPhase::Baseline    ? "baseline"
                : prog.phase == UVPhase::Searching  ? "searching"
              : prog.phase == UVPhase::OffsetSearching ? "offset_searching"
                : prog.phase == UVPhase::Validating ? "validating"
                : prog.phase == UVPhase::Done       ? "done"
                : "idle" )
          .arg( prog.iteration )
          .arg( prog.maxIterations )
          .arg( prog.currentCapMHz )
          .arg( prog.bestCapMHz )
            .arg( prog.currentOffsetMHz )
            .arg( prog.bestOffsetMHz )
          .arg( prog.tempC )
          .arg( prog.gpuClockMHz )
          .arg( prog.powerDrawW )
          .arg( prog.gpuUtilPct )
          .arg( prog.fps, 0, 'f', 1 )
          .arg( prog.baselineFps, 0, 'f', 1 )
          .arg( QString::fromStdString( prog.appName ) )
          .arg( QString::fromStdString( prog.message ) );

        emit m_adaptor->AutoUndervoltProgressChanged( json );
      } );

      QObject::connect( m_autoUndervoltWorker.get(), &AutoUndervoltWorker::finished,
        m_adaptor.get(), [this]( const UndervoltResult &result )
      {
        onAutoUndervoltFinished( result );
      } );
    }

    return true;
  }
  catch ( const std::exception &e )
  {
    syslog( LOG_ERR, "DBus service error: %s", e.what() );
    return false;
  }
}

void UccDBusService::onAutoUndervoltFinished( const UndervoltResult &result )
{
  if ( result.success && m_autoUndervoltWorker )
    persistAutoUndervoltProfile( result );

  emit m_adaptor->AutoUndervoltFinished(
    result.gpuFreqCapMHz,
    result.success,
    QString::fromStdString( result.message ),
    QString::fromStdString( result.appName ) );
}

void UccDBusService::persistAutoUndervoltProfile( const UndervoltResult &result )
{
  const auto &appKey = result.appName;
  if ( appKey.empty() || result.gpuFreqCapMHz <= 0 )
    return;

  std::string profileId;
  auto mapIt = m_settings.appGpuProfileMap.find( appKey );
  if ( mapIt != m_settings.appGpuProfileMap.end() )
    profileId = mapIt->second;
  if ( profileId.empty() )
    profileId = generateProfileId();

  const std::string profileName = "AutoUV: " + appKey;

  nlohmann::json profileJson;
  auto existing = m_settings.gpuProfiles.find( profileId );
  if ( existing != m_settings.gpuProfiles.end() )
  {
    try { profileJson = nlohmann::json::parse( existing->second ); }
    catch ( ... ) { profileJson = nlohmann::json::object(); }
  }

  profileJson[ "name" ] = profileName;
  if ( !profileJson.contains( "offsets" ) || !profileJson["offsets"].is_array() )
    profileJson[ "offsets" ] = nlohmann::json::array();

  // Upsert P0 graphics offset entry.
  bool updatedP0 = false;
  for ( auto &entry : profileJson["offsets"] )
  {
    if ( !entry.is_object() ) continue;
    if ( entry.value( "pstate", -1 ) == 0 )
    {
      entry[ "gpuOffsetMHz" ] = result.coreOffsetMHz;
      if ( !entry.contains( "vramOffsetMHz" ) )
        entry[ "vramOffsetMHz" ] = 0;
      updatedP0 = true;
      break;
    }
  }
  if ( !updatedP0 )
  {
    profileJson[ "offsets" ].push_back( {
      { "pstate", 0 },
      { "gpuOffsetMHz", result.coreOffsetMHz },
      { "vramOffsetMHz", 0 }
    } );
  }

  profileJson[ "gpuLockedClocks" ] = {
    { "enabled", true },
    { "min", result.gpuFreqCapMHz },
    { "max", result.gpuFreqCapMHz }
  };

  // AutoUV profiles must only control graphics cap + graphics offset.
  // Drop stale power/VRAM lock fields inherited from previously mapped
  // profiles, otherwise applyGpuOCProfile() can unexpectedly reset power
  // limits or force VRAM clock locks from old profile content.
  profileJson.erase( "vramLockedClocks" );
  profileJson.erase( "powerLimitW" );

  const auto nowEpochSec = static_cast< long long >(
    std::chrono::duration_cast< std::chrono::seconds >(
      std::chrono::system_clock::now().time_since_epoch() ).count() );

  profileJson[ "meta" ][ "autoUndervolt" ] = {
    { "appName", appKey },
    { "baselineClkMHz", result.baselineClkMHz },
    { "baselineFps", result.baselineFps },
    { "achievedFps", result.finalFps },
    { "coreOffsetMHz", result.coreOffsetMHz },
    { "baselineVoltageMv", result.baselineVoltageMv },
    { "achievedVoltageMv", result.finalVoltageMv },
    { "achievedPowerW", result.finalPowerW },
    { "lastUsedEpochSec", nowEpochSec }
  };

  const std::string profilePayload = profileJson.dump();
  m_settings.gpuProfiles[ profileId ] = profilePayload;
  m_settings.appGpuProfileMap[ appKey ] = profileId;

  bool found = false;
  for ( auto &gp : m_customGpuProfiles )
  {
    if ( gp.id == profileId )
    {
      gp.name = profileName;
      gp.json = profilePayload;
      found = true;
      break;
    }
  }
  if ( !found )
    m_customGpuProfiles.push_back( { profileId, profileName, profilePayload } );

  // Keep worker cache aligned with persisted settings.
  std::map< std::string, AppUndervoltProfile > syncedProfiles;
  for ( const auto &[app, p] : m_autoUndervoltWorker->profiles() )
    syncedProfiles[ app ] = p;

  AppUndervoltProfile cache;
  cache.appName = appKey;
  cache.gpuFreqCapMHz = result.gpuFreqCapMHz;
  cache.coreOffsetMHz = result.coreOffsetMHz;
  cache.baselineClkMHz = result.baselineClkMHz;
  cache.baselineFps = result.baselineFps;
  cache.achievedFps = result.finalFps;
  cache.achievedPowerW = result.finalPowerW;
  cache.achievedVoltageMv = result.finalVoltageMv;
  cache.lastUsed = std::chrono::system_clock::now();
  syncedProfiles[ appKey ] = cache;
  m_autoUndervoltWorker->loadProfiles( syncedProfiles );

  if ( !m_settingsManager.writeSettings( m_settings ) )
    syslog( LOG_WARNING, "[AutoUV] Failed to persist app->GPU profile mapping" );
  else
    updateDBusSettingsData();
}

void UccDBusService::shutdown()
{
  syslog( LOG_INFO, "Shutting down workers..." );

  // Phase 1: Signal ALL workers to stop (non-blocking).
  // This must happen before waiting, because some onWork() callbacks
  // use BlockingQueuedConnection to the main thread.  If we stop()+wait
  // sequentially, a worker stuck in BlockingQueuedConnection will deadlock
  // because the main thread is blocked in QThread::wait().
  requestStop();
  if ( m_fanControlWorker ) m_fanControlWorker->requestStop();
  if ( m_cpuWorker ) m_cpuWorker->requestStop();
  if ( m_displayWorker ) m_displayWorker->requestStop();
  if ( m_hardwareMonitorWorker ) m_hardwareMonitorWorker->requestStop();

  // Phase 2: Wait for all threads to finish while keeping the Qt event
  // loop alive.  Workers may have pending BlockingQueuedConnection calls
  // (e.g. LCTWaterCoolerWorker) that need the main thread to dispatch.
  // Pumping events here prevents deadlocks.
  auto waitPumpingEvents = []( QThread *t ) {
    if ( !t || !t->isRunning() )
      return;
    auto *app = QCoreApplication::instance();
    while ( !t->isFinished() )
    {
      if ( app )
        app->processEvents( QEventLoop::AllEvents, 50 );
      else
        QThread::msleep( 10 );
    }
    t->wait();   // join the thread
  };

  // Wait for the main service thread first, then sub-workers
  waitPumpingEvents( this );
  waitPumpingEvents( m_fanControlWorker.get() );
  waitPumpingEvents( m_cpuWorker.get() );
  waitPumpingEvents( m_displayWorker.get() );
  waitPumpingEvents( m_hardwareMonitorWorker.get() );

  // Phase 3: DBus cleanup on the main thread (where the objects live).
  // onExit() runs on the worker thread, so DBus operations there would
  // violate Qt's thread-affinity rules.
  if ( m_started )
  {
    try
    {
      QDBusConnection bus = QDBusConnection::systemBus();
      bus.unregisterService( SERVICE_NAME );
      bus.unregisterObject( OBJECT_PATH );
      syslog( LOG_INFO, "DBus service unregistered" );
    }
    catch ( const std::exception &e )
    {
      syslog( LOG_ERR, "DBus cleanup error: %s", e.what() );
    }
    m_adaptor.reset();
    m_dbusObject.reset();
    m_started = false;
  }

  syslog( LOG_INFO, "All workers stopped" );
}

void UccDBusService::onStart()
{
  m_started = true;
}

void UccDBusService::onWork()
{
  if ( not m_started )
    return;

  // On unsupported devices, skip all hardware polling
  if ( !m_dbusData.deviceSupported.load() )
    return;

  // ── Collect all sensor readings / fan RPMs into the metrics store ────
  {
    const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
      std::chrono::system_clock::now().time_since_epoch() ).count();

    // Temperature sensors
    for ( const auto &sensor : m_hw.tempSensors() )
    {
      if ( auto val = m_hw.readTemp( sensor ); val.has_value() )
        m_metricsStore.push( "sensor:" + sensor.id, now, *val );
    }

    // Thermal sources (aggregated)
    for ( const auto &ts : m_hw.thermalSources() )
    {
      if ( auto val = m_hw.readThermalSource( ts ); val.has_value() )
        m_metricsStore.push( "tsrc:" + ts.id, now, *val );
    }

    // Fan / pump RPMs
    if ( auto *fp = m_hw.fanProvider() )
    {
      for ( const auto &fan : m_hw.fans() )
      {
        if ( fan.canRead )
        {
          if ( auto rpm = fp->getFanRPM( fan ); rpm.has_value() )
            m_metricsStore.push( "fan:" + fan.id, now, static_cast< double >( *rpm ) );
        }
      }
    }

    // Hwmon voltage sensors
    for ( const auto &vs : m_voltageSensors )
    {
      std::ifstream ifs( vs.path );
      int millivolts = 0;
      if ( ifs >> millivolts )
        m_metricsStore.push( "voltage:" + vs.id, now, static_cast< double >( millivolts ) );
    }

    // Per-core CPU frequency
    double freqSum = 0;
    int freqCount = 0;
    for ( const auto &core : m_cpuFreqCores )
    {
      std::ifstream ifs( core.path );
      long khz = 0;
      if ( ifs >> khz )
      {
        const double mhz = static_cast< double >( khz ) / 1000.0;
        m_metricsStore.push( "cpufreq:" + std::to_string( core.coreIndex ), now, mhz );
        freqSum += mhz;
        ++freqCount;
      }
    }
    if ( freqCount > 0 )
      m_metricsStore.push( "cpufreq:avg", now, freqSum / freqCount );
  }

  // update tuxedo wmi availability (derived from HAL platform provider)
  m_dbusData.tuxedoWmiAvailable = ( m_hw.platformProvider() != nullptr );
  // update fan availability from HAL
  m_dbusData.fanHwmonAvailable = m_hw.hasFanControl();

  // Periodic NVIDIA cTGP offset validation (every 5 ticks = 5 s)
    if ( m_dbusData.cTGPAdjustmentSupported.load()
      && m_dbusData.nvidiaPowerCTRLAvailable.load()
      && m_profileSettingsWorker )
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

      // Update m_currentStateProfileId only if the state is mapped
      auto it = m_settings.stateMap.find( stateKey );
      m_currentStateProfileId = ( it != m_settings.stateMap.end() ) ? it->second : std::string();

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

        // Reset pump hysteresis so the auto-control loop picks up the correct
        // level for the new profile from a clean state.
        m_pumpHysSpeedIdx = 0;
        m_pumpHysThreshold = 0;
        m_wcTempFiltered = -1.0;  // reset EWMA so next sample seeds immediately

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

  if ( not enable )
  {
    m_dbusData.waterCoolerAvailable = false;
    m_dbusData.waterCoolerConnected = false;
  }

  if ( not m_waterCoolerWorker )
    return;

  if ( enable )
  {
    // Only start scanning if not already scanning/connected.
    // startScanning() calls cleanupBleController() which would tear down
    // an active BLE connection, causing pump/fan commands to fail.
    if ( not m_dbusData.waterCoolerAvailable.load() and not m_dbusData.waterCoolerConnected.load() )
      m_waterCoolerWorker->startScanning();
  }
  else
  {
    // Stop discovery and disconnect; stopScanning() now also disconnects the device
    m_waterCoolerWorker->stopScanning();
  }
}

void UccDBusService::onExit()
{
  // Only do thread-safe work here — this runs on the DaemonWorker thread.
  // DBus cleanup happens in shutdown() on the main thread.
  saveAutosave();

  // Restore all fans to auto/hardware control mode
  m_hw.restoreAllFanAuto();
}

// profile management implementation

void UccDBusService::loadProfiles()
{
  std::cout << "[ProfileManager] Loading profiles..." << std::endl;

  // Load default profiles from the HAL profile provider (or legacy device lookup)
  m_defaultProfiles = m_profileManager.getDefaultProfiles( m_deviceId );
  std::cout << "[ProfileManager] Loaded " << m_defaultProfiles.size() << " default profiles"
            << " (provider: " << ( m_hw.profileProvider() ? m_hw.profileProvider()->name() : "none" ) << ")"
            << std::endl;

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
      const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
      m_activeProfile = profile;
      m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
      snapProfileFrequencies( m_activeProfile );
      updateDBusActiveProfileData();
      return true;
    }
  }

  // fallback to default profile
  const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
  m_activeProfile = getDefaultProfile();
  m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
  snapProfileFrequencies( m_activeProfile );
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
      // Preserve runtime water cooler enable state across profile switches.
      // The user's explicit EnableWaterCooler() D-Bus call is authoritative.
      const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
      m_activeProfile = profile;
      m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
      snapProfileFrequencies( m_activeProfile );
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

        // Re-read TDP values after apply so D-Bus data reflects new hardware state
        readHardwareCapabilities();

        // Apply charging profile if the profile specifies one
        if ( !profile.chargingProfile.empty() && m_dbusData.chargingProfilesAvailable != "[]" )
        {
          std::cout << "[Profile] Applying charging profile '" << profile.chargingProfile << "'" << std::endl;
          if ( m_profileSettingsWorker->applyChargingProfile( profile.chargingProfile ) )
          {
            std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
            m_dbusData.currentChargingProfile = profile.chargingProfile;
          }
        }

        // Apply charging priority if the profile specifies one
        if ( !profile.chargingPriority.empty() && m_dbusData.chargingPrioritiesAvailable != "[]" )
        {
          std::cout << "[Profile] Applying charging priority '" << profile.chargingPriority << "'" << std::endl;
          if ( m_profileSettingsWorker->applyChargingPriority( profile.chargingPriority ) )
          {
            std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
            m_dbusData.currentChargingPriority = profile.chargingPriority;
          }
        }

        // Apply charge type and thresholds if the profile specifies them
        if ( !profile.chargeType.empty() )
        {
          std::cout << "[Profile] Applying charge type '" << profile.chargeType << "'" << std::endl;
          if ( m_profileSettingsWorker->setChargeType( profile.chargeType ) )
          {
            std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
            m_dbusData.chargeType = profile.chargeType;
          }
        }
        if ( profile.chargeStartThreshold >= 0 )
        {
          std::cout << "[Profile] Applying charge start threshold " << profile.chargeStartThreshold << std::endl;
          if ( m_profileSettingsWorker->setChargeStartThreshold( profile.chargeStartThreshold ) )
            m_dbusData.chargeStartThreshold = profile.chargeStartThreshold;
        }
        if ( profile.chargeEndThreshold >= 0 )
        {
          std::cout << "[Profile] Applying charge end threshold " << profile.chargeEndThreshold << std::endl;
          if ( m_profileSettingsWorker->setChargeEndThreshold( profile.chargeEndThreshold ) )
            m_dbusData.chargeEndThreshold = profile.chargeEndThreshold;
        }
      }

      if ( m_keyboardBacklightController.isAvailable()
           && m_settings.keyboardBacklightControlEnabled
           && !profile.keyboard.keyboardProfileId.empty() )
      {
        std::string kbData = resolveKeyboardProfileJSON( profile.keyboard.keyboardProfileId );
        if ( !kbData.empty() && kbData != "{}" )
        {
          bool kbResult = m_keyboardBacklightController.applyProfileKeyboardStates( kbData );
          std::cout << "[Profile] Keyboard apply result: " << ( kbResult ? "SUCCESS" : "FAILED" ) << std::endl;
        }
        else
        {
          std::cout << "[Profile] Keyboard profile '" << profile.keyboard.keyboardProfileId << "' not found" << std::endl;
        }
      }
      else
      {
        std::cout << "[Profile] Keyboard apply SKIPPED — one or more conditions not met" << std::endl;
      }

      // Emit ProfileChanged signal for DBus clients
      if ( m_adaptor )
        m_adaptor->emitProfileChanged( id,
                                       profile.keyboard.keyboardProfileId,
                                       profile.fan.fanProfile );

      return true;
    }
  }

  // fallback to default profile
  std::cout << "[Profile] Profile ID not found: " << id << ", using default" << std::endl;
  {
    const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
    m_activeProfile = getDefaultProfile();
    m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
  }
  snapProfileFrequencies( m_activeProfile );
  updateDBusActiveProfileData();

  // Emit ProfileChanged signal for DBus clients
  if ( m_adaptor )
  {
    m_adaptor->emitProfileChanged( m_activeProfile.id,
                                   m_activeProfile.keyboard.keyboardProfileId,
                                   m_activeProfile.fan.fanProfile );
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

    // Set as active profile, but preserve the runtime water cooler enable state.
    // The user's explicit EnableWaterCooler() D-Bus call is authoritative;
    // the stored profile may have a stale enableWaterCooler value.
    const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
    m_activeProfile = profile;
    m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
    snapProfileFrequencies( m_activeProfile );
    updateDBusActiveProfileData();

    // Try to resolve and apply fan curves from named fan profile
    try
    {
      FanProfile fp = resolveFanProfile( profile.fan.fanProfile );
      if ( fp.isValid() )
      {
        std::map< std::string, std::vector< ucc::hal::FanCurvePoint > > zoneCurves;
        for ( const auto &zc : fp.zoneCurves )
          if ( !zc.curve.empty() )
            zoneCurves[zc.zoneId] = zc.curve;

        std::cout << "[Profile] Using fan zones from profile '" << profile.fan.fanProfile << "'" << std::endl;

        if ( m_fanControlWorker && !zoneCurves.empty() )
        {
          m_fanControlWorker->applyTemporaryZoneCurves( zoneCurves );
          std::cout << "[Profile] Applied fan zones (" << zoneCurves.size() << " zones)" << std::endl;
        }

        // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
        const auto *pumpZone = fp.findZoneCurve( kWCPumpZoneId );
        if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
             && pumpZone && !pumpZone->curve.empty() )
        {
          int maxTemp = 0;
          {
            std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
            for ( const auto &fan : m_dbusData.fans )
              maxTemp = std::max( maxTemp, fan.temp.data );
          }
          // Lookup pump voltage from the wc-pump zone curve
          int pumpIdx = 0;
          for ( const auto &pt : pumpZone->curve )
          {
            if ( maxTemp >= pt.temp ) pumpIdx = std::min( pt.speed, 4 );
            else                      break;
          }
          // Reset hysteresis before a one-shot profile apply
          m_pumpHysSpeedIdx = 0;
          m_pumpHysThreshold = 0;
          static constexpr ucc::PumpVoltage pumpIdxToVoltage[] = {
            ucc::PumpVoltage::Off, ucc::PumpVoltage::V7, ucc::PumpVoltage::V8,
            ucc::PumpVoltage::V11, ucc::PumpVoltage::V12 };
          m_waterCoolerWorker->setPumpVoltage( static_cast<int>( pumpIdxToVoltage[ std::clamp( pumpIdx, 0, 4 ) ] ) );
          std::cout << "[Profile] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
        }
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

      // Re-read TDP values after apply so D-Bus data reflects new hardware state
      readHardwareCapabilities();

      // Apply charging profile if the profile specifies one
      if ( !profile.chargingProfile.empty() && m_dbusData.chargingProfilesAvailable != "[]" )
      {
        std::cout << "[Profile] Applying charging profile '" << profile.chargingProfile << "'" << std::endl;
        if ( m_profileSettingsWorker->applyChargingProfile( profile.chargingProfile ) )
        {
          std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
          m_dbusData.currentChargingProfile = profile.chargingProfile;
        }
      }

      // Apply charging priority if the profile specifies one
      if ( !profile.chargingPriority.empty() && m_dbusData.chargingPrioritiesAvailable != "[]" )
      {
        std::cout << "[Profile] Applying charging priority '" << profile.chargingPriority << "'" << std::endl;
        if ( m_profileSettingsWorker->applyChargingPriority( profile.chargingPriority ) )
        {
          std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
          m_dbusData.currentChargingPriority = profile.chargingPriority;
        }
      }

      // Apply charge type and thresholds if the profile specifies them
      if ( !profile.chargeType.empty() )
      {
        std::cout << "[Profile] Applying charge type '" << profile.chargeType << "'" << std::endl;
        if ( m_profileSettingsWorker->setChargeType( profile.chargeType ) )
        {
          std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
          m_dbusData.chargeType = profile.chargeType;
        }
      }
      if ( profile.chargeStartThreshold >= 0 )
      {
        std::cout << "[Profile] Applying charge start threshold " << profile.chargeStartThreshold << std::endl;
        if ( m_profileSettingsWorker->setChargeStartThreshold( profile.chargeStartThreshold ) )
          m_dbusData.chargeStartThreshold = profile.chargeStartThreshold;
      }
      if ( profile.chargeEndThreshold >= 0 )
      {
        std::cout << "[Profile] Applying charge end threshold " << profile.chargeEndThreshold << std::endl;
        if ( m_profileSettingsWorker->setChargeEndThreshold( profile.chargeEndThreshold ) )
          m_dbusData.chargeEndThreshold = profile.chargeEndThreshold;
      }
    }

    if ( m_keyboardBacklightController.isAvailable()
         && m_settings.keyboardBacklightControlEnabled
         && !profile.keyboard.keyboardProfileId.empty() )
    {
      std::string kbData = resolveKeyboardProfileJSON( profile.keyboard.keyboardProfileId );
      if ( !kbData.empty() && kbData != "{}" )
      {
        std::cout << "[Profile] Applying keyboard backlight settings from profile" << std::endl;
        m_keyboardBacklightController.applyProfileKeyboardStates( kbData );
      }
    }

    // Apply GPU OC and cTGP from the profile
    applyGpuOCFromProfile( profile );

    // Emit ProfileChanged signal for DBus clients
    if ( m_adaptor )
    {
      m_adaptor->emitProfileChanged( profile.id,
                                     profile.keyboard.keyboardProfileId,
                                     profile.fan.fanProfile );
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
  return m_profileManager.getDefaultCustomProfile();
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
  serializeProfilesJSON();
  if ( m_adaptor )
    m_adaptor->emitProfilesListChanged();

  std::cout << "[ProfileManager] Profile added successfully" << std::endl;
  return true;
}

bool UccDBusService::deleteCustomProfile( const std::string &profileId )
{
  std::cout << "[ProfileManager] Deleting profile '" << profileId << "' from memory" << std::endl;

  // Remove from in-memory profiles
  if ( auto it = std::remove_if( m_customProfiles.begin(), m_customProfiles.end(),
                           [&profileId]( const UccProfile &p ) { return p.id == profileId; } );
       it != m_customProfiles.end() )
  {
    m_customProfiles.erase( it, m_customProfiles.end() );

    // Update DBus data
    updateDBusActiveProfileData();
    serializeProfilesJSON();
    if ( m_adaptor )
      m_adaptor->emitProfilesListChanged();

    std::cout << "[ProfileManager] Profile deleted successfully" << std::endl;
    return true;
  }

  std::cerr << "[ProfileManager] Profile not found" << std::endl;
  return false;
}

bool UccDBusService::updateCustomProfile( const UccProfile &profile )
{
  std::cout << "[ProfileManager] Updating profile '" << profile.name << "' (ID: " << profile.id << ") in memory" << std::endl;
  std::cout << "[ProfileManager]   Active profile ID: '" << m_activeProfile.id << "'" << std::endl;
  std::cout << "[ProfileManager]   Incoming keyboard profile ID: '" << profile.keyboard.keyboardProfileId << "'" << std::endl;

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
  if ( auto it = std::ranges::find_if( m_customProfiles,
                         [&profile]( const UccProfile &p ) { return p.id == profile.id; } );
       it != m_customProfiles.end() )
  {
    *it = profile;

    // Update DBus data
    updateDBusActiveProfileData();
    serializeProfilesJSON();
    if ( m_adaptor )
      m_adaptor->emitProfilesListChanged();

    // Update active profile if it was the one modified
    if ( m_activeProfile.id == profile.id )
    {
      std::cout << "[ProfileManager] Updated profile is active, reapplying to system" << std::endl;
      const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
      m_activeProfile = profile;
      m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
      snapProfileFrequencies( m_activeProfile );
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
    else
    {
      std::cout << "[ProfileManager] Updated profile is NOT the active profile — not reapplying" << std::endl;
      std::cout << "[ProfileManager]   active='" << m_activeProfile.id << "' vs saved='" << profile.id << "'" << std::endl;
    }

    std::cout << "[ProfileManager] Profile updated successfully" << std::endl;
    return true;
  }

  std::cerr << "[ProfileManager] Profile not found for update" << std::endl;
  return false;
}

void UccDBusService::initializeDisplayModes()
{
  // Detect session type by reading /proc/<pid>/environ directly (no shell).
  // The old popen()-based approach was vulnerable to environment injection.
  std::string sessionType;
  const uid_t rootUid = 0;

  try
  {
    namespace fs = std::filesystem;
    for ( const auto &entry : fs::directory_iterator( "/proc" ) )
    {
      if ( not sessionType.empty() )
        break;

      const std::string pidName = entry.path().filename().string();
      if ( pidName.empty() or not std::isdigit( static_cast< unsigned char >( pidName[0] ) ) )
        continue;

      // Skip root-owned processes
      std::string loginuidPath = entry.path().string() + "/loginuid";
      std::ifstream loginuidFile( loginuidPath );
      if ( loginuidFile )
      {
        uid_t uid = 0;
        loginuidFile >> uid;
        if ( uid == rootUid or uid == static_cast< uid_t >( -1 ) )
          continue;
      }
      else
      {
        continue;
      }

      std::string environPath = entry.path().string() + "/environ";
      std::ifstream envFile( environPath, std::ios::binary );
      if ( not envFile )
        continue;

      std::string envContent( ( std::istreambuf_iterator< char >( envFile ) ),
                               std::istreambuf_iterator< char >() );

      std::istringstream envStream( envContent );
      std::string envEntry;
      while ( std::getline( envStream, envEntry, '\0' ) )
      {
        if ( envEntry.starts_with( "XDG_SESSION_TYPE=" ) )
        {
          sessionType = envEntry.substr( 17 );
          break;
        }
      }
    }
  }
  catch ( ... )
  {
    // Fall through with empty sessionType
  }

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
  dmiSKUDeviceMap[ "GEMINI17I04" ] = UniwillDeviceID::GEMINI17I04;
  dmiSKUDeviceMap[ "GEMINIGEN4I" ] = UniwillDeviceID::GEMINI17I04;
  dmiSKUDeviceMap[ "IBM15A10" ] = UniwillDeviceID::IBM15A10;
  dmiSKUDeviceMap[ "SIRIUS1601" ] = UniwillDeviceID::SIRIUS1601;
  dmiSKUDeviceMap[ "SIRIUS1602" ] = UniwillDeviceID::SIRIUS1602;

  // check for sku match
  if ( auto skuIt = dmiSKUDeviceMap.find( productSKU ); skuIt != dmiSKUDeviceMap.end() )
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

  if ( auto uwidIt = uwidDeviceMap.find( modelId ); uwidIt != uwidDeviceMap.end() )
  {
    return uwidIt->second;
  }

  // no device match found
  return std::nullopt;
}

void UccDBusService::computeDeviceCapabilities()
{
  // Delegate to UniwillProfileProvider when available
  if ( auto *uwProvider = dynamic_cast< ucc::hal::UniwillProfileProvider * >( m_hw.profileProvider() ) )
  {
    m_dbusData.waterCoolerSupported = uwProvider->supportsWaterCooler();
    m_dbusData.cTGPAdjustmentSupported = uwProvider->supportsCTGPAdjustment();
  }
  else
  {
    // Generic / non-Uniwill device: no water cooler, cTGP depends on sysfs
    m_dbusData.waterCoolerSupported = false;

    std::error_code ec;
    const std::string ctgpPath = "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";
    bool hardwareExists = std::filesystem::exists( ctgpPath, ec ) &&
                         std::filesystem::is_regular_file( ctgpPath, ec );
    m_dbusData.cTGPAdjustmentSupported = hardwareExists;
  }

  syslog( LOG_INFO, "Device capabilities: aquaris=%s, cTGP=%s",
          m_dbusData.waterCoolerSupported.load() ? "supported" : "not supported",
          m_dbusData.cTGPAdjustmentSupported.load() ? "supported" : "hidden" );
}

void UccDBusService::loadSettings()
{
  bool settingsChanged = false;

  if ( auto loadedSettings = m_settingsManager.readSettings(); loadedSettings.has_value() )
  {
    m_settings = *loadedSettings;
    std::cout << "[Settings] Loaded existing settings" << std::endl;
  }
  else
  {
    // Fresh install — create default settings with sensible stateMap.
    // Pick the default profile with the highest total TDP so the GUI
    // immediately shows meaningful power-limit values instead of 0 W.
    m_settings = TccSettings();

    std::string bestProfileId;
    int64_t     bestTdpSum = -1;

    for ( const auto &profile : m_defaultProfiles )
    {
      int64_t sum = 0;
      for ( int v : profile.odmPowerLimits.tdpValues )
        sum += v;

      if ( sum > bestTdpSum )
      {
        bestTdpSum = sum;
        bestProfileId = profile.id;
      }
    }

    if ( bestProfileId.empty() && !m_defaultProfiles.empty() )
      bestProfileId = m_defaultProfiles.back().id;

    if ( !bestProfileId.empty() )
    {
      m_settings.stateMap[ "power_ac" ]  = bestProfileId;
      m_settings.stateMap[ "power_bat" ] = bestProfileId;
      m_settings.stateMap[ "power_wc" ]  = bestProfileId;
    }

    std::cout << "[Settings] No settings file found — initialized default stateMap "
              << "(all states=" << bestProfileId << ")" << std::endl;
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

  // Load custom sub-profiles from settings into in-memory vectors
  auto loadSubProfiles = []( const std::map< std::string, std::string > &map,
                             std::vector< SubProfile > &out,
                             const char *label ) {
    out.clear();
    for ( const auto &[id, jsonStr] : map )
    {
      try
      {
        auto j = nlohmann::json::parse( jsonStr );
        SubProfile sp;
        sp.id = id;
        sp.name = j.value( "name", id );
        sp.json = jsonStr;
        out.push_back( std::move( sp ) );
        std::cout << "[Settings] Loaded " << label << " profile '" << sp.name
                  << "' (ID: " << id << ")" << std::endl;
      }
      catch ( const std::exception &e )
      {
        std::cerr << "[Settings] Failed to parse " << label << " profile '"
                  << id << "': " << e.what() << std::endl;
      }
    }
  };
  loadSubProfiles( m_settings.fanProfiles, m_customFanProfiles, "fan" );
  loadSubProfiles( m_settings.keyboardProfiles, m_customKeyboardProfiles, "keyboard" );
  loadSubProfiles( m_settings.gpuProfiles, m_customGpuProfiles, "GPU" );

  // IMPORTANT: Do NOT resync/clear m_settings.profiles!
  // Reason: m_settings.profiles is the authoritative source from the file.
  // Resyncing can change keys or representation, breaking stateMap lookups.
  // Keep m_settings.profiles exactly as loaded from file.
  // Only modify it when profiles are explicitly added/edited via API.

  // validate and fix state map if needed
  auto allProfiles = getAllProfiles();
  for ( const auto &stateKey : { "power_ac", "power_bat", "power_wc" } )
  {
    // check if state key exists in map – if not, leave it unassigned
    if ( m_settings.stateMap.find( stateKey ) == m_settings.stateMap.end() )
    {
      std::cout << "[Settings] No profile assigned to state '"
                << stateKey << "', waiting for ucc-gui" << std::endl;
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
                << stateKey << "' not found, removing assignment" << std::endl;
      m_settings.stateMap.erase( stateKey );
      settingsChanged = true;
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

void UccDBusService::initializeStartupProfile()
{
  // Skip on unsupported devices — no workers are running
  if ( !m_dbusData.deviceSupported.load() )
    return;

  UccProfile resolved = m_profileManager.resolveStartupProfile(
    m_deviceId,
    m_settings.stateMap,
    m_settings.profiles
  );

  if ( resolved.id.empty() )
  {
    syslog( LOG_INFO, "[Startup] No startup profile resolved — no state map entry for current power state" );
    // Still apply the default keyboard profile so the backlight is not left dark.
    if ( m_keyboardBacklightController.isAvailable()
         && m_settings.keyboardBacklightControlEnabled
         && !m_builtinKeyboardProfiles.empty() )
    {
      const auto &kbFallback = m_builtinKeyboardProfiles.front();
      syslog( LOG_INFO, "[Startup] Applying default keyboard profile '%s' as fallback",
              kbFallback.id.c_str() );
      m_keyboardBacklightController.applyProfileKeyboardStates( kbFallback.json );
    }
    return;
  }

  m_activeProfile = resolved;
  m_currentState = determineState();
  m_currentStateProfileId = resolved.id;

  snapProfileFrequencies( m_activeProfile );
  updateDBusActiveProfileData();

  syslog( LOG_INFO, "[Startup] Applying startup profile: %s (ID: %s)", resolved.name.c_str(), resolved.id.c_str() );
  applyStartupProfile();
}

void UccDBusService::applyStartupProfile()
{
  if ( m_activeProfile.id.empty() )
    return;

  const auto &profile = m_activeProfile;

  applyFanAndPumpSettings( profile );

  if ( m_cpuWorker )
    m_cpuWorker->reapplyProfile();

  if ( m_profileSettingsWorker )
    m_profileSettingsWorker->reapplyProfile();

  applyKeyboardFromProfile( profile );

  applyGpuOCFromProfile( profile );

  if ( m_dbusData.waterCoolerSupported )
    setWaterCoolerScanningEnabled( profile.fan.enableWaterCooler );
}

void UccDBusService::applyFanAndPumpSettings( const UccProfile &profile )
{
  // Resolve and apply fan curves from named profile
  try
  {
    FanProfile fp = resolveFanProfile( profile.fan.fanProfile );
    if ( fp.isValid() )
    {
      // Register custom thermal sources before zone rebuild
      if ( !fp.thermalSources.empty() )
      {
        m_hw.addThermalSources( fp.thermalSources );
        std::cout << "[FanPump] Registered " << fp.thermalSources.size()
                  << " custom thermal sources from profile" << std::endl;
      }

      // If the profile carries zone topology (fanIds), rebuild the zone model
      // and restart the worker so it picks up the new fan-to-zone mapping.
      bool zonesChanged = false;
      for ( const auto &zc : fp.zoneCurves )
      {
        if ( zc.hasTopology() )
        {
          zonesChanged = true;
          break;
        }
      }

      if ( zonesChanged )
      {
        rebuildFanZonesFromProfile( fp );

        // Restart the worker so onStart() reads the new zones
        if ( m_fanControlWorker )
        {
          m_fanControlWorker->stop();
          m_fanControlWorker->start();
          std::cout << "[FanPump] Restarted FanControlWorker with profile zones" << std::endl;
        }
      }

      // Apply curves as temporary overrides (works on both fresh and restarted worker)
      std::map< std::string, std::vector< ucc::hal::FanCurvePoint > > zoneCurves;
      for ( const auto &zc : fp.zoneCurves )
        if ( !zc.curve.empty() )
          zoneCurves[zc.zoneId] = zc.curve;

      std::cout << "[FanPump] Using fan zones from profile '" << profile.fan.fanProfile << "'" << std::endl;

      if ( m_fanControlWorker && !zoneCurves.empty() )
      {
        m_fanControlWorker->applyTemporaryZoneCurves( zoneCurves );
        std::cout << "[FanPump] Applied fan zones (" << zoneCurves.size() << " zones)" << std::endl;
      }

      // Apply per-zone thermal source overrides from the profile
      {
        std::map< std::string, std::string > thermalSources;
        for ( const auto &zc : fp.zoneCurves )
          if ( !zc.thermalSourceId.empty() )
            thermalSources[zc.zoneId] = zc.thermalSourceId;
        if ( m_fanControlWorker && !thermalSources.empty() )
          m_fanControlWorker->applyZoneThermalSources( thermalSources );
      }

      // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
      const auto *pumpZone = fp.findZoneCurve( kWCPumpZoneId );
      if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
           && pumpZone && !pumpZone->curve.empty() )
      {
        int maxTemp = 0;
        {
          std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
          for ( const auto &fan : m_dbusData.fans )
            maxTemp = std::max( maxTemp, fan.temp.data );
        }
        int pumpIdx = 0;
        for ( const auto &pt : pumpZone->curve )
        {
          if ( maxTemp >= pt.temp ) pumpIdx = std::min( pt.speed, 4 );
          else                      break;
        }
        m_pumpHysSpeedIdx = 0;
        m_pumpHysThreshold = 0;
        static constexpr ucc::PumpVoltage pumpIdxToVoltage[] = {
          ucc::PumpVoltage::Off, ucc::PumpVoltage::V7, ucc::PumpVoltage::V8,
          ucc::PumpVoltage::V11, ucc::PumpVoltage::V12 };
        m_waterCoolerWorker->setPumpVoltage( static_cast<int>( pumpIdxToVoltage[ std::clamp( pumpIdx, 0, 4 ) ] ) );
        std::cout << "[FanPump] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
      }
    }
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[FanPump] Failed to apply fan/pump settings: " << e.what() << std::endl;
  }
}

void UccDBusService::applyGpuOCFromProfile( const UccProfile &profile )
{
  // Profiles with no GPU profile selected should not touch GPU state at all.
  if ( profile.gpuProfileId.empty() )
    return;

  // Resolve GPU OC profile data from the sub-profile store
  std::string gpuData = resolveGpuProfileJSON( profile.gpuProfileId );
  if ( gpuData.empty() || gpuData == "{}" )
  {
    std::cerr << "[GpuOC] GPU profile '" << profile.gpuProfileId << "' not found" << std::endl;
    return;
  }

  // Extract and apply cTGP offset from GPU profile data.
  // When cTGP is available, strip powerLimitW so the NVML path does not
  // also fire — cTGP handles power via its own sysfs interface.
  const bool ctgpActive = m_profileSettingsWorker
                       && m_dbusData.cTGPAdjustmentSupported.load()
                       && m_dbusData.nvidiaPowerCTRLAvailable.load();
  if ( ctgpActive )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( gpuData ) );
    if ( doc.isObject() )
    {
      QJsonObject obj = doc.object();
      if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj[ "nvidiaPowerCTRLProfile" ].isObject() )
      {
        const int ctgpOffset = obj[ "nvidiaPowerCTRLProfile" ].toObject().value( "cTGPOffset" ).toInt( 0 );
        std::cout << "[GpuOC] Applying cTGP offset from profile: " << ctgpOffset << std::endl;
        m_profileSettingsWorker->applyNVIDIAPowerOffset( ctgpOffset );
      }
      // Remove powerLimitW so the OC worker does not also set NVML power limit
      obj.remove( "powerLimitW" );
      gpuData = QJsonDocument( obj ).toJson( QJsonDocument::Compact ).toStdString();
    }
  }

  // Apply GPU OC settings (clock offsets, locked clocks, power limit)
  if ( m_nvidiaOCWorker && m_nvidiaOCWorker->isAvailable() )
  {
    std::cout << "[GpuOc] Applying GPU OC profile '" << profile.gpuProfileId
              << "' from profile '" << profile.name << "'" << std::endl;
    if ( !m_nvidiaOCWorker->applyGpuOCProfile( gpuData, 0 ) )
      std::cerr << "[GpuOC] Failed to apply GPU OC profile data" << std::endl;
  }
}

void UccDBusService::applyKeyboardFromProfile( const UccProfile &profile )
{
  if ( !m_keyboardBacklightController.isAvailable()
       || !m_settings.keyboardBacklightControlEnabled )
    return;

  std::string kbId = profile.keyboard.keyboardProfileId;
  if ( kbId.empty() )
  {
    // No keyboard profile assigned — fall back to the first built-in keyboard profile.
    if ( m_builtinKeyboardProfiles.empty() )
      return;
    kbId = m_builtinKeyboardProfiles.front().id;
    std::cout << "[Keyboard] No keyboard profile assigned, using built-in fallback '" << kbId << "'" << std::endl;
  }

  std::string kbData = resolveKeyboardProfileJSON( kbId );
  if ( kbData.empty() || kbData == "{}" )
  {
    std::cerr << "[Keyboard] Keyboard profile '" << kbId << "' not found" << std::endl;
    return;
  }

  bool kbResult = m_keyboardBacklightController.applyProfileKeyboardStates( kbData );
  std::cout << "[Keyboard] Apply keyboard profile '" << kbId << "': "
            << ( kbResult ? "SUCCESS" : "FAILED" ) << std::endl;
}

FanProfile UccDBusService::resolveFanProfile( const std::string &fanProfileId ) const
{
  if ( fanProfileId.empty() )
    return FanProfile();

  // Check custom fan profiles first
  for ( const auto &fp : m_customFanProfiles )
  {
    if ( fp.id == fanProfileId )
    {
      // Parse JSON to FanProfile
      return ProfileManager::parseFanProfileJSON( fp.json );
    }
  }

  // Check daemon-managed built-in fan profiles
  for ( const auto &fp : m_builtinFanProfiles )
  {
    if ( fp.id == fanProfileId )
      return ProfileManager::parseFanProfileJSON( fp.json );
  }

  if ( !m_builtinFanProfiles.empty() )
    return ProfileManager::parseFanProfileJSON( m_builtinFanProfiles.front().json );

  return FanProfile();
}

std::string UccDBusService::resolveGpuProfileJSON( const std::string &gpuProfileId ) const
{
  if ( gpuProfileId.empty() )
    return {};

  // Check custom GPU profiles first
  for ( const auto &gp : m_customGpuProfiles )
  {
    if ( gp.id == gpuProfileId )
      return gp.json;
  }

  // Check built-in GPU profiles
  for ( const auto &gp : m_builtinGpuProfiles )
  {
    if ( gp.id == gpuProfileId )
      return gp.json;
  }

  return {};
}

std::string UccDBusService::resolveKeyboardProfileJSON( const std::string &keyboardProfileId ) const
{
  if ( keyboardProfileId.empty() )
    return {};

  // Check custom keyboard profiles first
  for ( const auto &kp : m_customKeyboardProfiles )
  {
    if ( kp.id == keyboardProfileId )
      return kp.json;
  }

  // Check built-in keyboard profiles
  for ( const auto &kp : m_builtinKeyboardProfiles )
  {
    if ( kp.id == keyboardProfileId )
      return kp.json;
  }

  return {};
}

void UccDBusService::onFanTemperatureUpdate( size_t fanIndex, int64_t timestamp, int temp )
{
  {
    std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
    if ( fanIndex < m_dbusData.fans.size() )
      m_dbusData.fans[ fanIndex ].temp.set( timestamp, temp );
  }

  // Push temperature to history store
  if ( fanIndex == 0 )
    m_metricsStore.push( "cpuTemp", timestamp, temp );
  else if ( fanIndex == 1 )
    m_metricsStore.push( "gpuTemp", timestamp, temp );

  // Auto-control water cooler fan and pump voltage based on CPU temperature
  if ( !m_dbusData.waterCoolerConnected.load() || !m_activeProfile.fan.autoControlWC || fanIndex != 0 )
    return;

  try
  {
    updateWaterCoolerAutoControl( temp );
  }
  catch ( ... ) { /* ignore errors in water cooler auto-control */ }
}

void UccDBusService::updateWaterCoolerAutoControl( int temp )
{
  // Apply asymmetric EWMA to the raw sensor reading so that the
  // water-cooler fan and pump see a smooth temperature signal,
  // matching the filtering the main fan control loop uses.
  if ( m_wcTempFiltered < 0.0 )
    m_wcTempFiltered = static_cast< double >( temp );
  else
  {
    const double alpha = ( temp > m_wcTempFiltered )
                           ? WC_TEMP_ALPHA_RISING : WC_TEMP_ALPHA_FALLING;
    m_wcTempFiltered += alpha * ( static_cast< double >( temp ) - m_wcTempFiltered );
  }
  const int wcTemp = static_cast< int >( std::round( m_wcTempFiltered ) );

  const std::string &fpName = m_activeProfile.fan.fanProfile;
  FanProfile fp = resolveFanProfile( fpName );

  // Overlay water cooler fan curve from temporary curves if active
  if ( m_fanControlWorker && m_fanControlWorker->hasTemporaryCurves() )
  {
    const auto &tempCurves = m_fanControlWorker->tempZoneCurves();
    auto wcFanIt = tempCurves.find( kWCFanZoneId );
    if ( wcFanIt != tempCurves.end() && !wcFanIt->second.empty() )
    {
      auto *wcFanZone = fp.findZoneCurve( kWCFanZoneId );
      if ( wcFanZone )
        wcFanZone->curve = wcFanIt->second;
    }
  }

  // Overlay pump curve from temporary curves if active
  if ( m_fanControlWorker && m_fanControlWorker->hasTemporaryCurves() )
  {
    const auto &tempCurves = m_fanControlWorker->tempZoneCurves();
    auto wcPumpIt = tempCurves.find( kWCPumpZoneId );
    if ( wcPumpIt != tempCurves.end() && !wcPumpIt->second.empty() )
    {
      auto *wcPumpZone = fp.findZoneCurve( kWCPumpZoneId );
      if ( wcPumpZone )
        wcPumpZone->curve = wcPumpIt->second;
    }
  }

  const int snappedTemp = ( ( wcTemp + 2 ) / 5 ) * 5;  // round to nearest 5°C
  const int wcFanSpeed = fp.getSpeedForZone( snappedTemp, kWCFanZoneId );
  if ( m_waterCoolerWorker )
    m_waterCoolerWorker->setFanSpeed( std::max( wcFanSpeed, 0 ) );

  // Temperature LED mode: compute gradient color from fan speed
  if ( m_waterCoolerLedMode.load() == static_cast< int32_t >( ucc::RGBState::Temperature ) )
  {
    const float t = static_cast< float >( std::clamp( wcFanSpeed, 0, 100 ) ) / 100.0f;
    const int ledR = static_cast< int >( t * 255.0f );
    const int ledG = 0;
    const int ledB = static_cast< int >( ( 1.0f - t ) * 255.0f );
    if ( m_waterCoolerWorker )
      m_waterCoolerWorker->setLEDColor( ledR, ledG, ledB,
        static_cast< int >( ucc::RGBState::Static ) );
  }

  // Auto-control pump voltage with hysteresis.
  // Step-up happens immediately at the table threshold; step-down requires
  // the temperature to fall at least PUMP_HYSTERESIS_DEG below the
  // threshold that last triggered an upward transition.
  static constexpr ucc::PumpVoltage pumpIdxToVoltage[] = {
      ucc::PumpVoltage::Off, ucc::PumpVoltage::V7, ucc::PumpVoltage::V8,
      ucc::PumpVoltage::V11, ucc::PumpVoltage::V12 };

  // Read pump curve from the wc-pump zone
  const auto *pumpZone = fp.findZoneCurve( kWCPumpZoneId );
  int rawIdx = 0;
  if ( pumpZone )
  {
    for ( const auto &pt : pumpZone->curve )
    {
      if ( wcTemp >= pt.temp ) rawIdx = std::min( pt.speed, 4 );
      else                     break;
    }
  }

  if ( rawIdx > m_pumpHysSpeedIdx )
  {
    // Temperature rising – apply new level and record its table threshold.
    m_pumpHysSpeedIdx = rawIdx;
    m_pumpHysThreshold = 0;
    if ( pumpZone )
      for ( const auto &pt : pumpZone->curve )
        if ( std::min( pt.speed, 4 ) == rawIdx ) { m_pumpHysThreshold = pt.temp; break; }
  }
  else if ( rawIdx < m_pumpHysSpeedIdx )
  {
    // Temperature falling – only step down once we are past the dead-band.
    if ( wcTemp < m_pumpHysThreshold - PUMP_HYSTERESIS_DEG )
    {
      m_pumpHysSpeedIdx = rawIdx;
      m_pumpHysThreshold = 0;
      if ( pumpZone )
        for ( const auto &pt : pumpZone->curve )
          if ( std::min( pt.speed, 4 ) == rawIdx ) { m_pumpHysThreshold = pt.temp; break; }
    }
  }

  const ucc::PumpVoltage pumpSpeedValue =
      pumpIdxToVoltage[ std::clamp( m_pumpHysSpeedIdx, 0, 4 ) ];
  if ( m_waterCoolerWorker )
    m_waterCoolerWorker->setPumpVoltage( static_cast<int>( pumpSpeedValue ) );
}

void UccDBusService::applyFullProfile( const UccProfile &profile )
{
  // Preserve runtime water cooler enable state across profile re-application.
  // The user's explicit EnableWaterCooler() D-Bus call is authoritative;
  // the stored profile may have a stale enableWaterCooler value.
  const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
  m_activeProfile = profile;
  m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
  snapProfileFrequencies( m_activeProfile );
  updateDBusActiveProfileData();

  // Resolve and apply fan curves from named profile
  try
  {
    FanProfile fp = resolveFanProfile( profile.fan.fanProfile );
    if ( fp.isValid() )
    {
      std::map< std::string, std::vector< ucc::hal::FanCurvePoint > > zoneCurves;
      for ( const auto &zc : fp.zoneCurves )
        if ( !zc.curve.empty() )
          zoneCurves[zc.zoneId] = zc.curve;

      std::cout << "[State] Using fan zones from profile '" << profile.fan.fanProfile << "'" << std::endl;

      if ( m_fanControlWorker && !zoneCurves.empty() )
      {
        m_fanControlWorker->applyTemporaryZoneCurves( zoneCurves );
        std::cout << "[State] Applied fan zones (" << zoneCurves.size() << " zones)" << std::endl;
      }

      // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
      const auto *pumpZone = fp.findZoneCurve( kWCPumpZoneId );
      if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
           && pumpZone && !pumpZone->curve.empty() )
      {
        int maxTemp = 0;
        {
          std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
          for ( const auto &fan : m_dbusData.fans )
            maxTemp = std::max( maxTemp, fan.temp.data );
        }
        int pumpIdx = 0;
        for ( const auto &pt : pumpZone->curve )
        {
          if ( maxTemp >= pt.temp ) pumpIdx = std::min( pt.speed, 4 );
          else                      break;
        }
        m_pumpHysSpeedIdx = 0;
        m_pumpHysThreshold = 0;
        static constexpr ucc::PumpVoltage pumpIdxToVoltage[] = {
          ucc::PumpVoltage::Off, ucc::PumpVoltage::V7, ucc::PumpVoltage::V8,
          ucc::PumpVoltage::V11, ucc::PumpVoltage::V12 };
        m_waterCoolerWorker->setPumpVoltage( static_cast<int>( pumpIdxToVoltage[ std::clamp( pumpIdx, 0, 4 ) ] ) );
        std::cout << "[State] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
      }
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
  applyKeyboardFromProfile( profile );

  // Apply GPU OC and cTGP from the profile
  applyGpuOCFromProfile( profile );

  // Water cooler scanning state is preserved from the runtime flag
  // (set via EnableWaterCooler D-Bus call). Do NOT re-read enableWaterCooler
  // from the stored profile — it may be stale.

  // Emit ProfileChanged signal for DBus clients
  if ( m_adaptor )
    m_adaptor->emitProfileChanged( profile.id,
                                   profile.keyboard.keyboardProfileId,
                                   profile.fan.fanProfile );
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

  // Try persistent (custom) profiles first
  if ( auto profileIt = m_settings.profiles.find( profileId );
       profileIt != m_settings.profiles.end() )
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

  // Serialize all profiles — built-in (editable=false) + custom (editable=true)
  std::ostringstream allProfilesJSON;
  allProfilesJSON << "[";

  size_t allIdx = 0;
  // Built-in profiles (not editable)
  for ( const auto &profile : m_defaultProfiles )
  {
    if ( allIdx > 0 ) allProfilesJSON << ",";
    allProfilesJSON << profileToJSON( profile, defaultOnlineCores, defaultScalingMin, defaultScalingMax, false );
    ++allIdx;
  }
  // Custom profiles (editable)
  for ( const auto &profile : m_customProfiles )
  {
    if ( allIdx > 0 ) allProfilesJSON << ",";
    allProfilesJSON << profileToJSON( profile, defaultOnlineCores, defaultScalingMin, defaultScalingMax, true );
    ++allIdx;
  }
  allProfilesJSON << "]";

  // Default-only list (backward compat)
  std::ostringstream defaultProfilesJSON;
  defaultProfilesJSON << "[";
  for ( size_t i = 0; i < m_defaultProfiles.size(); ++i )
  {
    if ( i > 0 ) defaultProfilesJSON << ",";
    defaultProfilesJSON << profileToJSON( m_defaultProfiles[i], defaultOnlineCores, defaultScalingMin, defaultScalingMax, false );
  }
  defaultProfilesJSON << "]";

  // Custom-only list (backward compat)
  std::ostringstream customProfilesJSON;
  customProfilesJSON << "[";
  for ( size_t i = 0; i < m_customProfiles.size(); ++i )
  {
    if ( i > 0 ) customProfilesJSON << ",";
    customProfilesJSON << profileToJSON( m_customProfiles[i], defaultOnlineCores, defaultScalingMin, defaultScalingMax, true );
  }
  customProfilesJSON << "]";

  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.profilesJSON = allProfilesJSON.str();
  m_dbusData.defaultProfilesJSON = defaultProfilesJSON.str();
  m_dbusData.customProfilesJSON = customProfilesJSON.str();
  m_dbusData.defaultValuesProfileJSON = profileToJSON( defaultProfile,
                                                       defaultOnlineCores,
                                                       defaultScalingMin,
                                                       defaultScalingMax );

  std::cout << "[DBus] Re-serialized profile JSONs (built-in=" << m_defaultProfiles.size()
            << " custom=" << m_customProfiles.size() << ")" << std::endl;
}

void UccDBusService::fillDeviceSpecificDefaults( std::vector< UccProfile > &profiles )
{
  const int32_t cpuMinFreq = getCpuMinFrequency();
  const int32_t cpuMaxFreq = getCpuMaxFrequency();

  // Get TDP info from HAL TDP provider
  std::vector< TDPInfo > tdpInfo;
  {
    auto *platform = m_hw.tdpProvider();
    const int nrTDPs = platform ? platform->getNumberTDPs() : 0;
    if ( nrTDPs > 0 )
    {
      const auto descriptors = platform->getTDPDescriptors();

      for ( int i = 0; i < nrTDPs; ++i )
      {
        TDPInfo info;
        info.current = 0;
        info.min = 0;
        info.max = 0;
        info.descriptor = ( i < static_cast< int >( descriptors.size() ) )
                            ? descriptors[ static_cast< size_t >( i ) ]
                            : "";

        if ( auto v = platform->getTDPMin( i ) ) info.min = static_cast< uint32_t >( *v );
        if ( auto v = platform->getTDPMax( i ) ) info.max = static_cast< uint32_t >( *v );
        if ( auto v = platform->getTDP( i ) )    info.current = static_cast< uint32_t >( *v );

        tdpInfo.push_back( info );
      }

      std::cout << "[fillDeviceSpecificDefaults] TDP info available: " << tdpInfo.size() << " entries" << std::endl;
      for ( size_t i = 0; i < tdpInfo.size(); ++i )
      {
        std::cout << "[fillDeviceSpecificDefaults]   TDP[" << i << "]: min=" << tdpInfo[i].min
                  << ", max=" << tdpInfo[i].max << ", current=" << tdpInfo[i].current << std::endl;
      }
    }
    else
    {
      std::cout << "[fillDeviceSpecificDefaults] No TDP hardware available" << std::endl;
    }
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
        profile.odmPowerLimits.tdpValues.push_back( static_cast< int >( tdpInfo[i].max ) );
        std::cout << "[fillDeviceSpecificDefaults]     Added TDP[" << i << "] = " << tdpInfo[i].max << std::endl;
      }
    }

    // Clamp existing TDP values to hardware min/max range.
    // Default profiles have hardcoded placeholders (e.g. {5,10,15}) that
    // may lie outside the actual hardware capabilities.
    if ( !tdpInfo.empty() )
    {
      for ( size_t i = 0; i < profile.odmPowerLimits.tdpValues.size() && i < tdpInfo.size(); ++i )
      {
        int &val = profile.odmPowerLimits.tdpValues[ i ];
        const int hwMin = static_cast< int >( tdpInfo[ i ].min );
        const int hwMax = static_cast< int >( tdpInfo[ i ].max );

        if ( val < hwMin )
        {
          std::cout << "[fillDeviceSpecificDefaults]   TDP[" << i << "] " << val
                    << " below hw min " << hwMin << ", clamping" << std::endl;
          val = hwMin;
        }
        else if ( val > hwMax )
        {
          std::cout << "[fillDeviceSpecificDefaults]   TDP[" << i << "] " << val
                    << " above hw max " << hwMax << ", clamping" << std::endl;
          val = hwMax;
        }
      }
    }

    // Snap CPU frequencies to nearest available hardware frequency
    snapProfileFrequencies( profile );

    // Assign default keyboard profile if none referenced
    if ( profile.keyboard.keyboardProfileId.empty() && !m_builtinKeyboardProfiles.empty() )
      profile.keyboard.keyboardProfileId = m_builtinKeyboardProfiles.front().id;

    // Assign default GPU profile if none referenced
    if ( profile.gpuProfileId.empty() && !m_builtinGpuProfiles.empty() )
      profile.gpuProfileId = m_builtinGpuProfiles.front().id;

    // Assign/repair fan profile to the platform's available built-in set.
    const auto hasBuiltinFanProfile = [this]( const std::string &id ) {
      for ( const auto &fp : m_builtinFanProfiles )
        if ( fp.id == id )
          return true;
      return false;
    };

    if ( ( profile.fan.fanProfile.empty() || !hasBuiltinFanProfile( profile.fan.fanProfile ) )
         && !m_builtinFanProfiles.empty() )
    {
      profile.fan.fanProfile = m_builtinFanProfiles.front().id;
    }

    std::cout << "[fillDeviceSpecificDefaults]   Final TDP values: " << profile.odmPowerLimits.tdpValues.size() << std::endl;
  }
}

void UccDBusService::snapProfileFrequencies( UccProfile &profile )
{
  if ( m_cpuWorker )
    m_cpuWorker->snapProfileFrequencies( profile );
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
      result.resize( static_cast< size_t >( cardNumber + 1 ) );
    }

    // Extract port name (everything after "card0-")
    std::string portName = name.substr( dashPos + 1 );
    result[ static_cast< size_t >( cardNumber ) ].push_back( portName );
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

void UccDBusService::discoverVoltageSensors()
{
  namespace fs = std::filesystem;
  const fs::path hwmonBase = "/sys/class/hwmon";
  if ( !fs::exists( hwmonBase ) )
    return;

  for ( auto const& hwEntry : fs::directory_iterator( hwmonBase ) )
  {
    auto hwPath = hwEntry.path();
    // Read chip name for labeling
    std::string chipName;
    {
      std::ifstream f( hwPath / "name" );
      if ( f )
        std::getline( f, chipName );
    }

    // Scan for inN_input files
    for ( int i = 0; i < 32; ++i )
    {
      auto inputFile = hwPath / ( "in" + std::to_string( i ) + "_input" );
      if ( !fs::exists( inputFile ) )
        continue;

      std::string label;
      auto labelFile = hwPath / ( "in" + std::to_string( i ) + "_label" );
      {
        std::ifstream f( labelFile );
        if ( f )
          std::getline( f, label );
      }

      VoltageSensorInfo vs;
      vs.path = inputFile.string();
      vs.id = hwPath.filename().string() + "_in" + std::to_string( i );
      if ( !label.empty() )
        vs.label = label;
      else if ( !chipName.empty() )
        vs.label = chipName + " in" + std::to_string( i );
      else
        vs.label = hwPath.filename().string() + " in" + std::to_string( i );

      m_voltageSensors.push_back( std::move( vs ) );
    }
  }

  syslog( LOG_INFO, "Discovered %zu voltage sensors", m_voltageSensors.size() );
}

void UccDBusService::discoverCpuFreqCores()
{
  namespace fs = std::filesystem;
  const fs::path cpuBase = "/sys/devices/system/cpu";

  for ( int i = 0; i < 1024; ++i )
  {
    auto freqPath = cpuBase / ( "cpu" + std::to_string( i ) ) / "cpufreq" / "scaling_cur_freq";
    if ( !fs::exists( freqPath ) )
    {
      if ( i > 0 )
        break;
      continue;
    }

    CpuFreqCore core;
    core.coreIndex = i;
    core.path = freqPath.string();
    m_cpuFreqCores.push_back( std::move( core ) );
  }

  syslog( LOG_INFO, "Discovered %zu CPU frequency cores", m_cpuFreqCores.size() );
}
