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

#include "profiles/UccProfile.hpp"
#include "profiles/DefaultProfiles.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <algorithm>
#include <cstdlib>
#include <sys/stat.h>
#include <cstring>
#include <iostream>

/**
 * @brief Manages TCC profile loading, saving, and manipulation
 * 
 * Handles reading/writing profiles from/to JSON files, profile validation,
 * and merging with default profiles. Mirrors TypeScript ConfigHandler functionality.
 */
class ProfileManager
{
public:
  /**
   * @brief Construct profile manager with default paths
   */
  ProfileManager() = default;

  /**
   * @brief Get default custom profiles (template for new profiles)
   * @return Vector containing the default custom profile
   */
  [[nodiscard]] std::vector< UccProfile > getDefaultCustomProfiles() const noexcept
  {
    std::vector< UccProfile > profiles;
    profiles.push_back( getDefaultCustomProfile() );
    return profiles;
  }

  /**
   * @brief Get all default (hardcoded) profiles for a specific device
   * @param device Optional device identifier for device-specific profiles
   * @return Vector of default profiles
   */
  [[nodiscard]] std::vector< UccProfile > getDefaultProfiles( std::optional< UniwillDeviceID > device = std::nullopt ) const noexcept
  {
    std::vector< UccProfile > result;
    
    // If device is specified, check for device-specific profiles
    if ( device.has_value() )
    {
      if ( auto it = deviceProfiles.find( *device ); it != deviceProfiles.end() )
        return it->second;
    }
    
    // Fallback to legacy profiles only for unknown/undetected devices
    result = legacyProfiles;
    syslog( LOG_INFO, "Device not found. Loading %zu legacy default profiles", result.size() );
    return result;
  }

  /**
   * @brief Get custom profiles, returning defaults on error
   * @return Vector of custom profiles
   */
  [[nodiscard]] std::vector< UccProfile > getCustomProfilesNoThrow() noexcept
  { return getDefaultCustomProfiles(); }

  /**
   * @brief Get all profiles (default + custom)
   * @return Vector containing all profiles
   */
  [[nodiscard]] std::vector< UccProfile > getAllProfiles() noexcept
  {
    auto defaultProfiles = getDefaultProfiles();
    auto customProfiles = getCustomProfilesNoThrow();
    
    defaultProfiles.insert( defaultProfiles.end(), 
                            customProfiles.begin(), 
                            customProfiles.end() );
    return defaultProfiles;
  }

  /**
   * @brief Find profile by ID
   * @param profiles Vector to search
   * @param id Profile ID to find
   * @return Pointer to profile if found, nullptr otherwise
   */
  [[nodiscard]] static const UccProfile *findProfileById( 
    const std::vector< UccProfile > &profiles, 
    const std::string &id ) noexcept
  {
    for ( const auto &profile : profiles )
    {
      if ( profile.id == id )
      {
        return &profile;
      }
    }
    return nullptr;
  }

  /**
   * @brief Find profile by name
   * @param profiles Vector to search
   * @param name Profile name to find
   * @return Pointer to profile if found, nullptr otherwise
   */
  [[nodiscard]] static const UccProfile *findProfileByName( 
    const std::vector< UccProfile > &profiles, 
    const std::string &name ) noexcept
  {
    for ( const auto &profile : profiles )
    {
      if ( profile.name == name )
      {
        return &profile;
      }
    }
    return nullptr;
  }

  /**
   * @brief Public wrapper to parse a fan table JSON array into entries
   * @param json JSON array string containing fan table entries
   * @return Vector of FanTableEntry parsed from JSON
   */
  [[nodiscard]] static std::vector< FanTableEntry > parseFanTableFromJSON( const std::string &json )
  {
    return parseFanTable( json );
  }

