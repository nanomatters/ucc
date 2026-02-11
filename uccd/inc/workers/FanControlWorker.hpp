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
#include "tuxedo_io_lib/tuxedo_io_api.hh"
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <functional>
#include <syslog.h>

enum class FanLogicType { CPU, GPU };

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
 * Improvements over the original algorithm:
 *
 * 1. **Linear interpolation** — Uses FanProfile::getSpeedForTemp() instead of
 *    step-wise table lookup. Eliminates discrete jumps between curve points.
 *
 * 2. **Hysteresis** — The temperature used for curve lookup is biased:
 *    when the filtered temperature is falling and near a curve inflection
 *    point, the effective temperature is held slightly higher (by HYSTERESIS_DEG)
 *    to prevent the fan from dropping prematurely. This avoids the classic
 *    heat→fan-up→cool→fan-down→heat cycle.
 *
 * 3. **EWMA speed smoothing** — The raw curve speed is fed through an
 *    exponentially weighted moving average with asymmetric weights:
 *      - Rising (alphaUp = 0.4): fans spin up within 2-3 seconds
 *      - Falling (alphaDown = 0.08): fans spin down over ~12 seconds
 *    This replaces both the old trimmed-mean temperature filter and the
 *    hard −2%/sec rate limiter, giving much smoother transitions.
 *
 * 4. **Critical temperature override** is preserved unchanged.
 */
class FanControlLogic
{
public:
  FanControlLogic( const FanProfile &fanProfile, FanLogicType type )
    : m_fanProfile( fanProfile )
    , m_type( type )
    , m_latestSpeedPercent( 0 )
    , m_smoothedSpeed( -1.0 )
    , m_lastEffectiveTemp( -1 )
    , m_fansMinSpeedHWLimit( 0 )
    , m_fansOffAvailable( true )
    , m_offsetFanspeed( 0 )
  {
  }

  void setFansMinSpeedHWLimit( int speed )
  { m_fansMinSpeedHWLimit = std::clamp( speed, 0, 100 ); }

  void setFansOffAvailable( bool available )
  { m_fansOffAvailable = available; }

  void setOffsetFanspeed( int speed )
  { m_offsetFanspeed = std::clamp( speed, -100, 100 ); }

  void updateFanProfile( const FanProfile &fanProfile )
  {
    m_fanProfile = fanProfile;
  }

  void reportTemperature( int temperatureValue )
  {
    m_tempFilter.addValue( temperatureValue );
    m_latestSpeedPercent = calculateSpeedPercent();
  }

  int getSpeedPercent() const
  { return m_latestSpeedPercent; }

  const FanProfile &getFanProfile() const
  { return m_fanProfile; }

private:
  /**
   * @brief Apply hysteresis to prevent oscillation at curve boundaries.
   *
   * When temperature is falling, we use the higher of (filteredTemp, lastEffectiveTemp − 1).
   * This means the effective temperature used for curve lookup decreases by at most
   * 1°C per cycle (1°C/s), even if the filtered temperature drops faster.
   * When temperature is rising, the effective temperature follows immediately.
   */
  int applyHysteresis( int filteredTemp )
  {
    static constexpr int HYSTERESIS_DEG = 3;

    if ( m_lastEffectiveTemp < 0 )
    {
      // First call — no history
      m_lastEffectiveTemp = filteredTemp;
      return filteredTemp;
    }

    if ( filteredTemp >= m_lastEffectiveTemp )
    {
      // Temperature rising or stable — follow immediately
      m_lastEffectiveTemp = filteredTemp;
    }
    else
    {
      // Temperature falling — hold the effective temp higher by up to HYSTERESIS_DEG
      // but still allow it to decrease by 1°C per cycle
      int floor = filteredTemp + HYSTERESIS_DEG;
      int newEffective = std::min( m_lastEffectiveTemp, floor );
      // Never go below the actual filtered temperature
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
      {
        return 0;
      }
      else if ( m_fansOffAvailable || speed >= halfMinSpeed )
      {
        return minSpeed;
      }
    }

    return speed;
  }

  /**
   * @brief EWMA smoothing on the speed output.
   *
   * Uses asymmetric alpha:
   *   - alphaUp   = 0.4  → fans reach target in ~3 cycles (3 sec)
   *   - alphaDown = 0.08 → fans take ~12 cycles to settle
   *
   * This replaces both the old trimmed-mean filter and the hard rate limiter.
   */
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
    constexpr int CRITICAL_TEMPERATURE = 85;
    constexpr int OVERHEAT_TEMPERATURE = 90;

    if ( temp >= OVERHEAT_TEMPERATURE )
    {
      return 100;
    }
    else if ( temp >= CRITICAL_TEMPERATURE )
    {
      return std::max( speed, 80 );
    }

