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

#include <fstream>
#include <string>

namespace ucc
{

struct MemoryUsage
{
  int totalMiB = 0;
  int usedMiB = 0;
  int availableMiB = 0;

  [[nodiscard]] bool isAvailable() const noexcept
  {
    return totalMiB > 0;
  }
};

[[nodiscard]] inline long long parseMeminfoValueKiB( const std::string &line )
{
  long long value = 0;
  bool found = false;
  for ( const char c : line )
  {
    if ( c >= '0' and c <= '9' )
    {
      value = value * 10 + ( c - '0' );
      found = true;
    }
    else if ( found )
    {
      break;
    }
  }
  return found ? value : 0;
}

[[nodiscard]] inline MemoryUsage readMemoryUsage( const std::string &meminfoPath = "/proc/meminfo" )
{
  std::ifstream meminfo( meminfoPath );
  if ( not meminfo.is_open() )
    return {};

  long long totalKiB = 0;
  long long availableKiB = 0;
  std::string line;
  while ( std::getline( meminfo, line ) )
  {
    if ( line.rfind( "MemTotal:", 0 ) == 0 )
      totalKiB = parseMeminfoValueKiB( line );
    else if ( line.rfind( "MemAvailable:", 0 ) == 0 )
      availableKiB = parseMeminfoValueKiB( line );
  }

  if ( totalKiB <= 0 )
    return {};

  if ( availableKiB < 0 )
    availableKiB = 0;
  if ( availableKiB > totalKiB )
    availableKiB = totalKiB;

  MemoryUsage usage;
  usage.totalMiB = static_cast< int >( totalKiB / 1024 );
  usage.availableMiB = static_cast< int >( availableKiB / 1024 );
  usage.usedMiB = usage.totalMiB - usage.availableMiB;
  return usage;
}

} // namespace ucc
