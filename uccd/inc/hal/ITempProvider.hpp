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
 * @brief Describes a single temperature sensor.
 */
struct TempSensorInfo
{
  std::string id;         // unique id, e.g. "hwmon4_temp1", "tuxedio_cpu"
  std::string label;      // human-readable, e.g. "CPU Tctl", "Chipset"
  std::string source;     // driver name: "k10temp", "nct6799", "asusec"
  std::string hwmonPath;  // base hwmon directory
  int index = 0;          // sensor number within the hwmon chip
};

/**
 * @brief Abstract interface for temperature sensor providers.
 */
class ITempProvider
{
public:
  virtual ~ITempProvider() = default;

  /// Human-readable provider name.
  virtual std::string name() const = 0;

  /// Probe hardware.  Return true if usable.
  virtual bool detect() = 0;

  /// Enumerate all temperature sensors found by this provider.
  virtual std::vector< TempSensorInfo > enumerateSensors() = 0;

  /// Read temperature in degrees Celsius.
  virtual std::optional< double > readTempCelsius( const TempSensorInfo &sensor ) = 0;

  /// Read critical temperature threshold (if known).
  virtual std::optional< double > getCriticalTemp( [[maybe_unused]] const TempSensorInfo &sensor )
  {
    return std::nullopt;
  }

  /// Re-read hardware state (e.g. after resume from suspend).
  virtual void refresh() {}
};

} // namespace ucc::hal
