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

#include "DaemonWorker.hpp"
#include "../profiles/UccProfile.hpp"
#include "../profiles/FanProfile.hpp"
#include "../hal/HardwareManager.hpp"
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <functional>
#include <syslog.h>

/**
 * @brief Temperature filter using Exponentially Weighted Moving Average (EWMA)
 *
 * Replaces the old trimmed-mean buffer.  EWMA reacts faster to genuine
 * temperature changes while still rejecting single-sample noise.
 * Two different smoothing factors are used:
 *   - alphaRising  (0.5) — when new reading is above current estimate,
 *                           respond quickly to heating.
 *   - alphaFalling (0.15) — when new reading is below current estimate,
 *                           cool-down is smoothed more aggressively to
 *                           prevent premature fan-speed drops.
 */
class TemperatureFilter
{
public:
  TemperatureFilter()
    : m_value( -1.0 )
    , m_alphaRising( 0.5 )
    , m_alphaFalling( 0.15 )
  {}

  void addValue( int raw )
  {
    if ( m_value < 0.0 )
    {
      // First sample — initialise immediately
      m_value = static_cast< double >( raw );
      return;
    }

    const double alpha = ( raw > m_value ) ? m_alphaRising : m_alphaFalling;
    m_value = m_value + alpha * ( static_cast< double >( raw ) - m_value );
  }

  int getFilteredValue() const
  {
    return ( m_value < 0.0 ) ? 0 : static_cast< int >( std::round( m_value ) );
  }

private:
  double m_value;
  double m_alphaRising;   // weight for rising temperatures (fast response)
  double m_alphaFalling;  // weight for falling temperatures (slow decay)
};

/**
 * @brief Fan speed controller with interpolation, hysteresis, and EWMA smoothing
 *
 * Operates on a single fan curve (vector of FanCurvePoint).
 * One instance per fan zone.
 */
class FanControlLogic
{
public:
  explicit FanControlLogic( std::vector< ucc::hal::FanCurvePoint > curve,
                            int hysteresisDeg = 3,
                            ucc::hal::FanDeviceType deviceType = ucc::hal::FanDeviceType::Fan )
    : m_curve( std::move( curve ) )
    , m_hysteresisDeg( hysteresisDeg )
    , m_latestSpeedPercent( 0 )
    , m_smoothedSpeed( -1.0 )
    , m_lastEffectiveTemp( -1 )
    , m_fansMinSpeedHWLimit( 0 )
    , m_fansOffAvailable( true )
    , m_deviceType( deviceType )
  {
  }

  void setFansMinSpeedHWLimit( int speed )
  { m_fansMinSpeedHWLimit = std::clamp( speed, 0, 100 ); }

  void setFansOffAvailable( bool available )
  { m_fansOffAvailable = available; }

  void updateCurve( std::vector< ucc::hal::FanCurvePoint > curve )
  { m_curve = std::move( curve ); }

  void reportTemperature( int temperatureValue )
  {
    m_tempFilter.addValue( temperatureValue );
    m_latestSpeedPercent = calculateSpeedPercent();
  }

  int getSpeedPercent() const
  { return m_latestSpeedPercent; }

  const std::vector< ucc::hal::FanCurvePoint > &curve() const noexcept
  { return m_curve; }

private:
  int applyHysteresis( int filteredTemp )
  {
    if ( m_lastEffectiveTemp < 0 )
    {
      m_lastEffectiveTemp = filteredTemp;
      return filteredTemp;
    }

    if ( filteredTemp >= m_lastEffectiveTemp )
    {
      m_lastEffectiveTemp = filteredTemp;
    }
    else
    {
      int floor = filteredTemp + m_hysteresisDeg;
      int newEffective = std::min( m_lastEffectiveTemp, floor );
      newEffective = std::max( newEffective, filteredTemp );
      m_lastEffectiveTemp = newEffective;
    }

    return m_lastEffectiveTemp;
  }

