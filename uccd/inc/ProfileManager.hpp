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
    std::vector< ucc::hal::FanCurvePoint > curve;
    try
    {
      auto arr = nlohmann::json::parse( json );
      if ( !arr.is_array() ) return curve;
      for ( const auto &item : arr )
      {
        ucc::hal::FanCurvePoint pt;
        pt.temp = item.value( "temp", 0 );
        pt.speed = item.value( "speed", 0 );
        curve.push_back( pt );
      }
    }
    catch ( ... ) { /* malformed — return empty */ }
    return curve;
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
    try
    {
      auto j = nlohmann::json::parse( json );
      fp.id   = j.value( "id", "" );
      fp.name = j.value( "name", "" );

      if ( j.contains( "zones" ) && j["zones"].is_array() )
      {
        for ( const auto &zj : j["zones"] )
        {
          ucc::hal::FanZoneCurve zc;
          zc.zoneId         = zj.value( "id", "" );
          zc.name           = zj.value( "name", "" );
          zc.deviceType     = ucc::hal::fanDeviceTypeFromString( zj.value( "deviceType", "fan" ) );
          zc.hysteresisDeg  = zj.value( "hysteresisDeg", 3 );
          zc.enabled        = zj.value( "enabled", true );
          zc.thermalSourceId = zj.value( "thermalSourceId", "" );

          if ( zj.contains( "fanIds" ) && zj["fanIds"].is_array() )
            for ( const auto &fid : zj["fanIds"] )
              if ( fid.is_string() ) zc.fanIds.push_back( fid.get< std::string >() );

          if ( zj.contains( "curve" ) && zj["curve"].is_array() )
            for ( const auto &pt : zj["curve"] )
              zc.curve.push_back( { pt.value( "temp", 0 ), pt.value( "speed", 0 ) } );

          if ( !zc.zoneId.empty() )
            fp.zoneCurves.push_back( std::move( zc ) );
        }
      }

      if ( j.contains( "thermalSources" ) && j["thermalSources"].is_array() )
      {
        for ( const auto &item : j["thermalSources"] )
        {
          if ( !item.is_object() ) continue;
          ucc::hal::ThermalSource ts;
          ts.id       = item.value( "id", "" );
          ts.label    = item.value( "label", "" );
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
    catch ( ... ) { /* malformed — return partial */ }
    return fp;
  }

  /**
   * @brief Parse a single profile from JSON object
   * @param json JSON object string
   * @return Parsed profile
   */
  [[nodiscard]] static UccProfile parseProfileJSON( const std::string &json )
  {
    UccProfile profile;
    try
    {
      auto j = nlohmann::json::parse( json );

      profile.id          = j.value( "id", "" );
      profile.name        = j.value( "name", "" );
      profile.description = j.value( "description", "" );

      if ( auto it = j.find( "display" ); it != j.end() && it->is_object() )
      {
        const auto &d = *it;
        profile.display.brightness    = d.value( "brightness", 100 );
        profile.display.useBrightness = d.value( "useBrightness", false );
        profile.display.refreshRate   = d.value( "refreshRate", -1 );
        profile.display.useRefRate    = d.value( "useRefRate", false );
        profile.display.xResolution   = d.value( "xResolution", -1 );
        profile.display.yResolution   = d.value( "yResolution", -1 );
        profile.display.useResolution = d.value( "useResolution", false );
      }

      if ( auto it = j.find( "cpu" ); it != j.end() && it->is_object() )
      {
        const auto &c = *it;
        if ( int32_t v = c.value( "onlineCores", -1 ); v >= 0 )
          profile.cpu.onlineCores = v;
        if ( int32_t v = c.value( "scalingMinFrequency", -1 ); v >= 0 )
          profile.cpu.scalingMinFrequency = v;
        if ( int32_t v = c.value( "scalingMaxFrequency", -1 ); v >= 0 )
          profile.cpu.scalingMaxFrequency = v;
        profile.cpu.governor = c.value( "governor", "" );
        profile.cpu.energyPerformancePreference = c.value( "energyPerformancePreference", "" );
        profile.cpu.noTurbo = c.value( "noTurbo", false );
      }

      if ( auto it = j.find( "webcam" ); it != j.end() && it->is_object() )
      {
        profile.webcam.status    = it->value( "status", true );
        profile.webcam.useStatus = it->value( "useStatus", true );
      }

      if ( auto it = j.find( "fan" ); it != j.end() && it->is_object() )
      {
        profile.fan.useControl      = it->value( "useControl", true );
        profile.fan.fanProfile      = it->value( "fanProfile", "fan-balanced" );
        profile.fan.autoControlWC   = it->value( "autoControlWC", true );
        profile.fan.enableWaterCooler = it->value( "enableWaterCooler", ucc::WATER_COOLER_INITIAL_STATE );
        std::cout << "[ProfileManager] Parsed profile '" << profile.name << "'" << std::endl;
      }

      if ( auto it = j.find( "odmProfile" ); it != j.end() && it->is_object() )
      {
        if ( std::string name = it->value( "name", "" ); !name.empty() )
          profile.odmProfile.name = name;
      }

      if ( auto it = j.find( "odmPowerLimits" ); it != j.end() && it->is_object() )
      {
        if ( it->contains( "tdpValues" ) && (*it)["tdpValues"].is_array() )
          for ( const auto &v : (*it)["tdpValues"] )
            if ( v.is_number_integer() ) profile.odmPowerLimits.tdpValues.push_back( v.get< int >() );
      }

      profile.keyboard.keyboardProfileId = j.value( "selectedKeyboardProfile", "" );
      profile.gpuProfileId      = j.value( "gpuProfileId", "" );
      profile.chargingProfile   = j.value( "chargingProfile", "" );
      profile.chargingPriority  = j.value( "chargingPriority", "" );
      profile.chargeType        = j.value( "chargeType", "" );
      profile.chargeStartThreshold = j.value( "chargeStartThreshold", -1 );
      profile.chargeEndThreshold   = j.value( "chargeEndThreshold", -1 );
    }
    catch ( ... ) { /* malformed — return partial */ }
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
    try
    {
      auto arr = nlohmann::json::parse( json );
      if ( !arr.is_array() ) return profiles;
      for ( const auto &elem : arr )
      {
        auto profile = parseProfileJSON( elem.dump() );
        if ( !profile.id.empty() )
          profiles.push_back( std::move( profile ) );
      }
    }
    catch ( ... ) { /* malformed */ }
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
   * @brief Serialize profiles to JSON array
   */
  [[nodiscard]] static std::string profilesToJSON( const std::vector< UccProfile > &profiles )
  {
    nlohmann::json arr = nlohmann::json::array();
    for ( const auto &p : profiles )
    {
      arr.push_back( nlohmann::json::parse( profileToJSON( p ) ) );
    }
    return arr.dump();
  }

  // --- Public serialization utilities ---
public:
  [[nodiscard]] static std::string profileToJSON( const UccProfile &profile )
  {
    nlohmann::json j;
    j["id"]          = profile.id;
    j["name"]        = profile.name;
    j["description"] = profile.description;

    j["display"] = {
      { "brightness",    profile.display.brightness },
      { "useBrightness", profile.display.useBrightness },
      { "refreshRate",   profile.display.refreshRate },
      { "useRefRate",    profile.display.useRefRate },
      { "xResolution",   profile.display.xResolution },
      { "yResolution",   profile.display.yResolution },
      { "useResolution", profile.display.useResolution },
    };

    j["cpu"] = {
      { "onlineCores",              profile.cpu.onlineCores.value_or( -1 ) },
      { "scalingMinFrequency",      profile.cpu.scalingMinFrequency.value_or( -1 ) },
      { "scalingMaxFrequency",      profile.cpu.scalingMaxFrequency.value_or( -1 ) },
      { "governor",                 profile.cpu.governor },
      { "energyPerformancePreference", profile.cpu.energyPerformancePreference },
      { "noTurbo",                  profile.cpu.noTurbo },
    };

    j["webcam"] = {
      { "status",    profile.webcam.status },
      { "useStatus", profile.webcam.useStatus },
    };

    j["fan"] = {
      { "useControl",       profile.fan.useControl },
      { "fanProfile",       profile.fan.fanProfile },
      { "autoControlWC",    profile.fan.autoControlWC },
      { "enableWaterCooler", profile.fan.enableWaterCooler },
    };

    j["odmProfile"]    = {{ "name", profile.odmProfile.name.value_or( "" ) }};
    j["odmPowerLimits"] = {{ "tdpValues", profile.odmPowerLimits.tdpValues }};

    if ( !profile.gpuProfileId.empty() )
      j["gpuProfileId"] = profile.gpuProfileId;
    if ( !profile.keyboard.keyboardProfileId.empty() )
      j["selectedKeyboardProfile"] = profile.keyboard.keyboardProfileId;
    if ( !profile.chargingProfile.empty() )
      j["chargingProfile"] = profile.chargingProfile;
    if ( !profile.chargingPriority.empty() )
      j["chargingPriority"] = profile.chargingPriority;
    if ( !profile.chargeType.empty() )
      j["chargeType"] = profile.chargeType;
    if ( profile.chargeStartThreshold >= 0 )
      j["chargeStartThreshold"] = profile.chargeStartThreshold;
    if ( profile.chargeEndThreshold >= 0 )
      j["chargeEndThreshold"] = profile.chargeEndThreshold;

    return j.dump();
  }

  /**
   * @brief Serialize fan profile to JSON
   */


  /**
   * @brief Serialize fan curve to JSON
   */
  [[nodiscard]] static std::string fanCurveToJSON( const std::vector< ucc::hal::FanCurvePoint > &curve )
  {
    nlohmann::json arr = nlohmann::json::array();
    for ( const auto &pt : curve )
      arr.push_back( {{ "temp", pt.temp }, { "speed", pt.speed }} );
    return arr.dump();
  }

};
