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
    , m_getActiveProfile( std::move( getActiveProfile ) )
    , m_getCpuSettingsEnabled( std::move( getCpuSettingsEnabled ) )
    , m_logFunction( std::move( logFunction ) )
    , m_cpuCtrl( std::make_unique< CpuController >() )
  {
    // check for EPP write quirks for specific devices
    m_noEPPWriteQuirk = false; // todo: implement device detection if needed
    m_validationFailureCount = 0;
    m_reapplyGaveUp = false;
  }

  void onStart() override
  {
    if ( m_getCpuSettingsEnabled() && !m_getActiveProfile().id.empty() )
    {
      applyCpuProfile( m_getActiveProfile() );
    }
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
   * @return Vector of available governor names, or nullopt if unavailable
   */
  std::optional< std::vector< std::string > > getAvailableGovernors()
  {
    if ( m_cpuCtrl->cores.empty() )
      return std::nullopt;

    return m_cpuCtrl->cores[ 0 ].scalingAvailableGovernors.read();
  }

private:
  std::function< UccProfile() > m_getActiveProfile;
  std::function< bool() > m_getCpuSettingsEnabled;
  std::function< void( const std::string & ) > m_logFunction;
  std::unique_ptr< CpuController > m_cpuCtrl;
  bool m_noEPPWriteQuirk;
  int m_validationFailureCount;
  bool m_reapplyGaveUp;

  static constexpr int maxReapplyAttempts = 3;

  const std::vector< std::string > m_preferredAcpiFreqGovernors = {
    "ondemand", "schedutil", "conservative"
  };

  const std::vector< std::string > m_preferredPerformanceAcpiFreqGovernors = {
    "performance"
  };

  void logLine( const std::string &message, int priority = LOG_INFO )
  {
    if ( m_logFunction )
    {
      m_logFunction( message );
    }

    syslog( priority, "%s", message.c_str() );
  }

  /**
   * @brief Find default governor for current system
   */
  std::optional< std::string > findDefaultGovernor()
  {
    if ( m_cpuCtrl->cores.empty() )
      return std::nullopt;

    auto scalingDriver = m_cpuCtrl->cores[ 0 ].scalingDriver.read();

    if ( not scalingDriver.has_value() )
      return std::nullopt;

    auto driverEnum = CpuController::getScalingDriverEnum( *scalingDriver );

    // intel_pstate and amd-pstate-epp use fixed 'powersave' governor
    if ( driverEnum == ScalingDriver::intel_pstate or driverEnum == ScalingDriver::amd_pstate_epp )
    {
      return "powersave";
    }

    // for other drivers (acpi-cpufreq), prefer ondemand/schedutil/conservative
    auto availableGovernors = m_cpuCtrl->cores[ 0 ].scalingAvailableGovernors.read();

    if ( not availableGovernors.has_value() )
      return std::nullopt;

    for ( const auto &preferred : m_preferredAcpiFreqGovernors )
    {
      if ( std::find( availableGovernors->begin(), availableGovernors->end(), preferred ) != availableGovernors->end() )
      {
        return preferred;
      }
    }

    return std::nullopt;
  }

  /**
   * @brief Find performance governor for current system
   */
  std::optional< std::string > findPerformanceGovernor()
  {
    if ( m_cpuCtrl->cores.empty() )
      return std::nullopt;

    auto scalingDriver = m_cpuCtrl->cores[ 0 ].scalingDriver.read();

    if ( not scalingDriver.has_value() )
      return std::nullopt;

    auto driverEnum = CpuController::getScalingDriverEnum( *scalingDriver );

    // intel_pstate and amd-pstate-epp use fixed 'performance' governor
    if ( driverEnum == ScalingDriver::intel_pstate or driverEnum == ScalingDriver::amd_pstate_epp )
    {
      return "performance";
    }

    // for other drivers, use 'performance' if available
    auto availableGovernors = m_cpuCtrl->cores[ 0 ].scalingAvailableGovernors.read();

    if ( not availableGovernors.has_value() )
      return std::nullopt;

    for ( const auto &preferred : m_preferredPerformanceAcpiFreqGovernors )
    {
      if ( std::find( availableGovernors->begin(), availableGovernors->end(), preferred ) != availableGovernors->end() )
      {
        return preferred;
      }
    }

    return std::nullopt;
  }

  /**
   * @brief Apply CPU settings from profile
   */
  void applyCpuProfile( const UccProfile &profile )
  {
    // reset everything to default before applying new settings
    setCpuDefaultConfig();

    // Set the governor from profile
    if ( !profile.cpu.governor.empty() )
    {
      m_cpuCtrl->setGovernor( profile.cpu.governor );
    }
    else
    {
      // fallback to default
      auto governor = findDefaultGovernor();
      m_cpuCtrl->setGovernor( governor );
    }

    if ( not m_noEPPWriteQuirk )
    {
      m_cpuCtrl->setEnergyPerformancePreference( profile.cpu.energyPerformancePreference );
    }

    m_cpuCtrl->setGovernorScalingMinFrequency( profile.cpu.scalingMinFrequency );
    m_cpuCtrl->setGovernorScalingMaxFrequency( profile.cpu.scalingMaxFrequency );

    // set number of online cores
    m_cpuCtrl->useCores( profile.cpu.onlineCores );

    // set no_turbo if available
    if ( m_cpuCtrl->intelPstateNoTurbo.isAvailable() )
    {
      m_cpuCtrl->intelPstateNoTurbo.write( profile.cpu.noTurbo );
    }
  }

  /**
   * @brief Reset CPU settings to defaults
   */
  void setCpuDefaultConfig()
  {
    m_cpuCtrl->useCores( std::nullopt ); // all cores
    m_cpuCtrl->setGovernorScalingMinFrequency( std::nullopt ); // min
    m_cpuCtrl->setGovernorScalingMaxFrequency( std::nullopt ); // max
    m_cpuCtrl->setGovernor( findDefaultGovernor() );

    if ( not m_noEPPWriteQuirk )
    {
      m_cpuCtrl->setEnergyPerformancePreference( "default" );
    }

    if ( m_cpuCtrl->intelPstateNoTurbo.isAvailable() )
    {
      m_cpuCtrl->intelPstateNoTurbo.write( false );
    }
  }

  /**
   * @brief Validate current CPU frequency settings match profile
   */
  bool validateCpuFreq()
  {
    const UccProfile profile = m_getActiveProfile();
    {
      // validate scaling frequencies match profile
      for ( const auto &core : m_cpuCtrl->cores )
      {
        if ( core.coreIndex == 0 or core.online.read().value_or( false ) )
        {
          // check minimum frequency
          if ( profile.cpu.scalingMinFrequency.has_value() )
          {
            auto currentMin = core.scalingMinFreq.read();

            if ( currentMin.has_value() and *currentMin != *profile.cpu.scalingMinFrequency )
            {
              logLine( "CpuWorker: Unexpected value core" + std::to_string( core.coreIndex )
                       + " minimum scaling frequency " + std::to_string( *currentMin )
                       + " instead of " + std::to_string( *profile.cpu.scalingMinFrequency ), LOG_DEBUG );
              return false;
            }
          }

          // check maximum frequency
          if ( profile.cpu.scalingMaxFrequency.has_value() )
          {
            auto currentMax = core.scalingMaxFreq.read();

            if ( currentMax.has_value() and *currentMax != *profile.cpu.scalingMaxFrequency )
            {
              logLine( "CpuWorker: Unexpected value core" + std::to_string( core.coreIndex )
                       + " maximum scaling frequency " + std::to_string( *currentMax )
                       + " instead of " + std::to_string( *profile.cpu.scalingMaxFrequency ), LOG_DEBUG );
              return false;
            }
          }

          // check governor
          auto defaultGovernor = findDefaultGovernor();

          if ( defaultGovernor.has_value() )
          {
            auto currentGovernor = core.scalingGovernor.read();

            if ( currentGovernor.has_value() and *currentGovernor != *defaultGovernor )
            {
              logLine( "CpuWorker: Unexpected value core" + std::to_string( core.coreIndex )
                       + " scaling governor '" + *currentGovernor + "' instead of '" + *defaultGovernor + "'", LOG_DEBUG );
              return false;
            }
          }

          // check energy performance preference
          if ( not m_noEPPWriteQuirk and not profile.cpu.energyPerformancePreference.empty() )
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
      if ( m_cpuCtrl->intelPstateNoTurbo.isAvailable() )
      {
        auto currentNoTurbo = m_cpuCtrl->intelPstateNoTurbo.read();

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