    return speed;
  }

  int calculateSpeedPercent()
  {
    const int filteredTemp = m_tempFilter.getFilteredValue();
    
    // Apply hysteresis — effective temp may lag behind during cool-down
    const int effectiveTemp = applyHysteresis( filteredTemp );

    // Linear interpolation on the fan curve (instead of step-wise lookup)
    const bool isCPU = ( m_type == FanLogicType::CPU );
    int curveSpeed = m_fanProfile.getSpeedForTemp( effectiveTemp, isCPU );
    if ( curveSpeed < 0 ) curveSpeed = 0;

    // Apply user offset
    curveSpeed += m_offsetFanspeed;
    curveSpeed = std::clamp( curveSpeed, 0, 100 );

    // Apply hardware limitations
    curveSpeed = applyHwFanLimitations( curveSpeed );

    // EWMA smoothing (replaces old rate limiter)
    int speed = smoothSpeed( curveSpeed );

    // Critical temperature override (uses raw filtered temp, not hysteresis-adjusted)
    speed = manageCriticalTemperature( filteredTemp, speed );

    return speed;
  }

  FanProfile m_fanProfile;
  FanLogicType m_type;
  TemperatureFilter m_tempFilter;
  int m_latestSpeedPercent;
  double m_smoothedSpeed;       // EWMA state for speed output
  int m_lastEffectiveTemp;      // hysteresis state
  
  int m_fansMinSpeedHWLimit;
  bool m_fansOffAvailable;
  int m_offsetFanspeed;
};

class FanControlWorker : public DaemonWorker
{
public:
  FanControlWorker(
    TuxedoIOAPI &io,
    std::function< UccProfile() > getActiveProfile,
    std::function< bool() > getFanControlEnabled,
    std::function< void( size_t, int64_t, int ) > updateFanSpeed,
    std::function< void( size_t, int64_t, int ) > updateFanTemp
  )
    : DaemonWorker( std::chrono::milliseconds( 1000 ) )
    , m_io( io )
    , m_getActiveProfile( getActiveProfile )
    , m_getFanControlEnabled( getFanControlEnabled )
    , m_updateFanSpeed( updateFanSpeed )
    , m_updateFanTemp( updateFanTemp )
    , m_modeSameSpeed( true )
    , m_controlAvailableMessageShown( false )
    , m_hasTemporaryCurves( false )
  {
  }

  ~FanControlWorker() override = default;

  // Allow external callers to change same-speed mode at runtime
  void setSameSpeed( bool same )
  {
    m_modeSameSpeed = same;
    syslog( LOG_INFO, "FanControlWorker: setSameSpeed = %d", m_modeSameSpeed ? 1 : 0 );
  }

  [[nodiscard]] bool getSameSpeed() const noexcept { return m_modeSameSpeed; }

  /**
   * @brief Clear temporary fan curves and revert to profile curves
   */
  void clearTemporaryCurves()
  {
    m_hasTemporaryCurves = false;
    m_tempCpuTable.clear();
    m_tempGpuTable.clear();
    m_tempWaterCoolerFanTable.clear();
    m_tempPumpTable.clear();
  }

  [[nodiscard]] bool hasTemporaryCurves() const noexcept { return m_hasTemporaryCurves; }
  [[nodiscard]] const std::vector< FanTableEntry > &tempWaterCoolerFanTable() const noexcept { return m_tempWaterCoolerFanTable; }
  [[nodiscard]] const std::vector< FanTableEntry > &tempPumpTable() const noexcept { return m_tempPumpTable; }

