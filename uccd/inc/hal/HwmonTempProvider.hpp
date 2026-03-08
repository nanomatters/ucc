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

#include "hal/ITempProvider.hpp"
#include "SysfsNode.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <syslog.h>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Generic hwmon-based temperature sensor provider.
 *
 * Scans /sys/class/hwmon/ for chips that expose temp*_input sysfs files.
 * Works on any Linux machine: k10temp, coretemp, nct67xx, asus-ec-sensors, etc.
 */
class HwmonTempProvider final : public ITempProvider
{
public:
  HwmonTempProvider() = default;

  std::string name() const override { return "hwmon-temps"; }

  bool detect() override
  {
    m_sensors.clear();

    namespace fs = std::filesystem;
    const fs::path hwmonBase = "/sys/class/hwmon";
    if ( !fs::exists( hwmonBase ) )
      return false;

    for ( const auto &entry : fs::directory_iterator( hwmonBase ) )
    {
      const fs::path hwmonDir = fs::canonical( entry.path() );
      std::string chipName = readLine( ( hwmonDir / "name" ).string() );
      if ( chipName.empty() )
        continue;

      // Scan for temp*_input files (temp1_input, temp2_input, etc.)
      for ( int i = 1; i <= 32; ++i )
      {
        const std::string tempInputPath = hwmonDir.string() + "/temp" + std::to_string( i ) + "_input";
        if ( !fs::exists( tempInputPath ) )
          continue;

        TempSensorInfo si;
        si.id = entry.path().filename().string() + "_temp" + std::to_string( i );
        si.source = chipName;
        si.hwmonPath = hwmonDir.string();
        si.index = i;

        // Try to read label
        const std::string labelPath = hwmonDir.string() + "/temp" + std::to_string( i ) + "_label";
        if ( fs::exists( labelPath ) )
          si.label = readLine( labelPath );

        if ( si.label.empty() )
          si.label = chipName + " temp" + std::to_string( i );

        m_sensors.push_back( std::move( si ) );
      }
    }

    if ( !m_sensors.empty() )
    {
      syslog( LOG_INFO, "HwmonTempProvider: detected %zu temperature sensors",
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
    const std::string path = sensor.hwmonPath + "/temp" + std::to_string( sensor.index ) + "_input";
    SysfsNode< int32_t > node( path );
    auto val = node.read();
    if ( val.has_value() )
    {
      // hwmon temps are in millidegrees Celsius
      return static_cast< double >( *val ) / 1000.0;
    }
    return std::nullopt;
  }

  std::optional< double > getCriticalTemp( const TempSensorInfo &sensor ) override
  {
    const std::string path = sensor.hwmonPath + "/temp" + std::to_string( sensor.index ) + "_crit";
    SysfsNode< int32_t > node( path );
    auto val = node.read();
    if ( val.has_value() )
      return static_cast< double >( *val ) / 1000.0;
    return std::nullopt;
  }

private:
  std::vector< TempSensorInfo > m_sensors;

  static std::string readLine( const std::string &path )
  {
    std::ifstream file( path );
    if ( !file.is_open() )
      return {};
    std::string line;
    std::getline( file, line );
    while ( !line.empty() && ( line.back() == '\n' || line.back() == '\r' || line.back() == ' ' ) )
      line.pop_back();
    return line;
  }
};

} // namespace ucc::hal
