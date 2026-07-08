/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "SysfsNode.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ucc::uniwill
{
namespace fs = std::filesystem;
using Sysfs = SysfsNode< std::string >;

inline constexpr uint32_t TDP_MIN_WATTS = 25;
inline constexpr int32_t FAN_MIN_SPEED_PERCENT = 25;

struct PlatformProfileSink
{
  std::string profilePath;
  std::string choicesPath;
  std::string description;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not profilePath.empty() and not choicesPath.empty();
  }
};

struct DriverPaths
{
  std::string platformDevicePath;
  std::string hwmonPath;
  PlatformProfileSink platformProfile;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not platformDevicePath.empty();
  }
};

struct DriverInfo
{
  std::string infoPath;
  std::string projectIdPath;
  std::string moduleIdPath;
  std::string romIdPath;
  std::string ecFirmwareVersionPath;
  std::optional< int32_t > projectId;
  std::string moduleId;
  std::string romId;
  std::string ecFirmwareVersion;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return projectId.has_value() or not moduleId.empty() or not romId.empty()
           or not ecFirmwareVersion.empty();
  }
};

struct CpuPowerLimit
{
  std::string descriptor;
  std::string valuePath;
  std::string maxPath;
  uint32_t min = 0;
  uint32_t max = 0;
  uint32_t current = 0;
};

struct CtgpInfo
{
  std::string offsetPath;
  std::string offsetMaxPath;
  std::string tgpBasePath;
  int32_t currentOffset = 0;
  int32_t maxOffset = 0;
  int32_t tgpBase = 0;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not offsetPath.empty();
  }
};

struct DgpuPlatformState
{
  std::optional< int32_t > tgpBase;
  std::optional< int32_t > currentCtgpOffset;
  std::optional< int32_t > maxDynamicBoostOffset;
  std::optional< bool > dynamicBoostEnabled;
  std::string muxMode;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return tgpBase.has_value() or currentCtgpOffset.has_value()
           or maxDynamicBoostOffset.has_value() or dynamicBoostEnabled.has_value()
           or not muxMode.empty();
  }
};

struct FanChannel
{
  size_t index = 0;
  std::string label;
  std::string fanInputPath;
  std::string tempInputPath;
  std::string pwmPath;
  std::string pwmEnablePath;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not fanInputPath.empty() or not tempInputPath.empty() or not pwmPath.empty();
  }

  [[nodiscard]] bool canWritePwmMode() const noexcept
  {
    return not pwmEnablePath.empty();
  }

  [[nodiscard]] bool canWritePwm() const noexcept
  {
    return not pwmPath.empty();
  }

  [[nodiscard]] bool canUseManualControl() const noexcept
  {
    return canWritePwm() and canWritePwmMode();
  }
};

struct FanInfo
{
  std::string hwmonPath;
  std::vector< FanChannel > channels;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not hwmonPath.empty() and not channels.empty();
  }
};

[[nodiscard]] inline bool fanManualControlAvailable( const FanInfo &info )
{
  return std::ranges::any_of( info.channels, []( const FanChannel &channel ) {
    return channel.canUseManualControl();
  } );
}

[[nodiscard]] inline bool fanOffAvailable( const FanInfo &info )
{
  return fanManualControlAvailable( info );
}

[[nodiscard]] inline int32_t fanMinimumSpeedPercent( const FanInfo &info )
{
  return fanManualControlAvailable( info ) ? FAN_MIN_SPEED_PERCENT : 0;
}

struct FanReading
{
  int32_t temperatureCelsius = -1;
  int32_t speedPercent = -1;
  int32_t rpm = -1;
};

struct UsbCPowerPriority
{
  std::string path;
  std::string current;
  std::vector< std::string > choices;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not path.empty();
  }
};

struct WaterCoolerBridge
{
  std::string enablePath;
  bool enabled = false;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not enablePath.empty();
  }
};

