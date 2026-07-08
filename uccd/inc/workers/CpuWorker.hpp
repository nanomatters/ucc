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
#include "../CpuController.hpp"
#include "../profiles/UccProfile.hpp"
#include <functional>
#include <vector>
#include <string>
#include <set>
#include <syslog.h>

/**
 * @brief Worker for managing CPU frequency and governor settings
 *
 * Applies CPU settings from the active profile including:
 * - Scaling governor
 * - Energy performance preference
 * - Min/max scaling frequencies
 * - Online core count
 * - Turbo/boost settings
 */
class CpuWorker : public DaemonWorker
{
public:
  explicit CpuWorker(
    std::function< UccProfile() > getActiveProfile,
    std::function< bool() > getCpuSettingsEnabled,
    std::function< void( const std::string & ) > logFunction )
    : DaemonWorker( std::chrono::milliseconds( 10000 ), false )
    , m_cpuCtrl()
    , m_getActiveProfile( std::move( getActiveProfile ) )
    , m_getCpuSettingsEnabled( std::move( getCpuSettingsEnabled ) )
    , m_logFunction( std::move( logFunction ) )
    , m_noEPPWriteQuirk( false )
    , m_validationFailureCount( 0 )
    , m_reapplyGaveUp( false )
  {
    // capture the system's current governor and EPP as defaults
    // these are the values the system was booted with, before we modify anything
    if ( not m_cpuCtrl.cores.empty() )
    {
      m_systemDefaultGovernor = m_cpuCtrl.cores[ 0 ].scalingGovernor.read();
      m_systemDefaultEPP = m_cpuCtrl.cores[ 0 ].energyPerformancePreference.read();

      if ( m_systemDefaultGovernor.has_value() )
        logLine( "CpuWorker: System default governor: '" + *m_systemDefaultGovernor + "'", LOG_INFO );

      if ( m_systemDefaultEPP.has_value() )
        logLine( "CpuWorker: System default EPP: '" + *m_systemDefaultEPP + "'", LOG_INFO );
    }
  }

  void onStart() override
  {
    if ( m_getCpuSettingsEnabled() && not m_getActiveProfile().id.empty() )
      applyCpuProfile( m_getActiveProfile() );
  }

  void onWork() override
  {
    // validate current CPU settings match profile
    if ( m_getCpuSettingsEnabled() and not validateCpuFreq() )
    {
      if ( m_reapplyGaveUp )
        return;

      ++m_validationFailureCount;

      if ( m_validationFailureCount >= maxReapplyAttempts )
      {
        logLine( "CpuWorker: CPU settings keep being reverted by another service "
                 "(e.g. tuned, power-profiles-daemon). Giving up after "
                 + std::to_string( maxReapplyAttempts ) + " attempts.", LOG_WARNING );
        m_reapplyGaveUp = true;
        return;
      }

      logLine( "CpuWorker: Incorrect settings, reapplying profile "
               "(attempt " + std::to_string( m_validationFailureCount )
               + "/" + std::to_string( maxReapplyAttempts ) + ")", LOG_INFO );
      applyCpuProfile( m_getActiveProfile() );
    }
    else
    {
      if ( m_validationFailureCount > 0 )
        m_validationFailureCount = 0;
      if ( m_reapplyGaveUp )
        m_reapplyGaveUp = false;
    }
  }

  void onExit() override
  {
    setCpuDefaultConfig();
  }

  /**
   * @brief Re-apply CPU settings from active profile
   *
   * Call this when the profile changes to re-apply CPU settings
   */
  void reapplyProfile()
  {
    if ( m_getCpuSettingsEnabled() )
    {
      m_validationFailureCount = 0;
      m_reapplyGaveUp = false;
      applyCpuProfile( m_getActiveProfile() );
    }
  }

  /**
   * @brief Get available CPU governors
   *
   * @return Vector of available governor names or nullopt if unavailable
   */
  std::optional< std::vector< std::string > > getAvailableGovernors()
  {
    if ( m_cpuCtrl.cores.empty() )
      return std::nullopt;

    return m_cpuCtrl.cores[ 0 ].scalingAvailableGovernors.read();
  }

  /**
   * @brief Get available energy performance preferences
   *
   * @return Vector of available EPP names or nullopt if unavailable
   */
  std::optional< std::vector< std::string > > getAvailableEPPs()
  {
    if ( m_cpuCtrl.cores.empty() )
      return std::nullopt;

    return m_cpuCtrl.cores[ 0 ].energyPerformanceAvailablePreferences.read();
  }

