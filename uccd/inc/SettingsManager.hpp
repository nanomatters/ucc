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

#include "TccSettings.hpp"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iostream>
#include <optional>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>

class SettingsManager
{
public:
  static constexpr const char *SETTINGS_FILE = "/etc/ucc/settings";
  
  [[nodiscard]] std::optional< TccSettings > readSettings() const noexcept
  {
    try
    {
      namespace fs = std::filesystem;
      
      if ( !fs::exists( SETTINGS_FILE ) )
      {
        std::cout << "[Settings] Settings file not found, using defaults" << std::endl;
        return std::nullopt;
      }
      
      std::ifstream file( SETTINGS_FILE );
      if ( !file )
      {
        std::cerr << "[Settings] Failed to open settings file" << std::endl;
        return std::nullopt;
      }
      
      std::string content( ( std::istreambuf_iterator< char >( file ) ),
                           std::istreambuf_iterator< char >() );
      
      auto result = parseSettingsJSONInternal( content );
      
      // FIX #5: File recovery backup on read failure
      if ( !result.has_value() )
      {
        std::cerr << "[Settings] Failed to parse settings file, attempting recovery" << std::endl;
        
        // Check if backup exists
        std::string backupFile = std::string(SETTINGS_FILE) + ".backup";
        if ( fs::exists( backupFile ) )
        {
          std::cout << "[Settings] Backup file found, attempting to restore" << std::endl;
          std::ifstream backupIfs( backupFile );
          if ( backupIfs )
          {
            std::string backupContent( ( std::istreambuf_iterator< char >( backupIfs ) ),
                                       std::istreambuf_iterator< char >() );
            auto backupResult = parseSettingsJSONInternal( backupContent );
            
            if ( backupResult.has_value() )
            {
              std::cout << "[Settings] Successfully recovered settings from backup" << std::endl;
              
              // Create dated backup of corrupted file for investigation
              auto now = std::time(nullptr);
              auto tm = *std::localtime(&now);
              std::ostringstream datetimeOss;
              datetimeOss << std::put_time(&tm, "%Y%m%d_%H%M%S");
              std::string corruptedBackup = std::string(SETTINGS_FILE) + ".corrupted_" + datetimeOss.str();
              
              try
              {
                fs::copy_file( SETTINGS_FILE, corruptedBackup );
                std::cout << "[Settings] Saved corrupted settings as: " << corruptedBackup << std::endl;
              }
              catch ( const std::exception &e )
              {
                std::cerr << "[Settings] Failed to backup corrupted file: " << e.what() << std::endl;
              }
              
              return backupResult;
            }
            else
            {
              std::cerr << "[Settings] Backup file is also corrupted, cannot recover" << std::endl;
            }
          }
        }
        else
        {
          std::cout << "[Settings] No backup file available for recovery" << std::endl;
        }
      }
      
      return result;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[Settings] Exception reading settings: " << e.what() << std::endl;
      return std::nullopt;
    }
  }
  
  [[nodiscard]] bool writeSettings( const TccSettings &settings ) const noexcept
  {
    try
    {
      namespace fs = std::filesystem;
      
      // Create directory if needed
      fs::create_directories( fs::path( SETTINGS_FILE ).parent_path() );
      
      // Serialize settings to JSON
      std::string json = settingsToJSON( settings );
      
      // FIX #5: Create backup before writing new settings
      if ( fs::exists( SETTINGS_FILE ) )
      {
        try
        {
          std::string backupFile = std::string(SETTINGS_FILE) + ".backup";
          fs::copy_file( SETTINGS_FILE, backupFile, fs::copy_options::overwrite_existing );
          std::cout << "[Settings] Backed up current settings to: " << backupFile << std::endl;
        }
        catch ( const std::exception &e )
        {
          std::cerr << "[Settings] Warning: Failed to create backup: " << e.what() << std::endl;
          // Continue with write even if backup fails
        }
      }
      
      // Write to file
      std::ofstream file( SETTINGS_FILE );
      if ( !file )
      {
        std::cerr << "[Settings] Failed to create settings file" << std::endl;
        return false;
      }
      
      file << json;
      
      // Set permissions
      fs::permissions( SETTINGS_FILE,
                      fs::perms::owner_read | fs::perms::owner_write |
                      fs::perms::group_read | fs::perms::others_read );
      
      std::cout << "[Settings] Settings written successfully" << std::endl;
      return true;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[Settings] Exception writing settings: " << e.what() << std::endl;
      return false;
    }
  }