struct HwmonTelemetry
{
  std::string hwmonPath;
  std::optional< int32_t > cpuTemperatureCelsius;
  std::optional< int32_t > gpuTemperatureCelsius;
  std::optional< int32_t > batteryTemperatureCelsius;
  std::optional< int32_t > ssdTemperatureCelsius;
  std::optional< double > systemPowerWatts;
  std::optional< double > gpuPowerAllocationWatts;
  std::optional< double > thermalBudgetWatts;
  std::optional< double > adapterCurrentAmps;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not hwmonPath.empty();
  }
};

// On-DIMM thermal sensor reading. Standard DDR SPD sensors (DDR5 spd5118 hub,
// DDR4 jc42), one hwmon device per populated slot, exposed as temp1_input.
struct DramTemperature
{
  int slot = 0;                 // slot index, derived from the SPD i2c address (0x50->0)
  int temperatureCelsius = 0;
  std::string inputPath;
};

[[nodiscard]] inline fs::path sysfsPath( const std::string &sysfsRoot, const fs::path &relative )
{
  const fs::path root = sysfsRoot.empty() ? fs::path( "/sys" ) : fs::path( sysfsRoot );
  return root / relative;
}

[[nodiscard]] inline std::optional< std::string > readFirstLine( const fs::path &path )
{
  return Sysfs( path ).read();
}

[[nodiscard]] inline std::optional< uint32_t > readUint32( const fs::path &path )
{
  const auto value = SysfsNode< uint64_t >( path ).read();
  if ( value && *value <= std::numeric_limits< uint32_t >::max() )
    return static_cast< uint32_t >( *value );

  return std::nullopt;
}

[[nodiscard]] inline std::optional< int64_t > readInt64( const fs::path &path )
{
  return SysfsNode< int64_t >( path ).read();
}

[[nodiscard]] inline std::optional< int32_t > readInt32( const fs::path &path )
{
  return SysfsNode< int32_t >( path ).read();
}

[[nodiscard]] inline std::optional< int32_t > parseInt32AutoBase( const std::string &line )
{
  try
  {
    size_t parsedCharacters = 0;
    const long parsed = std::stol( line, &parsedCharacters, 0 );

    if ( parsedCharacters == 0 or
         parsed < std::numeric_limits< int32_t >::min() or
         parsed > std::numeric_limits< int32_t >::max() )
      return std::nullopt;

    return static_cast< int32_t >( parsed );
  }
  catch ( ... )
  {
  }

  return std::nullopt;
}

[[nodiscard]] inline int32_t pwmToPercent( int32_t pwm )
{
  pwm = std::clamp< int32_t >( pwm, 0, 255 );
  return static_cast< int32_t >( std::lround( static_cast< double >( pwm ) * 100.0 / 255.0 ) );
}

[[nodiscard]] inline int32_t percentToPwm( int32_t percent )
{
  percent = std::clamp< int32_t >( percent, 0, 100 );
  return static_cast< int32_t >( std::lround( static_cast< double >( percent ) * 255.0 / 100.0 ) );
}

[[nodiscard]] inline int32_t millidegreesToCelsius( int32_t millidegrees )
{
  return static_cast< int32_t >( std::lround( static_cast< double >( millidegrees ) / 1000.0 ) );
}

[[nodiscard]] inline std::optional< int32_t > readTemperatureCelsius( const fs::path &path )
{
  if ( const auto millidegrees = readInt32( path ) )
    return millidegreesToCelsius( *millidegrees );

  return std::nullopt;
}

[[nodiscard]] inline std::optional< double > readMicrounitsAsUnits( const fs::path &path )
{
  if ( const auto microunits = readInt64( path ) )
    return static_cast< double >( *microunits ) / 1000000.0;

  return std::nullopt;
}

[[nodiscard]] inline std::optional< double > readMilliunitsAsUnits( const fs::path &path )
{
  if ( const auto milliunits = readInt64( path ) )
    return static_cast< double >( *milliunits ) / 1000.0;

  return std::nullopt;
}

