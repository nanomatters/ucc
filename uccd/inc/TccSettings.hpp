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
#include <map>
#include <vector>
#include <optional>

enum class ProfileState
{
  AC,  // power_ac - plugged in
  BAT, // power_bat - on battery
  WC   // power_wc - water cooler connected
};

/**
 * @brief YCbCr 4:2:0 port configuration
 */
struct YCbCr420Port
{
  std::string port;     // e.g., "eDP-1", "HDMI-A-1"
  bool enabled = false;
};

/**
 * @brief YCbCr 4:2:0 card configuration
 */
struct YCbCr420Card
{
  int card = 0;  // DRI card number
  std::vector< YCbCr420Port > ports;
};

struct TccSettings
{
  bool fahrenheit = false;
  std::map< std::string, std::string > stateMap;  // Maps "power_ac" and "power_bat" to profile IDs
  std::map< std::string, std::string > profiles;  // Maps profile IDs to full profile JSON
  std::optional< std::string > shutdownTime;  // null in TypeScript
  bool cpuSettingsEnabled = true;
  bool fanControlEnabled = true;
  bool keyboardBacklightControlEnabled = true;
  std::vector< YCbCr420Card > ycbcr420Workaround;  // YUV420 workaround per card/port
  std::optional< std::string > chargingProfile;  // null in TypeScript
  std::optional< std::string > chargingPriority;  // null in TypeScript
  // keyboardBacklightStates omitted for now - complex nested structure
  
  // Default constructor – stateMap starts empty.
  // uccd must not auto-assign profiles; it waits for ucc-gui to assign them.
  TccSettings() = default;
};
