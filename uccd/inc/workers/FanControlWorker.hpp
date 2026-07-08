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
#include "../UniwillSysfs.hpp"
#include <atomic>
#include <vector>
#include <algorithm>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <syslog.h>
#include <utility>
#include <cmath>

enum class FanLogicType { CPU, GPU };

/**
 * @brief Temperature filter using Exponentially Weighted Moving Average (EWMA).
 *
 * alphaRising (0.5) responds quickly to heating; alphaFalling (0.15) smooths
 * cool-down more aggressively to prevent premature fan-speed drops.
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
  double m_alphaRising;
  double m_alphaFalling;
};

/**
 * @brief Fan-speed controller: interpolation + hysteresis + EWMA speed
 * smoothing + hardware min/off limits + critical-temperature safety override.
 * Ported from the pre-uniwill baseline (main) so manual-mode control keeps the
 * same closed-loop behaviour on top of the uniwill hwmon manual PWM ABI.
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
  {
  }

  void setFansMinSpeedHWLimit( int speed )
  { m_fansMinSpeedHWLimit = std::clamp( speed, 0, 100 ); }

  void setFansOffAvailable( bool available )
  { m_fansOffAvailable = available; }

  void updateFanProfile( const FanProfile &fanProfile )
  { m_fanProfile = fanProfile; }

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
  int applyHysteresis( int filteredTemp )
  {
    static constexpr int HYSTERESIS_DEG = 3;

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
      int floor = filteredTemp + HYSTERESIS_DEG;
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
    const int effectiveTemp = applyHysteresis( filteredTemp );

    const bool isCPU = ( m_type == FanLogicType::CPU );
    int curveSpeed = m_fanProfile.getSpeedForTemp( effectiveTemp, isCPU );
    if ( curveSpeed < 0 ) curveSpeed = 0;
    curveSpeed = std::clamp( curveSpeed, 0, 100 );

    curveSpeed = applyHwFanLimitations( curveSpeed );

    int speed = smoothSpeed( curveSpeed );

    // Critical-temperature override uses the raw filtered temp (safety floor).
    speed = manageCriticalTemperature( filteredTemp, speed );

    return speed;
  }

  FanProfile m_fanProfile;
  FanLogicType m_type;
  TemperatureFilter m_tempFilter;
  int m_latestSpeedPercent;
  double m_smoothedSpeed;
  int m_lastEffectiveTemp;
  int m_fansMinSpeedHWLimit;
  bool m_fansOffAvailable;
};

class FanControlWorker : public DaemonWorker
{
public:
  FanControlWorker(
    std::function< UccProfile() > getActiveProfile,
    std::function< bool() > getFanControlEnabled,
    std::function< void( bool, int, bool ) > updateFanHardwareInfo,
    std::function< void( size_t, int64_t, int ) > updateFanSpeed,
    std::function< void( size_t, int64_t, int ) > updateFanTemp,
    std::string sysfsRoot = "/sys"
  )
    : DaemonWorker( std::chrono::milliseconds( 1000 ) )
    , m_sysfsRoot( std::move( sysfsRoot ) )
    , m_getActiveProfile( getActiveProfile )
    , m_getFanControlEnabled( getFanControlEnabled )
    , m_updateFanHardwareInfo( updateFanHardwareInfo )
    , m_updateFanSpeed( updateFanSpeed )
    , m_updateFanTemp( updateFanTemp )
    , m_modeSameSpeed( true )
    , m_sameSpeedOverride( false )
    , m_sameSpeedOverrideActive( false )
    , m_controlAvailableMessageShown( false )
    , m_hasTemporaryCurves( false )
  {
  }

  ~FanControlWorker() override = default;

  // Allow external callers to change same-speed mode at runtime
  void setSameSpeed( bool same )
  {
    m_sameSpeedOverride = same;
    m_sameSpeedOverrideActive = true;
    m_modeSameSpeed = same;
    syslog( LOG_INFO, "FanControlWorker: setSameSpeed = %d", m_modeSameSpeed.load() ? 1 : 0 );
  }

  [[nodiscard]] bool getSameSpeed() const noexcept { return m_modeSameSpeed.load(); }

  /**
   * @brief Clear temporary fan curves and revert to profile curves
   */
  void clearTemporaryCurves()
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );
    m_hasTemporaryCurves = false;
    m_tempCpuTable.clear();
    m_tempGpuTable.clear();
    m_tempWaterCoolerFanTable.clear();
    m_tempPumpTable.clear();
  }

  [[nodiscard]] bool hasTemporaryCurves() const
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );
    return m_hasTemporaryCurves;
  }

  [[nodiscard]] std::vector< FanTableEntry > tempWaterCoolerFanTable() const
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );
    return m_tempWaterCoolerFanTable;
  }

  [[nodiscard]] std::vector< FanTableEntry > tempPumpTable() const
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );
    return m_tempPumpTable;
  }

  [[nodiscard]] bool applyTemporaryFanCurves( const std::vector< FanTableEntry > &cpuTable,
                                const std::vector< FanTableEntry > &gpuTable,
                                const std::vector< FanTableEntry > &waterCoolerFanTable = {},
                                const std::vector< FanTableEntry > &pumpTable = {} )
  {
    {
      std::lock_guard< std::mutex > lock( m_curveMutex );

      m_tempCpuTable = cpuTable;
      m_tempGpuTable = gpuTable;
      m_tempWaterCoolerFanTable = waterCoolerFanTable;
      m_tempPumpTable = pumpTable;
      m_hasTemporaryCurves = true;
    }

    if ( !m_getFanControlEnabled() )
    {
      syslog( LOG_WARNING, "FanControlWorker: fan control is disabled; temporary fan curves were not applied" );
      return false;
    }

    const UccProfile profile = m_getActiveProfile();
    const bool sameSpeed =
      m_sameSpeedOverrideActive.load() ? m_sameSpeedOverride.load() : profile.fan.sameSpeed;
    m_modeSameSpeed = sameSpeed;

    return applyManualFanControl( profile, sameSpeed );
  }