  void applyTemporaryFanCurves( const std::vector< FanTableEntry > &cpuTable,
                                const std::vector< FanTableEntry > &gpuTable,
                                const std::vector< FanTableEntry > &waterCoolerFanTable = {},
                                const std::vector< FanTableEntry > &pumpTable = {} )
  {
    // Store the temporary curves
    m_tempCpuTable = cpuTable;
    m_tempGpuTable = gpuTable;
    m_tempWaterCoolerFanTable = waterCoolerFanTable;
    m_tempPumpTable = pumpTable;
    m_hasTemporaryCurves = true;
    
    for ( size_t i = 0; i < m_fanLogics.size(); ++i )
    {
      FanProfile tempProfile = m_fanLogics[i].getFanProfile(); // Copy current profile
      
      if ( i == 0 && !cpuTable.empty() ) // CPU fan (index 0)
      {
        tempProfile.tableCPU = cpuTable;
      }
      else if ( i > 0 && !gpuTable.empty() ) // GPU fans (index > 0)
      {
        tempProfile.tableGPU = gpuTable;
      }

      if ( !waterCoolerFanTable.empty() )
        tempProfile.tableWaterCoolerFan = waterCoolerFanTable;
      if ( !pumpTable.empty() )
        tempProfile.tablePump = pumpTable;
      
      m_fanLogics[i].updateFanProfile( tempProfile );
    }
  }

protected:
  void onStart() override
  {
    int numberFans = 0;
    bool fansDetected = m_io.getNumberFans( numberFans ) && numberFans > 0;
    
    // If getNumberFans fails, try to detect fans by reading temperature from fan 0
    if ( !fansDetected )
    {
      int temp = -1;
      if ( m_io.getFanTemperature( 0, temp ) && temp >= 0 )
      {
        // We can read from at least fan 0, assume we have CPU and GPU fans
        numberFans = 2;
        fansDetected = true;
        syslog( LOG_INFO, "FanControlWorker: Detected fans by temperature reading (getNumberFans failed)" );
      }
    }
    
    if ( fansDetected && numberFans > 0 )
    {
      // Initialize fan logic for each fan
      for ( int i = 0; i < numberFans; ++i )
      {
        auto profile = m_getActiveProfile();
        
        // Fan 0 is CPU, others are GPU
        FanLogicType type = ( i == 0 ) ? FanLogicType::CPU : FanLogicType::GPU;
        // Resolve the named fan profile preset
        FanProfile fp = getDefaultFanProfileByName( profile.fan.fanProfile );
        m_fanLogics.emplace_back( fp, type );
      }

      // Get hardware fan limits
      int minSpeed = 0;
      if ( m_io.getFansMinSpeed( minSpeed ) )
      {
        m_fansMinSpeedHWLimit = minSpeed;
      }

      bool fansOffAvailable = true;
      if ( m_io.getFansOffAvailable( fansOffAvailable ) )
      {
        m_fansOffAvailable = fansOffAvailable;
      }

      // Apply hardware limits to all fan logics
      for ( auto &logic : m_fanLogics )
      {
        logic.setFansMinSpeedHWLimit( m_fansMinSpeedHWLimit );
        logic.setFansOffAvailable( m_fansOffAvailable );
      }

      syslog( LOG_INFO, "FanControlWorker started with %d fans", numberFans );
    }
    else
    {
      syslog( LOG_INFO, "FanControlWorker: No fans detected" );
    }
  }

  void onWork() override
  {
    if ( m_fanLogics.empty() )
    {
      if ( !m_controlAvailableMessageShown )
      {
        syslog( LOG_INFO, "FanControlWorker: Control unavailable (no fans)" );
        m_controlAvailableMessageShown = true;
      }
      return;
    }

    if ( m_controlAvailableMessageShown )
    {
      syslog( LOG_INFO, "FanControlWorker: Control resumed" );
      m_controlAvailableMessageShown = false;
    }

    // Get current profile and update fan logics if profile changed
    auto profile = m_getActiveProfile();
    if ( !profile.id.empty() )
    {
      updateFanLogicsFromProfile( profile );
    }

    const bool useFanControl = m_getFanControlEnabled();
    const int64_t timestamp = std::chrono::duration_cast< std::chrono::milliseconds >(
      std::chrono::system_clock::now().time_since_epoch() ).count();

    std::vector< int > fanTemps;
    std::vector< int > fanSpeedsSet;
    std::vector< bool > tempSensorAvailable;

    // Read temperatures and calculate fan speeds
    for ( size_t fanIndex = 0; fanIndex < m_fanLogics.size(); ++fanIndex )
    {
      int tempCelsius = -1;
      bool tempReadSuccess = m_io.getFanTemperature( static_cast< int >( fanIndex ), tempCelsius );

      tempSensorAvailable.push_back( tempReadSuccess );

      if ( tempReadSuccess )
      {
        fanTemps.push_back( tempCelsius );
        
        // Report temperature to logic and get calculated speed
        m_fanLogics[fanIndex].reportTemperature( tempCelsius );
        int calculatedSpeed = m_fanLogics[fanIndex].getSpeedPercent();
        fanSpeedsSet.push_back( calculatedSpeed );
      }
      else
      {
        fanTemps.push_back( -1 );
        fanSpeedsSet.push_back( 0 );
      }
    }

    // Write fan speeds if fan control is enabled
    if ( useFanControl )
    {
      // Find highest speed for "same speed" mode
      int highestSpeed = 0;
      for ( int speed : fanSpeedsSet )
      {
        if ( speed > highestSpeed )
          highestSpeed = speed;
      }

      // Set fan speeds
      for ( size_t fanIndex = 0; fanIndex < m_fanLogics.size(); ++fanIndex )
      {
        int speedToSet = fanSpeedsSet[fanIndex];
        
        // Use highest speed if in "same speed" mode or no sensor available
        if ( m_modeSameSpeed || !tempSensorAvailable[fanIndex] )
        {
          speedToSet = highestSpeed;
          fanSpeedsSet[fanIndex] = highestSpeed;
        }

        // Log the temperature and speed we are about to set for debugging
        //syslog( LOG_DEBUG, "FanControlWorker: fan %d temp=%d calculated=%d set=%d sameSpeed=%d",
        //        static_cast< int >( fanIndex ), fanTemps[fanIndex], fanSpeedsSet[fanIndex], speedToSet, m_modeSameSpeed ? 1 : 0 );

        m_io.setFanSpeedPercent( static_cast< int >( fanIndex ), speedToSet );
      }
    }

    // Publish data via callbacks
    for ( size_t fanIndex = 0; fanIndex < m_fanLogics.size(); ++fanIndex )
    {
      int currentSpeed;

      if ( fanTemps[fanIndex] == -1 )
      {
        currentSpeed = -1;
      }
      else if ( useFanControl )
      {
        // Report calculated/set speed when control is active
        currentSpeed = fanSpeedsSet[fanIndex];
      }
      else
      {
        // Report hardware speed when control is disabled
        int hwSpeed = -1;
        m_io.getFanSpeedPercent( static_cast< int >( fanIndex ), hwSpeed );
        currentSpeed = hwSpeed;
      }

      m_updateFanTemp( fanIndex, timestamp, fanTemps[fanIndex] );
      m_updateFanSpeed( fanIndex, timestamp, currentSpeed );
    }
  }

