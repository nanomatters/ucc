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
#include "hal/IProfileProvider.hpp"
#include "hal/FanZone.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <syslog.h>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Fan provider that aggregates fans from multiple child providers.
 *
 * When more than one IFanProvider detects fans (e.g. hwmon for chassis fans
 * AND NVML for GPU fans), this composite merges them into a single provider
 * so the rest of the code (FanControlWorker, D-Bus) sees one flat fan list.
 */
class CompositeFanProvider : public IFanProvider
{
public:
  explicit CompositeFanProvider( std::vector< IFanProvider * > children )
    : m_children( std::move( children ) )
  {
    std::string n = "composite(";
    for ( size_t i = 0; i < m_children.size(); ++i )
    {
      if ( i > 0 ) n += '+';
      n += m_children[i]->name();
    }
    n += ')';
    m_name = std::move( n );

    for ( auto *child : m_children )
    {
      for ( auto &fan : child->enumerateFans() )
      {
        m_fanOwner[fan.id] = child;
        m_allFans.push_back( std::move( fan ) );
      }
    }
  }

  std::string name() const override { return m_name; }
  bool detect() override { return true; }
  int priority() const override { return 100; }
  std::vector< FanInfo > enumerateFans() override { return m_allFans; }

  std::optional< int > getFanRPM( const FanInfo &fan ) override
  {
    auto *p = ownerOf( fan );
    return p ? p->getFanRPM( fan ) : std::nullopt;
  }

  std::optional< int > getFanSpeedPercent( const FanInfo &fan ) override
  {
    auto *p = ownerOf( fan );
    return p ? p->getFanSpeedPercent( fan ) : std::nullopt;
  }

  bool setFanSpeedPercent( const FanInfo &fan, int percent ) override
  {
    auto *p = ownerOf( fan );
    return p ? p->setFanSpeedPercent( fan, percent ) : false;
  }

  bool setFanAuto( const FanInfo &fan ) override
  {
    auto *p = ownerOf( fan );
    return p ? p->setFanAuto( fan ) : false;
  }

  void refresh() override
  {
    for ( auto *c : m_children ) c->refresh();
  }

  void restoreAllAuto() override
  {
    for ( auto *c : m_children ) c->restoreAllAuto();
  }

  int getMinSpeedPercent( const FanInfo &fan ) override
  {
    auto *p = ownerOf( fan );
    return p ? p->getMinSpeedPercent( fan ) : 0;
  }

  bool canTurnOff( const FanInfo &fan ) override
  {
    auto *p = ownerOf( fan );
    return p ? p->canTurnOff( fan ) : false;
  }

private:
  IFanProvider *ownerOf( const FanInfo &fan )
  {
    auto it = m_fanOwner.find( fan.id );
    return it != m_fanOwner.end() ? it->second : nullptr;
  }

