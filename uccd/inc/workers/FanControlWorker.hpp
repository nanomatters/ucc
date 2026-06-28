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
#include <mutex>
#include <sstream>
#include <string>
#include <syslog.h>
#include <utility>

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
    markCurveDirty();
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
    m_curveDirty = true;
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

  void applyTemporaryFanCurves( const std::vector< FanTableEntry > &cpuTable,
                                const std::vector< FanTableEntry > &gpuTable,
                                const std::vector< FanTableEntry > &waterCoolerFanTable = {},
                                const std::vector< FanTableEntry > &pumpTable = {} )
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );

    m_tempCpuTable = cpuTable;
    m_tempGpuTable = gpuTable;
    m_tempWaterCoolerFanTable = waterCoolerFanTable;
    m_tempPumpTable = pumpTable;
    m_hasTemporaryCurves = true;
    m_curveDirty = true;
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
        ucc::uniwill::FAN_MIN_SPEED_PERCENT,
        available );
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
      const std::string signature = makeCurveSignature( profile, sameSpeed );
      if ( takeCurveDirtyFlag() or signature != m_lastAppliedCurveSignature )
      {
        if ( applyDriverFanCurves( profile, sameSpeed ) )
          m_lastAppliedCurveSignature = signature;
      }
    }
    else if ( !m_controlModeInitialized or m_lastControlEnabled )
    {
      if ( ucc::uniwill::writeFanMode( m_fanInfo, 2 ) )
        syslog( LOG_INFO, "FanControlWorker: returned fans to firmware automatic mode" );
      m_lastAppliedCurveSignature.clear();
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
    bool dirty = false;
    std::vector< FanTableEntry > cpu;
    std::vector< FanTableEntry > gpu;
    std::vector< FanTableEntry > waterCoolerFan;
    std::vector< FanTableEntry > pump;
  };

  void markCurveDirty()
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );
    m_curveDirty = true;
  }

  [[nodiscard]] bool takeCurveDirtyFlag()
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );
    const bool dirty = m_curveDirty;
    m_curveDirty = false;
    return dirty;
  }

  [[nodiscard]] TemporaryCurveSnapshot temporaryCurves() const
  {
    std::lock_guard< std::mutex > lock( m_curveMutex );
    return TemporaryCurveSnapshot{
      m_hasTemporaryCurves,
      m_curveDirty,
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

  [[nodiscard]] std::vector< ucc::uniwill::FanCurvePoint > buildCurvePoints(
    const FanProfile &fanProfile,
    bool useCpuCurve,
    bool sameSpeed ) const
  {
    std::vector< ucc::uniwill::FanCurvePoint > points;
    points.reserve( static_cast< size_t >( ucc::uniwill::FAN_AUTO_POINT_COUNT ) );

    for ( const int32_t temp : ucc::uniwill::FAN_AUTO_POINT_TEMPERATURES_C )
    {
      int32_t speed = -1;
      if ( sameSpeed )
      {
        const int32_t cpuSpeed = fanProfile.getSpeedForTemp( temp, true );
        const int32_t gpuSpeed = fanProfile.getSpeedForTemp( temp, false );
        speed = std::max( cpuSpeed, gpuSpeed );
      }
      else
      {
        speed = fanProfile.getSpeedForTemp( temp, useCpuCurve );
      }

      if ( speed < 0 )
        speed = fanProfile.getSpeedForTemp( temp, true );
      if ( speed < 0 )
        speed = 0;

      points.push_back( {
        temp,
        std::clamp< int32_t >( speed, 0, 100 )
      } );
    }

    return points;
  }

  [[nodiscard]] std::string makeCurveSignature( const UccProfile &profile, bool sameSpeed ) const
  {
    const auto temporary = temporaryCurves();
    std::ostringstream signature;

    auto appendTable = [ &signature ]( const std::vector< FanTableEntry > &table ) {
      signature << table.size() << ':';
      for ( const auto &[ temp, speed ] : table )
        signature << temp << ',' << speed << ';';
    };

    signature << profile.id << '|'
              << profile.fan.fanProfile << '|'
              << ( sameSpeed ? 1 : 0 ) << '|';

    appendTable( profile.fan.tableCPU );
    appendTable( profile.fan.tableGPU );
    appendTable( temporary.cpu );
    appendTable( temporary.gpu );

    return signature.str();
  }

  bool applyDriverFanCurves( const UccProfile &profile, bool sameSpeed )
  {
    const FanProfile fanProfile = resolveFanProfile( profile );
    bool wroteAnyCurve = false;

    for ( const auto &channel : m_fanInfo.channels )
    {
      if ( !channel.supportsCustomAuto )
        continue;

      const bool useCpuCurve = channel.index == 0;
      const auto points = buildCurvePoints( fanProfile, useCpuCurve, sameSpeed );
      if ( !ucc::uniwill::writeFanCurve( channel, points ) )
      {
        syslog( LOG_WARNING, "FanControlWorker: failed writing fan curve for channel %zu",
                channel.index );
        continue;
      }

      wroteAnyCurve = true;
    }

    if ( !wroteAnyCurve )
    {
      syslog( LOG_WARNING, "FanControlWorker: no writable custom-auto fan curve channels" );
      return false;
    }

    if ( !ucc::uniwill::writeFanMode( m_fanInfo, 3 ) )
    {
      syslog( LOG_WARNING, "FanControlWorker: failed enabling custom-auto fan mode" );
      return false;
    }

    syslog( LOG_INFO, "FanControlWorker: applied uniwill custom-auto fan curves" );
    return true;
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
  std::string m_lastAppliedCurveSignature;
  std::string m_lastProfileId;

  // Temporary fan curve tracking
  mutable std::mutex m_curveMutex;
  bool m_hasTemporaryCurves;
  bool m_curveDirty = true;
  std::vector< FanTableEntry > m_tempCpuTable;
  std::vector< FanTableEntry > m_tempGpuTable;
  std::vector< FanTableEntry > m_tempWaterCoolerFanTable;
  std::vector< FanTableEntry > m_tempPumpTable;
};