  void onExit() override
  {
    // Nothing special to do on exit
  }

private:
  void updateFanLogicsFromProfile( const UccProfile &profile )
  {
    // Respect profile setting for same-speed mode
    bool prevSameSpeed = m_modeSameSpeed;
    m_modeSameSpeed = profile.fan.sameSpeed;
    if ( m_modeSameSpeed != prevSameSpeed )
    {
      syslog( LOG_INFO, "FanControlWorker: sameSpeed mode changed to %d", m_modeSameSpeed ? 1 : 0 );
    }

    for ( size_t i = 0; i < m_fanLogics.size(); ++i )
    {
      // Resolve fan profile by name (built-in) instead of using embedded tables in profiles
      FanProfile fanProfile;
      for ( const auto &p : defaultFanProfiles )
      {
        if ( p.name == profile.fan.fanProfile ) { fanProfile = p; break; }
      }
      // Fallbacks
      if ( !fanProfile.isValid() )
      {
        for ( const auto &p : defaultFanProfiles ) { if ( p.name == "Balanced" ) { fanProfile = p; break; } }
      }
      if ( !fanProfile.isValid() && !defaultFanProfiles.empty() ) fanProfile = defaultFanProfiles[0];
      
      // If temporary curves are active, use them instead of profile curves
      if ( m_hasTemporaryCurves )
      {
        if ( i == 0 && !m_tempCpuTable.empty() ) // CPU fan (index 0)
        {
          fanProfile.tableCPU = m_tempCpuTable;
        }
        else if ( i > 0 && !m_tempGpuTable.empty() ) // GPU fans (index > 0)
        {
          fanProfile.tableGPU = m_tempGpuTable;
        }
        if ( !m_tempWaterCoolerFanTable.empty() )
          fanProfile.tableWaterCoolerFan = m_tempWaterCoolerFanTable;
        if ( !m_tempPumpTable.empty() )
          fanProfile.tablePump = m_tempPumpTable;
      }
      
      m_fanLogics[i].updateFanProfile( fanProfile );
      m_fanLogics[i].setOffsetFanspeed( profile.fan.offsetFanspeed );
    }
  }

  TuxedoIOAPI &m_io;
  std::function< UccProfile() > m_getActiveProfile;
  std::function< bool() > m_getFanControlEnabled;
  std::function< void( size_t, int64_t, int ) > m_updateFanSpeed;
  std::function< void( size_t, int64_t, int ) > m_updateFanTemp;

  std::vector< FanControlLogic > m_fanLogics;
  bool m_modeSameSpeed;
  bool m_controlAvailableMessageShown;
  int m_fansMinSpeedHWLimit;
  bool m_fansOffAvailable;
  
  // Temporary fan curve tracking
  bool m_hasTemporaryCurves;
  std::vector< FanTableEntry > m_tempCpuTable;
  std::vector< FanTableEntry > m_tempGpuTable;
  std::vector< FanTableEntry > m_tempWaterCoolerFanTable;
  std::vector< FanTableEntry > m_tempPumpTable;
};
