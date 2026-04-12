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

#include <cstdint>
#include <string>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Capability flags — what this system can do.
 *
 * Each bit represents an independent hardware subsystem that may or may
 * not be available on the current machine.  The GUI uses these flags to
 * dynamically show/hide sections.
 */
enum class HwCapability : uint32_t
{
  None              = 0,
  FanMonitoring     = 1 << 0,   // can read fan RPM
  FanControl        = 1 << 1,   // can set fan speed (PWM)
  TempMonitoring    = 1 << 2,   // can read temperatures
  CpuFreqControl    = 1 << 3,   // cpufreq governor/EPP
  CpuTdpControl     = 1 << 4,   // RAPL / OEM TDP limits
  GpuMonitoring     = 1 << 5,   // NVML / hwmon GPU temps
  GpuOverclock      = 1 << 6,   // NVML OC
  KeyboardBacklight = 1 << 7,   // sysfs LED class
  WebcamSwitch      = 1 << 8,   // OEM-specific
  ChargingControl   = 1 << 9,   // battery charge thresholds (laptop only)
  FnLock            = 1 << 10,  // OEM keyboard module
  DisplayControl    = 1 << 11,  // brightness, refresh rate
  PowerSupply       = 1 << 12,  // battery / AC status
  OdmProfiles       = 1 << 13,  // vendor perf profiles (Uniwill/TDP modes)
  PlatformLeds      = 1 << 14,  // chassis RGB, logo LED, etc.
  MultiplePowerStates = 1 << 15, // device has battery → AC/BAT power states
};

inline constexpr HwCapability operator|( HwCapability a, HwCapability b ) noexcept
{
  return static_cast< HwCapability >(
    static_cast< uint32_t >( a ) | static_cast< uint32_t >( b ) );
}

inline constexpr HwCapability operator&( HwCapability a, HwCapability b ) noexcept
{
  return static_cast< HwCapability >(
    static_cast< uint32_t >( a ) & static_cast< uint32_t >( b ) );
}

inline constexpr HwCapability &operator|=( HwCapability &a, HwCapability b ) noexcept
{
  a = a | b;
  return a;
}

inline constexpr bool hasCapability( HwCapability set, HwCapability flag ) noexcept
{
  return ( static_cast< uint32_t >( set ) & static_cast< uint32_t >( flag ) ) != 0;
}

/**
 * @brief System form factor, detected from DMI chassis_type.
 */
enum class ChassisType
{
  Desktop,        // chassis_type 3,4,5,6,7
  Laptop,         // chassis_type 9,10,14
  Convertible,    // chassis_type 31,32
  Tablet,         // chassis_type 30
  Server,         // chassis_type 17,23,25,28,29
  AllInOne,       // chassis_type 13,24
  Unknown
};

/**
 * @brief Convert DMI chassis_type integer to ChassisType enum.
 */
inline ChassisType chassisTypeFromDmi( int dmiType ) noexcept
{
  switch ( dmiType )
  {
    case 3: case 4: case 5: case 6: case 7:
      return ChassisType::Desktop;
    case 9: case 10: case 14:
      return ChassisType::Laptop;
    case 31: case 32:
      return ChassisType::Convertible;
    case 30:
      return ChassisType::Tablet;
    case 17: case 23: case 25: case 28: case 29:
      return ChassisType::Server;
    case 13: case 24:
      return ChassisType::AllInOne;
    default:
      return ChassisType::Unknown;
  }
}

inline std::string chassisTypeToString( ChassisType type ) noexcept
{
  switch ( type )
  {
    case ChassisType::Desktop:     return "desktop";
    case ChassisType::Laptop:      return "laptop";
    case ChassisType::Convertible: return "convertible";
    case ChassisType::Tablet:      return "tablet";
    case ChassisType::Server:      return "server";
    case ChassisType::AllInOne:    return "all-in-one";
    case ChassisType::Unknown:     return "unknown";
  }
  return "unknown";
}

/**
 * @brief Serialize a capability bitmask into a JSON array of strings.
 */
inline std::string capabilitiesToJSON( HwCapability caps )
{
  std::vector< std::string > names;

  auto check = [&]( HwCapability flag, const char *name ) {
    if ( hasCapability( caps, flag ) )
      names.push_back( name );
  };

  check( HwCapability::FanMonitoring,     "fanMonitoring" );
  check( HwCapability::FanControl,        "fanControl" );
  check( HwCapability::TempMonitoring,    "tempMonitoring" );
  check( HwCapability::CpuFreqControl,    "cpuFreqControl" );
  check( HwCapability::CpuTdpControl,     "cpuTdpControl" );
  check( HwCapability::GpuMonitoring,     "gpuMonitoring" );
  check( HwCapability::GpuOverclock,      "gpuOverclock" );
  check( HwCapability::KeyboardBacklight, "keyboardBacklight" );
  check( HwCapability::WebcamSwitch,      "webcamSwitch" );
  check( HwCapability::ChargingControl,   "chargingControl" );
  check( HwCapability::FnLock,            "fnLock" );
  check( HwCapability::DisplayControl,    "displayControl" );
  check( HwCapability::PowerSupply,       "powerSupply" );
  check( HwCapability::OdmProfiles,       "odmProfiles" );
  check( HwCapability::PlatformLeds,      "platformLeds" );
  check( HwCapability::MultiplePowerStates, "multiplePowerStates" );

  std::string result = "[";
  for ( size_t i = 0; i < names.size(); ++i )
  {
    if ( i > 0 ) result += ",";
    result += "\"" + names[i] + "\"";
  }
  result += "]";
  return result;
}

} // namespace ucc::hal
