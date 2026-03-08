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

#include "hal/HwCapability.hpp"
#include "hal/IFanProvider.hpp"
#include "hal/ITempProvider.hpp"
#include "hal/IPlatformProvider.hpp"
#include "hal/HwmonFanProvider.hpp"
#include "hal/HwmonTempProvider.hpp"
#include "hal/TuxedoIOProviders.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <syslog.h>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Central hardware abstraction manager.
 *
 * Owns all provider instances, probes them at startup, and selects the
 * best (highest priority) usable provider for each subsystem.  Downstream
 * code (FanControlWorker, HardwareMonitorWorker, UccDBusService) uses the
 * manager's accessors instead of talking to TuxedoIOAPI or sysfs directly.
 *
 * Lifetime: created once in UccDBusService constructor, lives for the
 * duration of the daemon.
 */
class HardwareManager
{
public:
  HardwareManager() = default;
  ~HardwareManager() = default;

  // non-copyable, non-movable (owns unique_ptrs to providers)
  HardwareManager( const HardwareManager & ) = delete;
  HardwareManager &operator=( const HardwareManager & ) = delete;

  // ---------------------------------------------------------------
  //  Provider registration (call before detect())
  // ---------------------------------------------------------------

  void addFanProvider( std::unique_ptr< IFanProvider > p )
  {
    m_fanProviders.push_back( std::move( p ) );
  }

  void addTempProvider( std::unique_ptr< ITempProvider > p )
  {
    m_tempProviders.push_back( std::move( p ) );
  }

  void addPlatformProvider( std::unique_ptr< IPlatformProvider > p )
  {
    m_platformProviders.push_back( std::move( p ) );
  }

  // ---------------------------------------------------------------
  //  Detection — call once after all providers are registered
  // ---------------------------------------------------------------

  /**
   * @brief Probe all registered providers and select the best available.
   *
   * Also reads the DMI chassis_type to populate m_chassisType.
   */
  void detect()
  {
    // Detect chassis type from DMI
    m_chassisType = detectChassisType();
    syslog( LOG_INFO, "[HardwareManager] Chassis type: %s",
            chassisTypeToString( m_chassisType ).c_str() );

    // --- Fan providers ---
    selectBest( m_fanProviders, m_activeFanProvider, "fan" );

    // --- Temp providers (may accumulate ALL that detect) ---
    m_activeTempProviders.clear();
    for ( auto &p : m_tempProviders )
    {
      if ( p->detect() )
      {
        syslog( LOG_INFO, "[HardwareManager] Temp provider '%s' detected",
                p->name().c_str() );
        m_activeTempProviders.push_back( p.get() );
      }
    }
    if ( m_activeTempProviders.empty() )
      syslog( LOG_WARNING, "[HardwareManager] No temperature providers detected" );

    // --- Platform providers ---
    selectBest( m_platformProviders, m_activePlatformProvider, "platform" );

    // Build aggregated capabilities
    m_capabilities = buildCapabilities();
    syslog( LOG_INFO, "[HardwareManager] Capabilities: 0x%08x",
            static_cast< uint32_t >( m_capabilities ) );

    // Enumerate fans and temp sensors from active providers
    m_fans.clear();
    if ( m_activeFanProvider )
      m_fans = m_activeFanProvider->enumerateFans();

    m_tempSensors.clear();
    for ( auto *tp : m_activeTempProviders )
    {
      auto sensors = tp->enumerateSensors();
      m_tempSensors.insert( m_tempSensors.end(), sensors.begin(), sensors.end() );
    }

    syslog( LOG_INFO, "[HardwareManager] %zu fans, %zu temperature sensors detected",
            m_fans.size(), m_tempSensors.size() );
  }

  // ---------------------------------------------------------------
  //  Accessors
  // ---------------------------------------------------------------

  ChassisType chassisType() const noexcept { return m_chassisType; }
  HwCapability capabilities() const noexcept { return m_capabilities; }

  /// Active fan provider (may be nullptr if no fans detected)
  IFanProvider *fanProvider() noexcept { return m_activeFanProvider; }
  const IFanProvider *fanProvider() const noexcept { return m_activeFanProvider; }

  /// All active temp providers
  const std::vector< ITempProvider * > &tempProviders() const noexcept { return m_activeTempProviders; }

  /// Active platform provider (may be nullptr)
  IPlatformProvider *platformProvider() noexcept { return m_activePlatformProvider; }
  const IPlatformProvider *platformProvider() const noexcept { return m_activePlatformProvider; }

  /// All enumerated fans (from the active fan provider)
  const std::vector< FanInfo > &fans() const noexcept { return m_fans; }

  /// All enumerated temperature sensors (from all active temp providers)
  const std::vector< TempSensorInfo > &tempSensors() const noexcept { return m_tempSensors; }

  bool hasFanControl() const noexcept
  {
    return hasCapability( m_capabilities, HwCapability::FanControl );
  }

  bool hasTempMonitoring() const noexcept
  {
    return hasCapability( m_capabilities, HwCapability::TempMonitoring );
  }

  // ---------------------------------------------------------------
  //  Convenience: find the "best" CPU temp sensor
  // ---------------------------------------------------------------