  int applyHwFanLimitations( int speed ) const
  {
    const int minSpeed = m_fansMinSpeedHWLimit;
    const int halfMinSpeed = minSpeed / 2;

    if ( speed < minSpeed )
    {
      if ( m_fansOffAvailable && speed < halfMinSpeed )
        return 0;
      else if ( m_fansOffAvailable || speed >= halfMinSpeed )
        return minSpeed;
    }

    return speed;
  }

  int smoothSpeed( int targetSpeed )
  {
    static constexpr double ALPHA_UP   = 0.4;
    static constexpr double ALPHA_DOWN = 0.08;

    if ( m_smoothedSpeed < 0.0 )
    {
      m_smoothedSpeed = static_cast< double >( targetSpeed );
      return targetSpeed;
    }

    const double alpha = ( targetSpeed > m_smoothedSpeed ) ? ALPHA_UP : ALPHA_DOWN;
    m_smoothedSpeed = m_smoothedSpeed + alpha * ( static_cast< double >( targetSpeed ) - m_smoothedSpeed );

    return static_cast< int >( std::round( m_smoothedSpeed ) );
  }

  int manageCriticalTemperature( int temp, int speed ) const
  {
    // Pumps follow their curve unconditionally — the user sets pump curves
    // deliberately and overriding them at high temps is counter-productive.
    if ( m_deviceType == ucc::hal::FanDeviceType::Pump
      || m_deviceType == ucc::hal::FanDeviceType::StagedPump )
      return speed;

    constexpr int CRITICAL_TEMPERATURE = 85;
    constexpr int OVERHEAT_TEMPERATURE = 90;

    if ( temp >= OVERHEAT_TEMPERATURE )
      return 100;
    else if ( temp >= CRITICAL_TEMPERATURE )
      return std::max( speed, 80 );

    return speed;
  }

  int calculateSpeedPercent()
  {
    const int filteredTemp = m_tempFilter.getFilteredValue();
    const int effectiveTemp = applyHysteresis( filteredTemp );

    int curveSpeed = ucc::hal::interpolateCurve( m_curve, effectiveTemp );
    if ( curveSpeed < 0 ) curveSpeed = 0;
    curveSpeed = std::clamp( curveSpeed, 0, 100 );

    curveSpeed = applyHwFanLimitations( curveSpeed );
    int speed = smoothSpeed( curveSpeed );
    // speed = manageCriticalTemperature( filteredTemp, speed );

    return speed;
  }

  std::vector< ucc::hal::FanCurvePoint > m_curve;
  int m_hysteresisDeg;
  TemperatureFilter m_tempFilter;
  int m_latestSpeedPercent;
  double m_smoothedSpeed;
  int m_lastEffectiveTemp;

  int m_fansMinSpeedHWLimit;
  bool m_fansOffAvailable;
  ucc::hal::FanDeviceType m_deviceType;
};

/**
 * @brief Zone-driven fan control worker.
 *
 * Runs one FanControlLogic per hardware zone.  Each zone reads its
 * ThermalSource, computes a speed from its curve, and writes to all
 * fans assigned to that zone.
 */
class FanControlWorker : public DaemonWorker
{
public:
  FanControlWorker(
    ucc::hal::HardwareManager &hw,
    std::function< UccProfile() > getActiveProfile,
    std::function< bool() > getFanControlEnabled,
    std::function< void( size_t, int64_t, int ) > updateFanSpeed,
    std::function< void( size_t, int64_t, int ) > updateFanTemp,
    std::function< FanProfile( const std::string & ) > resolveFanProfile,
    std::function< void( const std::string &, int64_t, int, int, int ) > updateZoneTelemetry = nullptr
  )
    : DaemonWorker( std::chrono::milliseconds( 1000 ) )
    , m_hw( hw )
    , m_getActiveProfile( getActiveProfile )
    , m_getFanControlEnabled( getFanControlEnabled )
    , m_updateFanSpeed( updateFanSpeed )
    , m_updateFanTemp( updateFanTemp )
    , m_resolveFanProfile( resolveFanProfile )
    , m_updateZoneTelemetry( std::move( updateZoneTelemetry ) )
    , m_controlAvailableMessageShown( false )
    , m_hasTemporaryCurves( false )
  {
  }