protected:
  void onStart() override
  {
    m_fanInfo = ucc::uniwill::readFanInfo( m_sysfsRoot );

    if ( m_updateFanHardwareInfo )
    {
      const bool available = m_fanInfo.isAvailable();
      m_updateFanHardwareInfo(
        available,
        ucc::uniwill::fanMinimumSpeedPercent( m_fanInfo ),
        ucc::uniwill::fanOffAvailable( m_fanInfo ) );
    }

    if ( m_fanInfo.isAvailable() )
    {
      syslog( LOG_INFO, "FanControlWorker: using uniwill hwmon at %s with %zu fan channels",
              m_fanInfo.hwmonPath.c_str(), m_fanInfo.channels.size() );
    }
    else
    {
      syslog( LOG_INFO, "FanControlWorker: No uniwill hwmon fans detected" );
    }
  }

  void onWork() override
  {
    if ( !m_fanInfo.isAvailable() )
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

    const UccProfile profile = m_getActiveProfile();
    const bool useFanControl = m_getFanControlEnabled();
    if ( profile.id != m_lastProfileId )
    {
      m_lastProfileId = profile.id;
      m_sameSpeedOverrideActive = false;
    }

    const bool sameSpeed =
      m_sameSpeedOverrideActive.load() ? m_sameSpeedOverride.load() : profile.fan.sameSpeed;
    m_modeSameSpeed = sameSpeed;

    if ( useFanControl )
    {
      // Manual mode must re-evaluate the curve against the live temperature
      // every cycle.
      applyManualFanControl( profile, sameSpeed );
    }
    else if ( m_manualModeActive or !m_controlModeInitialized or m_lastControlEnabled )
    {
      if ( ucc::uniwill::writeFanMode( m_fanInfo, 2 ) )
        syslog( LOG_INFO, "FanControlWorker: returned fans to firmware automatic mode" );
      m_manualModeActive = false;
      m_lastManualPctCpu = -1;
      m_lastManualPctGpu = -1;
      m_sensorFailureFallbackActive = false;
      // Drop the control state so a later re-enable starts fresh (fresh EWMA /
      // hysteresis seed), matching the master's per-start FanControlLogic setup.
      m_fanLogics.clear();
    }

    m_controlModeInitialized = true;
    m_lastControlEnabled = useFanControl;

    publishFanReadings();
  }

  void onExit() override
  {
    if ( m_fanInfo.isAvailable() )
      ucc::uniwill::writeFanMode( m_fanInfo, 2 );
  }

