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

#include <optional>
#include <string>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Describes a single fan discovered by a provider.
 */
struct FanInfo
{
  std::string id;         // unique id, e.g. "hwmon3_fan1", "tuxedio_fan0"
  std::string label;      // human-readable, e.g. "CPU Fan", "Chassis Fan 1"
  std::string hwmonPath;  // base hwmon directory (for hwmon-based)
  int index = 0;          // fan number within the provider
  bool canRead = false;   // can read RPM
  bool canControl = false;// can write PWM
};

/**
 * @brief Abstract interface for fan monitoring and control providers.
 *
 * Each concrete provider talks to one hardware subsystem (hwmon, tuxedo_io, etc.).
 * The HardwareManager picks the best available provider at startup.
 */
class IFanProvider
{
public:
  virtual ~IFanProvider() = default;

  /// Human-readable provider name, e.g. "hwmon-nct6799", "tuxedo-io-uniwill"
  virtual std::string name() const = 0;

  /// Probe hardware.  Return true if this provider is usable on the current machine.
  virtual bool detect() = 0;

  /// Higher = preferred.  OEM drivers should return 10; generic hwmon returns 5.
  virtual int priority() const = 0;

  // ----- Discovery -----

  /// Enumerate all fans found by this provider.
  virtual std::vector< FanInfo > enumerateFans() = 0;

  // ----- Monitoring -----

  /// Read raw fan RPM.  Returns nullopt if unavailable.
  virtual std::optional< int > getFanRPM( const FanInfo &fan ) = 0;

  /// Read fan speed as percentage (0–100).  Returns nullopt if unavailable.
  virtual std::optional< int > getFanSpeedPercent( const FanInfo &fan ) = 0;

  // ----- Control -----

  /// Set fan speed as PWM percentage (0–100).  Returns false if unsupported.
  virtual bool setFanSpeedPercent( const FanInfo &fan, int percent ) = 0;

  /// Revert fan to automatic/firmware control.  Returns false if unsupported.
  virtual bool setFanAuto( const FanInfo &fan ) = 0;

  /// Re-read hardware state (e.g. after resume from suspend).
  virtual void refresh() {}

  /// Set all fans back to auto (cleanup on daemon exit).
  virtual void restoreAllAuto()
  {
    for ( const auto &fan : enumerateFans() )
      setFanAuto( fan );
  }

  // ----- Per-fan capability helpers -----

  virtual int getMinSpeedPercent( [[maybe_unused]] const FanInfo &fan ) { return 0; }
  virtual bool canTurnOff( [[maybe_unused]] const FanInfo &fan ) { return false; }
};

} // namespace ucc::hal
