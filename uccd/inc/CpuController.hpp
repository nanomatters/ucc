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

#include "SysfsNode.hpp"
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <cmath>

enum class ScalingDriver
{
  acpi_cpufreq,
  intel_pstate,
  amd_pstate,
  amd_pstate_epp,
  unknown
};

/**
 * @brief Controller for a single logical CPU core
 *
 * Manages sysfs interfaces for cpufreq parameters of a single core.
 */
class LogicalCpuController
{
public:
  const std::string basePath;
  const int32_t coreIndex;
  const std::string cpuPath;
  const std::string cpufreqPath;

  // cpuX/online
  SysfsNode< bool > online;

  // cpuX/cpufreq/*
  SysfsNode< int32_t > scalingCurFreq;
  SysfsNode< int32_t > scalingMinFreq;
  SysfsNode< int32_t > scalingMaxFreq;
  SysfsNode< std::vector< int32_t > > scalingAvailableFrequencies;
  SysfsNode< std::string > scalingDriver;
  SysfsNode< std::vector< std::string > > energyPerformanceAvailablePreferences;
  SysfsNode< std::string > energyPerformancePreference;
  SysfsNode< std::vector< std::string > > scalingAvailableGovernors;
  SysfsNode< std::string > scalingGovernor;
  SysfsNode< int32_t > cpuinfoMinFreq;
  SysfsNode< int32_t > cpuinfoMaxFreq;

  explicit LogicalCpuController( const std::string &base, int32_t index )
    : basePath( base )
    , coreIndex( index )
    , cpuPath( base + "/cpu" + std::to_string( index ) )
    , cpufreqPath( cpuPath + "/cpufreq" )
    , online( cpuPath + "/online" )
    , scalingCurFreq( cpufreqPath + "/scaling_cur_freq" )
    , scalingMinFreq( cpufreqPath + "/scaling_min_freq" )
    , scalingMaxFreq( cpufreqPath + "/scaling_max_freq" )
    , scalingAvailableFrequencies( cpufreqPath + "/scaling_available_frequencies", " " )
    , scalingDriver( cpufreqPath + "/scaling_driver" )
    , energyPerformanceAvailablePreferences( cpufreqPath + "/energy_performance_available_preferences", " " )
    , energyPerformancePreference( cpufreqPath + "/energy_performance_preference" )
    , scalingAvailableGovernors( cpufreqPath + "/scaling_available_governors", " " )
    , scalingGovernor( cpufreqPath + "/scaling_governor" )
    , cpuinfoMinFreq( cpufreqPath + "/cpuinfo_min_freq" )
    , cpuinfoMaxFreq( cpufreqPath + "/cpuinfo_max_freq" )
  {}

  /**
   * @brief Get reduced available frequency (middle of frequency range)
   */
  std::optional< int32_t > getReducedAvailableFreq() const
  {
    auto availableFreqs = scalingAvailableFrequencies.read();

    if ( not availableFreqs.has_value() or availableFreqs->empty() )
    {
      // fallback: use average of min and max
      auto minFreq = cpuinfoMinFreq.read();
      auto maxFreq = cpuinfoMaxFreq.read();

      if ( minFreq.has_value() and maxFreq.has_value() )
      {
        return ( *minFreq + *maxFreq ) / 2;
      }

      return std::nullopt;
    }

    return ( *availableFreqs )[ availableFreqs->size() / 2 ];
  }
};

/**
 * @brief Controller for CPU frequency and governor settings
 *
 * Manages scaling governors, frequencies, energy performance preferences,
 * online cores, and turbo/boost settings for all logical cores.
 */
class CpuController
{
public:
  static constexpr const char *basePath = "/sys/devices/system/cpu";

  std::vector< LogicalCpuController > cores;

  // /sys/devices/system/cpu/...
  SysfsNode< int32_t > kernelMax;
  SysfsNode< std::vector< int32_t > > offline;
  SysfsNode< std::vector< int32_t > > online;
  SysfsNode< std::vector< int32_t > > possible;
  SysfsNode< std::vector< int32_t > > present;

  // intel_pstate
  SysfsNode< bool > intelPstateNoTurbo;

  // boost
  SysfsNode< bool > boost;

  explicit CpuController()
    : kernelMax( std::string( basePath ) + "/kernel_max" )
    , offline( std::string( basePath ) + "/offline", " " )
    , online( std::string( basePath ) + "/online", " " )
    , possible( std::string( basePath ) + "/possible", " " )
    , present( std::string( basePath ) + "/present", " " )
    , intelPstateNoTurbo( std::string( basePath ) + "/intel_pstate/no_turbo" )
    , boost( std::string( basePath ) + "/cpufreq/boost" )
  {
    getAvailableLogicalCores();
  }

