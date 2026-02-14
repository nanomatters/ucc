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
#include <cstdint>
#include <cmath>
#include <iostream>

#include "../../libucc-dbus/CommonTypes.hpp"

/**
 * @brief Fan table entry
 *
 * Represents a single temperature/speed mapping point in a fan curve.
 */
struct FanTableEntry
{
  int32_t temp;   // temperature in degrees Celsius
  int32_t speed;  // fan speed in percentage (0-100)

  FanTableEntry() : temp( 0 ), speed( 0 ) {}

  FanTableEntry( int32_t t, int32_t s ) : temp( t ), speed( s ) {}
};

/**
 * @brief Fan profile
 *
 * Contains temperature-to-fan-speed mapping tables for CPU, GPU, and pump fans.
 * Mirrors the ITccFanProfile TypeScript interface.
 */
class FanProfile
{
public:
  std::string id;
  std::string name;
  std::vector< FanTableEntry > tableCPU;
  std::vector< FanTableEntry > tableGPU;
  std::vector< FanTableEntry > tablePump;
  std::vector< FanTableEntry > tableWaterCoolerFan;

  FanProfile() = default;

  FanProfile( const std::string &profileId,
              const std::string &profileName )
    : id( profileId ),
      name( profileName )
  {
  }

  FanProfile( const std::string &profileId,
              const std::string &profileName,
              const std::vector< FanTableEntry > &cpu,
              const std::vector< FanTableEntry > &gpu )
    : id( profileId ),
      name( profileName ),
      tableCPU( cpu ),
      tableGPU( gpu )
  {
  }

  FanProfile( const std::string &profileId,
              const std::string &profileName,
              const std::vector< FanTableEntry > &cpu,
              const std::vector< FanTableEntry > &gpu,
              const std::vector< FanTableEntry > &pump )
    : id( profileId ),
      name( profileName ),
      tableCPU( cpu ),
      tableGPU( gpu ),
      tablePump( pump )
  {
  }

  FanProfile( const std::string &profileId,
              const std::string &profileName,
              const std::vector< FanTableEntry > &cpu,
              const std::vector< FanTableEntry > &gpu,
              const std::vector< FanTableEntry > &pump,
              const std::vector< FanTableEntry > &wcFan )
    : id( profileId ),
      name( profileName ),
      tableCPU( cpu ),
      tableGPU( gpu ),
      tablePump( pump ),
      tableWaterCoolerFan( wcFan )
  {
  }

  /**
   * @brief Check if profile has valid data
   * @return true if both CPU and GPU tables are populated
   */
  [[nodiscard]] bool isValid() const noexcept
  {
    return not tableCPU.empty() and not tableGPU.empty();
  }

  /**
   * @brief Get fan speed for a given temperature
   *
   * @param temp Temperature in degrees Celsius
   * @param useCPU true to use CPU table, false for GPU table
   * @return Fan speed percentage (0-100), or -1 if not found
   */
  [[nodiscard]] int32_t getSpeedForTemp( int32_t temp, bool useCPU = true ) const noexcept
  {
    const auto &table = useCPU ? tableCPU : tableGPU;

    if ( table.empty() )
      return -1;

    // If temp is at or below first entry, return first speed
    if ( temp <= table.front().temp )
      return table.front().speed;

    // find exact match or interpolate
    for ( size_t i = 1; i < table.size(); ++i )
    {
      const auto &prev = table[i-1];
      const auto &entry = table[i];

      if ( entry.temp == temp ) return entry.speed;

      if ( temp > prev.temp && temp < entry.temp )
      {
        int32_t tempDiff = entry.temp - prev.temp;
        if ( tempDiff == 0 ) return prev.speed;
        double frac = static_cast<double>( temp - prev.temp ) / static_cast<double>( tempDiff );
        return static_cast<int32_t>( std::lround( prev.speed + frac * ( entry.speed - prev.speed ) ) );
      }
    }

    // temperature is beyond the table, return last speed
    return table.back().speed;
  }

  /**
   * @brief Get water cooler fan speed for a given temperature
   *
   * Uses the water cooler fan table with interpolation, falling back to
   * max(cpu,gpu) if the table is empty.
   *
   * @param temp Temperature in degrees Celsius
   * @return Fan speed percentage (0-100)
   */
  [[nodiscard]] int32_t getWaterCoolerFanSpeedForTemp( int32_t temp ) const noexcept
  {
    if ( tableWaterCoolerFan.empty() )
    {
      // Fallback: use max of CPU and GPU speed
      int32_t cpuSpeed = getSpeedForTemp( temp, true );
      int32_t gpuSpeed = getSpeedForTemp( temp, false );
      return std::max( cpuSpeed, gpuSpeed );
    }

    const auto &table = tableWaterCoolerFan;

    if ( temp <= table.front().temp )
      return table.front().speed;

    for ( size_t i = 1; i < table.size(); ++i )
    {
      const auto &prev = table[i-1];
      const auto &entry = table[i];
      if ( entry.temp == temp ) return entry.speed;
      if ( temp > prev.temp && temp < entry.temp )
      {
        int32_t tempDiff = entry.temp - prev.temp;
        if ( tempDiff == 0 ) return prev.speed;
        double frac = static_cast<double>( temp - prev.temp ) / static_cast<double>( tempDiff );
        return static_cast<int32_t>( std::lround( prev.speed + frac * ( entry.speed - prev.speed ) ) );
      }
    }

    return table.back().speed;
  }

  /**
   * @brief Get pump speed value for a given temperature from tablePump
   *
   * Uses step-wise (floor) lookup rather than interpolation, since pump speed
   * values are discrete voltage levels (0=Off, 1=V7, 2=V8, 3=V11, 4=V12).
   *
   * @param temp Temperature in degrees Celsius
   * @return Pump speed value (0-3), or -1 if pump table is empty
   */
  [[nodiscard]] ucc::PumpVoltage getPumpSpeedForTemp( int32_t temp ) const noexcept
  {
    static constexpr ucc::PumpVoltage pumpSpeedToVoltage[] = { ucc::PumpVoltage::Off, ucc::PumpVoltage::V7, ucc::PumpVoltage::V8,
                                                               ucc::PumpVoltage::V11, ucc::PumpVoltage::V12 };
    ucc::PumpVoltage result = ucc::PumpVoltage::Off;

    for ( const auto &[ entryTemp, speed ] : tablePump )
    {
      // std::cout << "[FanProfile] Checking pump table entry: temp=" << entryTemp << "°C, speed=" << speed << std::endl;
      if ( temp >= entryTemp )
        result = pumpSpeedToVoltage[ std::min( speed, 4 ) ];
      else
        break;
    }

    return result;
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

// default fan profile presets
extern const std::vector< FanProfile > defaultFanProfiles;

std::string getFanProfileJson( const std::string &idOrName );
bool setFanProfileJson( const std::string &idOrName, const std::string &json );

/// Return a FanProfile by ID or name from the built-in presets.
/// Tries ID first, then falls back to name match, then Balanced, then first profile.
FanProfile getDefaultFanProfile( const std::string &idOrName );