  /**
   * @brief Parse a single profile from JSON object
   * @param json JSON object string
   * @return Parsed profile
   */
  [[nodiscard]] static UccProfile parseProfileJSON( const std::string &json )
  {
    UccProfile profile;
    
    profile.id = extractString( json, "id" );
    profile.name = extractString( json, "name" );
    profile.description = extractString( json, "description" );

    // Parse display settings
    std::string displayJson = extractObject( json, "display" );
    if ( !displayJson.empty() )
    {
      profile.display.brightness = extractInt( displayJson, "brightness", 100 );
      profile.display.useBrightness = extractBool( displayJson, "useBrightness", false );
      profile.display.refreshRate = extractInt( displayJson, "refreshRate", -1 );
      profile.display.useRefRate = extractBool( displayJson, "useRefRate", false );
      profile.display.xResolution = extractInt( displayJson, "xResolution", -1 );
      profile.display.yResolution = extractInt( displayJson, "yResolution", -1 );
      profile.display.useResolution = extractBool( displayJson, "useResolution", false );
    }

    // Parse CPU settings
    std::string cpuJson = extractObject( json, "cpu" );
    if ( !cpuJson.empty() )
    {
      int32_t onlineCores = extractInt( cpuJson, "onlineCores", -1 );
      if ( onlineCores >= 0 )
      {
        profile.cpu.onlineCores = onlineCores;
      }
      
      int32_t scalingMin = extractInt( cpuJson, "scalingMinFrequency", -1 );
      if ( scalingMin >= 0 )
      {
        profile.cpu.scalingMinFrequency = scalingMin;
      }
      
      int32_t scalingMax = extractInt( cpuJson, "scalingMaxFrequency", -1 );
      if ( scalingMax >= 0 )
      {
        profile.cpu.scalingMaxFrequency = scalingMax;
      }
      
      profile.cpu.governor = extractString( cpuJson, "governor", "powersave" );
      profile.cpu.energyPerformancePreference = extractString( cpuJson, "energyPerformancePreference", "balance_performance" );
      profile.cpu.noTurbo = extractBool( cpuJson, "noTurbo", false );
    }

    // Parse webcam settings
    std::string webcamJson = extractObject( json, "webcam" );
    if ( !webcamJson.empty() )
    {
      profile.webcam.status = extractBool( webcamJson, "status", true );
      profile.webcam.useStatus = extractBool( webcamJson, "useStatus", true );
    }

    // Parse fan settings
    std::string fanJson = extractObject( json, "fan" );
    if ( !fanJson.empty() )
    {
      profile.fan.useControl = extractBool( fanJson, "useControl", true );
      profile.fan.fanProfile = extractString( fanJson, "fanProfile", "fan-balanced" );
      profile.fan.offsetFanspeed = extractInt( fanJson, "offsetFanspeed", 0 );
      profile.fan.sameSpeed = extractBool( fanJson, "sameSpeed", true );
      profile.fan.autoControlWC = extractBool( fanJson, "autoControlWC", true );
      
      // Debug: log the parsed fan offset and sameSpeed
      std::cout << "[ProfileManager] Parsed profile '" << profile.name 
                << "' offsetFanspeed: " << profile.fan.offsetFanspeed
                << " sameSpeed: " << ( profile.fan.sameSpeed ? "true" : "false" ) << std::endl;
      
      // Parse embedded fan tables if present (GUI embeds full fan curves in custom profiles)
      std::string tableCPUJson = extractArray( fanJson, "tableCPU" );
      if ( !tableCPUJson.empty() )
        profile.fan.tableCPU = parseFanTable( tableCPUJson );

      std::string tableGPUJson = extractArray( fanJson, "tableGPU" );
      if ( !tableGPUJson.empty() )
        profile.fan.tableGPU = parseFanTable( tableGPUJson );

      std::string tablePumpJson = extractArray( fanJson, "tablePump" );
      if ( !tablePumpJson.empty() )
        profile.fan.tablePump = parseFanTable( tablePumpJson );

      std::string tableWCFanJson = extractArray( fanJson, "tableWaterCoolerFan" );
      if ( !tableWCFanJson.empty() )
        profile.fan.tableWaterCoolerFan = parseFanTable( tableWCFanJson );

      if ( profile.fan.hasEmbeddedTables() )
      {
        std::cout << "[ProfileManager] Profile '" << profile.name 
                  << "' has embedded fan tables: CPU=" << profile.fan.tableCPU.size()
                  << " GPU=" << profile.fan.tableGPU.size()
                  << " Pump=" << profile.fan.tablePump.size()
                  << " WCFan=" << profile.fan.tableWaterCoolerFan.size() << std::endl;
      }
    }

    // Parse ODM profile
    std::string odmProfileJson = extractObject( json, "odmProfile" );
    if ( !odmProfileJson.empty() )
    {
      std::string odmName = extractString( odmProfileJson, "name" );
      if ( !odmName.empty() )
      {
        profile.odmProfile.name = odmName;
      }
    }

    // Parse ODM power limits
    std::string odmPowerJson = extractObject( json, "odmPowerLimits" );
    if ( !odmPowerJson.empty() )
    {
      profile.odmPowerLimits.tdpValues = extractIntArray( odmPowerJson, "tdpValues" );
    }

    // Parse NVIDIA power control
    std::string nvidiaJson = extractObject( json, "nvidiaPowerCTRLProfile" );
    if ( !nvidiaJson.empty() )
    {
      TccNVIDIAPowerCTRLProfile nvidiaProfile;
      nvidiaProfile.cTGPOffset = extractInt( nvidiaJson, "cTGPOffset", 0 );
      profile.nvidiaPowerCTRLProfile = nvidiaProfile;
    }

    // Parse keyboard settings
    std::string keyboardJson = extractObject( json, "keyboard" );
    if ( !keyboardJson.empty() )
    {
      profile.keyboard.keyboardProfileData = keyboardJson;
      profile.keyboard.keyboardProfileName = extractString( keyboardJson, "keyboardProfileName", "" );
    }

    // Fallback: also check top-level "selectedKeyboardProfile" (written by GUI/DBus serialization)
    if ( profile.keyboard.keyboardProfileName.empty() )
    {
      std::string topLevelKeyboardProfile = extractString( json, "selectedKeyboardProfile", "" );
      if ( !topLevelKeyboardProfile.empty() )
        profile.keyboard.keyboardProfileName = topLevelKeyboardProfile;
    }

    // Parse charging profile (firmware-level charging mode stored per-profile)
    profile.chargingProfile = extractString( json, "chargingProfile", "" );
    profile.chargingPriority = extractString( json, "chargingPriority", "" );
    profile.chargeType = extractString( json, "chargeType", "" );
    profile.chargeStartThreshold = extractInt( json, "chargeStartThreshold", -1 );
    profile.chargeEndThreshold = extractInt( json, "chargeEndThreshold", -1 );

    return profile;
  }