  /**
   * @brief Discover and populate available logical CPU cores
   */
  void getAvailableLogicalCores()
  {
    cores.clear();

    auto possibleCores = possible.read();
    auto presentCores = present.read();

    if ( not possibleCores.has_value() or not presentCores.has_value() )
      return;

    std::vector< int32_t > coreIndexToAdd;

    for ( int32_t possibleIndex : *possibleCores )
    {
      if ( std::find( presentCores->begin(), presentCores->end(), possibleIndex ) != presentCores->end() )
      {
        coreIndexToAdd.push_back( possibleIndex );
      }
    }

    std::sort( coreIndexToAdd.begin(), coreIndexToAdd.end() );

    for ( int32_t coreIndex : coreIndexToAdd )
    {
      LogicalCpuController newCore( basePath, coreIndex );

      // core 0 doesn't have online control, always include it
      if ( coreIndex == 0 or newCore.online.isAvailable() )
      {
        cores.push_back( std::move( newCore ) );
      }
    }
  }

  /**
   * @brief Set number of online CPU cores
   *
   * @param numberOfCores Number of cores to enable (defaults to all available)
   */
  void useCores( std::optional< int32_t > numberOfCores = std::nullopt )
  {
    if ( not numberOfCores.has_value() )
      numberOfCores = static_cast< int32_t >( cores.size() );

    if ( *numberOfCores == 0 )
      return;

    for ( size_t i = 1; i < cores.size(); ++i )
    {
      if ( not cores[ i ].online.isAvailable() )
        continue;

      if ( static_cast< int32_t >( i ) < *numberOfCores )
      {
        cores[ i ].online.write( true );
      }
      else
      {
        cores[ i ].online.write( false );
      }
    }
  }

  /**
   * @brief Find closest value in a sorted vector
   */
  static int32_t findClosestValue( int32_t target, const std::vector< int32_t > &values )
  {
    if ( values.empty() )
      return target;

    auto it = std::lower_bound( values.begin(), values.end(), target );

    if ( it == values.end() )
      return values.back();

    if ( it == values.begin() )
      return values.front();

    // check if previous value is closer
    auto prev = std::prev( it );

    if ( std::abs( *it - target ) < std::abs( *prev - target ) )
      return *it;

    return *prev;
  }

  /**
   * @brief Set maximum scaling frequency for all cores
   *
   * @param setMaxFrequency Target frequency (-1 for reduced, undefined for max)
   */
  void setGovernorScalingMaxFrequency( std::optional< int32_t > setMaxFrequency = std::nullopt )
  {
    std::optional< std::string > scalingDriverStr;

    for ( auto &core : cores )
    {
      if ( not core.scalingMinFreq.isAvailable() or not core.scalingMaxFreq.isAvailable()
           or not core.cpuinfoMinFreq.isAvailable() or not core.cpuinfoMaxFreq.isAvailable() )
        continue;

      if ( core.coreIndex != 0 and core.online.read().value_or( false ) == false )
        continue;

      auto coreMinFrequency = core.cpuinfoMinFreq.read();
      auto coreMaxFrequency = core.cpuinfoMaxFreq.read();
      auto scalingMinFrequency = core.scalingMinFreq.read();
      auto availableFrequencies = core.scalingAvailableFrequencies.read();

      if ( not coreMinFrequency.has_value() or not coreMaxFrequency.has_value()
           or not scalingMinFrequency.has_value() )
        continue;

      if ( not scalingDriverStr.has_value() )
        scalingDriverStr = core.scalingDriver.read();

      int32_t newMaxFrequency;

      // default to max available
      if ( not setMaxFrequency.has_value() )
      {
        newMaxFrequency = *coreMaxFrequency;
      }
      else if ( *setMaxFrequency == -1 )
      {
        // special case: -1 means "reduced"
        if ( boost.isAvailable() and scalingDriverStr.has_value() and *scalingDriverStr == "acpi-cpufreq" )
        {
          newMaxFrequency = *coreMaxFrequency;
        }
        else
        {
          auto reduced = core.getReducedAvailableFreq();
          newMaxFrequency = reduced.value_or( *coreMaxFrequency );
        }
      }
      else
      {
        newMaxFrequency = *setMaxFrequency;
      }

      // enforce min/max limits
      if ( newMaxFrequency > *coreMaxFrequency )
        newMaxFrequency = *coreMaxFrequency;
      else if ( newMaxFrequency < *scalingMinFrequency )
        newMaxFrequency = *scalingMinFrequency;

      // snap to available frequency if defined
      if ( availableFrequencies.has_value() and not availableFrequencies->empty() )
      {
        std::vector< int32_t > filtered;

        for ( int32_t freq : *availableFrequencies )
        {
          if ( freq >= *scalingMinFrequency )
            filtered.push_back( freq );
        }

        if ( not filtered.empty() )
        {
          newMaxFrequency = findClosestValue( newMaxFrequency, filtered );
        }
      }

      core.scalingMaxFreq.write( newMaxFrequency );
    }

    // handle boost for AMD (boost not included in max frequency)
    if ( cores.empty() )
      return;

    auto maxFrequency = cores[ 0 ].cpuinfoMaxFreq.read();
    auto availableFrequencies = cores[ 0 ].scalingAvailableFrequencies.read();

    int32_t maximumAvailableFrequency = maxFrequency.value_or( 0 );

    if ( availableFrequencies.has_value() and not availableFrequencies->empty() )
    {
      maximumAvailableFrequency = availableFrequencies->front();
    }

    if ( boost.isAvailable() and scalingDriverStr.has_value() and *scalingDriverStr == "acpi-cpufreq" )
    {
      if ( not setMaxFrequency.has_value() or *setMaxFrequency > maximumAvailableFrequency )
      {
        boost.write( true );
      }
      else
      {
        boost.write( false );
      }
    }
  }

