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

#include "HwCapability.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Abstract interface for OEM / platform-specific features.
 *
 * Covers vendor performance profiles, webcam switches, Fn Lock,
 * TDP control, and other features that are only available on
 * specific hardware via WMI/ACPI/EC interfaces.
 */
class IPlatformProvider
{
public:
  virtual ~IPlatformProvider() = default;

  /// Human-readable provider name.
  virtual std::string name() const = 0;

  /// Probe hardware.  Return true if usable.
  virtual bool detect() = 0;

  /// Higher priority providers are preferred when multiple detect.
  virtual int priority() const = 0;

  /// Bitmask of HwCapability flags this provider adds.
  virtual HwCapability capabilities() const = 0;

  // ----- OEM performance profiles -----

  virtual std::vector< std::string > getOdmProfiles() { return {}; }
  virtual bool setOdmProfile( [[maybe_unused]] const std::string &profile ) { return false; }
  virtual std::string getDefaultOdmProfile() { return {}; }

  // ----- Webcam switch -----

  virtual std::optional< bool > getWebcam() { return std::nullopt; }
  virtual bool setWebcam( [[maybe_unused]] bool enabled ) { return false; }

  // ----- Fn Lock -----

  virtual std::optional< bool > getFnLock() { return std::nullopt; }
  virtual bool setFnLock( [[maybe_unused]] bool enabled ) { return false; }

  // ----- TDP control -----

  virtual int getNumberTDPs() { return 0; }
  virtual std::vector< std::string > getTDPDescriptors() { return {}; }
  virtual std::optional< int > getTDPMin( [[maybe_unused]] int index ) { return std::nullopt; }
  virtual std::optional< int > getTDPMax( [[maybe_unused]] int index ) { return std::nullopt; }
  virtual std::optional< int > getTDP( [[maybe_unused]] int index ) { return std::nullopt; }
  virtual bool setTDP( [[maybe_unused]] int index, [[maybe_unused]] int value ) { return false; }

  // ----- Chassis LED / RGB -----

  virtual bool setLedColor( [[maybe_unused]] const std::string &zone,
                            [[maybe_unused]] uint8_t r,
                            [[maybe_unused]] uint8_t g,
                            [[maybe_unused]] uint8_t b ) { return false; }
};

} // namespace ucc::hal