  /**
   * @brief Get the default custom profile template
   */
  [[nodiscard]] static UccProfile getDefaultCustomProfile() noexcept
  {
    UccProfile profile;
    profile.id = "__default_custom_profile__";
    profile.name = "TUXEDO Defaults";
    profile.description = "Edit profile to change behaviour";
    
    // All fields already have sensible defaults from constructors
    return profile;
  }

  /**
   * @brief Parse JSON array of profiles
   * @param json JSON string containing profile array
   * @return Vector of parsed profiles
   */
  [[nodiscard]] static std::vector< UccProfile > parseProfilesJSON( const std::string &json )
  {
    std::vector< UccProfile > profiles;
    
    // Simple JSON array parser
    size_t pos = json.find( '[' );
    if ( pos == std::string::npos )
    {
      return profiles;
    }

    size_t depth = 0;
    size_t start = pos + 1;
    
    for ( size_t i = pos; i < json.length(); ++i )
    {
      char c = json[ i ];
      
      if ( c == '{' )
      {
        if ( depth == 0 )
        {
          start = i;
        }
        ++depth;
      }
      else if ( c == '}' )
      {
        --depth;
        if ( depth == 0 )
        {
          std::string profileJson = json.substr( start, i - start + 1 );
          auto profile = parseProfileJSON( profileJson );
          if ( !profile.id.empty() )
          {
            profiles.push_back( profile );
          }
        }
      }
    }

    return profiles;
  }

private:
  /**
   * @brief Fill missing fields in a profile from defaults (matches TypeScript recursivelyFillObject)
   * @param profile Profile to fill
   * @param defaultProfile Default profile template to fill from
   * @return true if any field was filled
   */
  [[nodiscard]] static bool fillMissingFields( UccProfile &profile, const UccProfile &defaultProfile ) noexcept
  {
    bool modified = false;
    
    // note: In C++ with our struct design, we use special sentinel values to indicate "undefined"
    // for optionals, we check has_value(). For ints, we use -1. For strings, empty string.

    // fill description if missing
    if ( profile.description.empty() and not defaultProfile.description.empty() )
    {
      profile.description = defaultProfile.description;
      modified = true;
    }

    // fill display fields
    // note: we don't fill these because they have valid defaults (brightness=100, refreshRate=-1, etc)
    // only fill if they were explicitly undefined, which in JSON would be null or missing
    
    // fill CPU fields
    if ( not profile.cpu.onlineCores.has_value() and defaultProfile.cpu.onlineCores.has_value() )
    {
      profile.cpu.onlineCores = defaultProfile.cpu.onlineCores;
      modified = true;
    }
    
    if ( not profile.cpu.scalingMinFrequency.has_value() and defaultProfile.cpu.scalingMinFrequency.has_value() )
    {
      profile.cpu.scalingMinFrequency = defaultProfile.cpu.scalingMinFrequency;
      modified = true;
    }
    
    if ( not profile.cpu.scalingMaxFrequency.has_value() and defaultProfile.cpu.scalingMaxFrequency.has_value() )
    {
      profile.cpu.scalingMaxFrequency = defaultProfile.cpu.scalingMaxFrequency;
      modified = true;
    }
    
    if ( profile.cpu.governor.empty() and not defaultProfile.cpu.governor.empty() )
    {
      profile.cpu.governor = defaultProfile.cpu.governor;
      modified = true;
    }
    
    if ( profile.cpu.energyPerformancePreference.empty() and not defaultProfile.cpu.energyPerformancePreference.empty() )
    {
      profile.cpu.energyPerformancePreference = defaultProfile.cpu.energyPerformancePreference;
      modified = true;
    }
    
    // fill fan profile if missing
    if ( profile.fan.fanProfile.empty() and not defaultProfile.fan.fanProfile.empty() )
    {
      profile.fan.fanProfile = defaultProfile.fan.fanProfile;
      modified = true;
    }

    // Fill embedded fan tables from default if profile has none
    if ( profile.fan.tableCPU.empty() and not defaultProfile.fan.tableCPU.empty() )
    {
      profile.fan.tableCPU = defaultProfile.fan.tableCPU;
      modified = true;
    }
    if ( profile.fan.tableGPU.empty() and not defaultProfile.fan.tableGPU.empty() )
    {
      profile.fan.tableGPU = defaultProfile.fan.tableGPU;
      modified = true;
    }
    if ( profile.fan.tablePump.empty() and not defaultProfile.fan.tablePump.empty() )
    {
      profile.fan.tablePump = defaultProfile.fan.tablePump;
      modified = true;
    }
    if ( profile.fan.tableWaterCoolerFan.empty() and not defaultProfile.fan.tableWaterCoolerFan.empty() )
    {
      profile.fan.tableWaterCoolerFan = defaultProfile.fan.tableWaterCoolerFan;
      modified = true;
    }
    
    // fill ODM profile name
    if ( not profile.odmProfile.name.has_value() and defaultProfile.odmProfile.name.has_value() )
    {
      profile.odmProfile.name = defaultProfile.odmProfile.name;
      modified = true;
    }
    
    return modified;
  }