  ~FanControlWorker() override = default;

  /**
   * @brief Apply temporary per-zone curves (overrides profile curves until cleared).
   * @param zoneCurves Map of zoneId → curve points
   */
  void applyTemporaryZoneCurves( const std::map< std::string, std::vector< ucc::hal::FanCurvePoint > > &zoneCurves )
  {
    m_tempZoneCurves = zoneCurves;
    m_hasTemporaryCurves = true;

    // Update existing logics immediately
    for ( auto &[zoneId, logic] : m_zoneLogics )
    {
      auto it = zoneCurves.find( zoneId );
      if ( it != zoneCurves.end() && !it->second.empty() )
        logic.updateCurve( it->second );
    }
  }

  void clearTemporaryCurves()
  {
    m_hasTemporaryCurves = false;
    m_tempZoneCurves.clear();
  }

  [[nodiscard]] bool hasTemporaryCurves() const noexcept { return m_hasTemporaryCurves; }

  /**
   * @brief Update the thermal source for one or more zones.
   * @param sources Map of zoneId → thermalSourceId
   *
   * This allows profiles to override which temperature sensor drives each zone
   * without restarting the worker.
   */
  void applyZoneThermalSources( const std::map< std::string, std::string > &sources )
  {
    for ( auto &zone : m_zoneInfos )
    {
      auto it = sources.find( zone.id );
      if ( it != sources.end() && !it->second.empty() )
        zone.thermalSourceId = it->second;
    }
  }

  [[nodiscard]] const std::map< std::string, std::vector< ucc::hal::FanCurvePoint > > &
  tempZoneCurves() const noexcept { return m_tempZoneCurves; }

protected:
  void onStart() override
  {
    auto *fanProvider = m_hw.fanProvider();
    if ( !fanProvider )
    {
      syslog( LOG_INFO, "FanControlWorker: No fan provider available" );
      return;
    }

    m_providerFans = fanProvider->enumerateFans();
    if ( m_providerFans.empty() )
    {
      syslog( LOG_INFO, "FanControlWorker: No fans detected" );
      return;
    }

    // Build a fan-index lookup for publishing callbacks
    m_fanIndexMap.clear();
    for ( size_t i = 0; i < m_providerFans.size(); ++i )
      m_fanIndexMap[m_providerFans[i].id] = i;

    // Get the default zones from HardwareManager
    const auto &hwZones = m_hw.defaultFanZones();

    // Resolve the active fan profile's curves
    auto profile = m_getActiveProfile();
    FanProfile fanProfile = m_resolveFanProfile( profile.fan.fanProfile );

    // Get hardware fan limits
    m_fansMinSpeedHWLimit = 0;
    m_fansOffAvailable = true;
    if ( !m_providerFans.empty() )
    {
      m_fansMinSpeedHWLimit = fanProvider->getMinSpeedPercent( m_providerFans[0] );
      m_fansOffAvailable = fanProvider->canTurnOff( m_providerFans[0] );
    }

    // Create one FanControlLogic per zone
    m_zoneLogics.clear();
    m_zoneInfos.clear();

    for ( const auto &hwZone : hwZones )
    {
      if ( !hwZone.enabled )
        continue;

      // Use profile curve if available, else hardware default curve
      auto curve = hwZone.curve;
      const auto *profileCurve = fanProfile.findZoneCurve( hwZone.id );
      if ( profileCurve && !profileCurve->curve.empty() )
        curve = profileCurve->curve;

      FanControlLogic logic( curve, hwZone.hysteresisDeg, hwZone.defaultType );
      logic.setFansMinSpeedHWLimit( m_fansMinSpeedHWLimit );
      logic.setFansOffAvailable( m_fansOffAvailable );

      m_zoneLogics.emplace( hwZone.id, std::move( logic ) );
      m_zoneInfos.push_back( hwZone );
    }

    syslog( LOG_INFO, "FanControlWorker started with %zu zones, %zu fans (provider: %s)",
            m_zoneLogics.size(), m_providerFans.size(), fanProvider->name().c_str() );
  }

