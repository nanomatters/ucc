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

#include "platform/gpu/nvidia/NvidiaGpuPowerProvider.hpp"

#include <algorithm>
#include <cmath>

namespace ucc::hal
{

bool NvidiaGpuPowerProvider::isAvailable( unsigned int deviceIndex ) const noexcept
{
  if ( !m_nvml || !m_nvml->isAvailable() || m_nvml->deviceCount() <= deviceIndex )
    return false;

  return getDefaultLimitW( deviceIndex ).has_value() && getMaxLimitW( deviceIndex ).has_value();
}

std::optional< int > NvidiaGpuPowerProvider::getDefaultLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( !m_nvml )
    return std::nullopt;

  const auto value = m_nvml->getPowerDefaultLimitW( deviceIndex );
  if ( !value )
    return std::nullopt;

  return static_cast< int >( std::lround( *value ) );
}

std::optional< int > NvidiaGpuPowerProvider::getMaxLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( !m_nvml )
    return std::nullopt;

  const auto value = m_nvml->getPowerMaxLimitW( deviceIndex );
  if ( !value )
    return std::nullopt;

  return static_cast< int >( std::lround( *value ) );
}

std::optional< int > NvidiaGpuPowerProvider::getMinLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( !m_nvml )
    return std::nullopt;

  const auto value = m_nvml->getPowerMinLimitW( deviceIndex );
  if ( !value )
    return std::nullopt;

  return static_cast< int >( std::lround( *value ) );
}

std::optional< int > NvidiaGpuPowerProvider::getCurrentLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( !m_nvml )
    return std::nullopt;

  const auto value = m_nvml->getEnforcedPowerLimitW( deviceIndex );
  if ( !value )
    return std::nullopt;

  return static_cast< int >( std::lround( *value ) );
}

bool NvidiaGpuPowerProvider::setLimitW( unsigned int deviceIndex, double watts ) const noexcept
{
  if ( !isAvailable( deviceIndex ) || watts <= 0.0 )
    return false;

  double clamped = watts;
  if ( const auto minW = m_nvml->getPowerMinLimitW( deviceIndex ) )
    clamped = std::max( clamped, *minW );
  if ( const auto maxW = m_nvml->getPowerMaxLimitW( deviceIndex ) )
    clamped = std::min( clamped, *maxW );

  const auto mw = static_cast< unsigned int >( std::lround( clamped * 1000.0 ) );
  return m_nvml->setPowerLimit( deviceIndex, mw );
}

bool NvidiaGpuPowerProvider::resetLimit( unsigned int deviceIndex ) const noexcept
{
  return m_nvml && m_nvml->resetPowerLimit( deviceIndex );
}

} // namespace ucc::hal