  std::vector< IFanProvider * > m_children;
  std::vector< FanInfo > m_allFans;
  std::map< std::string, IFanProvider * > m_fanOwner;
  std::string m_name;
};

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

  void addProfileProvider( std::unique_ptr< IProfileProvider > p )
  {
    m_profileProviders.push_back( std::move( p ) );
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

    // --- Fan providers (accumulate ALL that detect, merge via composite) ---
    {
      std::vector< IFanProvider * > detected;
      std::sort( m_fanProviders.begin(), m_fanProviders.end(),
                 []( const auto &a, const auto &b )
                 { return a->priority() > b->priority(); } );

      for ( auto &p : m_fanProviders )
      {
        if ( p->detect() )
        {
          syslog( LOG_INFO, "[HardwareManager] fan provider '%s' (priority %d) detected",
                  p->name().c_str(), p->priority() );
          detected.push_back( p.get() );
        }
      }

      if ( detected.size() == 1 )
      {
        m_activeFanProvider = detected[0];
      }
      else if ( detected.size() > 1 )
      {
        m_compositeFanProvider = std::make_unique< CompositeFanProvider >( std::move( detected ) );
        m_activeFanProvider = m_compositeFanProvider.get();
      }

      if ( !m_activeFanProvider )
        syslog( LOG_INFO, "[HardwareManager] No fan provider detected" );
    }

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

    // --- Platform providers (accumulate ALL that detect, like temp) ---
    m_activePlatformProviders.clear();
    m_activePlatformProvider = nullptr;
    {
      // Sort by priority descending so the first detected is highest priority
      std::sort( m_platformProviders.begin(), m_platformProviders.end(),
                 []( const auto &a, const auto &b )
                 { return a->priority() > b->priority(); } );

      for ( auto &p : m_platformProviders )
      {
        if ( p->detect() )
        {
          syslog( LOG_INFO, "[HardwareManager] platform provider '%s' (priority %d) detected",
                  p->name().c_str(), p->priority() );
          m_activePlatformProviders.push_back( p.get() );
          if ( !m_activePlatformProvider )
            m_activePlatformProvider = p.get(); // highest priority (sorted first)
        }
      }

      if ( m_activePlatformProviders.empty() )
        syslog( LOG_INFO, "[HardwareManager] No platform provider detected" );
      else
        syslog( LOG_INFO, "[HardwareManager] %zu platform providers detected (primary: '%s')",
                m_activePlatformProviders.size(), m_activePlatformProvider->name().c_str() );
    }

    // --- Profile providers ---
    selectBest( m_profileProviders, m_activeProfileProvider, "profile" );

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

    // Auto-generate thermal sources
    buildDefaultThermalSources();

    syslog( LOG_INFO, "[HardwareManager] %zu thermal sources generated",
            m_thermalSources.size() );
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

  /// Primary (highest-priority) platform provider (may be nullptr).
  /// Use this for OEM-specific features (ODM profiles, webcam, FnLock).
  IPlatformProvider *platformProvider() noexcept { return m_activePlatformProvider; }
  const IPlatformProvider *platformProvider() const noexcept { return m_activePlatformProvider; }

  /// All detected platform providers, ordered by priority (highest first).
  const std::vector< IPlatformProvider * > &platformProviders() const noexcept { return m_activePlatformProviders; }

  /**
   * @brief Find the best platform provider that supports TDP control.
   *
   * On laptops with an OEM driver (e.g. TuxedoIO), system-level TDP is
   * preferred over CPU-only TDP.  On desktops without an OEM driver, a
   * CPU-specific provider (RyzenAdj, RAPL) fills in.
   *
   * Returns the highest-priority detected provider whose
   * getNumberTDPs() > 0, or nullptr if none.
   */
  IPlatformProvider *tdpProvider() noexcept
  {
    for ( auto *p : m_activePlatformProviders )
      if ( p->getNumberTDPs() > 0 )
        return p;
    return nullptr;
  }
  const IPlatformProvider *tdpProvider() const noexcept
  {
    for ( auto *p : m_activePlatformProviders )
      if ( p->getNumberTDPs() > 0 )
        return p;
    return nullptr;
  }

  /// Active profile provider (may be nullptr — should not happen if GenericProfileProvider is registered)
  IProfileProvider *profileProvider() noexcept { return m_activeProfileProvider; }
  const IProfileProvider *profileProvider() const noexcept { return m_activeProfileProvider; }

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
  //  Thermal sources and fan zones
  // ---------------------------------------------------------------

  /// Auto-generated + custom thermal sources.
  const std::vector< ThermalSource > &thermalSources() const noexcept { return m_thermalSources; }

  /// Current fan zones (set externally by the daemon from profile data or defaults).
  const std::vector< FanZone > &defaultFanZones() const noexcept { return m_fanZones; }

  /// Replace the fan zone topology.
  /// Called by the daemon after loading profile zone data or building per-fan defaults.
  void setFanZones( std::vector< FanZone > zones ) { m_fanZones = std::move( zones ); }

  /// Register or update (custom) thermal sources from a profile.
  /// Existing sources with matching IDs are replaced so that strategy/sensor
  /// changes in a saved profile take effect without restarting the daemon.
  void addThermalSources( const std::vector< ThermalSource > &sources )
  {
    for ( const auto &src : sources )
    {
      auto it = std::find_if( m_thermalSources.begin(), m_thermalSources.end(),
                              [&]( const ThermalSource &ts ) { return ts.id == src.id; } );
      if ( it != m_thermalSources.end() )
        *it = src; // replace — strategy / sensorIds may have changed
      else
        m_thermalSources.push_back( src );
    }
  }

  /// Find a thermal source by id.
  const ThermalSource *findThermalSource( const std::string &id ) const noexcept
  {
    for ( const auto &ts : m_thermalSources )
      if ( ts.id == id ) return &ts;
    return nullptr;
  }

  /// Find a temp sensor by id (across all providers).
  const TempSensorInfo *findTempSensor( const std::string &id ) const noexcept
  {
    for ( const auto &s : m_tempSensors )
      if ( s.id == id ) return &s;
    return nullptr;
  }

  /// Read the effective temperature of a ThermalSource (resolves strategy).
  std::optional< double > readThermalSource( const ThermalSource &source ) const
  {
    if ( source.sensorIds.empty() )
      return std::nullopt;

    switch ( source.strategy )
    {
      case ThermalStrategy::Single:
      {
        auto *sensor = findTempSensor( source.sensorIds[0] );
        if ( sensor )
          return readTemp( *sensor );
        return std::nullopt;
      }

      case ThermalStrategy::Max:
      {
        double maxTemp = -1000.0;
        bool anyRead = false;
        for ( const auto &sid : source.sensorIds )
        {
          auto *sensor = findTempSensor( sid );
          if ( sensor )
          {
            auto val = readTemp( *sensor );
            if ( val.has_value() )
            {
              maxTemp = std::max( maxTemp, val.value() );
              anyRead = true;
            }
          }
        }
        return anyRead ? std::optional< double >( maxTemp ) : std::nullopt;
      }

      case ThermalStrategy::Average:
      {
        double sum = 0.0;
        double count = 0.0;
        for ( const auto &sid : source.sensorIds )
        {
          auto *sensor = findTempSensor( sid );
          if ( !sensor )
            continue;
          if ( auto val = readTemp( *sensor ); val.has_value() )
          {
            sum += val.value();
            count += 1.0;
          }
        }
        if ( count > 0.0 )
          return sum / count;
        return std::nullopt;
      }

      case ThermalStrategy::WeightedAvg:
      {
        double weightedSum = 0.0;
        double weightTotal = 0.0;
        for ( size_t i = 0; i < source.sensorIds.size(); ++i )
        {
          double w = ( i < source.weights.size() ) ? source.weights[i] : 1.0;
          auto *sensor = findTempSensor( source.sensorIds[i] );
          if ( sensor )
          {
            auto val = readTemp( *sensor );
            if ( val.has_value() )
            {
              weightedSum += val.value() * w;
              weightTotal += w;
            }
          }
        }
        if ( weightTotal > 0.0 )
          return weightedSum / weightTotal;
        return std::nullopt;
      }

      case ThermalStrategy::Percentile90:
      {
        std::vector< double > values;
        values.reserve( source.sensorIds.size() );
        for ( const auto &sid : source.sensorIds )
        {
          auto *sensor = findTempSensor( sid );
          if ( !sensor )
            continue;
          if ( auto val = readTemp( *sensor ); val.has_value() )
            values.push_back( val.value() );
        }

        if ( values.empty() )
          return std::nullopt;

        std::sort( values.begin(), values.end() );
        const size_t idx = static_cast< size_t >(
          std::clamp< int >( static_cast< int >( std::ceil( 0.90 * static_cast< double >( values.size() ) ) ) - 1,
                            0,
                            static_cast< int >( values.size() ) - 1 ) );
        return values[idx];
      }

      case ThermalStrategy::SafetyClampedAvg:
      {
        // Optional custom threshold via weights[0], default 85C.
        const double thresholdC = ( !source.weights.empty() && source.weights[0] > 0.0 )
                                ? source.weights[0]
                                : 85.0;

        double sum = 0.0;
        double count = 0.0;
        double maxTemp = -1000.0;

        for ( const auto &sid : source.sensorIds )
        {
          auto *sensor = findTempSensor( sid );
          if ( !sensor )
            continue;
          if ( auto val = readTemp( *sensor ); val.has_value() )
          {
            const double t = val.value();
            sum += t;
            count += 1.0;
            maxTemp = std::max( maxTemp, t );
          }
        }

        if ( count <= 0.0 )
          return std::nullopt;

        const double avg = sum / count;
        return ( maxTemp >= thresholdC ) ? std::optional< double >( maxTemp )
                                         : std::optional< double >( avg );
      }
    }
    return std::nullopt;
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

  static bool hasBattery() noexcept
  {
    std::error_code ec;
    for ( const auto &entry : std::filesystem::directory_iterator( "/sys/class/power_supply", ec ) )
    {
      if ( ec ) break;
      const auto name = entry.path().filename().string();
      if ( name.rfind( "BAT", 0 ) == 0 )
        return true;
    }
    return false;
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

    // Merge capabilities from ALL detected platform providers
    for ( auto *p : m_activePlatformProviders )
      caps |= p->capabilities();

    // Detect battery → multiple power states (AC/BAT/WC profiles)
    if ( hasBattery() )
      caps |= HwCapability::MultiplePowerStates;

    return caps;
  }

  // ---------------------------------------------------------------
  //  Default thermal-source / fan-zone builders
  // ---------------------------------------------------------------

  void buildDefaultThermalSources()
  {
    // No hardcoded thermal sources — all sources are user-created
    // (stored in profiles and merged via addThermalSources).
    m_thermalSources.clear();
  }

  // Registered providers (owned)
  std::vector< std::unique_ptr< IFanProvider > > m_fanProviders;
  std::vector< std::unique_ptr< ITempProvider > > m_tempProviders;
  std::vector< std::unique_ptr< IPlatformProvider > > m_platformProviders;
  std::vector< std::unique_ptr< IProfileProvider > > m_profileProviders;

  // Active (selected) providers (raw pointers into owned vectors)
  IFanProvider *m_activeFanProvider = nullptr;
  std::unique_ptr< CompositeFanProvider > m_compositeFanProvider;
  std::vector< ITempProvider * > m_activeTempProviders;
  std::vector< IPlatformProvider * > m_activePlatformProviders;
  IPlatformProvider *m_activePlatformProvider = nullptr; // highest-priority (primary)
  IProfileProvider *m_activeProfileProvider = nullptr;

  // Cached enumerations
  std::vector< FanInfo > m_fans;
  std::vector< TempSensorInfo > m_tempSensors;

  // Zone model
  std::vector< ThermalSource > m_thermalSources;
  std::vector< FanZone > m_fanZones;

  // System info
  ChassisType m_chassisType = ChassisType::Unknown;
  HwCapability m_capabilities = HwCapability::None;
};

} // namespace ucc::hal
