/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ucc::uniwill
{
namespace fs = std::filesystem;

inline constexpr uint32_t TDP_MIN_WATTS = 25;

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
