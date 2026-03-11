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
#include "hal/ITempProvider.hpp"

#include <syslog.h>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Temperature provider for NVIDIA GPUs via NVML.
 *
 * On systems where the NVIDIA proprietary driver does not expose an hwmon
 * interface (e.g. RTX 5090), this is the only way to read GPU temperature.
 * Uses sensor ID "gpu-dgpu-temp" for backward compatibility with existing
 * user-created thermal sources that reference it.
 */
class NvmlTempProvider final : public ITempProvider
{
public:
  explicit NvmlTempProvider( NvmlWrapper *nvml )
    : m_nvml( nvml )
  {
  }

  std::string name() const override { return "nvml-temps"; }

  bool detect() override
  {
    m_sensors.clear();

    if ( !m_nvml || !m_nvml->isAvailable() )
      return false;

    const unsigned int count = m_nvml->deviceCount();
    for ( unsigned int i = 0; i < count; ++i )
    {
      // Verify we can actually read temperature for this device
      auto temp = m_nvml->getTemperatureDegC( i );
      if ( !temp.has_value() )
        continue;

      TempSensorInfo si;
      // Use gpu-dgpu-temp for backward compatibility with user configurations
      si.id = ( i == 0 ) ? "gpu-dgpu-temp"
                          : "gpu-dgpu-temp-" + std::to_string( i );
      si.label = ( count == 1 ) ? "GPU" : "GPU " + std::to_string( i );
      si.source = "nvidia";
      si.hwmonPath = {};
      si.index = static_cast< int >( i );

      m_sensors.push_back( std::move( si ) );
    }

    if ( !m_sensors.empty() )
    {
      syslog( LOG_INFO, "NvmlTempProvider: detected %zu NVIDIA GPU temperature sensor(s)",
              m_sensors.size() );
    }

    return !m_sensors.empty();
  }

  std::vector< TempSensorInfo > enumerateSensors() override
  {
    return m_sensors;
  }

  std::optional< double > readTempCelsius( const TempSensorInfo &sensor ) override
  {
    if ( !m_nvml || !m_nvml->isAvailable() )
      return std::nullopt;

    auto val = m_nvml->getTemperatureDegC( static_cast< unsigned int >( sensor.index ) );
    if ( val.has_value() )
      return static_cast< double >( *val );
    return std::nullopt;
  }

private:
  NvmlWrapper *m_nvml = nullptr;
  std::vector< TempSensorInfo > m_sensors;
};

} // namespace ucc::hal