  /**
   * @brief Get total number of available logical CPU cores
   *
   * @return Number of logical cores discovered from sysfs or 0 if unavailable
   */
  int32_t getCoreCount() const
  {
    return static_cast< int32_t >( m_cpuCtrl.cores.size() );
  }

  /**
   * @brief Snap profile frequency values to nearest available hardware frequencies
   *
   * Call this after receiving a profile (from file, D-Bus, etc.) to ensure
   * min/max frequencies match actual hardware-supported values before storing.
   * This prevents mismatches between what the kernel accepts and what we expect.
   */
  void snapProfileFrequencies( UccProfile &profile )
  {
    if ( m_cpuCtrl.cores.empty() )
      return;

    auto availableFrequencies = m_cpuCtrl.cores[ 0 ].scalingAvailableFrequencies.read();

    if ( not availableFrequencies.has_value() or availableFrequencies->empty() )
      return;

    if ( profile.cpu.scalingMinFrequency.has_value() and *profile.cpu.scalingMinFrequency > 0 )
    {
      int32_t snapped = CpuController::findClosestValue( *profile.cpu.scalingMinFrequency, *availableFrequencies );

      if ( snapped != *profile.cpu.scalingMinFrequency )
      {
        logLine( "CpuWorker: Snapping min frequency "
                 + std::to_string( *profile.cpu.scalingMinFrequency ) + " -> "
                 + std::to_string( snapped ), LOG_DEBUG );
        profile.cpu.scalingMinFrequency = snapped;
      }
    }

    if ( profile.cpu.scalingMaxFrequency.has_value() and *profile.cpu.scalingMaxFrequency > 0 )
    {
      int32_t snapped = CpuController::findClosestValue( *profile.cpu.scalingMaxFrequency, *availableFrequencies );

      if ( snapped != *profile.cpu.scalingMaxFrequency )
      {
        logLine( "CpuWorker: Snapping max frequency "
                 + std::to_string( *profile.cpu.scalingMaxFrequency ) + " -> "
                 + std::to_string( snapped ), LOG_DEBUG );
        profile.cpu.scalingMaxFrequency = snapped;
      }
    }
  }

  std::optional< std::string > getDefaultGovernor( void )
  {
    if ( not m_defaultGovernor.has_value() )
      m_defaultGovernor = findDefaultGovernor();

    return m_defaultGovernor;
  }

private:
  CpuController m_cpuCtrl;
  std::function< UccProfile() > m_getActiveProfile;
  std::function< bool() > m_getCpuSettingsEnabled;
  std::function< void( const std::string & ) > m_logFunction;
  bool m_noEPPWriteQuirk;
  int m_validationFailureCount;
  bool m_reapplyGaveUp;
  std::optional< std::string > m_defaultGovernor;
  std::optional< std::string > m_systemDefaultGovernor;
  std::optional< std::string > m_systemDefaultEPP;
  std::set< std::string > m_warnedGovernors;
  std::set< std::string > m_warnedEPPs;

  static constexpr int maxReapplyAttempts = 3;

  void logLine( const std::string &message, int priority = LOG_INFO )
  {
    if ( m_logFunction )
    {
      m_logFunction( message );
    }
    else
    {
      syslog( priority, "%s", message.c_str() );
    }
  }

  /**
   * @brief Find default governor for current system
   *
   * Returns the governor that was active when CpuWorker was constructed,
   * i.e. whatever the system was booted with.
   */
  std::optional< std::string > findDefaultGovernor( void )
  {
    return m_systemDefaultGovernor;
  }

  /**
   * @brief Check whether a governor is available on this system
   */
  bool isGovernorAvailable( const std::string &governor )
  {
    if ( m_cpuCtrl.cores.empty() )
      return false;

    auto available = m_cpuCtrl.cores[ 0 ].scalingAvailableGovernors.read();

    if ( not available.has_value() )
      return false;

    return std::ranges::find( *available, governor ) != available->end();
  }

  /**
   * @brief Check whether an energy performance preference is available on this system
   */
  bool isEPPAvailable( const std::string &epp )
  {
    if ( m_cpuCtrl.cores.empty() )
      return false;

    auto available = m_cpuCtrl.cores[ 0 ].energyPerformanceAvailablePreferences.read();

    if ( not available.has_value() )
      return false;

    return std::ranges::find( *available, epp ) != available->end();
  }