private:
  struct TemporaryCurveSnapshot
  {
    bool active = false;
    std::vector< FanTableEntry > cpu;
    std::vector< FanTableEntry > gpu;
    std::vector< FanTableEntry > waterCoolerFan;
    std::vector< FanTableEntry > pump;
  };

  [[nodiscard]] TemporaryCurveSnapshot temporaryCurves() const
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );
    return TemporaryCurveSnapshot{
      m_hasTemporaryCurves,
      m_tempCpuTable,
      m_tempGpuTable,
      m_tempWaterCoolerFanTable,
      m_tempPumpTable
    };
  }

  [[nodiscard]] FanProfile resolveFanProfile( const UccProfile &profile ) const
  {
    FanProfile fanProfile = getDefaultFanProfile( profile.fan.fanProfile );

    if ( profile.fan.hasEmbeddedTables() )
    {
      fanProfile.tableCPU = profile.fan.tableCPU;
      fanProfile.tableGPU = profile.fan.tableGPU;
    }

    const auto temporary = temporaryCurves();
    if ( temporary.active )
    {
      if ( !temporary.cpu.empty() )
        fanProfile.tableCPU = temporary.cpu;
      if ( !temporary.gpu.empty() )
        fanProfile.tableGPU = temporary.gpu;
      if ( !temporary.waterCoolerFan.empty() )
        fanProfile.tableWaterCoolerFan = temporary.waterCoolerFan;
      if ( !temporary.pump.empty() )
        fanProfile.tablePump = temporary.pump;
    }

    return fanProfile;
  }

  // Drive the fans directly in manual mode (pwm_enable=1). The daemon
  // interpolates the curve at the live temperature and writes PWM every cycle,
  // matching the behaviour of the pre-uniwill control path.
  bool applyManualFanControl( const UccProfile &profile, bool sameSpeed )
  {
    std::lock_guard< std::mutex > applyLock( m_applyMutex );
    const FanProfile fanProfile = resolveFanProfile( profile );

    // The driver rejects pwm writes unless the fan is in manual mode; engage it
    // once on entry (repeated mode writes would re-trigger the EC boost ramp).
    if ( !m_manualModeActive )
    {
      if ( !ucc::uniwill::writeFanMode( m_fanInfo, 1 ) )
      {
        syslog( LOG_WARNING, "FanControlWorker: failed enabling manual fan mode" );
        return false;
      }
      m_manualModeActive = true;
      syslog( LOG_INFO, "FanControlWorker: manual fan control engaged (pwm_enable=1)" );
    }

    // One FanControlLogic per channel, carrying persistent EWMA/hysteresis
    // state (created lazily, kept across cycles). Refresh curve + HW limits.
    if ( m_fanLogics.size() != m_fanInfo.channels.size() )
    {
      m_fanLogics.clear();
      const int32_t minPct = ucc::uniwill::fanMinimumSpeedPercent( m_fanInfo );
      const bool offAvail = ucc::uniwill::fanOffAvailable( m_fanInfo );
      for ( const auto &channel : m_fanInfo.channels )
      {
        FanControlLogic logic( fanProfile,
                               channel.index == 0 ? FanLogicType::CPU : FanLogicType::GPU );
        logic.setFansMinSpeedHWLimit( minPct );
        logic.setFansOffAvailable( offAvail );
        m_fanLogics.push_back( logic );
      }
    }

    // Compute each channel's target through the full control logic (EWMA temp
    // filter -> hysteresis -> curve -> HW min/off limits -> speed smoothing ->
    // critical-temperature safety override).
    std::vector< int > speeds( m_fanInfo.channels.size(), -1 );
    int highestSpeed = 0;
    bool anyTemperatureRead = false;
    for ( size_t i = 0; i < m_fanInfo.channels.size(); ++i )
    {
      m_fanLogics[ i ].updateFanProfile( fanProfile );

      const int32_t temp = ucc::uniwill::readFanReading( m_fanInfo.channels[ i ] ).temperatureCelsius;
      if ( temp < 0 )
        continue;

      m_fanLogics[ i ].reportTemperature( temp );
      speeds[ i ] = m_fanLogics[ i ].getSpeedPercent();
      highestSpeed = std::max( highestSpeed, speeds[ i ] );
      anyTemperatureRead = true;
    }

    if ( !anyTemperatureRead )
    {
      if ( !m_sensorFailureFallbackActive )
      {
        syslog( LOG_WARNING,
                "FanControlWorker: all fan temperature reads failed; holding last manual speed or using fail-safe" );
        m_sensorFailureFallbackActive = true;
      }
    }
    else if ( m_sensorFailureFallbackActive )
    {
      syslog( LOG_INFO, "FanControlWorker: fan temperature reads recovered" );
      m_sensorFailureFallbackActive = false;
    }

    // Write pwm per channel. In sameSpeed mode (or for a fan whose sensor read
    // failed) mirror the highest computed speed, matching the master behaviour.
    bool wroteAny = false;
    const int32_t highestLastSpeed = std::max( m_lastManualPctCpu, m_lastManualPctGpu );
    const int32_t failSafeSpeed = highestLastSpeed > 0 ? highestLastSpeed : 100;
    const auto channelFallbackSpeed = [ this, failSafeSpeed ]( const auto &channel ) {
      const int32_t last = ( channel.index == 0 ) ? m_lastManualPctCpu : m_lastManualPctGpu;
      return last > 0 ? last : failSafeSpeed;
    };

    for ( size_t i = 0; i < m_fanInfo.channels.size(); ++i )
    {
      const auto &channel = m_fanInfo.channels[ i ];
      if ( not channel.canUseManualControl() )
        continue;

      int pct = speeds[ i ];
      if ( sameSpeed )
        pct = anyTemperatureRead ? highestSpeed : failSafeSpeed;
      else if ( pct < 0 )
        pct = anyTemperatureRead ? highestSpeed : channelFallbackSpeed( channel );
      pct = std::clamp( pct, 0, 100 );

      if ( ucc::uniwill::writeFanPwm( channel, pct ) )
      {
        wroteAny = true;
        const int32_t pwm = ucc::uniwill::percentToPwm( pct );
        int32_t &last = ( channel.index == 0 ) ? m_lastManualPctCpu : m_lastManualPctGpu;
        if ( pct != last )
        {
          std::cout << "[Fan]   channel " << channel.index << " ("
                    << ( sameSpeed ? "sameSpeed" : ( channel.index == 0 ? "CPU" : "GPU" ) )
                    << ") -> " << pct << "% (pwm=" << pwm << ")" << std::endl;
          last = pct;
        }
      }
    }

    return wroteAny;
  }

  void publishFanReadings()
  {
    const int64_t timestamp = std::chrono::duration_cast< std::chrono::milliseconds >(
      std::chrono::system_clock::now().time_since_epoch() ).count();

    for ( size_t fanIndex = 0; fanIndex < m_fanInfo.channels.size(); ++fanIndex )
    {
      const auto reading = ucc::uniwill::readFanReading( m_fanInfo.channels[ fanIndex ] );
      m_updateFanTemp( fanIndex, timestamp, reading.temperatureCelsius );
      m_updateFanSpeed( fanIndex, timestamp, reading.speedPercent );
    }
  }

  std::string m_sysfsRoot;
  ucc::uniwill::FanInfo m_fanInfo;
  std::function< UccProfile() > m_getActiveProfile;
  std::function< bool() > m_getFanControlEnabled;
  std::function< void( bool, int, bool ) > m_updateFanHardwareInfo;
  std::function< void( size_t, int64_t, int ) > m_updateFanSpeed;
  std::function< void( size_t, int64_t, int ) > m_updateFanTemp;

  std::atomic< bool > m_modeSameSpeed;
  std::atomic< bool > m_sameSpeedOverride;
  std::atomic< bool > m_sameSpeedOverrideActive;
  bool m_controlAvailableMessageShown;
  bool m_controlModeInitialized = false;
  bool m_lastControlEnabled = false;
  bool m_manualModeActive = false;
  bool m_sensorFailureFallbackActive = false;
  int32_t m_lastManualPctCpu = -1;
  int32_t m_lastManualPctGpu = -1;
  std::vector< FanControlLogic > m_fanLogics;
  std::string m_lastProfileId;

  // Temporary fan curve tracking
  std::mutex m_applyMutex;
  mutable std::mutex m_curveMutex;
  bool m_hasTemporaryCurves;
  std::vector< FanTableEntry > m_tempCpuTable;
  std::vector< FanTableEntry > m_tempGpuTable;
  std::vector< FanTableEntry > m_tempWaterCoolerFanTable;
  std::vector< FanTableEntry > m_tempPumpTable;
};
