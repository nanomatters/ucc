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
#include <fstream>
#include <sstream>
#include <optional>
#include <filesystem>

/**
 * @brief Template class for reading/writing sysfs files with type safety
 *
 * Provides type-safe access to sysfs files with automatic parsing and formatting.
 * Supports reading and writing primitive types, strings, and vectors.
 *
 * Supported types:
 * - bool (read/write as 0/1)
 * - int32_t
 * - int64_t
 * - std::string
 * - std::vector<int32_t> (with range support like "0-7")
 * - std::vector<std::string>
 *
 * @tparam T The type of data stored in the sysfs file
 */
template< typename T >
class SysfsNode
{
public:
  explicit SysfsNode( const std::string &path, const std::string &delimiter = "" )
    : m_path( path )
    , m_delimiter( delimiter )
  {}

  /**
   * @brief Check if the sysfs node exists and is accessible
   */
  [[nodiscard]] bool isAvailable() const noexcept
  {
    return std::filesystem::exists( m_path );
  }

  /**
   * @brief Read value from sysfs node
   * @return Optional containing the value, or nullopt if read failed
   */
  [[nodiscard]] std::optional< T > read() const noexcept
  {
    try
    {
      std::ifstream file( m_path );

      if ( not file.is_open() )
        return std::nullopt;

      return readImpl( file );
    }
    catch ( ... )
    {
      return std::nullopt;
    }
  }

  /**
   * @brief Write value to sysfs node
   * @param value The value to write
   * @return true if write succeeded, false otherwise
   */
  bool write( const T &value ) noexcept
  {
    try
    {
      std::ofstream file( m_path );

      if ( not file.is_open() )
        return false;

      return writeImpl( file, value );
    }
    catch ( ... )
    {
      return false;
    }
  }

private:
  std::string m_path;
  std::string m_delimiter;

  // Specialization for bool
  [[nodiscard]] std::optional< T > readImpl( std::ifstream &file ) const
    requires std::is_same_v< T, bool >
  {
    int value;
    file >> value;

    if ( file.fail() )
      return std::nullopt;

    return value != 0;
  }

  bool writeImpl( std::ofstream &file, const T &value ) const
    requires std::is_same_v< T, bool >
  {
    file << ( value ? 1 : 0 );
    return not file.fail();
  }

  // Specialization for int32_t
  [[nodiscard]] std::optional< T > readImpl( std::ifstream &file ) const
    requires std::is_same_v< T, int32_t >
  {
    int32_t value;
    file >> value;

    if ( file.fail() )
      return std::nullopt;

    return value;
  }

  bool writeImpl( std::ofstream &file, const T &value ) const
    requires std::is_same_v< T, int32_t >
  {
    file << value;
    return not file.fail();
  }

  // Specialization for int64_t
  [[nodiscard]] std::optional< T > readImpl( std::ifstream &file ) const
    requires std::is_same_v< T, int64_t >
  {
    int64_t value;
    file >> value;

    if ( file.fail() )
      return std::nullopt;

    return value;
  }

  bool writeImpl( std::ofstream &file, const T &value ) const
    requires std::is_same_v< T, int64_t >
  {
    file << value;
    return not file.fail();
  }

  // Specialization for std::string
  [[nodiscard]] std::optional< T > readImpl( std::ifstream &file ) const
    requires std::is_same_v< T, std::string >
  {
    std::string value;
    std::getline( file, value );

    if ( file.fail() )
      return std::nullopt;

    // Trim whitespace
    if ( not value.empty() and value.back() == '\n' )
      value.pop_back();

    if ( not value.empty() and value.back() == '\r' )
      value.pop_back();

    return value;
  }

  bool writeImpl( std::ofstream &file, const T &value ) const
    requires std::is_same_v< T, std::string >
  {
    file << value;
    return not file.fail();
  }

  // Specialization for std::vector<int32_t>
  [[nodiscard]] std::optional< T > readImpl( std::ifstream &file ) const
    requires std::is_same_v< T, std::vector< int32_t > >
  {
    std::string line;
    std::getline( file, line );

    if ( file.fail() or line.empty() )
      return std::nullopt;

    std::vector< int32_t > result;
    std::istringstream iss( line );

    // Handle range format (e.g., "0-7" or "0,2,4-7")
    std::string token;
    const char delim = m_delimiter.empty() ? ',' : m_delimiter[ 0 ];

    while ( std::getline( iss, token, delim ) )
    {
      // Trim whitespace
      token.erase( 0, token.find_first_not_of( " \t\n\r" ) );
      token.erase( token.find_last_not_of( " \t\n\r" ) + 1 );

      if ( token.empty() )
        continue;

      // Check for range (e.g., "0-7")
      size_t dashPos = token.find( '-' );

      if ( dashPos != std::string::npos )
      {
        int32_t start = std::stoi( token.substr( 0, dashPos ) );
        int32_t end = std::stoi( token.substr( dashPos + 1 ) );

        for ( int32_t i = start; i <= end; ++i )
          result.push_back( i );
      }
      else
      {
        result.push_back( std::stoi( token ) );
      }
    }

    return result;
  }

  bool writeImpl( std::ofstream &file, const T &value ) const
    requires std::is_same_v< T, std::vector< int32_t > >
  {
    const char delim = m_delimiter.empty() ? ',' : m_delimiter[ 0 ];

    for ( size_t i = 0; i < value.size(); ++i )
    {
      if ( i > 0 )
        file << delim;

      file << value[ i ];
    }

    return not file.fail();
  }

  // Specialization for std::vector<std::string>
  [[nodiscard]] std::optional< T > readImpl( std::ifstream &file ) const
    requires std::is_same_v< T, std::vector< std::string > >
  {
    std::string line;
    std::getline( file, line );

    if ( file.fail() or line.empty() )
      return std::nullopt;

    std::vector< std::string > result;
    std::istringstream iss( line );
    const char delim = m_delimiter.empty() ? ' ' : m_delimiter[ 0 ];

    std::string token;

    while ( std::getline( iss, token, delim ) )
    {
      // Trim whitespace
      token.erase( 0, token.find_first_not_of( " \t\n\r" ) );
      token.erase( token.find_last_not_of( " \t\n\r" ) + 1 );

      if ( not token.empty() )
        result.push_back( token );
    }

    return result;
  }

  bool writeImpl( std::ofstream &file, const T &value ) const
    requires std::is_same_v< T, std::vector< std::string > >
  {
    const char delim = m_delimiter.empty() ? ' ' : m_delimiter[ 0 ];

    for ( size_t i = 0; i < value.size(); ++i )
    {
      if ( i > 0 )
        file << delim;

      file << value[ i ];
    }

    return not file.fail();
  }
};
