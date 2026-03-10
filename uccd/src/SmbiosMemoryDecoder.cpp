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

#include "SmbiosMemoryDecoder.hpp"

#include <cstdint>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

namespace
{

std::string trim( const std::string &s )
{
  const auto start = s.find_first_not_of( " \t\r\n" );
  if ( start == std::string::npos )
    return {};
  return s.substr( start, s.find_last_not_of( " \t\r\n" ) - start + 1 );
}

uint16_t readLe16( const std::vector< uint8_t > &buf, size_t off )
{
  if ( off + 1 >= buf.size() )
    return 0;
  return static_cast< uint16_t >(
      static_cast< uint16_t >( buf[off] ) |
      static_cast< uint16_t >( static_cast< uint16_t >( buf[off + 1] ) << 8 ) );
}

uint32_t readLe32( const std::vector< uint8_t > &buf, size_t off )
{
  if ( off + 3 >= buf.size() )
    return 0;
  return static_cast< uint32_t >( buf[off] ) |
         ( static_cast< uint32_t >( buf[off + 1] ) << 8 ) |
         ( static_cast< uint32_t >( buf[off + 2] ) << 16 ) |
         ( static_cast< uint32_t >( buf[off + 3] ) << 24 );
}

std::string readSmbiosString( const uint8_t *strStart, size_t strLen, uint8_t index )
{
  if ( index == 0 || !strStart || strLen == 0 )
    return {};

  uint8_t currentIndex = 1;
  size_t pos = 0;
  while ( pos < strLen )
  {
    size_t len = 0;
    while ( pos + len < strLen && strStart[pos + len] != 0 )
      ++len;
    if ( len == 0 )
      break;
    if ( currentIndex == index )
      return trim( std::string( reinterpret_cast< const char * >( strStart + pos ), len ) );
    pos += len + 1;
    ++currentIndex;
  }

  return {};
}

std::string memoryTypeToString( uint8_t typeCode )
{
  switch ( typeCode )
  {
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x22: return "DDR5";
    default: return {};
  }
}

std::vector< uint8_t > readBinaryFile( const std::string &path )
{
  std::ifstream in( path, std::ios::binary );
  if ( !in.is_open() )
    return {};

  in.seekg( 0, std::ios::end );
  const auto size = in.tellg();
  if ( size <= 0 )
    return {};
  in.seekg( 0, std::ios::beg );

  std::vector< uint8_t > data( static_cast< size_t >( size ) );
  in.read( reinterpret_cast< char * >( data.data() ), static_cast< std::streamsize >( data.size() ) );
  if ( !in )
    return {};
  return data;
}

int decodeSizeMiB( uint16_t sizeField, uint32_t extSizeField )
{
  if ( sizeField == 0 || sizeField == 0xFFFF )
    return 0;

  if ( sizeField == 0x7FFF )
    return static_cast< int >( extSizeField );

  if ( ( sizeField & 0x8000u ) != 0 )
  {
    // Bit 15 set -> units are KiB
    const uint32_t kib = static_cast< uint32_t >( sizeField & 0x7FFFu );
    return static_cast< int >( kib / 1024u );
  }

  return static_cast< int >( sizeField );
}

} // namespace

std::vector< MemoryModuleInfo > detectMemoryModulesFromSmbios()
{
  std::vector< MemoryModuleInfo > modules;

  const auto dmi = readBinaryFile( "/sys/firmware/dmi/tables/DMI" );
  if ( dmi.empty() )
    return modules;

  size_t off = 0;
  while ( off + 4 <= dmi.size() )
  {
    const uint8_t type = dmi[off];
    const uint8_t len = dmi[off + 1];
    if ( len < 4 || off + len > dmi.size() )
      break;

    size_t strStart = off + len;
    size_t end = strStart;
    while ( end + 1 < dmi.size() )
    {
      if ( dmi[end] == 0 && dmi[end + 1] == 0 )
      {
        end += 2;
        break;
      }
      ++end;
    }

    if ( end > dmi.size() )
      break;

    if ( type == 17 )
    {
      MemoryModuleInfo mod{};

      const uint16_t sizeField = readLe16( dmi, off + 0x0C );
      const uint32_t extSizeField = ( len >= 0x20 ) ? readLe32( dmi, off + 0x1C ) : 0;
      mod.sizeMiB = decodeSizeMiB( sizeField, extSizeField );

      if ( mod.sizeMiB > 0 )
      {
        const uint8_t locatorIndex = ( len > 0x10 ) ? dmi[off + 0x10] : 0;
        const uint8_t bankIndex = ( len > 0x11 ) ? dmi[off + 0x11] : 0;
        const uint8_t typeCode = ( len > 0x12 ) ? dmi[off + 0x12] : 0;

        mod.locator = readSmbiosString( dmi.data() + strStart, end - strStart, locatorIndex );
        mod.bankLocator = readSmbiosString( dmi.data() + strStart, end - strStart, bankIndex );
        mod.type = memoryTypeToString( typeCode );

        // SMBIOS 2.3+: Speed at offset 0x15
        mod.maxSpeedMTs = ( len >= 0x17 ) ? static_cast< int >( readLe16( dmi, off + 0x15 ) ) : 0;
        // SMBIOS 2.7+: Configured speed at offset 0x20
        mod.configuredSpeedMTs = ( len >= 0x22 ) ? static_cast< int >( readLe16( dmi, off + 0x20 ) ) : 0;
        if ( mod.configuredSpeedMTs <= 0 )
          mod.configuredSpeedMTs = mod.maxSpeedMTs;

        // SMBIOS 3.2+: Configured voltage at offset 0x26 (millivolts)
        mod.configuredVoltageMv = ( len >= 0x28 ) ? static_cast< int >( readLe16( dmi, off + 0x26 ) ) : 0;

        modules.push_back( mod );
      }
    }

    off = end;
  }

  return modules;
}
