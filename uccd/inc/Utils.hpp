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

#include <string>
#include <vector>
#include <filesystem>
#include <cstdio>

namespace TccUtils
{

/**
 * @brief Execute a shell command and capture output
 * @param command Command to execute
 * @return Command output or empty string on error
 */
[[nodiscard]] inline std::string executeCommand( const std::string &command ) noexcept
{
  try
  {
    FILE *pipe = popen( command.c_str(), "r" );
    if ( not pipe )
      return "";

    std::string result;
    char buffer[ 128 ];
    while ( fgets( buffer, sizeof( buffer ), pipe ) != nullptr )
    {
      result += buffer;
    }

    pclose( pipe );
    return result;
  }
  catch ( const std::exception & )
  {
    return "";
  }
}

/**
 * @brief Get list of device names in a directory
 * @param sourceDir Directory to list
 * @return Vector of device/file names
 */
[[nodiscard]] inline std::vector< std::string > getDeviceList( const std::string &sourceDir ) noexcept
{
  std::vector< std::string > devices;
  
  try
  {
    if ( !std::filesystem::exists( sourceDir ) || !std::filesystem::is_directory( sourceDir ) )
    {
      return devices;
    }

    for ( const auto &entry : std::filesystem::directory_iterator( sourceDir ) )
    {
      if ( entry.is_directory() || entry.is_symlink() || entry.is_regular_file() )
      {
        devices.push_back( entry.path().filename().string() );
      }
    }
  }
  catch ( const std::exception & )
  {
    // Return empty vector on error
  }
  
  return devices;
}

} // namespace TccUtils
