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

#pragma once

#include "SysfsNode.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief A physical non-rotational block device.
 */
struct StorageDeviceInfo
{
  std::string name;        // e.g. "nvme0n1"
  std::string model;       // e.g. "Samsung SSD 990 PRO"
  std::string vendor;      // if exposed separately by the kernel
  std::uint64_t sizeBytes = 0;
};

struct StorageTemperature
{
  std::string name;        // block device name, e.g. "nvme0n1"
  int temperatureCelsius = 0;
  std::string inputPath;
};

namespace storage_detail
{
namespace fs = std::filesystem;
using Sysfs = SysfsNode< std::string >;

[[nodiscard]] inline fs::path sysfsPath( const std::string &sysfsRoot, const fs::path &relative )
{
  const fs::path root = sysfsRoot.empty() ? fs::path( "/sys" ) : fs::path( sysfsRoot );
  return root / relative;
}

[[nodiscard]] inline std::string trim( const std::string &s )
{
  const auto start = s.find_first_not_of( " \t\r\n" );
  if ( start == std::string::npos )
    return {};
  return s.substr( start, s.find_last_not_of( " \t\r\n" ) - start + 1 );
}

[[nodiscard]] inline std::optional< std::string > readFirstLine( const fs::path &path )
{
  auto value = Sysfs( path ).read();
  if ( !value )
    return std::nullopt;

  return trim( *value );
}

[[nodiscard]] inline std::string collapseWhitespace( const std::string &input )
{
  std::ostringstream oss;
  bool previousWasSpace = false;
  bool hasOutput = false;

  for ( const unsigned char ch : input )
  {
    if ( std::isspace( ch ) )
    {
      previousWasSpace = true;
      continue;
    }

    if ( previousWasSpace && hasOutput )
      oss << ' ';
    oss << static_cast< char >( ch );
    hasOutput = true;
    previousWasSpace = false;
  }

  return oss.str();
}

[[nodiscard]] inline bool hasPrefix( std::string_view value, std::string_view prefix )
{
  return value.starts_with( prefix );
}

[[nodiscard]] inline bool containsCaseInsensitive( std::string_view value, std::string_view needle )
{
  std::string haystack( value );
  std::string wanted( needle );
  std::ranges::transform( haystack, haystack.begin(), []( unsigned char c ) {
    return static_cast< char >( std::tolower( c ) );
  } );
  std::ranges::transform( wanted, wanted.begin(), []( unsigned char c ) {
    return static_cast< char >( std::tolower( c ) );
  } );
  return haystack.find( wanted ) != std::string::npos;
}

[[nodiscard]] inline bool isIgnoredBlockDeviceName( const std::string &name )
{
  for ( const std::string_view prefix : { "loop", "ram", "zram", "fd", "sr", "dm-", "md" } )
  {
    if ( hasPrefix( name, prefix ) )
      return true;
  }
  return false;
}

[[nodiscard]] inline bool pathIsAtOrBelow( const fs::path &path, const fs::path &parent )
{
  const std::string childString = path.lexically_normal().string();
  const std::string parentString = parent.lexically_normal().string();
  return childString == parentString ||
         ( childString.starts_with( parentString ) &&
           childString.size() > parentString.size() &&
           childString[ parentString.size() ] == '/' );
}

[[nodiscard]] inline std::optional< int > readPreferredTemperature( const fs::path &hwmonPath,
                                                                    fs::path &inputPath )
{
  std::optional< int > firstTemperature;
  fs::path firstInputPath;

  for ( int index = 1; index <= 16; ++index )
  {
    const fs::path candidateInput = hwmonPath / ( "temp" + std::to_string( index ) + "_input" );
    const auto millidegrees = SysfsNode< int64_t >( candidateInput ).read();
    if ( !millidegrees )
      continue;

    if ( !firstTemperature )
    {
      firstTemperature = static_cast< int >( std::lround( static_cast< double >( *millidegrees ) / 1000.0 ) );
      firstInputPath = candidateInput;
    }

    const auto label = readFirstLine( hwmonPath / ( "temp" + std::to_string( index ) + "_label" ) );
    if ( label && containsCaseInsensitive( *label, "composite" ) )
    {
      inputPath = candidateInput;
      return static_cast< int >( std::lround( static_cast< double >( *millidegrees ) / 1000.0 ) );
    }
  }

  if ( firstTemperature )
    inputPath = firstInputPath;
  return firstTemperature;
}

} // namespace storage_detail

