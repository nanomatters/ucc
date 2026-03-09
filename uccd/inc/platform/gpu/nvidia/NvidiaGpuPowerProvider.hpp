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

#include "NvmlWrapper.hpp"

#include <optional>

namespace ucc::hal
{

/**
 * Generic NVIDIA desktop/mobile power-limit provider via NVML.
 * This is independent of Uniwill/Tuxedo cTGP sysfs control.
 */
class NvidiaGpuPowerProvider
{
public:
  explicit NvidiaGpuPowerProvider( NvmlWrapper *nvml )
    : m_nvml( nvml )
  {
  }

  [[nodiscard]] bool isAvailable( unsigned int deviceIndex = 0 ) const noexcept;
  [[nodiscard]] std::optional< int > getDefaultLimitW( unsigned int deviceIndex = 0 ) const noexcept;
  [[nodiscard]] std::optional< int > getMaxLimitW( unsigned int deviceIndex = 0 ) const noexcept;
  [[nodiscard]] std::optional< int > getMinLimitW( unsigned int deviceIndex = 0 ) const noexcept;
  [[nodiscard]] std::optional< int > getCurrentLimitW( unsigned int deviceIndex = 0 ) const noexcept;

  [[nodiscard]] bool setLimitW( unsigned int deviceIndex, double watts ) const noexcept;
  [[nodiscard]] bool resetLimit( unsigned int deviceIndex ) const noexcept;

private:
  NvmlWrapper *m_nvml;
};

} // namespace ucc::hal
