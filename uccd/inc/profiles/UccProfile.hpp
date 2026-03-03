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

#include "FanProfile.hpp"
#include "CommonTypes.hpp"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>

/**
 * @brief GPU OC profile — stores per-pstate clock offsets, locked clocks, and power limit.
 *
 * Mirrors the data surfaced by NvmlWrapper and is serialised as part of
 * the system profile JSON.  Only NVIDIA GPUs via NVML are supported.
 */
struct GpuOCClockOffset
{
  unsigned int pstate = 0;
  int offsetMHz = 0;
};

struct GpuOCLockedClockRange
{
  unsigned int minMHz = 0;
  unsigned int maxMHz = 0;
  bool enabled = false;
};

struct UccProfileGpuOC
{
  std::string gpuProfileId;   ///< UUID reference kept in UccProfile
  std::string gpuProfileName;

  // Per-pstate GPU core offsets
  std::vector< GpuOCClockOffset > gpuCoreOffsets;
  // Per-pstate VRAM offsets
  std::vector< GpuOCClockOffset > vramOffsets;

  // Locked clock ranges (optional)
  GpuOCLockedClockRange gpuLockedClocks;
  GpuOCLockedClockRange vramLockedClocks;

  // Power limit in watts (0 = use default)
  double powerLimitW = 0.0;

  UccProfileGpuOC() = default;
};

/**
 * @brief Display settings for a profile
 */
struct UccProfileDisplay
{
  int32_t brightness;
  bool useBrightness;
  int32_t refreshRate;
  bool useRefRate;
  int32_t xResolution;
  int32_t yResolution;
  bool useResolution;

  UccProfileDisplay()
    : brightness( 100 ),
      useBrightness( false ),
      refreshRate( -1 ),
      useRefRate( false ),
      xResolution( -1 ),
      yResolution( -1 ),
      useResolution( false )
  {
  }
};

/**
 * @brief CPU settings for a profile
 */
struct UccProfileCpu
{
  std::optional< int32_t > onlineCores;
  std::optional< int32_t > scalingMinFrequency;
  std::optional< int32_t > scalingMaxFrequency;
  std::string governor;
  std::string energyPerformancePreference;
  bool noTurbo;

  UccProfileCpu()
    : noTurbo( false )
  {
    // governor and energyPerformancePreference default to empty,
    // meaning "use the system's current value" (read at CpuWorker startup)
  }
};

/**
 * @brief Webcam settings for a profile
 */
struct UccProfileWebcam
{
  bool status;
  bool useStatus;

  UccProfileWebcam()
    : status( true ),
      useStatus( true )
  {
  }
};

/**
 * @brief Fan control settings for a profile
 */
struct UccProfileFanControl
{
  bool useControl;
  std::string fanProfile;
  bool sameSpeed; // when true, all fans are driven at the same percent (highest)
  bool autoControlWC; // when true, automatically control water cooler based on system temperature
  bool enableWaterCooler; // when true, water cooler BLE scanning/connection is enabled

  // Embedded fan curve tables (populated when profile JSON includes them)
  std::vector< FanTableEntry > tableCPU;
  std::vector< FanTableEntry > tableGPU;
  std::vector< FanTableEntry > tablePump;
  std::vector< FanTableEntry > tableWaterCoolerFan;

  UccProfileFanControl()
    : useControl( true ),
      fanProfile( "fan-balanced" ),
      sameSpeed( true ),
      autoControlWC( true ),
      enableWaterCooler( ucc::WATER_COOLER_INITIAL_STATE )
  {
  }

  /**
   * @brief Check if this profile has embedded fan tables
   * @return true if at least CPU and GPU tables are populated
   */
  [[nodiscard]] bool hasEmbeddedTables() const noexcept
  {
    return !tableCPU.empty() && !tableGPU.empty();
  }
};

/**
 * @brief ODM profile settings
 */
struct UccODMProfile
{
  std::optional< std::string > name;

  UccODMProfile() = default;

  UccODMProfile( const std::string &profileName )
    : name( profileName )
  {
  }
};

/**
 * @brief ODM power limits
 */
struct UccODMPowerLimits
{
  std::vector< int32_t > tdpValues;

  UccODMPowerLimits() = default;

  UccODMPowerLimits( const std::vector< int32_t > &values )
    : tdpValues( values )
  {
  }
};