  /**
   * @brief Find the primary CPU temperature sensor.
   *
   * Heuristic: look for sensors labelled "Tctl", "Tdie", "Package id 0",
   * or from k10temp/coretemp drivers, then fall back to the first sensor
   * from any provider.
   */
  const TempSensorInfo *findCpuTempSensor() const noexcept
  {
    static const std::vector< std::string > cpuLabels = {
      "Tctl", "Tdie", "Package id 0", "CPU", "cpu"
    };
    static const std::vector< std::string > cpuDrivers = {
      "k10temp", "coretemp", "zenpower"
    };

    // First pass: match by label
    for ( const auto &sensor : m_tempSensors )
    {
      for ( const auto &label : cpuLabels )
      {
        if ( sensor.label.find( label ) != std::string::npos )
          return &sensor;
      }
    }

    // Second pass: match by driver
    for ( const auto &sensor : m_tempSensors )
    {
      for ( const auto &drv : cpuDrivers )
      {
        if ( sensor.source == drv )
          return &sensor;
      }
    }

    // TuxedoIO temp sensor index 0 is always CPU
    for ( const auto &sensor : m_tempSensors )
    {
      if ( sensor.id.find( "tuxedio_temp0" ) != std::string::npos )
        return &sensor;
    }

    // Last resort: first sensor
    return m_tempSensors.empty() ? nullptr : &m_tempSensors.front();
  }

  /**
   * @brief Find the primary GPU temperature sensor (discrete GPU).
   *
   * Looks for sensors from NVIDIA (nouveau/nvidia) or AMD (amdgpu) hwmon,
   * or TuxedoIO temp index 1.
   */
  const TempSensorInfo *findGpuTempSensor() const noexcept
  {
    static const std::vector< std::string > gpuDrivers = {
      "nvidia", "nouveau", "amdgpu"
    };
    static const std::vector< std::string > gpuLabels = {
      "GPU", "gpu", "edge"
    };

    for ( const auto &sensor : m_tempSensors )
    {
      for ( const auto &drv : gpuDrivers )
      {
        if ( sensor.source == drv )
          return &sensor;
      }
    }

    for ( const auto &sensor : m_tempSensors )
    {
      for ( const auto &label : gpuLabels )
      {
        if ( sensor.label.find( label ) != std::string::npos )
          return &sensor;
      }
    }

    // TuxedoIO temp sensor index 1 is GPU
    for ( const auto &sensor : m_tempSensors )
    {
      if ( sensor.id.find( "tuxedio_temp1" ) != std::string::npos )
        return &sensor;
    }

    return nullptr;
  }

  // ---------------------------------------------------------------
  //  Convenience: read temperature for a sensor
  // ---------------------------------------------------------------

  std::optional< double > readTemp( const TempSensorInfo &sensor ) const
  {
    for ( auto *tp : m_activeTempProviders )
    {
      // Match by provider name (from sensor.source or id prefix)
      auto sensors = tp->enumerateSensors();
      for ( const auto &s : sensors )
      {
        if ( s.id == sensor.id )
          return tp->readTempCelsius( sensor );
      }
    }
    return std::nullopt;
  }

  // ---------------------------------------------------------------
  //  Shutdown — restore fans to auto mode
  // ---------------------------------------------------------------

  void restoreAllFanAuto()
  {
    if ( m_activeFanProvider )
      m_activeFanProvider->restoreAllAuto();
  }

private:
  ChassisType detectChassisType() const
  {
    std::ifstream file( "/sys/class/dmi/id/chassis_type" );
    if ( !file.is_open() )
      return ChassisType::Unknown;

    int val = 0;
    file >> val;
    return chassisTypeFromDmi( val );
  }

  template< typename ProviderPtr >
  void selectBest( std::vector< std::unique_ptr< ProviderPtr > > &providers,
                   ProviderPtr *&active,
                   const char *label )
  {
    active = nullptr;
    int bestPriority = -1;

    // Sort by priority descending, then probe in order
    std::sort( providers.begin(), providers.end(),
               []( const auto &a, const auto &b )
               { return a->priority() > b->priority(); } );

    for ( auto &p : providers )
    {
      if ( p->detect() )
      {
        syslog( LOG_INFO, "[HardwareManager] %s provider '%s' (priority %d) detected",
                label, p->name().c_str(), p->priority() );
        if ( p->priority() > bestPriority )
        {
          active = p.get();
          bestPriority = p->priority();
        }
      }
    }

    if ( !active )
      syslog( LOG_INFO, "[HardwareManager] No %s provider detected", label );
  }

  HwCapability buildCapabilities() const
  {
    HwCapability caps = HwCapability::None;

    if ( m_activeFanProvider )
    {
      auto fans = m_activeFanProvider->enumerateFans();
      for ( const auto &f : fans )
      {
        if ( f.canRead )
          caps |= HwCapability::FanMonitoring;
        if ( f.canControl )
          caps |= HwCapability::FanControl;
      }
    }

    if ( !m_activeTempProviders.empty() )
      caps |= HwCapability::TempMonitoring;

    if ( m_activePlatformProvider )
      caps |= m_activePlatformProvider->capabilities();

    return caps;
  }

  // Registered providers (owned)
  std::vector< std::unique_ptr< IFanProvider > > m_fanProviders;
  std::vector< std::unique_ptr< ITempProvider > > m_tempProviders;
  std::vector< std::unique_ptr< IPlatformProvider > > m_platformProviders;

  // Active (selected) providers (raw pointers into owned vectors)
  IFanProvider *m_activeFanProvider = nullptr;
  std::vector< ITempProvider * > m_activeTempProviders;
  IPlatformProvider *m_activePlatformProvider = nullptr;

  // Cached enumerations
  std::vector< FanInfo > m_fans;
  std::vector< TempSensorInfo > m_tempSensors;

  // System info
  ChassisType m_chassisType = ChassisType::Unknown;
  HwCapability m_capabilities = HwCapability::None;
};

} // namespace ucc::hal