  // public wrapper for parsing settings JSON (used by --new_settings)
  [[nodiscard]] std::optional< TccSettings > parseSettingsJSON( const std::string &json ) const noexcept
  {
    return parseSettingsJSONInternal( json );
  }
  
private:
  [[nodiscard]] std::optional< TccSettings > parseSettingsJSONInternal( const std::string &json ) const noexcept
  {
    try
    {
      auto j = nlohmann::json::parse(json);
      TccSettings settings;
      
      // Parse stateMap
      if (j.contains("stateMap")) {
        auto& stateMap = j["stateMap"];
        if (stateMap.contains("power_ac")) settings.stateMap["power_ac"] = stateMap["power_ac"];
        if (stateMap.contains("power_bat")) settings.stateMap["power_bat"] = stateMap["power_bat"];
        if (stateMap.contains("power_wc")) settings.stateMap["power_wc"] = stateMap["power_wc"];
      }
      
      // Parse profiles map
      if (j.contains("profiles")) {
        auto& profiles = j["profiles"];
        for (auto& [key, value] : profiles.items()) {
          settings.profiles[key] = value.dump();
        }
      }
      
      // Parse boolean settings
      if (j.contains("fahrenheit")) settings.fahrenheit = j["fahrenheit"];
      if (j.contains("cpuSettingsEnabled")) settings.cpuSettingsEnabled = j["cpuSettingsEnabled"];
      if (j.contains("fanControlEnabled")) settings.fanControlEnabled = j["fanControlEnabled"];
      if (j.contains("keyboardBacklightControlEnabled")) settings.keyboardBacklightControlEnabled = j["keyboardBacklightControlEnabled"];
      
      // Parse optional string fields
      if (j.contains("shutdownTime") && j["shutdownTime"].is_string()) settings.shutdownTime = j["shutdownTime"];
      if (j.contains("chargingProfile") && j["chargingProfile"].is_string()) settings.chargingProfile = j["chargingProfile"];
      if (j.contains("chargingPriority") && j["chargingPriority"].is_string()) settings.chargingPriority = j["chargingPriority"];
      
      // Parse ycbcr420Workaround array
      if (j.contains("ycbcr420Workaround")) {
        auto& ycbcr = j["ycbcr420Workaround"];
        for (size_t i = 0; i < ycbcr.size() && i < settings.ycbcr420Workaround.size(); ++i) {
          auto& card = settings.ycbcr420Workaround[i];
          for (auto& [port, enabled] : ycbcr[i].items()) {
            card.ports.push_back({port, enabled});
          }
        }
      }
      
      return settings;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[Settings] Exception parsing settings JSON: " << e.what() << std::endl;
      return std::nullopt;
    }
  }
  
  [[nodiscard]] std::string settingsToJSON( const TccSettings &settings ) const noexcept
  {
    std::ostringstream json;
    
    json << "{\n";
    json << "  \"fahrenheit\": " << ( settings.fahrenheit ? "true" : "false" ) << ",\n";
    json << "  \"stateMap\": {\n";
    {
      size_t stateCount = 0;
      for ( const auto &[key, val] : settings.stateMap )
      {
        if ( stateCount > 0 ) json << ",\n";
        json << "    \"" << key << "\": \"" << val << "\"";
        stateCount++;
      }
      if ( stateCount > 0 ) json << "\n";
    }
    json << "  },\n";
    
    // Serialize profiles map
    json << "  \"profiles\": {\n";
    size_t profileCount = 0;
    for ( const auto &[profileId, profileJson] : settings.profiles )
    {
      if ( profileCount > 0 ) json << ",\n";
      json << "    \"" << profileId << "\": " << profileJson;
      profileCount++;
    }
    json << "\n  },\n";
    json << "  \"shutdownTime\": " << ( settings.shutdownTime.has_value() ? "\"" + settings.shutdownTime.value() + "\"" : "null" ) << ",\n";
    json << "  \"cpuSettingsEnabled\": " << ( settings.cpuSettingsEnabled ? "true" : "false" ) << ",\n";
    json << "  \"fanControlEnabled\": " << ( settings.fanControlEnabled ? "true" : "false" ) << ",\n";
    json << "  \"keyboardBacklightControlEnabled\": " << ( settings.keyboardBacklightControlEnabled ? "true" : "false" ) << ",\n";
    
    // Serialize ycbcr420Workaround array
    json << "  \"ycbcr420Workaround\": [";
    for ( size_t cardIdx = 0; cardIdx < settings.ycbcr420Workaround.size(); ++cardIdx )
    {
      if ( cardIdx > 0 ) json << ", ";
      json << "{";
      const auto &card = settings.ycbcr420Workaround[cardIdx];
      for ( size_t portIdx = 0; portIdx < card.ports.size(); ++portIdx )
      {
        if ( portIdx > 0 ) json << ", ";
        const auto &port = card.ports[portIdx];
        json << "\"" << port.port << "\": " << ( port.enabled ? "true" : "false" );
      }
      json << "}";
    }
    json << "],\n";
    
    json << "  \"chargingProfile\": " << ( settings.chargingProfile.has_value() ? "\"" + settings.chargingProfile.value() + "\"" : "null" ) << ",\n";
    json << "  \"chargingPriority\": " << ( settings.chargingPriority.has_value() ? "\"" + settings.chargingPriority.value() + "\"" : "null" ) << ",\n";
    json << "  \"keyboardBacklightStates\": []\n";
    json << "}\n";
    
    return json.str();
  }
};
