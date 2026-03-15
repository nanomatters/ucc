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
#include "hal/IFanProvider.hpp"

#include <syslog.h>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Fan provider for desktop NVIDIA GPUs via NVML.
 *
 * Uses nvmlDeviceSetFanSpeed_v2 / nvmlDeviceSetDefaultFanSpeed_v2 for
 * per-fan speed control.  RPM is not available via NVML on desktop GPUs
 * (nvmlDeviceGetFanSpeedRPM crashes with SIGSEGV on fans 1+2 of RTX 5090),
 * so getFanRPM() always returns nullopt.
 *
 * Priority 7: above generic hwmon (5) but below OEM tuxedo-io (10).
 */
class NvidiaGpuFanProvider final : public IFanProvider
{
public:
  explicit NvidiaGpuFanProvider( NvmlWrapper *nvml )
    : m_nvml( nvml )
  {
  }

  std::string name() const override { return "nvml-gpu-fans"; }
  int priority() const override { return 7; }

  bool detect() override
  {
    m_fans.clear();
    m_minSpeed = 0;

    if ( !m_nvml || !m_nvml->isAvailable() )
      return false;

    // We need at least the set function to be useful as a controllable provider
    const bool canControl = ( m_nvml->getNumFans( 0 ).has_value() );
    if ( !canControl )
      return false;

    const unsigned int gpuCount = m_nvml->deviceCount();
    for ( unsigned int gi = 0; gi < gpuCount; ++gi )
    {
      auto numFans = m_nvml->getNumFans( gi );
      if ( !numFans.has_value() || *numFans == 0 )
        continue;

      // Try to read min/max fan speed for this GPU
      auto minMax = m_nvml->getMinMaxFanSpeedPct( gi );
      if ( minMax.has_value() )
        m_minSpeed = static_cast< int >( minMax->first );

      // Check if we can actually read fan speed (basic sanity)
      auto testRead = m_nvml->getFanSpeedPct( gi, 0 );
      if ( !testRead.has_value() )
        continue;

      for ( unsigned int fi = 0; fi < *numFans; ++fi )
      {
        FanInfo fan;
        fan.id = "nvml_gpu" + std::to_string( gi ) + "_fan" + std::to_string( fi );
        const std::string gpuName = m_nvml->getDeviceName( gi ).value_or( "NVIDIA GPU" );
        fan.sourceName = ( gpuCount == 1 )
                           ? gpuName
                           : gpuName + " (GPU " + std::to_string( gi ) + ")";
        fan.label = ( gpuCount == 1 )
                      ? "GPU Fan " + std::to_string( fi + 1 )
                      : "GPU " + std::to_string( gi ) + " Fan " + std::to_string( fi + 1 );
        fan.hwmonPath = {};
        fan.index = static_cast< int >( fi );
        fan.canRead = true;
        fan.canControl = true; // Will be validated on first write
        fan.deviceType = FanDeviceType::Virtual;

        // Store the GPU index in a way we can recover it:
        // encode as (gpuIndex << 16) | fanIndex
        fan.index = static_cast< int >( ( gi << 16 ) | fi );

        m_fans.push_back( std::move( fan ) );
      }
    }

    if ( !m_fans.empty() )
    {
      syslog( LOG_INFO, "NvidiaGpuFanProvider: detected %zu NVIDIA GPU fan(s)",
              m_fans.size() );
    }

    return !m_fans.empty();
  }

  std::vector< FanInfo > enumerateFans() override
  {
    return m_fans;
  }

  // ---- Monitoring ----

  std::optional< int > getFanRPM( [[maybe_unused]] const FanInfo &fan ) override
  {
    // DANGER: nvmlDeviceGetFanSpeedRPM crashes with SIGSEGV on fans 1+2.
    // Do NOT attempt to read RPM via NVML.
    return std::nullopt;
  }

  std::optional< int > getFanSpeedPercent( const FanInfo &fan ) override
  {
    auto [gi, fi] = decodeFanIndex( fan );
    auto val = m_nvml->getFanSpeedPct( gi, fi );
    if ( val.has_value() )
      return static_cast< int >( *val );
    return std::nullopt;
  }

  // ---- Control ----

  bool setFanSpeedPercent( const FanInfo &fan, int percent ) override
  {
    auto [gi, fi] = decodeFanIndex( fan );
    // Switch to manual policy before setting speed
    m_nvml->setFanControlPolicy( gi, fi, 1 ); // 1 = manual
    return m_nvml->setFanSpeed( gi, fi, static_cast< unsigned int >( percent ) );
  }

  bool setFanAuto( const FanInfo &fan ) override
  {
    auto [gi, fi] = decodeFanIndex( fan );
    // Restore default firmware-controlled fan speed
    bool ok = m_nvml->resetFanSpeed( gi, fi );
    // Also switch policy back to auto
    m_nvml->setFanControlPolicy( gi, fi, 0 ); // 0 = auto
    return ok;
  }

  void restoreAllAuto() override
  {
    for ( const auto &fan : m_fans )
      setFanAuto( fan );
  }

  int getMinSpeedPercent( [[maybe_unused]] const FanInfo &fan ) override
  {
    return m_minSpeed;
  }

  bool canTurnOff( [[maybe_unused]] const FanInfo &fan ) override
  {
    // GPU fans should never be turned off completely
    return false;
  }

private:
  NvmlWrapper *m_nvml = nullptr;
  std::vector< FanInfo > m_fans;
  int m_minSpeed = 0;

  /// Decode the packed GPU/fan index from FanInfo::index.
  static std::pair< unsigned int, unsigned int > decodeFanIndex( const FanInfo &fan )
  {
    unsigned int packed = static_cast< unsigned int >( fan.index );
    return { packed >> 16, packed & 0xFFFF };
  }
};

} // namespace ucc::hal