  // Fan tables can be embedded in profiles or supplied as named presets via GetFanProfile.

  /**
   * @brief Parse fan table from JSON array
   */
  [[nodiscard]] static std::vector< FanTableEntry > parseFanTable( const std::string &json )
  {
    std::vector< FanTableEntry > table;
    
    size_t depth = 0;
    size_t start = 0;
    
    for ( size_t i = 0; i < json.length(); ++i )
    {
      char c = json[ i ];
      
      if ( c == '{' )
      {
        if ( depth == 0 )
        {
          start = i;
        }
        ++depth;
      }
      else if ( c == '}' )
      {
        --depth;
        if ( depth == 0 )
        {
          std::string entryJson = json.substr( start, i - start + 1 );
          FanTableEntry entry;
          entry.temp = extractInt( entryJson, "temp", 0 );
          entry.speed = extractInt( entryJson, "speed", 0 );
          table.push_back( entry );
        }
      }
    }
    
    return table;
  }

  /**
   * @brief Serialize profiles to JSON array
   */
  [[nodiscard]] static std::string profilesToJSON( const std::vector< UccProfile > &profiles )
  {
    std::ostringstream oss;
    oss << "[";
    
    for ( size_t i = 0; i < profiles.size(); ++i )
    {
      if ( i > 0 )
      {
        oss << ",";
      }
      oss << profileToJSON( profiles[ i ] );
    }
    
    oss << "]";
    return oss.str();
  }