/**
 * @brief Keyboard backlight settings for a profile
 */
struct UccProfileKeyboard
{
  std::string keyboardProfileData;  // JSON string containing keyboard backlight states
  std::string keyboardProfileName;  // Name of the keyboard profile for reference
  std::string keyboardProfileId;    // UUID of the keyboard profile (from top-level selectedKeyboardProfile)

  UccProfileKeyboard()
    : keyboardProfileData( "{}" ),
      keyboardProfileName( "" ),
      keyboardProfileId( "" )
  {
  }

  UccProfileKeyboard( const std::string &data, const std::string &name )
    : keyboardProfileData( data ),
      keyboardProfileName( name ),
      keyboardProfileId( "" )
  {
  }
};

/**
 * @brief Complete TCC profile
 *
 * Contains all settings for a system profile including CPU, display,
 * fan control, webcam, ODM, and keyboard settings.
 */
struct UccProfile
{
  std::string id;
  std::string name;
  std::string description;
  UccProfileDisplay display;
  UccProfileCpu cpu;
  UccProfileWebcam webcam;
  UccProfileFanControl fan;
  UccProfileKeyboard keyboard;
  UccODMProfile odmProfile;
  UccODMPowerLimits odmPowerLimits;
  std::string gpuProfileId;     ///< UUID reference to GPU OC profile (stored in GUI QSettings)
  std::string gpuOCProfileData; ///< Embedded GPU OC profile JSON (offsets, locked clocks, powerLimitW)
  std::string chargingProfile;  ///< firmware-level charging profile descriptor (e.g. "balanced")
  std::string chargingPriority; ///< USB-C PD charging priority (e.g. "charge_battery", "performance")
  std::string chargeType;       ///< charge type: "Standard" or "Custom"
  int32_t chargeStartThreshold = -1; ///< start charging below this % (-1 = not set)
  int32_t chargeEndThreshold   = -1; ///< stop charging at this %  (-1 = not set)

  UccProfile() = default;

  UccProfile( const std::string &profileId, const std::string &profileName )
    : id( profileId ),
      name( profileName )
  {
  }

  // deep copy constructor
  UccProfile( const UccProfile &other )
    : id( other.id ),
      name( other.name ),
      description( other.description ),
      display( other.display ),
      cpu( other.cpu ),
      webcam( other.webcam ),
      fan( other.fan ),
      keyboard( other.keyboard ),
      odmProfile( other.odmProfile ),
      odmPowerLimits( other.odmPowerLimits ),
      gpuProfileId( other.gpuProfileId ),
      gpuOCProfileData( other.gpuOCProfileData ),
      chargingProfile( other.chargingProfile ),
      chargingPriority( other.chargingPriority ),
      chargeType( other.chargeType ),
      chargeStartThreshold( other.chargeStartThreshold ),
      chargeEndThreshold( other.chargeEndThreshold )
  {
  }

  // deep copy assignment
  UccProfile &operator=( const UccProfile &other )
  {
    if ( this != &other )
    {
      id = other.id;
      name = other.name;
      description = other.description;
      display = other.display;
      cpu = other.cpu;
      webcam = other.webcam;
      fan = other.fan;
      keyboard = other.keyboard;
      odmProfile = other.odmProfile;
      odmPowerLimits = other.odmPowerLimits;
      gpuProfileId = other.gpuProfileId;
      gpuOCProfileData = other.gpuOCProfileData;
      chargingProfile = other.chargingProfile;
      chargingPriority = other.chargingPriority;
      chargeType = other.chargeType;
      chargeStartThreshold = other.chargeStartThreshold;
      chargeEndThreshold = other.chargeEndThreshold;
    }
    return *this;
  }
};

/**
 * @brief Generate a unique profile ID
 * @return A unique profile identifier string
 */
std::string generateProfileId();

// default profile ID constants
namespace DefaultProfileIDs
{
  inline constexpr const char *MaxEnergySave = "Max Energy Save [Built-in]";
  inline constexpr const char *Quiet = "Quiet [Built-in]";
  inline constexpr const char *Office = "Office [Built-in]";
  inline constexpr const char *HighPerformance = "High Performance [Built-in]";
}

// profile to image mapping
extern const std::map< std::string, std::string > profileImageMap;
