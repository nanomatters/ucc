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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <sys/stat.h>

/**
 * @brief Autosave data structure
 * Matches TypeScript ITccAutosave interface
 */
struct TccAutosave
{
  int32_t displayBrightness;

  TccAutosave()
    : displayBrightness( 100 )
  {
  }
};

/**
 * @brief Manages autosave data persistence
 * 
 * Handles reading/writing autosave data (display brightness, etc.)
 * Mirrors TypeScript ConfigHandler autosave functionality.
 */
class AutosaveManager
{
public:
  AutosaveManager()
    : m_autosavePath( "/etc/ucc/autosave" ),
      m_autosaveFileMod( 0644 )
  {
  }

  explicit AutosaveManager( const std::string &autosavePath )
    : m_autosavePath( autosavePath ),
      m_autosaveFileMod( 0644 )
  {
  }

  /**
   * @brief Read autosave data from disk
   * @return Autosave data, or default if file doesn't exist
   */
  [[nodiscard]] TccAutosave readAutosave() noexcept
  {
    try
    {
      std::ifstream file( m_autosavePath );
      if ( !file.is_open() )
      {
        return TccAutosave(); // Return defaults
      }

      std::stringstream buffer;
      buffer << file.rdbuf();
      std::string content = buffer.str();

      return parseAutosaveJSON( content );
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[Autosave] Error reading autosave: " << e.what() << std::endl;
      return TccAutosave();
    }
  }

  /**
   * @brief Write autosave data to disk
   * @param autosave Autosave data to write
   * @return true on success, false on error
   */
  [[nodiscard]] bool writeAutosave( const TccAutosave &autosave ) noexcept
  {
    try
    {
      // Create directory if it doesn't exist
      std::filesystem::path path( m_autosavePath );
      std::filesystem::path dir = path.parent_path();
      
      if ( !dir.empty() && !std::filesystem::exists( dir ) )
      {
        std::filesystem::create_directories( dir );
      }

      // Serialize to JSON
      std::string json = autosaveToJSON( autosave );

      // Write to file
      std::ofstream file( m_autosavePath, std::ios::trunc );
      if ( !file.is_open() )
      {
        return false;
      }

      file << json;
      file.close();

      // Set file permissions
      chmod( m_autosavePath.c_str(), m_autosaveFileMod );

      return true;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[Autosave] Error writing autosave: " << e.what() << std::endl;
      return false;
    }
  }

  [[nodiscard]] const std::string &getAutosavePath() const noexcept { return m_autosavePath; }

private:
  std::string m_autosavePath;
  mode_t m_autosaveFileMod;

  /**
   * @brief Parse autosave data from JSON
   */
  [[nodiscard]] static TccAutosave parseAutosaveJSON( const std::string &json ) noexcept
  {
    TccAutosave autosave;

    // Simple JSON parsing for displayBrightness
    size_t pos = json.find( "\"displayBrightness\"" );
    if ( pos != std::string::npos )
    {
      pos = json.find( ':', pos );
      if ( pos != std::string::npos )
      {
        size_t start = pos + 1;
        while ( start < json.length() && std::isspace( json[ start ] ) )
        {
          ++start;
        }
        
        size_t end = start;
        while ( end < json.length() && ( std::isdigit( json[ end ] ) || json[ end ] == '-' ) )
        {
          ++end;
        }
        
        if ( end > start )
        {
          try
          {
            autosave.displayBrightness = std::stoi( json.substr( start, end - start ) );
          }
          catch ( ... )
          {
            // Use default
          }
        }
      }
    }

    return autosave;
  }

  /**
   * @brief Convert autosave data to JSON
   */
  [[nodiscard]] static std::string autosaveToJSON( const TccAutosave &autosave ) noexcept
  {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"displayBrightness\": " << autosave.displayBrightness << "\n"
        << "}";
    return oss.str();
  }
};
