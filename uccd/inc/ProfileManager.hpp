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
#include "hal/IProfileProvider.hpp"
#include "CommonTypes.hpp"
#include "StateUtils.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include <algorithm>
#include <cstdlib>
#include <sys/stat.h>
#include <cstring>
#include <iostream>
#include <map>
#include <syslog.h>

/**
 * @brief Manages TCC profile loading, saving, and manipulation
 *
 * Handles reading/writing profiles from/to JSON files, profile validation,
 * and merging with default profiles. Mirrors TypeScript ConfigHandler functionality.
 */
class ProfileManager
{
public:
  ProfileManager() = default;

  /// Set the HAL profile provider (call after HardwareManager::detect()).
  void setProfileProvider( ucc::hal::IProfileProvider *provider ) noexcept
  {
    m_profileProvider = provider;
  }

  [[nodiscard]] std::vector< UccProfile > getDefaultCustomProfiles() const noexcept
  {
    std::vector< UccProfile > profiles;
    profiles.push_back( getDefaultCustomProfile() );
    return profiles;
  }

  [[nodiscard]] std::vector< UccProfile > getDefaultProfiles( std::optional< UniwillDeviceID > device = std::nullopt ) const noexcept
  {
    (void) device;

    // Prefer HAL profile provider when available
    if ( m_profileProvider )
      return m_profileProvider->getDefaultProfiles();

    // Fallback if provider is unavailable.
    UccProfile profile;
    profile.id = DefaultProfileIDs::Office;
    profile.name = "Default";
    profile.description = "Generic fallback profile";
    profile.fan.useControl = true;
    profile.fan.fanProfile = "fan-platform-default";
    return { profile };
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
   * @brief Public wrapper to parse a fan curve JSON array into points
   * @param json JSON array string containing {temp,speed} entries
   * @return Vector of FanCurvePoint parsed from JSON
   */
  [[nodiscard]] static std::vector< ucc::hal::FanCurvePoint > parseFanCurveFromJSON( const std::string &json )
  {
    return parseFanCurve( json );
  }

  /**
   * @brief Parse a fan profile JSON string into a FanProfile struct.
   *
   * Expected format:
   * {
   *   "id": "...",
   *   "name": "...",
   *   "zones": [
   *     { "id": "zone-cpu", "name": "CPU", "deviceType": "Fan",
   *       "hysteresisDeg": 3, "enabled": true,
   *       "curve": [{"temp":20,"speed":0}, ...] },
   *     ...
   *   ]
   * }
   */
  [[nodiscard]] static FanProfile parseFanProfileJSON( const std::string &json )
  {
    FanProfile fp;
    fp.id = extractString( json, "id" );
    fp.name = extractString( json, "name" );

    std::string zonesArrayJson = extractArray( json, "zones" );
    if ( !zonesArrayJson.empty() )
    {
      // Walk zone objects inside the array
      size_t depth = 0;
      size_t start = 0;
      for ( size_t i = 0; i < zonesArrayJson.length(); ++i )
      {
        char c = zonesArrayJson[i];
        if ( c == '{' )
        {
          if ( depth == 0 ) start = i;
          ++depth;
        }
        else if ( c == '}' )
        {
          --depth;
          if ( depth == 0 )
          {
            std::string zoneJson = zonesArrayJson.substr( start, i - start + 1 );
            ucc::hal::FanZoneCurve zc;
            zc.zoneId = extractString( zoneJson, "id" );
            zc.name = extractString( zoneJson, "name" );
            zc.deviceType = ucc::hal::fanDeviceTypeFromString(
              extractString( zoneJson, "deviceType", "fan" ) );
            zc.fanIds = extractStringArray( zoneJson, "fanIds" );
            zc.hysteresisDeg = extractInt( zoneJson, "hysteresisDeg", 3 );
            zc.enabled = extractBool( zoneJson, "enabled", true );
            zc.thermalSourceId = extractString( zoneJson, "thermalSourceId" );

            std::string curveJson = extractArray( zoneJson, "curve" );
            if ( !curveJson.empty() )
              zc.curve = parseFanCurve( curveJson );

            if ( !zc.zoneId.empty() )
              fp.zoneCurves.push_back( std::move( zc ) );
          }
        }
      }
    }

    // Parse custom thermal sources
    std::string tsArrayJson = extractArray( json, "thermalSources" );
    if ( !tsArrayJson.empty() )
    {
      try
      {
        auto arr = nlohmann::json::parse( tsArrayJson );
        if ( arr.is_array() )
        {
          for ( const auto &item : arr )
          {
            if ( !item.is_object() ) continue;
            ucc::hal::ThermalSource ts;
            ts.id    = item.value( "id", "" );
            ts.label = item.value( "label", "" );
            ts.strategy = ucc::hal::thermalStrategyFromString( item.value( "strategy", "single" ) );
            if ( item.contains( "sensorIds" ) && item["sensorIds"].is_array() )
              for ( const auto &s : item["sensorIds"] )
                if ( s.is_string() ) ts.sensorIds.push_back( s.get< std::string >() );
            if ( item.contains( "weights" ) && item["weights"].is_array() )
              for ( const auto &w : item["weights"] )
                if ( w.is_number() ) ts.weights.push_back( w.get< double >() );
            if ( !ts.id.empty() && !ts.sensorIds.empty() )
              fp.thermalSources.push_back( std::move( ts ) );
          }
        }
      }
      catch ( ... ) { /* malformed JSON — ignore */ }
    }

    return fp;
  }

  /**
   * @brief Parse a single profile from JSON object
   * @param json JSON object string
   * @return Parsed profile
   */
  /**
   * @brief Parse profile from JSON string
   * 
   * SECURITY NOTE (VULN-27): This function uses hand-rolled JSON parsing.
   * For better security and maintainability, this should be refactored to use
   * nlohmann::json instead. The library is already a project dependency and
   * provides automatic escaping, proper error handling, and type safety.
  * Refactoring should preserve support for the current profile JSON schema.
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

      profile.cpu.governor = extractString( cpuJson, "governor", "" );
      profile.cpu.energyPerformancePreference = extractString( cpuJson, "energyPerformancePreference", "" );
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
      profile.fan.sameSpeed = extractBool( fanJson, "sameSpeed", true );
      profile.fan.autoControlWC = extractBool( fanJson, "autoControlWC", true );
      profile.fan.enableWaterCooler = extractBool( fanJson, "enableWaterCooler", ucc::WATER_COOLER_INITIAL_STATE );

      // Debug: log the parsed fan settings
      std::cout << "[ProfileManager] Parsed profile '" << profile.name
                << "' sameSpeed: " << ( profile.fan.sameSpeed ? "true" : "false" ) << std::endl;
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

    // Parse keyboard settings — only the ID reference
    std::string keyboardJson = extractObject( json, "keyboard" );
    if ( !keyboardJson.empty() )
    {
      // Legacy: keyboard profile name may be stored inside the keyboard object
      // but we only keep the ID now.
    }

    // Top-level selectedKeyboardProfile is the UUID written by the GUI
    std::string topLevelKeyboardProfile = extractString( json, "selectedKeyboardProfile", "" );
    if ( !topLevelKeyboardProfile.empty() )
    {
      profile.keyboard.keyboardProfileId = topLevelKeyboardProfile;
    }

    // Parse GPU profile reference (ID only — embedded data removed)
    profile.gpuProfileId = extractString( json, "gpuProfileId", "" );

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
  [[nodiscard]] UccProfile getDefaultCustomProfile() const noexcept
  {
    if ( m_profileProvider )
      return m_profileProvider->getDefaultCustomProfile();

    UccProfile profile;
    profile.id = defaultCustomProfileID;
    profile.name = "Defaults";
    profile.description = "Edit profile to change behaviour";
    return profile;
  }

  ucc::hal::IProfileProvider *m_profileProvider = nullptr;

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

  /**
   * @brief Resolve the startup profile for the current power state.
   *
   * Determines power state, looks up the assigned profile ID from the state map,
   * and returns the parsed profile (from saved or built-in sources).
   *
   * @param deviceId  Device identifier for device-specific built-in profiles
   * @param stateMap  Map of power-state string -> profile ID
   * @param savedProfiles  Map of profile ID -> serialized JSON
   * @return The resolved UccProfile, or a default-constructed (empty) profile on failure
   */
  [[nodiscard]] UccProfile resolveStartupProfile(
    std::optional< UniwillDeviceID > deviceId,
    const std::map< std::string, std::string > &stateMap,
    const std::map< std::string, std::string > &savedProfiles ) noexcept
  {
    ProfileState currentState = determineState();
    std::string stateKey = profileStateToString( currentState );

    syslog( LOG_INFO, "[ProfileManager] Current power state: %s", stateKey.c_str() );

    auto stateMapIt = stateMap.find( stateKey );
    if ( stateMapIt == stateMap.end() )
    {
      syslog( LOG_INFO, "[ProfileManager] No profile assigned to state: %s", stateKey.c_str() );
      return {};
    }

    const std::string &profileId = stateMapIt->second;
    syslog( LOG_INFO, "[ProfileManager] State '%s' maps to profile: %s", stateKey.c_str(), profileId.c_str() );

    // Try saved (custom/persistent) profiles first
    if ( auto savedProfileIt = savedProfiles.find( profileId ); savedProfileIt != savedProfiles.end() )
    {
      try
      {
        UccProfile profile = parseProfileJSON( savedProfileIt->second );
        syslog( LOG_INFO, "[ProfileManager] Resolved saved profile: %s (ID: %s)", profile.name.c_str(), profile.id.c_str() );
        return profile;
      }
      catch ( const std::exception &e )
      {
        syslog( LOG_ERR, "[ProfileManager] Failed to parse saved profile '%s': %s", profileId.c_str(), e.what() );
      }
    }

    // Fall back to built-in profiles
    for ( const auto &profile : getDefaultProfiles( deviceId ) )
    {
      if ( profile.id == profileId )
      {
        syslog( LOG_INFO, "[ProfileManager] Resolved built-in profile: %s (ID: %s)", profile.name.c_str(), profile.id.c_str() );
        return profile;
      }
    }

    syslog( LOG_WARNING, "[ProfileManager] Profile '%s' not found in any source", profileId.c_str() );
    return {};
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

    // fill ODM profile name
    if ( not profile.odmProfile.name.has_value() and defaultProfile.odmProfile.name.has_value() )
    {
      profile.odmProfile.name = defaultProfile.odmProfile.name;
      modified = true;
    }

    return modified;
  }

  // Fan curves are embedded in zones or supplied via GetFanProfile.

  /**
   * @brief Parse fan curve from JSON array of {temp,speed} objects
   */
  [[nodiscard]] static std::vector< ucc::hal::FanCurvePoint > parseFanCurve( const std::string &json )
  {
    std::vector< ucc::hal::FanCurvePoint > curve;

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
          ucc::hal::FanCurvePoint pt;
          pt.temp = extractInt( entryJson, "temp", 0 );
          pt.speed = extractInt( entryJson, "speed", 0 );
          curve.push_back( pt );
        }
      }
    }

    return curve;
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
   * 
   * SECURITY NOTE (VULN-27): This function uses string concatenation to build JSON.
   * Modern approach: refactor to use nlohmann::json::object for type-safe serialization
   * with automatic proper escaping and validation. The library is already a dependency.
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
        << "\"sameSpeed\":" << ( profile.fan.sameSpeed ? "true" : "false" ) << ","
        << "\"autoControlWC\":" << ( profile.fan.autoControlWC ? "true" : "false" ) << ","
        << "\"enableWaterCooler\":" << ( profile.fan.enableWaterCooler ? "true" : "false" )
        << "},"
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

    oss << "]}";

    // GPU OC profile — ID reference only
    if ( !profile.gpuProfileId.empty() )
    {
      oss << ",\"gpuProfileId\":\"" << jsonEscape( profile.gpuProfileId ) << "\"";
    }

    // Keyboard — ID reference only
    {
      const std::string &kbRef = profile.keyboard.keyboardProfileId;
      if ( !kbRef.empty() )
        oss << ",\"selectedKeyboardProfile\":\"" << jsonEscape( kbRef ) << "\"";
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
   * @brief Serialize fan curve to JSON
   */
  [[nodiscard]] static std::string fanCurveToJSON( const std::vector< ucc::hal::FanCurvePoint > &curve )
  {
    std::ostringstream oss;
    oss << "[";

    for ( size_t i = 0; i < curve.size(); ++i )
    {
      if ( i > 0 ) oss << ",";
      oss << "{"
          << "\"temp\":" << curve[ i ].temp << ","
          << "\"speed\":" << curve[ i ].speed
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

  [[nodiscard]] static std::vector< std::string > extractStringArray( const std::string &json, const std::string &key )
  {
    std::vector< std::string > result;
    std::string arrayJson = extractArray( json, key );
    if ( arrayJson.empty() )
      return result;

    size_t pos = 1; // Skip '['
    while ( pos < arrayJson.length() )
    {
      while ( pos < arrayJson.length() && ( std::isspace( arrayJson[pos] ) || arrayJson[pos] == ',' ) )
        ++pos;
      if ( pos >= arrayJson.length() || arrayJson[pos] == ']' )
        break;
      if ( arrayJson[pos] == '"' )
      {
        ++pos;
        size_t end = arrayJson.find( '"', pos );
        if ( end == std::string::npos )
          break;
        result.push_back( arrayJson.substr( pos, end - pos ) );
        pos = end + 1;
      }
      else
        ++pos;
    }
    return result;
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