  // --- Public serialization utilities ---
public:
  /**
   * @brief Serialize single profile to JSON (complete format for file storage)
   */
  [[nodiscard]] static std::string profileToJSON( const UccProfile &profile )
  {
    std::ostringstream oss;
    
    oss << "{"
        << "\"id\":\"" << jsonEscape( profile.id ) << "\","
        << "\"name\":\"" << jsonEscape( profile.name ) << "\","
        << "\"description\":\"" << jsonEscape( profile.description ) << "\","
        << "\"display\":{"
        << "\"brightness\":" << profile.display.brightness << ","
        << "\"useBrightness\":" << ( profile.display.useBrightness ? "true" : "false" ) << ","
        << "\"refreshRate\":" << profile.display.refreshRate << ","
        << "\"useRefRate\":" << ( profile.display.useRefRate ? "true" : "false" ) << ","
        << "\"xResolution\":" << profile.display.xResolution << ","
        << "\"yResolution\":" << profile.display.yResolution << ","
        << "\"useResolution\":" << ( profile.display.useResolution ? "true" : "false" )
        << "},"
        << "\"cpu\":{"
        << "\"onlineCores\":" << ( profile.cpu.onlineCores.has_value() ? std::to_string( *profile.cpu.onlineCores ) : "-1" ) << ","
        << "\"scalingMinFrequency\":" << ( profile.cpu.scalingMinFrequency.has_value() ? std::to_string( *profile.cpu.scalingMinFrequency ) : "-1" ) << ","
        << "\"scalingMaxFrequency\":" << ( profile.cpu.scalingMaxFrequency.has_value() ? std::to_string( *profile.cpu.scalingMaxFrequency ) : "-1" ) << ","
        << "\"governor\":\"" << jsonEscape( profile.cpu.governor ) << "\","
        << "\"energyPerformancePreference\":\"" << jsonEscape( profile.cpu.energyPerformancePreference ) << "\","
        << "\"noTurbo\":" << ( profile.cpu.noTurbo ? "true" : "false" )
        << "},"
        << "\"webcam\":{"
        << "\"status\":" << ( profile.webcam.status ? "true" : "false" ) << ","
        << "\"useStatus\":" << ( profile.webcam.useStatus ? "true" : "false" )
        << "},"
        << "\"fan\":{"
        << "\"useControl\":" << ( profile.fan.useControl ? "true" : "false" ) << ","
        << "\"fanProfile\":\"" << jsonEscape( profile.fan.fanProfile ) << "\","
        << "\"offsetFanspeed\":" << profile.fan.offsetFanspeed << ","
        << "\"sameSpeed\":" << ( profile.fan.sameSpeed ? "true" : "false" ) << ","
        << "\"autoControlWC\":" << ( profile.fan.autoControlWC ? "true" : "false" );

    // Embed fan tables if present
    if ( !profile.fan.tableCPU.empty() )
    {
      oss << ",\"tableCPU\":" << fanTableToJSON( profile.fan.tableCPU );
    }
    if ( !profile.fan.tableGPU.empty() )
    {
      oss << ",\"tableGPU\":" << fanTableToJSON( profile.fan.tableGPU );
    }
    if ( !profile.fan.tablePump.empty() )
    {
      oss << ",\"tablePump\":" << fanTableToJSON( profile.fan.tablePump );
    }
    if ( !profile.fan.tableWaterCoolerFan.empty() )
    {
      oss << ",\"tableWaterCoolerFan\":" << fanTableToJSON( profile.fan.tableWaterCoolerFan );
    }

    oss << "},"
        << "\"odmProfile\":{"
        << "\"name\":\"" << jsonEscape( profile.odmProfile.name.value_or( "" ) ) << "\""
        << "},"
        << "\"odmPowerLimits\":{"
        << "\"tdpValues\":[";
    
    for ( size_t i = 0; i < profile.odmPowerLimits.tdpValues.size(); ++i )
    {
      if ( i > 0 ) oss << ",";
      oss << profile.odmPowerLimits.tdpValues[ i ];
    }
    
    oss << "]},"
        << "\"nvidiaPowerCTRLProfile\":{"
        << "\"cTGPOffset\":" << ( profile.nvidiaPowerCTRLProfile.has_value() ? profile.nvidiaPowerCTRLProfile->cTGPOffset : 0 )
        << "}";

    // Keyboard section
    if ( !profile.keyboard.keyboardProfileData.empty() && profile.keyboard.keyboardProfileData != "{}" )
    {
      oss << ",\"keyboard\":" << profile.keyboard.keyboardProfileData;
    }
    else
    {
      oss << ",\"keyboard\":{}";
    }

    if ( !profile.keyboard.keyboardProfileName.empty() )
    {
      oss << ",\"selectedKeyboardProfile\":\"" << jsonEscape( profile.keyboard.keyboardProfileName ) << "\"";
    }

    // Charging profile (firmware-level charging mode)
    if ( !profile.chargingProfile.empty() )
    {
      oss << ",\"chargingProfile\":\"" << jsonEscape( profile.chargingProfile ) << "\"";
    }
    if ( !profile.chargingPriority.empty() )
    {
      oss << ",\"chargingPriority\":\"" << jsonEscape( profile.chargingPriority ) << "\"";
    }
    if ( !profile.chargeType.empty() )
    {
      oss << ",\"chargeType\":\"" << jsonEscape( profile.chargeType ) << "\"";
    }
    if ( profile.chargeStartThreshold >= 0 )
    {
      oss << ",\"chargeStartThreshold\":" << profile.chargeStartThreshold;
    }
    if ( profile.chargeEndThreshold >= 0 )
    {
      oss << ",\"chargeEndThreshold\":" << profile.chargeEndThreshold;
    }

    oss << "}";
    
    return oss.str();
  }