[[nodiscard]] inline std::vector< std::string > splitWords( const std::string &line )
{
  std::vector< std::string > words;
  std::istringstream stream( line );
  std::string word;

  while ( stream >> word )
    words.push_back( word );

  return words;
}

[[nodiscard]] inline bool contains( const std::vector< std::string > &values, const std::string &value )
{
  return std::find( values.begin(), values.end(), value ) != values.end();
}

[[nodiscard]] inline std::vector< std::string > readChoices( const std::string &choicesPath )
{
  if ( auto line = readFirstLine( choicesPath ) )
    return splitWords( *line );

  return {};
}

[[nodiscard]] inline bool looksLikeUniwillProfileChoices(
  const std::vector< std::string > &choices )
{
  return contains( choices, "quiet" ) and contains( choices, "balanced" );
}

[[nodiscard]] inline std::optional< fs::path > findPlatformDevice( const std::string &sysfsRoot )
{
  const std::array< fs::path, 2 > roots = {
    sysfsPath( sysfsRoot, "bus/platform/devices" ),
    sysfsPath( sysfsRoot, "devices/platform" )
  };

  for ( const auto &root : roots )
  {
    try
    {
      if ( not Sysfs::exists( root ) )
        continue;

      for ( const fs::path &entry : Sysfs::directoryEntries( root ) )
      {
        if ( entry.filename().string().starts_with( "INOU0000:" ) )
          return entry;
      }
    }
    catch ( ... )
    {
    }
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional< fs::path > findHwmonDeviceUncached( const std::string &sysfsRoot )
{
  const fs::path root = sysfsPath( sysfsRoot, "class/hwmon" );

  try
  {
    if ( not Sysfs::exists( root ) )
      return std::nullopt;

    for ( const fs::path &entry : Sysfs::directoryEntries( root ) )
    {
      if ( auto name = readFirstLine( entry / "name" ); name and *name == "uniwill" )
        return entry;
    }
  }
  catch ( ... )
  {
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional< fs::path > findHwmonDevice( const std::string &sysfsRoot )
{
  if ( sysfsRoot != "/sys" and not sysfsRoot.empty() )
    return findHwmonDeviceUncached( sysfsRoot );

  static std::mutex cacheMutex;
  static std::optional< fs::path > cachedHwmonPath;

  std::lock_guard< std::mutex > lock( cacheMutex );
  if ( cachedHwmonPath )
  {
    if ( auto name = readFirstLine( *cachedHwmonPath / "name" ); name and *name == "uniwill" )
      return cachedHwmonPath;

    cachedHwmonPath.reset();
  }

  cachedHwmonPath = findHwmonDeviceUncached( "/sys" );
  return cachedHwmonPath;
}

[[nodiscard]] inline std::optional< PlatformProfileSink > findNamedPlatformProfile(
  const std::string &sysfsRoot,
  const std::string &name )
{
  const fs::path root = sysfsPath( sysfsRoot, "class/platform-profile" );

  try
  {
    if ( not Sysfs::exists( root ) )
      return std::nullopt;

    for ( const fs::path &entry : Sysfs::directoryEntries( root ) )
    {
      const fs::path profilePath = entry / "profile";
      const fs::path choicesPath = entry / "choices";

      if ( not Sysfs::exists( profilePath ) or not Sysfs::exists( choicesPath ) )
        continue;

      if ( auto foundName = readFirstLine( entry / "name" ); foundName and *foundName == name )
      {
        return PlatformProfileSink{
          profilePath.string(),
          choicesPath.string(),
          name
        };
      }
    }
  }
  catch ( ... )
  {
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional< PlatformProfileSink > findPlatformProfile(
  const std::string &sysfsRoot )
{
  if ( auto sink = findNamedPlatformProfile( sysfsRoot, "uniwill-platform-profile" ) )
    return sink;

  if ( not findPlatformDevice( sysfsRoot ) )
    return std::nullopt;

  const fs::path profilePath = sysfsPath( sysfsRoot, "firmware/acpi/platform_profile" );
  const fs::path choicesPath = sysfsPath( sysfsRoot, "firmware/acpi/platform_profile_choices" );
  if ( not Sysfs::exists( profilePath ) or not Sysfs::exists( choicesPath ) )
    return std::nullopt;

  if ( not looksLikeUniwillProfileChoices( readChoices( choicesPath.string() ) ) )
    return std::nullopt;

  return PlatformProfileSink{
    profilePath.string(),
    choicesPath.string(),
    "ACPI platform_profile (uniwill)"
  };
}

[[nodiscard]] inline DriverPaths discover( const std::string &sysfsRoot = "/sys" )
{
  DriverPaths paths;

  if ( auto platformDevice = findPlatformDevice( sysfsRoot ) )
    paths.platformDevicePath = platformDevice->string();

  if ( auto hwmon = findHwmonDevice( sysfsRoot ) )
    paths.hwmonPath = hwmon->string();

  if ( auto platformProfile = findPlatformProfile( sysfsRoot ) )
    paths.platformProfile = *platformProfile;

  return paths;
}

[[nodiscard]] inline DriverInfo readDriverInfo( const std::string &sysfsRoot = "/sys" )
{
  DriverInfo info;
  const auto platformDevice = findPlatformDevice( sysfsRoot );

  if ( not platformDevice )
    return info;

  const fs::path infoPath = *platformDevice / "info";
  if ( not Sysfs::exists( infoPath ) )
    return info;

  info.infoPath = infoPath.string();

  const fs::path projectIdPath = infoPath / "project_id";
  if ( Sysfs::exists( projectIdPath ) )
  {
    info.projectIdPath = projectIdPath.string();
    if ( auto projectId = readFirstLine( projectIdPath ) )
      info.projectId = parseInt32AutoBase( *projectId );
  }

  const fs::path moduleIdPath = infoPath / "module_id";
  if ( Sysfs::exists( moduleIdPath ) )
  {
    info.moduleIdPath = moduleIdPath.string();
    info.moduleId = readFirstLine( moduleIdPath ).value_or( "" );
  }

  const fs::path romIdPath = infoPath / "rom_id";
  if ( Sysfs::exists( romIdPath ) )
  {
    info.romIdPath = romIdPath.string();
    info.romId = readFirstLine( romIdPath ).value_or( "" );
  }

  const fs::path ecFirmwareVersionPath = infoPath / "ec_firmware_version";
  if ( Sysfs::exists( ecFirmwareVersionPath ) )
  {
    info.ecFirmwareVersionPath = ecFirmwareVersionPath.string();
    info.ecFirmwareVersion = readFirstLine( ecFirmwareVersionPath ).value_or( "" );
  }

  return info;
}

[[nodiscard]] inline HwmonTelemetry readHwmonTelemetry( const std::string &sysfsRoot = "/sys" )
{
  HwmonTelemetry telemetry;
  const auto hwmon = findHwmonDevice( sysfsRoot );

  if ( not hwmon )
    return telemetry;

  telemetry.hwmonPath = hwmon->string();
  telemetry.cpuTemperatureCelsius = readTemperatureCelsius( *hwmon / "temp1_input" );
  telemetry.gpuTemperatureCelsius = readTemperatureCelsius( *hwmon / "temp2_input" );
  telemetry.batteryTemperatureCelsius = readTemperatureCelsius( *hwmon / "temp3_input" );
  telemetry.ssdTemperatureCelsius = readTemperatureCelsius( *hwmon / "temp4_input" );
  telemetry.systemPowerWatts = readMicrounitsAsUnits( *hwmon / "power1_input" );
  telemetry.gpuPowerAllocationWatts = readMicrounitsAsUnits( *hwmon / "power2_input" );
  telemetry.thermalBudgetWatts = readMicrounitsAsUnits( *hwmon / "power3_input" );
  telemetry.adapterCurrentAmps = readMilliunitsAsUnits( *hwmon / "curr1_input" );

  return telemetry;
}

// Derive the DIMM slot index from an SPD i2c device name like "11-0050" -> 0,
// "11-0051" -> 1. SPD/thermal sensors live at 0x50..0x57. Returns -1 if unknown.
[[nodiscard]] inline int dramSlotFromI2cName( const std::string &deviceName )
{
  const auto dash = deviceName.find_last_of( '-' );
  if ( dash == std::string::npos )
    return -1;
  try
  {
    const long addr = std::stol( deviceName.substr( dash + 1 ), nullptr, 16 );
    if ( addr >= 0x50 and addr <= 0x57 )
      return static_cast< int >( addr - 0x50 );
  }
  catch ( ... )
  {
  }
  return -1;
}

// Read on-DIMM temperatures from the standard DDR SPD thermal sensors
// (DDR5 "spd5118" / DDR4 "jc42" hwmon devices), one per populated slot.
[[nodiscard]] inline std::vector< DramTemperature > readDramTemperatures(
  const std::string &sysfsRoot = "/sys" )
{
  std::vector< DramTemperature > temps;
  const fs::path root = sysfsPath( sysfsRoot, "class/hwmon" );

  try
  {
    if ( not Sysfs::exists( root ) )
      return temps;

    for ( const fs::path &entry : Sysfs::directoryEntries( root ) )
    {
      const auto name = readFirstLine( entry / "name" );
      if ( not name or ( *name != "spd5118" and *name != "jc42" ) )
        continue;

      const fs::path inputPath = entry / "temp1_input";
      const auto millidegrees = readInt32( inputPath );
      if ( not millidegrees )
        continue;

      // The hwmon's "device" symlink basename carries the i2c address (e.g. 11-0050).
      int slot = static_cast< int >( temps.size() );
      if ( const auto devLink = Sysfs::readSymlink( entry / "device" ) )
      {
        const int parsed = dramSlotFromI2cName( devLink->filename().string() );
        if ( parsed >= 0 )
          slot = parsed;
      }

      temps.push_back( DramTemperature{
        slot,
        millidegreesToCelsius( *millidegrees ),
        inputPath.string()
      } );
    }
  }
  catch ( ... )
  {
  }

  std::sort( temps.begin(), temps.end(),
             []( const DramTemperature &a, const DramTemperature &b ) { return a.slot < b.slot; } );

  return temps;
}

[[nodiscard]] inline FanInfo readFanInfo( const std::string &sysfsRoot = "/sys" )
{
  FanInfo info;
  const auto hwmon = findHwmonDevice( sysfsRoot );

  if ( not hwmon )
    return info;

  info.hwmonPath = hwmon->string();

  for ( size_t channelIndex = 0; channelIndex < 2; ++channelIndex )
  {
    const size_t number = channelIndex + 1;
    const fs::path fanInputPath = *hwmon / ( "fan" + std::to_string( number ) + "_input" );
    const fs::path fanLabelPath = *hwmon / ( "fan" + std::to_string( number ) + "_label" );
    const fs::path tempInputPath = *hwmon / ( "temp" + std::to_string( number ) + "_input" );
    const fs::path pwmPath = *hwmon / ( "pwm" + std::to_string( number ) );
    const fs::path pwmEnablePath = *hwmon / ( "pwm" + std::to_string( number ) + "_enable" );

    FanChannel channel{
      channelIndex,
      readFirstLine( fanLabelPath ).value_or( channelIndex == 0 ? "Main" : "Secondary" ),
      Sysfs::exists( fanInputPath ) ? fanInputPath.string() : "",
      Sysfs::exists( tempInputPath ) ? tempInputPath.string() : "",
      Sysfs::exists( pwmPath ) ? pwmPath.string() : "",
      Sysfs::exists( pwmEnablePath ) ? pwmEnablePath.string() : ""
    };

    if ( channel.isAvailable() )
      info.channels.push_back( channel );
  }

  return info;
}

[[nodiscard]] inline FanReading readFanReading( const FanChannel &channel )
{
  FanReading reading;

  if ( not channel.tempInputPath.empty() )
  {
    if ( const auto tempMillidegrees = readInt32( channel.tempInputPath ) )
      reading.temperatureCelsius = millidegreesToCelsius( *tempMillidegrees );
  }

  if ( not channel.pwmPath.empty() )
  {
    if ( const auto pwm = readInt32( channel.pwmPath ) )
      reading.speedPercent = pwmToPercent( *pwm );
  }

  if ( not channel.fanInputPath.empty() )
    reading.rpm = readInt32( channel.fanInputPath ).value_or( -1 );

  return reading;
}

inline bool writeFanMode( const FanInfo &info, int32_t mode )
{
  bool wroteAny = false;

  for ( const auto &channel : info.channels )
  {
    if ( not channel.canWritePwmMode() )
      continue;

    if ( SysfsNode< int32_t >( channel.pwmEnablePath ).write( mode ) )
      wroteAny = true;
  }

  return wroteAny;
}

inline bool writeFanPwm( const FanChannel &channel, int32_t speedPercent )
{
  if ( not channel.canWritePwm() )
    return false;

  return SysfsNode< int32_t >( channel.pwmPath ).write( percentToPwm( speedPercent ) );
}

[[nodiscard]] inline std::string translateUsbCPowerPriority( const std::string &value )
{
  if ( value == "charge_battery" )
    return "charging";

  return value;
}

[[nodiscard]] inline UsbCPowerPriority readUsbCPowerPriority(
  const std::string &sysfsRoot = "/sys" )
{
  UsbCPowerPriority priority;
  const auto platformDevice = findPlatformDevice( sysfsRoot );

  if ( not platformDevice )
    return priority;

  const fs::path path = *platformDevice / "usb_c_power_priority";
  if ( not Sysfs::exists( path ) )
    return priority;

  priority.path = path.string();
  priority.current = translateUsbCPowerPriority( readFirstLine( path ).value_or( "" ) );
  priority.choices = { "charging", "performance" };

  return priority;
}

inline bool writeUsbCPowerPriority(
  const UsbCPowerPriority &priority,
  const std::string &value )
{
  const std::string translatedValue = translateUsbCPowerPriority( value );

  if ( not priority.isAvailable() or not contains( priority.choices, translatedValue ) )
    return false;

  return SysfsNode< std::string >( priority.path ).write( translatedValue );
}

[[nodiscard]] inline WaterCoolerBridge readWaterCoolerBridge(
  const std::string &sysfsRoot = "/sys" )
{
  WaterCoolerBridge bridge;
  const auto platformDevice = findPlatformDevice( sysfsRoot );

  if ( not platformDevice )
    return bridge;

  const fs::path enablePath = *platformDevice / "wc" / "enable";
  if ( not Sysfs::exists( enablePath ) )
    return bridge;

  bridge.enablePath = enablePath.string();
  bridge.enabled = SysfsNode< bool >( bridge.enablePath ).read().value_or( false );

  return bridge;
}

inline SysfsWriteResult writeWaterCoolerBridgeEnable(
  const WaterCoolerBridge &bridge,
  bool enable )
{
  if ( not bridge.isAvailable() )
    return { false, ENOENT, 0 };

  return SysfsNode< bool >( bridge.enablePath ).writeDetailed( enable );
}

inline SysfsWriteResult writeWaterCoolerBridgeEnable(
  bool enable,
  const std::string &sysfsRoot = "/sys" )
{
  return writeWaterCoolerBridgeEnable( readWaterCoolerBridge( sysfsRoot ), enable );
}

[[nodiscard]] inline std::vector< CpuPowerLimit > readCpuPowerLimits(
  const std::string &sysfsRoot = "/sys" )
{
  std::vector< CpuPowerLimit > limits;
  const auto platformDevice = findPlatformDevice( sysfsRoot );

  if ( not platformDevice )
    return limits;

  const std::array< const char *, 3 > names = { "pl1", "pl2", "pl4" };

  for ( const char *name : names )
  {
    const fs::path valuePath = *platformDevice / "cpu" / name;
    const fs::path maxPath = *platformDevice / "cpu" / ( std::string( name ) + "_max" );
    const auto current = readUint32( valuePath );
    const auto max = readUint32( maxPath );

    if ( not current or not max )
      continue;

    limits.push_back( CpuPowerLimit{
      std::string( name ),
      valuePath.string(),
      maxPath.string(),
      TDP_MIN_WATTS,
      *max,
      *current
    } );
  }

  return limits;
}

[[nodiscard]] inline CtgpInfo readCtgpInfo( const std::string &sysfsRoot = "/sys" )
{
  CtgpInfo info;
  const auto platformDevice = findPlatformDevice( sysfsRoot );

  if ( not platformDevice )
    return info;

  const fs::path dgpuPath = *platformDevice / "dgpu";
  const fs::path offsetPath = dgpuPath / "ctgp_offset";
  if ( not Sysfs::exists( offsetPath ) )
    return info;

  info.offsetPath = offsetPath.string();
  info.offsetMaxPath = ( dgpuPath / "ctgp_offset_max" ).string();
  info.tgpBasePath = ( dgpuPath / "tgp_base" ).string();
  info.currentOffset = readInt32( offsetPath ).value_or( 0 );
  info.maxOffset = readInt32( info.offsetMaxPath ).value_or( 0 );
  info.tgpBase = readInt32( info.tgpBasePath ).value_or( 0 );

  return info;
}

[[nodiscard]] inline DgpuPlatformState readDgpuPlatformState( const std::string &sysfsRoot = "/sys" )
{
  DgpuPlatformState state;
  const auto platformDevice = findPlatformDevice( sysfsRoot );

  if ( not platformDevice )
    return state;

  const fs::path dgpuPath = *platformDevice / "dgpu";

  const auto readOptionalInt32 = []( const fs::path &path ) -> std::optional< int32_t > {
    if ( not Sysfs::exists( path ) )
      return std::nullopt;
    return readInt32( path );
  };

  const auto readOptionalBool = []( const fs::path &path ) -> std::optional< bool > {
    if ( not Sysfs::exists( path ) )
      return std::nullopt;
    return SysfsNode< bool >( path ).read();
  };

  state.tgpBase = readOptionalInt32( dgpuPath / "tgp_base" );
  state.currentCtgpOffset = readOptionalInt32( dgpuPath / "ctgp_offset" );
  state.maxDynamicBoostOffset = readOptionalInt32( dgpuPath / "db_offset_max" );
  state.dynamicBoostEnabled = readOptionalBool( dgpuPath / "dynamic_boost_enable" );
  state.muxMode = readFirstLine( dgpuPath / "mux_mode" ).value_or( "" );

  return state;
}

[[nodiscard]] inline std::string translatePlatformProfileName(
  const std::string &requestedName,
  const std::vector< std::string > &availableProfiles )
{
  if ( requestedName.empty() or contains( availableProfiles, requestedName ) )
    return requestedName;

  if ( requestedName == "power_save" and contains( availableProfiles, "quiet" ) )
    return "quiet";

  if ( requestedName == "enthusiast" and contains( availableProfiles, "balanced" ) )
    return "balanced";

  if ( requestedName == "overboost" )
  {
    if ( contains( availableProfiles, "performance" ) )
      return "performance";

    if ( contains( availableProfiles, "balanced" ) )
      return "balanced";
  }

  return requestedName;
}
}