  void onWork() override
  {
    if ( m_zoneLogics.empty() )
    {
      if ( !m_controlAvailableMessageShown )
      {
        syslog( LOG_INFO, "FanControlWorker: Control unavailable (no zones)" );
        m_controlAvailableMessageShown = true;
      }
      return;
    }

    if ( m_controlAvailableMessageShown )
    {
      syslog( LOG_INFO, "FanControlWorker: Control resumed" );
      m_controlAvailableMessageShown = false;
    }

    // Update curves from profile if changed
    auto profile = m_getActiveProfile();
    if ( !profile.id.empty() )
      updateZoneLogicsFromProfile( profile );

    const bool useFanControl = m_getFanControlEnabled();
    const int64_t timestamp = std::chrono::duration_cast< std::chrono::milliseconds >(
      std::chrono::system_clock::now().time_since_epoch() ).count();

    auto *fanProvider = m_hw.fanProvider();
    if ( !fanProvider )
      return;

    // Per-fan tracking for publishing
    std::vector< int > fanTemps( m_providerFans.size(), -1 );
    std::vector< int > fanSpeeds( m_providerFans.size(), 0 );
    // Tracks whether each fan slot was claimed by a zone.
    // A fan claimed by a zone but with no thermal source should run at 0,
    // not inherit highestSpeed.  Only truly unzoned fans fall back.
    std::vector< bool > fanHasZone( m_providerFans.size(), false );

    int highestSpeed = 0;

    // Process each zone — collect per-zone telemetry alongside per-fan data
    struct ZoneTelemetryData { int temp; int speed; int rpm = -1; };
    std::vector< std::pair< std::string, ZoneTelemetryData > > zoneTelemetry;

    for ( const auto &zone : m_zoneInfos )
    {
      auto logicIt = m_zoneLogics.find( zone.id );
      if ( logicIt == m_zoneLogics.end() )
        continue;

      // Read temperature from the zone's thermal source
      int tempCelsius = -1;
      bool tempReadSuccess = false;
      const auto *thermalSource = m_hw.findThermalSource( zone.thermalSourceId );
      if ( thermalSource )
      {
        auto val = m_hw.readThermalSource( *thermalSource );
        if ( val.has_value() )
        {
          tempCelsius = static_cast< int >( std::round( val.value() ) );
          tempReadSuccess = true;
        }
      }

      int zoneSpeed = 0;
      if ( tempReadSuccess )
      {
        logicIt->second.reportTemperature( tempCelsius );
        zoneSpeed = logicIt->second.getSpeedPercent();
      }

      if ( zoneSpeed > highestSpeed )
        highestSpeed = zoneSpeed;

      zoneTelemetry.push_back( { zone.id, { tempCelsius, zoneSpeed } } );

      // Apply speed to all fans in this zone and record for publishing
      for ( const auto &fanId : zone.fanIds )
      {
        auto indexIt = m_fanIndexMap.find( fanId );
        if ( indexIt == m_fanIndexMap.end() )
          continue;

        size_t fanIndex = indexIt->second;
        fanTemps[fanIndex] = tempCelsius;
        fanSpeeds[fanIndex] = zoneSpeed;
        fanHasZone[fanIndex] = true;
      }
    }

    // Write fan speeds
    if ( useFanControl )
    {
      for ( size_t i = 0; i < m_providerFans.size(); ++i )
      {
        int speedToSet = fanSpeeds[i];

        // Fan has no zone at all → safety fallback to highest active zone speed.
        // Fans that belong to a zone but whose zone has no thermal source keep
        // their computed speed (0) — the user's curve must not be overridden.
        if ( !fanHasZone[i] )
        {
          speedToSet = highestSpeed;
          fanSpeeds[i] = highestSpeed;
        }

        fanProvider->setFanSpeedPercent( m_providerFans[i], speedToSet );
      }
    }

    // Read back actual RPMs from hardware (always, regardless of control mode)
    std::vector< int > fanRpms( m_providerFans.size(), -1 );
    for ( size_t i = 0; i < m_providerFans.size(); ++i )
    {
      auto rpm = fanProvider->getFanRPM( m_providerFans[i] );
      if ( rpm.has_value() )
        fanRpms[i] = rpm.value();
    }

    // Backfill per-zone average RPM into zoneTelemetry
    for ( auto &[zoneId, zt] : zoneTelemetry )
    {
      for ( const auto &zone : m_zoneInfos )
      {
        if ( zone.id != zoneId )
          continue;
        int rpmSum = 0, rpmCount = 0;
        for ( const auto &fanId : zone.fanIds )
        {
          auto indexIt = m_fanIndexMap.find( fanId );
          if ( indexIt == m_fanIndexMap.end() ) continue;
          int r = fanRpms[indexIt->second];
          if ( r >= 0 ) { rpmSum += r; ++rpmCount; }
        }
        zt.rpm = rpmCount > 0 ? rpmSum / rpmCount : -1;
        break;
      }
    }

    // Publish per-fan data via callbacks
    for ( size_t i = 0; i < m_providerFans.size(); ++i )
    {
      int currentSpeed;

      if ( fanTemps[i] == -1 )
      {
        currentSpeed = -1;
      }
      else if ( useFanControl )
      {
        currentSpeed = fanSpeeds[i];
      }
      else
      {
        int hwSpeed = -1;
        auto pct = fanProvider->getFanSpeedPercent( m_providerFans[i] );
        if ( pct.has_value() )
          hwSpeed = pct.value();
        currentSpeed = hwSpeed;
      }

      m_updateFanTemp( i, timestamp, fanTemps[i] );
      m_updateFanSpeed( i, timestamp, currentSpeed );
    }

    // Publish per-zone telemetry
    if ( m_updateZoneTelemetry )
    {
      for ( const auto &[zoneId, zt] : zoneTelemetry )
        m_updateZoneTelemetry( zoneId, timestamp, zt.temp, zt.speed, zt.rpm );
    }
  }