  /**
   * @brief Serialize fan profile to JSON
   */


  /**
   * @brief Serialize fan table to JSON
   */
  [[nodiscard]] static std::string fanTableToJSON( const std::vector< FanTableEntry > &table )
  {
    std::ostringstream oss;
    oss << "[";
    
    for ( size_t i = 0; i < table.size(); ++i )
    {
      if ( i > 0 ) oss << ",";
      oss << "{"
          << "\"temp\":" << table[ i ].temp << ","
          << "\"speed\":" << table[ i ].speed
          << "}";
    }
    
    oss << "]";
    return oss.str();
  }

private:
  // JSON parsing helper functions
  [[nodiscard]] static std::string extractString( const std::string &json, const std::string &key, const std::string &defaultValue = "" )
  {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find( searchKey );
    if ( pos == std::string::npos )
    {
      return defaultValue;
    }

    pos = json.find( ':', pos );
    if ( pos == std::string::npos )
    {
      return defaultValue;
    }

    pos = json.find( '"', pos );
    if ( pos == std::string::npos )
    {
      return defaultValue;
    }

    size_t end = json.find( '"', pos + 1 );
    if ( end == std::string::npos )
    {
      return defaultValue;
    }

    return json.substr( pos + 1, end - pos - 1 );
  }

  [[nodiscard]] static int32_t extractInt( const std::string &json, const std::string &key, int32_t defaultValue = 0 )
  {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find( searchKey );
    if ( pos == std::string::npos )
    {
      return defaultValue;
    }

    pos = json.find( ':', pos );
    if ( pos == std::string::npos )
    {
      return defaultValue;
    }

    // Skip whitespace
    ++pos;
    while ( pos < json.length() && std::isspace( json[ pos ] ) )
    {
      ++pos;
    }

    if ( pos >= json.length() )
    {
      return defaultValue;
    }

    // Parse number
    size_t end = pos;
    if ( json[ end ] == '-' )
    {
      ++end;
    }
    
    while ( end < json.length() && std::isdigit( json[ end ] ) )
    {
      ++end;
    }

    if ( end == pos || ( end == pos + 1 && json[ pos ] == '-' ) )
    {
      return defaultValue;
    }

    try
    {
      return std::stoi( json.substr( pos, end - pos ) );
    }
    catch ( ... )
    {
      return defaultValue;
    }
  }