  /**
   * @brief Apply CPU settings from profile
   */
  void applyCpuProfile( const UccProfile &profile )
  {
    // reset everything to default before applying new settings
    setCpuDefaultConfig();

    // resolve desired governor (profile value or system default)
    auto governor = not profile.cpu.governor.empty()
                      ? std::optional< std::string >( profile.cpu.governor )
                      : getDefaultGovernor();

    if ( governor.has_value() )
    {
      if ( isGovernorAvailable( *governor ) )
      {
        m_cpuCtrl.setGovernor( governor );
      }
      else if ( m_warnedGovernors.insert( *governor ).second )
      {
        logLine( "CpuWorker: Governor '" + *governor
                 + "' is not available on this system, leaving governor unmodified", LOG_WARNING );
      }
    }

    // validate and set EPP
    if ( not m_noEPPWriteQuirk and not profile.cpu.energyPerformancePreference.empty() )
    {
      if ( isEPPAvailable( profile.cpu.energyPerformancePreference ) )
      {
        m_cpuCtrl.setEnergyPerformancePreference( profile.cpu.energyPerformancePreference );

        // Verify EPP write actually took effect - some kernels (e.g. intel_pstate
        // on Arrow Lake with CachyOS 6.19+) reject sysfs EPP writes with EBUSY
        // even though the correct value is already set at the MSR level.
        if ( not m_cpuCtrl.cores.empty() )
        {
          auto currentEPP = m_cpuCtrl.cores[ 0 ].energyPerformancePreference.read();

          if ( currentEPP.has_value() and *currentEPP != profile.cpu.energyPerformancePreference )
          {
            m_noEPPWriteQuirk = true;
            logLine( "CpuWorker: EPP write to sysfs was rejected by the kernel "
                     "(requested '" + profile.cpu.energyPerformancePreference
                     + "', got '" + *currentEPP + "'). "
                     "Disabling sysfs EPP management — the hardware EPP may "
                     "already be correct at the MSR level.", LOG_WARNING );
          }
        }
      }
      else if ( m_warnedEPPs.insert( profile.cpu.energyPerformancePreference ).second )
      {
        logLine( "CpuWorker: Energy performance preference '" + profile.cpu.energyPerformancePreference
                 + "' is not available on this system, leaving EPP unmodified", LOG_WARNING );
      }
    }

    m_cpuCtrl.setGovernorScalingMinFrequency( profile.cpu.scalingMinFrequency );
    m_cpuCtrl.setGovernorScalingMaxFrequency( profile.cpu.scalingMaxFrequency );

    // set number of online cores
    m_cpuCtrl.useCores( profile.cpu.onlineCores );

    // set no_turbo if available
    if ( m_cpuCtrl.intelPstateNoTurbo.isAvailable() )
      m_cpuCtrl.intelPstateNoTurbo.write( profile.cpu.noTurbo );

    // set hwp_dynamic_boost if available
    if ( m_cpuCtrl.intelHwpDynamicBoost.isAvailable() )
      m_cpuCtrl.intelHwpDynamicBoost.write( profile.cpu.hwpDynamicBoost );
  }

  /**
   * @brief Reset CPU settings to defaults
   */
  void setCpuDefaultConfig()
  {
    m_cpuCtrl.useCores( std::nullopt ); // all cores
    m_cpuCtrl.setGovernorScalingMinFrequency( std::nullopt ); // min
    m_cpuCtrl.setGovernorScalingMaxFrequency( std::nullopt ); // max

    // restore the system's original EPP
    if ( not m_noEPPWriteQuirk and m_systemDefaultEPP.has_value()
         and isEPPAvailable( *m_systemDefaultEPP ) )
      m_cpuCtrl.setEnergyPerformancePreference( *m_systemDefaultEPP );

    if ( m_cpuCtrl.intelPstateNoTurbo.isAvailable() )
      m_cpuCtrl.intelPstateNoTurbo.write( false );

    if ( m_cpuCtrl.intelHwpDynamicBoost.isAvailable() )
      m_cpuCtrl.intelHwpDynamicBoost.write( false );
  }

