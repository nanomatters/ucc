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
#include <unordered_map>
#include <cstdint>

#include "hal/FanZone.hpp"

/**
 * @brief Fan profile — a named collection of per-zone fan curves.
 *
 * Each zone curve maps a zone ID to a temp→speed% curve.  The full zone
 * topology (fanIds, device type, name) lives in HardwareManager; the
 * profile only supplies curves, hysteresis, enable flags, and optional
 * thermal-source overrides.
 *
 * Built-in profiles (Silent, Quiet, Balanced, Cool, Freezy) define
 * curves for the standard zone IDs produced by auto-zone:
 *   zone-cpu, zone-gpu, zone-case, zone-pump, zone-misc, wc-fan, wc-pump.
 *
 * Custom profiles are created by the user and may contain an arbitrary
 * set of zone curves matching the hardware they were created on.
 */
class FanProfile
{
public:
  std::string id;
  std::string name;
  std::vector< ucc::hal::FanZoneCurve > zoneCurves;

  FanProfile() = default;

  FanProfile( const std::string &profileId,
              const std::string &profileName )
    : id( profileId ),
      name( profileName )
  {
  }

  FanProfile( const std::string &profileId,
              const std::string &profileName,
              std::vector< ucc::hal::FanZoneCurve > zc )
    : id( profileId ),
      name( profileName ),
      zoneCurves( std::move( zc ) )
  {
  }

  /**
   * @brief Check if profile has at least one zone curve
   */
  [[nodiscard]] bool isValid() const noexcept
  {
    return !zoneCurves.empty();
  }

  /**
   * @brief Find a zone curve by zone ID
   * @return Pointer to the zone curve or nullptr
   */
  [[nodiscard]] const ucc::hal::FanZoneCurve *findZoneCurve( const std::string &zoneId ) const noexcept
  {
    for ( const auto &zc : zoneCurves )
      if ( zc.zoneId == zoneId )
        return &zc;
    return nullptr;
  }

  /**
   * @brief Find a mutable zone curve by zone ID
   * @return Pointer to the zone curve or nullptr
   */
  [[nodiscard]] ucc::hal::FanZoneCurve *findZoneCurve( const std::string &zoneId ) noexcept
  {
    for ( auto &zc : zoneCurves )
      if ( zc.zoneId == zoneId )
        return &zc;
    return nullptr;
  }

  /**
   * @brief Get interpolated fan speed for a zone at a given temperature
   * @return Speed 0–100, or -1 if zone not found or curve empty
   */
  [[nodiscard]] int32_t getSpeedForZone( int32_t temp,
                                         const std::string &zoneId ) const noexcept
  {
    const auto *zc = findZoneCurve( zoneId );
    if ( !zc )
      return -1;
    return ucc::hal::interpolateCurve( zc->curve, temp );
  }
};

// Stable built-in fan profile IDs
namespace DefaultFanProfileIDs
{
  inline constexpr const char *Silent   = "fan-silent";
  inline constexpr const char *Quiet    = "fan-quiet";
  inline constexpr const char *Balanced = "fan-balanced";
  inline constexpr const char *Cool     = "fan-cool";
  inline constexpr const char *Freezy   = "fan-freezy";
}

// Well-known zone IDs used by built-in profiles and auto-zone
namespace WellKnownZoneIDs
{
  inline constexpr const char *CPU     = "zone-cpu";
  inline constexpr const char *GPU     = "zone-gpu";
  inline constexpr const char *Case    = "zone-case";
  inline constexpr const char *Pump    = "zone-pump";
  inline constexpr const char *Misc    = "zone-misc";
  inline constexpr const char *WCFan   = "wc-fan";
  inline constexpr const char *WCPump  = "wc-pump";
}

// default fan profile presets
extern const std::vector< FanProfile > defaultFanProfiles;

/// Serialise a built-in fan profile to JSON.
/// @param hwDeviceTypes  Optional map of zoneId → device-type string from
///                      HardwareManager so that the JSON reflects the actual
///                      hardware rather than hard-coded defaults.
std::string getFanProfileJson( const std::string &idOrName,
                               const std::unordered_map< std::string, std::string > &hwDeviceTypes = {} );
bool setFanProfileJson( const std::string &idOrName, const std::string &json );

/// Return a FanProfile by ID or name from the built-in presets.
/// Tries ID first, then falls back to name match, then Balanced, then first profile.
FanProfile getDefaultFanProfile( const std::string &idOrName );