  [[nodiscard]] static bool extractBool( const std::string &json, const std::string &key, bool defaultValue = false )
  {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find( searchKey );
    if ( pos == std::string::npos )
    {
      return defaultValue;
    }

    pos = json.find( ':', pos );
    if ( pos == std::string::npos )
    {
      return defaultValue;
    }

    size_t truePos = json.find( "true", pos );
    size_t falsePos = json.find( "false", pos );
    size_t commaPos = json.find( ',', pos );
    size_t bracePos = json.find( '}', pos );

    size_t nextDelimiter = std::min( commaPos, bracePos );

    if ( truePos != std::string::npos && truePos < nextDelimiter )
    {
      return true;
    }
    if ( falsePos != std::string::npos && falsePos < nextDelimiter )
    {
      return false;
    }

    return defaultValue;
  }

  [[nodiscard]] static std::string extractObject( const std::string &json, const std::string &key )
  {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find( searchKey );
    if ( pos == std::string::npos )
    {
      return "";
    }

    pos = json.find( ':', pos );
    if ( pos == std::string::npos )
    {
      return "";
    }

    pos = json.find( '{', pos );
    if ( pos == std::string::npos )
    {
      return "";
    }

    size_t depth = 1;
    size_t start = pos;
    ++pos;

    while ( pos < json.length() && depth > 0 )
    {
      if ( json[ pos ] == '{' )
      {
        ++depth;
      }
      else if ( json[ pos ] == '}' )
      {
        --depth;
      }
      ++pos;
    }

    if ( depth == 0 )
    {
      return json.substr( start, pos - start );
    }

    return "";
  }

  [[nodiscard]] static std::string extractArray( const std::string &json, const std::string &key )
  {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find( searchKey );
    if ( pos == std::string::npos )
    {
      return "";
    }

    pos = json.find( ':', pos );
    if ( pos == std::string::npos )
    {
      return "";
    }

    pos = json.find( '[', pos );
    if ( pos == std::string::npos )
    {
      return "";
    }

    size_t depth = 1;
    size_t start = pos;
    ++pos;

    while ( pos < json.length() && depth > 0 )
    {
      if ( json[ pos ] == '[' )
      {
        ++depth;
      }
      else if ( json[ pos ] == ']' )
      {
        --depth;
      }
      ++pos;
    }

    if ( depth == 0 )
    {
      return json.substr( start, pos - start );
    }

    return "";
  }

  [[nodiscard]] static std::vector< int32_t > extractIntArray( const std::string &json, const std::string &key )
  {
    std::vector< int32_t > result;
    std::string arrayJson = extractArray( json, key );
    
    if ( arrayJson.empty() )
    {
      return result;
    }

    size_t pos = 1; // Skip opening '['
    while ( pos < arrayJson.length() )
    {
      // Skip whitespace and commas
      while ( pos < arrayJson.length() && ( std::isspace( arrayJson[ pos ] ) || arrayJson[ pos ] == ',' ) )
      {
        ++pos;
      }

      if ( pos >= arrayJson.length() || arrayJson[ pos ] == ']' )
      {
        break;
      }

      // Parse number
      size_t start = pos;
      if ( arrayJson[ pos ] == '-' )
      {
        ++pos;
      }
      
      while ( pos < arrayJson.length() && std::isdigit( arrayJson[ pos ] ) )
      {
        ++pos;
      }

      if ( pos > start )
      {
        try
        {
          result.push_back( std::stoi( arrayJson.substr( start, pos - start ) ) );
        }
        catch ( ... )
        {
          // Skip invalid numbers
        }
      }
    }

    return result;
  }

  [[nodiscard]] static std::string jsonEscape( const std::string &value )
  {
    std::ostringstream oss;
    for ( const char c : value )
    {
      switch ( c )
      {
        case '"': oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\b': oss << "\\b"; break;
        case '\f': oss << "\\f"; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default: oss << c; break;
      }
    }
    return oss.str();
  }
};