  /**
   * @brief Validate current CPU frequency settings match profile
   */
  bool validateCpuFreq()
  {
    const UccProfile profile = m_getActiveProfile();
    {
      // Determine acpi-cpufreq fallback flag once - mirrors setGovernorScalingMaxFrequency
      bool acpiFallback = false;
      if ( m_cpuCtrl.boost.isAvailable() )
      {
        for ( const auto &firstCore : m_cpuCtrl.cores )
        {
          if ( auto driverStr = firstCore.scalingDriver.read() )
          {
            acpiFallback = ( *driverStr == "acpi-cpufreq" );
            break;
          }
        }
      }

      // validate scaling frequencies match profile
      for ( const auto &core : m_cpuCtrl.cores )
      {
        if ( core.coreIndex == 0 or core.online.read().value_or( false ) )
        {
          // check minimum frequency - compare against the per-core clamped+snapped value
          // that setGovernorScalingMinFrequency() actually wrote, not the raw profile target
          // (which would differ for E-cores / lower-binned P-cores)
          {
            auto currentMin = core.scalingMinFreq.read();
            auto expectedMin = CpuController::computeEffectiveMinFreq( core, profile.cpu.scalingMinFrequency );

            if ( currentMin.has_value() and expectedMin.has_value() and *currentMin != *expectedMin )
            {
              logLine( "CpuWorker: Unexpected value core" + std::to_string( core.coreIndex )
                       + " minimum scaling frequency " + std::to_string( *currentMin )
                       + " instead of " + std::to_string( *expectedMin ), LOG_DEBUG );
              return false;
            }
          }

          // check maximum frequency - compare against the per-core clamped+snapped value.
          // On heterogeneous CPUs (Intel Turbo Boost Max 3.0, P+E cores) every core type
          // has its own cpuinfo_max_freq ceiling; the profile stores the global requested
          // max but each core will have been written a different effective value.
          {
            auto currentMax = core.scalingMaxFreq.read();
            auto expectedMax = CpuController::computeEffectiveMaxFreq( core, profile.cpu.scalingMaxFrequency, acpiFallback );

            if ( currentMax.has_value() and expectedMax.has_value() and *currentMax != *expectedMax )
            {
              logLine( "CpuWorker: Unexpected value core" + std::to_string( core.coreIndex )
                       + " maximum scaling frequency " + std::to_string( *currentMax )
                       + " instead of " + std::to_string( *expectedMax ), LOG_DEBUG );
              return false;
            }
          }

          const auto expectedGovernor = not profile.cpu.governor.empty() ? std::optional< std::string >( profile.cpu.governor )
                                                                         : getDefaultGovernor();
          if ( expectedGovernor.has_value() and isGovernorAvailable( *expectedGovernor ) )
          {
            auto currentGovernor = core.scalingGovernor.read();

            if ( currentGovernor.has_value() and *currentGovernor != *expectedGovernor )
            {
              logLine( "CpuWorker: Unexpected value core" + std::to_string( core.coreIndex )
                       + " scaling governor '" + *currentGovernor + "' instead of '" + *expectedGovernor + "'", LOG_DEBUG );
              return false;
            }
          }

          // check energy performance preference (skip if value is not available on this system)
          if ( not m_noEPPWriteQuirk and not profile.cpu.energyPerformancePreference.empty()
               and isEPPAvailable( profile.cpu.energyPerformancePreference ) )
          {
            auto currentEPP = core.energyPerformancePreference.read();

            if ( currentEPP.has_value() and *currentEPP != profile.cpu.energyPerformancePreference )
            {
              logLine( "CpuWorker: Unexpected value core" + std::to_string( core.coreIndex )
                       + " energy performance preference => '" + *currentEPP
                       + "' instead of '" + profile.cpu.energyPerformancePreference + "'", LOG_DEBUG );
              return false;
            }
          }
        }
      }

      // check no_turbo setting
      if ( m_cpuCtrl.intelPstateNoTurbo.isAvailable() )
      {
        auto currentNoTurbo = m_cpuCtrl.intelPstateNoTurbo.read();

        if ( currentNoTurbo.has_value() and *currentNoTurbo != profile.cpu.noTurbo )
        {
          logLine( "CpuWorker: Unexpected value noTurbo => '"
                   + std::string( *currentNoTurbo ? "true" : "false" )
                   + "' instead of '" + std::string( profile.cpu.noTurbo ? "true" : "false" ) + "'", LOG_DEBUG );
          return false;
        }
      }
    }

    return true;
  }
};