  void onExit() override {}

private:
  void updateZoneLogicsFromProfile( const UccProfile &profile )
  {
    // Resolve the fan profile
    FanProfile fanProfile = m_resolveFanProfile( profile.fan.fanProfile );

    // Update each zone's curve
    for ( auto &[zoneId, logic] : m_zoneLogics )
    {
      // Temporary curves take precedence
      if ( m_hasTemporaryCurves )
      {
        auto it = m_tempZoneCurves.find( zoneId );
        if ( it != m_tempZoneCurves.end() && !it->second.empty() )
        {
          logic.updateCurve( it->second );
          continue;
        }
      }

      // Use profile curve if available
      const auto *profileCurve = fanProfile.findZoneCurve( zoneId );
      if ( profileCurve && !profileCurve->curve.empty() )
        logic.updateCurve( profileCurve->curve );
    }
  }

  ucc::hal::HardwareManager &m_hw;
  std::function< UccProfile() > m_getActiveProfile;
  std::function< bool() > m_getFanControlEnabled;
  std::function< void( size_t, int64_t, int ) > m_updateFanSpeed;
  std::function< void( size_t, int64_t, int ) > m_updateFanTemp;
  std::function< FanProfile( const std::string & ) > m_resolveFanProfile;
  std::function< void( const std::string &, int64_t, int, int, int ) > m_updateZoneTelemetry;

  // Zone-based control
  std::map< std::string, FanControlLogic > m_zoneLogics;
  std::vector< ucc::hal::FanZone > m_zoneInfos;

  // Provider data
  std::vector< ucc::hal::FanInfo > m_providerFans;
  std::map< std::string, size_t > m_fanIndexMap;  // fanId → index in m_providerFans

  bool m_controlAvailableMessageShown;
  int m_fansMinSpeedHWLimit = 0;
  bool m_fansOffAvailable = true;

  // Temporary zone curve overrides
  bool m_hasTemporaryCurves;
  std::map< std::string, std::vector< ucc::hal::FanCurvePoint > > m_tempZoneCurves;
};