[[nodiscard]] inline std::vector< StorageDeviceInfo > detectStorageDevices(
  const std::string &sysfsRoot = "/sys" )
{
  namespace fs = std::filesystem;
  std::vector< StorageDeviceInfo > devices;
  const fs::path blockBase = storage_detail::sysfsPath( sysfsRoot, "block" );

  if ( !storage_detail::Sysfs::exists( blockBase ) )
    return devices;

  try
  {
    for ( const fs::path &blockPath : storage_detail::Sysfs::directoryEntries( blockBase ) )
    {
      const std::string name = blockPath.filename().string();
      if ( storage_detail::isIgnoredBlockDeviceName( name ) )
        continue;

      const std::uint64_t sizeSectors = SysfsNode< std::uint64_t >( blockPath / "size" ).read().value_or( 0 );
      if ( sizeSectors == 0 )
        continue;

      const std::uint64_t removable = SysfsNode< std::uint64_t >( blockPath / "removable" ).read().value_or( 0 );
      if ( removable != 0 )
        continue;

      const std::uint64_t rotational = SysfsNode< std::uint64_t >( blockPath / "queue/rotational" ).read().value_or( 1 );
      if ( rotational != 0 )
        continue;

      const fs::path devicePath = blockPath / "device";

      StorageDeviceInfo device;
      device.name = name;
      device.model = storage_detail::collapseWhitespace(
        storage_detail::readFirstLine( devicePath / "model" ).value_or( "" ) );
      device.vendor = storage_detail::collapseWhitespace(
        storage_detail::readFirstLine( devicePath / "vendor" ).value_or( "" ) );
      device.sizeBytes = sizeSectors * 512ULL;

      devices.push_back( std::move( device ) );
    }
  }
  catch ( ... )
  {
  }

  std::sort( devices.begin(), devices.end(), []( const auto &a, const auto &b ) {
    return a.name < b.name;
  } );

  return devices;
}

[[nodiscard]] inline std::vector< StorageTemperature > readStorageTemperatures(
  const std::string &sysfsRoot = "/sys" )
{
  namespace fs = std::filesystem;
  std::vector< StorageTemperature > temperatures;

  struct DevicePath
  {
    std::string name;
    fs::path path;
  };

  std::vector< DevicePath > devicePaths;
  for ( const auto &device : detectStorageDevices( sysfsRoot ) )
  {
    const fs::path devicePath = storage_detail::sysfsPath( sysfsRoot, fs::path( "block" ) / device.name / "device" );
    if ( auto canonical = storage_detail::Sysfs::canonicalPath( devicePath ) )
      devicePaths.push_back( { device.name, *canonical } );
  }

  if ( devicePaths.empty() )
    return temperatures;

  const fs::path hwmonBase = storage_detail::sysfsPath( sysfsRoot, "class/hwmon" );
  if ( !storage_detail::Sysfs::exists( hwmonBase ) )
    return temperatures;

  try
  {
    for ( const fs::path &hwmonPath : storage_detail::Sysfs::directoryEntries( hwmonBase ) )
    {
      const auto hwmonCanonical = storage_detail::Sysfs::canonicalPath( hwmonPath );
      if ( !hwmonCanonical )
        continue;

      auto match = std::ranges::find_if( devicePaths, [&]( const DevicePath &device ) {
        return storage_detail::pathIsAtOrBelow( *hwmonCanonical, device.path );
      } );
      if ( match == devicePaths.end() )
        continue;

      fs::path inputPath;
      const auto temperature = storage_detail::readPreferredTemperature( hwmonPath, inputPath );
      if ( !temperature )
        continue;

      temperatures.push_back( StorageTemperature{
        match->name,
        *temperature,
        inputPath.string()
      } );
    }
  }
  catch ( ... )
  {
  }

  std::sort( temperatures.begin(), temperatures.end(), []( const auto &a, const auto &b ) {
    return a.name < b.name;
  } );

  return temperatures;
}
