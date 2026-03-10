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

#include "hal/FanZone.hpp"
#include "profiles/UccProfile.hpp"
#include "profiles/FanProfile.hpp"
#include <string>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Abstract interface for platform-specific default profiles.
 *
 * Each platform (Uniwill laptop, generic PC, ...) provides its own set
 * of built-in profiles tailored to the hardware capabilities.  The
 * HardwareManager selects the best profile provider during detect().
 */
class IProfileProvider
{
public:
  virtual ~IProfileProvider() = default;

  /// Human-readable provider name.
  virtual std::string name() const = 0;

  /// Probe hardware.  Return true if this provider can supply profiles.
  virtual bool detect() = 0;

  /// Higher priority providers are preferred when multiple detect.
  virtual int priority() const = 0;

  /// Return the set of built-in (read-only) default profiles.
  virtual std::vector< UccProfile > getDefaultProfiles() const = 0;

  /// Return the skeleton profile used as a starting point for new custom profiles.
  virtual UccProfile getDefaultCustomProfile() const = 0;

  /// Return platform-specific built-in fan profiles for the detected hardware topology.
  virtual std::vector< FanProfile > getDefaultFanProfiles(
    const std::vector< ucc::hal::FanZone > &zones ) const = 0;
};

} // namespace ucc::hal