  /**
   * @brief Set minimum scaling frequency for all cores
   *
   * @param setMinFrequency Target frequency (-2 for max, undefined for min)
   */
  void setGovernorScalingMinFrequency( std::optional< int32_t > setMinFrequency = std::nullopt )
  {
    for ( auto &core : cores )
    {
      if ( not core.scalingMinFreq.isAvailable() or not core.scalingMaxFreq.isAvailable()
           or not core.cpuinfoMinFreq.isAvailable() or not core.cpuinfoMaxFreq.isAvailable() )
        continue;

      if ( core.coreIndex != 0 and core.online.read().value_or( false ) == false )
        continue;

      auto coreMinFrequency = core.cpuinfoMinFreq.read();
      auto coreMaxFrequency = core.cpuinfoMaxFreq.read();
      auto scalingMaxFrequency = core.scalingMaxFreq.read();
      auto availableFrequencies = core.scalingAvailableFrequencies.read();

      if ( not coreMinFrequency.has_value() or not coreMaxFrequency.has_value()
           or not scalingMaxFrequency.has_value() )
        continue;

      int32_t newMinFrequency;

      // default to min available
      if ( not setMinFrequency.has_value() )
      {
        newMinFrequency = *coreMinFrequency;
      }
      else if ( *setMinFrequency == -2 )
      {
        // special case: -2 means "set to max"
        newMinFrequency = *coreMaxFrequency;
      }
      else
      {
        newMinFrequency = *setMinFrequency;

        // enforce min/max limits
        if ( newMinFrequency < *coreMinFrequency )
          newMinFrequency = *coreMinFrequency;
        else if ( newMinFrequency > *scalingMaxFrequency )
          newMinFrequency = *scalingMaxFrequency;
      }

      // snap to available frequency if defined
      if ( availableFrequencies.has_value() and not availableFrequencies->empty() )
      {
        std::vector< int32_t > filtered;

        for ( int32_t freq : *availableFrequencies )
        {
          if ( freq <= *scalingMaxFrequency )
            filtered.push_back( freq );
        }

        if ( not filtered.empty() )
        {
          newMinFrequency = findClosestValue( newMinFrequency, filtered );
        }
      }

      core.scalingMinFreq.write( newMinFrequency );
    }
  }

  /**
   * @brief Set scaling governor for all cores
   */
  void setGovernor( const std::optional< std::string > &governor )
  {
    if ( not governor.has_value() )
      return;

    for ( auto &core : cores )
    {
      if ( not core.scalingGovernor.isAvailable() or not core.scalingAvailableGovernors.isAvailable() )
        continue;

      if ( core.coreIndex != 0 and core.online.read().value_or( false ) == false )
        return;

      auto availableGovernors = core.scalingAvailableGovernors.read();

      if ( not availableGovernors.has_value() )
        continue;

      if ( std::find( availableGovernors->begin(), availableGovernors->end(), *governor ) != availableGovernors->end() )
      {
        core.scalingGovernor.write( *governor );
      }
    }
  }

  /**
   * @brief Set energy performance preference for all cores
   */
  void setEnergyPerformancePreference( const std::optional< std::string > &preference )
  {
    if ( not preference.has_value() )
      return;

    for ( auto &core : cores )
    {
      if ( not core.energyPerformancePreference.isAvailable()
           or not core.energyPerformanceAvailablePreferences.isAvailable() )
        continue;

      if ( core.coreIndex != 0 and core.online.read().value_or( false ) == false )
        return;

      auto availablePreferences = core.energyPerformanceAvailablePreferences.read();

      if ( not availablePreferences.has_value() )
        continue;

      if ( std::find( availablePreferences->begin(), availablePreferences->end(), *preference ) != availablePreferences->end() )
      {
        core.energyPerformancePreference.write( *preference );
      }
    }
  }

  /**
   * @brief Get scaling driver enum from string
   */
  static ScalingDriver getScalingDriverEnum( const std::string &driver )
  {
    if ( driver == "acpi-cpufreq" )
      return ScalingDriver::acpi_cpufreq;

    if ( driver == "intel_pstate" )
      return ScalingDriver::intel_pstate;

    if ( driver == "amd-pstate" )
      return ScalingDriver::amd_pstate;

    if ( driver == "amd-pstate-epp" )
      return ScalingDriver::amd_pstate_epp;

    return ScalingDriver::unknown;
  }
};
