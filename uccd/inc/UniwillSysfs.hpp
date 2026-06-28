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
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ucc::uniwill
{
namespace fs = std::filesystem;

inline constexpr uint32_t TDP_MIN_WATTS = 25;
inline constexpr int32_t FAN_MIN_SPEED_PERCENT = 25;
inline constexpr int32_t FAN_AUTO_POINT_COUNT = 16;
inline constexpr int32_t FAN_AUTO_POINT_HYSTERESIS_C = 3;

inline constexpr std::array< int32_t, FAN_AUTO_POINT_COUNT > FAN_AUTO_POINT_TEMPERATURES_C = {
  25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100
};

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
  std::optional< int32_t > projectId;
  std::string moduleId;
  std::string romId;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return projectId.has_value() or not moduleId.empty() or not romId.empty();
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

struct FanCurvePoint
{
  int32_t tempCelsius = 0;
  int32_t speedPercent = 0;
};

struct FanChannel
{
  size_t index = 0;
  std::string label;
  std::string fanInputPath;
  std::string tempInputPath;
  std::string pwmPath;
  std::string pwmEnablePath;
  bool supportsCustomAuto = false;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return not fanInputPath.empty() or not tempInputPath.empty() or not pwmPath.empty();
  }

  [[nodiscard]] bool canWritePwmMode() const noexcept
  {
    return not pwmEnablePath.empty();
  }

  [[nodiscard]] bool canUseCustomAuto() const noexcept
  {
    return supportsCustomAuto and canWritePwmMode();
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

[[nodiscard]] inline bool fanCustomAutoAvailable( const FanInfo &info )
{
  return std::ranges::any_of( info.channels, []( const FanChannel &channel ) {
    return channel.canUseCustomAuto();
  } );
}

[[nodiscard]] inline bool fanOffAvailable( const FanInfo &info )
{
  return fanCustomAutoAvailable( info );
}

[[nodiscard]] inline int32_t fanMinimumSpeedPercent( const FanInfo &info )
{
  return fanCustomAutoAvailable( info ) ? FAN_MIN_SPEED_PERCENT : 0;
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

[[nodiscard]] inline fs::path sysfsPath( const std::string &sysfsRoot, const fs::path &relative )
{
  const fs::path root = sysfsRoot.empty() ? fs::path( "/sys" ) : fs::path( sysfsRoot );
  return root / relative;
}

[[nodiscard]] inline std::optional< std::string > readFirstLine( const fs::path &path )
{
  std::ifstream file( path );
  if ( not file.is_open() )
    return std::nullopt;

  std::string line;
  if ( not std::getline( file, line ) )
    return std::nullopt;

  return line;
}

[[nodiscard]] inline std::optional< uint32_t > readUint32( const fs::path &path )
{
  if ( auto line = readFirstLine( path ) )
  {
    try
    {
      return static_cast< uint32_t >( std::stoul( *line ) );
    }
    catch ( ... )
    {
    }
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional< int32_t > readInt32( const fs::path &path )
{
  if ( auto line = readFirstLine( path ) )
  {
    try
    {
      return static_cast< int32_t >( std::stol( *line ) );
    }
    catch ( ... )
    {
    }
  }

  return std::nullopt;
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
      if ( not fs::exists( root ) )
        continue;

      for ( const auto &entry : fs::directory_iterator( root ) )
      {
        if ( entry.path().filename().string().starts_with( "INOU0000:" ) )
          return entry.path();
      }
    }
    catch ( ... )
    {
    }
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional< fs::path > findHwmonDevice( const std::string &sysfsRoot )
{
  const fs::path root = sysfsPath( sysfsRoot, "class/hwmon" );

  try
  {
    if ( not fs::exists( root ) )
      return std::nullopt;

    for ( const auto &entry : fs::directory_iterator( root ) )
    {
      if ( auto name = readFirstLine( entry.path() / "name" ); name and *name == "uniwill" )
        return entry.path();
    }
  }
  catch ( ... )
  {
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional< PlatformProfileSink > findNamedPlatformProfile(
  const std::string &sysfsRoot,
  const std::string &name )
{
  const fs::path root = sysfsPath( sysfsRoot, "class/platform-profile" );

  try
  {
    if ( not fs::exists( root ) )
      return std::nullopt;

    for ( const auto &entry : fs::directory_iterator( root ) )
    {
      const fs::path profilePath = entry.path() / "profile";
      const fs::path choicesPath = entry.path() / "choices";

      if ( not fs::exists( profilePath ) or not fs::exists( choicesPath ) )
        continue;

      if ( auto foundName = readFirstLine( entry.path() / "name" ); foundName and *foundName == name )
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
  if ( not fs::exists( profilePath ) or not fs::exists( choicesPath ) )
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
  if ( not fs::exists( infoPath ) )
    return info;

  info.infoPath = infoPath.string();

  const fs::path projectIdPath = infoPath / "project_id";
  if ( fs::exists( projectIdPath ) )
  {
    info.projectIdPath = projectIdPath.string();
    if ( auto projectId = readFirstLine( projectIdPath ) )
      info.projectId = parseInt32AutoBase( *projectId );
  }

  const fs::path moduleIdPath = infoPath / "module_id";
  if ( fs::exists( moduleIdPath ) )
  {
    info.moduleIdPath = moduleIdPath.string();
    info.moduleId = readFirstLine( moduleIdPath ).value_or( "" );
  }

  const fs::path romIdPath = infoPath / "rom_id";
  if ( fs::exists( romIdPath ) )
  {
    info.romIdPath = romIdPath.string();
    info.romId = readFirstLine( romIdPath ).value_or( "" );
  }

  return info;
}

[[nodiscard]] inline bool hasFanAutoPointFiles( const fs::path &hwmonPath, size_t channelIndex )
{
  const size_t number = channelIndex + 1;

  for ( const int point : { 1, FAN_AUTO_POINT_COUNT } )
  {
    const std::string prefix =
      "pwm" + std::to_string( number ) + "_auto_point" + std::to_string( point );

    if ( not fs::exists( hwmonPath / ( prefix + "_pwm" ) ) or
         not fs::exists( hwmonPath / ( prefix + "_temp" ) ) or
         not fs::exists( hwmonPath / ( prefix + "_temp_hyst" ) ) )
      return false;
  }

  return true;
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
      fs::exists( fanInputPath ) ? fanInputPath.string() : "",
      fs::exists( tempInputPath ) ? tempInputPath.string() : "",
      fs::exists( pwmPath ) ? pwmPath.string() : "",
      fs::exists( pwmEnablePath ) ? pwmEnablePath.string() : "",
      hasFanAutoPointFiles( *hwmon, channelIndex )
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

inline bool writeFanCurve( const FanChannel &channel, const std::vector< FanCurvePoint > &points )
{
  if ( not channel.supportsCustomAuto or points.size() < FAN_AUTO_POINT_COUNT )
    return false;

  const size_t number = channel.index + 1;
  const fs::path hwmonPath = fs::path( channel.pwmPath ).parent_path();

  for ( size_t i = 0; i < static_cast< size_t >( FAN_AUTO_POINT_COUNT ); ++i )
  {
    const int point = static_cast< int >( i + 1 );
    const std::string prefix =
      "pwm" + std::to_string( number ) + "_auto_point" + std::to_string( point );

    const int32_t tempCelsius = std::clamp< int32_t >( points[ i ].tempCelsius, 0, 255 );
    const int32_t hystCelsius =
      std::max< int32_t >( 0, tempCelsius - FAN_AUTO_POINT_HYSTERESIS_C );
    const int32_t pwm = percentToPwm( points[ i ].speedPercent );

    if ( not SysfsNode< int32_t >( ( hwmonPath / ( prefix + "_temp" ) ).string() )
               .write( tempCelsius * 1000 ) )
      return false;

    if ( not SysfsNode< int32_t >( ( hwmonPath / ( prefix + "_temp_hyst" ) ).string() )
               .write( hystCelsius * 1000 ) )
      return false;

    if ( not SysfsNode< int32_t >( ( hwmonPath / ( prefix + "_pwm" ) ).string() ).write( pwm ) )
      return false;
  }

  return true;
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
  if ( not fs::exists( path ) )
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
  if ( not fs::exists( offsetPath ) )
    return info;

  info.offsetPath = offsetPath.string();
  info.offsetMaxPath = ( dgpuPath / "ctgp_offset_max" ).string();
  info.tgpBasePath = ( dgpuPath / "tgp_base" ).string();
  info.currentOffset = readInt32( offsetPath ).value_or( 0 );
  info.maxOffset = readInt32( info.offsetMaxPath ).value_or( 0 );
  info.tgpBase = readInt32( info.tgpBasePath ).value_or( 0 );

  return info;
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
