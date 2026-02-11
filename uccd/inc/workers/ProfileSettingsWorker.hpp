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

#include "../profiles/UccProfile.hpp"
#include "../PowerSupplyController.hpp"
#include "../SysfsNode.hpp"
#include "../TccSettings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <syslog.h>
#include <vector>

// Forward declaration
class TuxedoIOAPI;

namespace fs = std::filesystem;

/**
 * @brief TDP information structure
 */
struct TDPInfo
{
  uint32_t min;
  uint32_t max;
  uint32_t current;
  std::string descriptor;
};

/**
 * @brief ProfileSettingsWorker - Merged worker for idle profile-related subsystems
 *
 * Replaces the former ODMProfileWorker, ODMPowerLimitWorker, ChargingWorker,
 * and YCbCr420WorkaroundWorker.  None of those needed periodic onWork() activity;
 * they only did real work at start-up and on explicit reapplyProfile() calls.
 * Combining them eliminates four dedicated QThreads.
 *
 * Lives on the main thread -- no DaemonWorker / QThread inheritance.
 */
class ProfileSettingsWorker
{
public:
  ProfileSettingsWorker(
    TuxedoIOAPI &ioApi,
    std::function< UccProfile() > getActiveProfileCallback,
    std::function< void( const std::vector< std::string > & ) > setOdmProfilesAvailableCallback,
    std::function< void( const std::string & ) > setOdmPowerLimitsJSON,
    std::function< void( const std::string & ) > logFunction,
    TccSettings &settings,
    std::atomic< bool > &modeReapplyPending,
    std::atomic< int32_t > &nvidiaPowerCTRLDefaultPowerLimit,
    std::atomic< int32_t > &nvidiaPowerCTRLMaxPowerLimit,
    std::atomic< bool > &nvidiaPowerCTRLAvailable,
    std::atomic< bool > &cTGPAdjustmentSupported )
    : m_ioApi( ioApi ),
      m_getActiveProfile( std::move( getActiveProfileCallback ) ),
      m_setOdmProfilesAvailable( std::move( setOdmProfilesAvailableCallback ) ),
      m_setOdmPowerLimitsJSON( std::move( setOdmPowerLimitsJSON ) ),
      m_logFunction( std::move( logFunction ) ),
      m_settings( settings ),
      m_modeReapplyPending( modeReapplyPending ),
      m_nvidiaPowerCTRLDefaultPowerLimit( nvidiaPowerCTRLDefaultPowerLimit ),
      m_nvidiaPowerCTRLMaxPowerLimit( nvidiaPowerCTRLMaxPowerLimit ),
      m_nvidiaPowerCTRLAvailable( nvidiaPowerCTRLAvailable ),
      m_cTGPAdjustmentSupported( cTGPAdjustmentSupported )
  {
  }

  /**
   * @brief Perform all one-time initialisation formerly done by each worker's onStart().
   *
   * Call this once after construction and after all callbacks are wired up.
   */
  void start();

  // =====================================================================
  //  ODM Profile API  (was ODMProfileWorker)
  // =====================================================================

  void reapplyODMProfile() { applyODMProfile(); }

  // =====================================================================
  //  ODM Power Limit API  (was ODMPowerLimitWorker)
  // =====================================================================

  void reapplyProfile()
  {
    logLine( "ProfileSettingsWorker: reapplyProfile() called" );
    applyODMPowerLimits();
    applyODMProfile();
  }

  std::vector< TDPInfo > getTDPInfo();
  bool setTDPValues( const std::vector< uint32_t > &values );

  // =====================================================================
  //  Charging API  (was ChargingWorker)
  // =====================================================================

  bool hasChargingProfile() const noexcept
  {
    return SysfsNode< std::string >( CHARGING_PROFILE ).isAvailable() and
           SysfsNode< std::string >( CHARGING_PROFILES_AVAILABLE ).isAvailable();
  }

  std::vector< std::string > getChargingProfilesAvailable() const noexcept
  {
    if ( not hasChargingProfile() )
      return {};

    auto profiles =
      SysfsNode< std::vector< std::string > >( CHARGING_PROFILES_AVAILABLE, " " ).read();
    return profiles.value_or( std::vector< std::string >{} );
  }

  std::string getCurrentChargingProfile() const noexcept
  {
    if ( m_currentChargingProfile.empty() )
      return "";

    return m_currentChargingProfile;
  }

  bool applyChargingProfile( const std::string &profileDescriptor ) noexcept;

  // --- Charging Priority ---

  bool hasChargingPriority() const noexcept
  {
    return SysfsNode< std::string >( CHARGING_PRIORITY ).isAvailable() and
           SysfsNode< std::string >( CHARGING_PRIORITIES_AVAILABLE ).isAvailable();
  }

  std::vector< std::string > getChargingPrioritiesAvailable() const noexcept
  {
    if ( not hasChargingPriority() )
      return {};

    auto prios = SysfsNode< std::vector< std::string > >( CHARGING_PRIORITIES_AVAILABLE, " " ).read();
    return prios.value_or( std::vector< std::string >{} );
  }

  std::string getCurrentChargingPriority() const noexcept
  {
    if ( m_currentChargingPriority.empty() )
      return "";

    return m_currentChargingPriority;
  }

  bool applyChargingPriority( const std::string &priorityDescriptor ) noexcept;

  // --- Charge Thresholds ---

  std::vector< int > getChargeStartAvailableThresholds() const noexcept;
  std::vector< int > getChargeEndAvailableThresholds() const noexcept;

  int getChargeStartThreshold() const noexcept
  {
    auto battery = PowerSupplyController::getFirstBattery();
    if ( not battery )
      return -1;

    return battery->getChargeControlStartThreshold();
  }

  bool setChargeStartThreshold( int value ) noexcept;

  int getChargeEndThreshold() const noexcept
  {
    auto battery = PowerSupplyController::getFirstBattery();
    if ( not battery )
      return -1;

    return battery->getChargeControlEndThreshold();
  }

  bool setChargeEndThreshold( int value ) noexcept;

  // --- Charge Type ---

  std::string getChargeType() const noexcept;
  bool setChargeType( const std::string &type ) noexcept;

  // --- Charging JSON ---

  std::string getChargingProfilesAvailableJSON() const noexcept;
  std::string getChargingPrioritiesAvailableJSON() const noexcept;
  std::string getChargeStartAvailableThresholdsJSON() const noexcept;
  std::string getChargeEndAvailableThresholdsJSON() const noexcept;

  // =====================================================================
  //  YCbCr 4:2:0 API  (was YCbCr420WorkaroundWorker)
  // =====================================================================

  bool isYCbCr420Available() const noexcept { return m_ycbcr420Available; }

  // =====================================================================
  //  NVIDIA Power Control API  (was NVIDIAPowerCTRLListener)
  // =====================================================================

  bool isNVIDIAPowerCTRLAvailable() const noexcept { return m_nvidiaPowerCTRLAvailable; }

  /**
   * @brief Called when the active profile changes to apply the new cTGP offset.
   */
  void onNVIDIAPowerProfileChanged()
  {
    if ( !m_nvidiaPowerCTRLAvailable )
      return;

    applyNVIDIACTGPOffset();
  }

  /**
   * @brief Periodic validation — checks if an external process changed the cTGP offset
   *        and re-applies the profile value if needed.
   *
   * Call this from the service's onWork() loop (original interval was 5 000 ms).
   */
  void validateNVIDIACTGPOffset();

private:
  // ----- ODM Profile internals -----

  enum class ODMProfileType
  {
    None,
    TuxedoPlatformProfile,
    AcpiPlatformProfile,
    TuxedoIOAPI
  };

  TuxedoIOAPI &m_ioApi;
  std::function< UccProfile() > m_getActiveProfile;
  std::function< void( const std::vector< std::string > & ) > m_setOdmProfilesAvailable;
  std::function< void( const std::string & ) > m_setOdmPowerLimitsJSON;
  std::function< void( const std::string & ) > m_logFunction;
  ODMProfileType m_odmProfileType = ODMProfileType::None;

  // --- Sysfs path constants ---

  static inline const std::string TUXEDO_PLATFORM_PROFILE =
    "/sys/bus/platform/devices/tuxedo_platform_profile/platform_profile";
  static inline const std::string TUXEDO_PLATFORM_PROFILE_CHOICES =
    "/sys/bus/platform/devices/tuxedo_platform_profile/platform_profile_choices";
  static inline const std::string ACPI_PLATFORM_PROFILE =
    "/sys/firmware/acpi/platform_profile";
  static inline const std::string ACPI_PLATFORM_PROFILE_CHOICES =
    "/sys/firmware/acpi/platform_profile_choices";

  static inline const std::string CHARGING_PROFILE =
    "/sys/devices/platform/tuxedo_keyboard/charging_profile/charging_profile";
  static inline const std::string CHARGING_PROFILES_AVAILABLE =
    "/sys/devices/platform/tuxedo_keyboard/charging_profile/charging_profiles_available";

  static inline const std::string CHARGING_PRIORITY =
    "/sys/devices/platform/tuxedo_keyboard/charging_priority/charging_prio";
  static inline const std::string CHARGING_PRIORITIES_AVAILABLE =
    "/sys/devices/platform/tuxedo_keyboard/charging_priority/charging_prios_available";

  static inline const std::string NVIDIA_CTGP_OFFSET =
    "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";

  void detectODMProfileType();
  std::vector< std::string > readPlatformProfileChoices( const std::string &path );

  bool getAvailableProfilesViaAPI( [[maybe_unused]] std::vector< std::string > &profiles )
  {
    return false;
  }

  std::string getDefaultProfileViaAPI() { return ""; }

  bool setProfileViaAPI( [[maybe_unused]] const std::string &profileName ) { return false; }

  void applyODMProfile();
  void applyPlatformProfile(
    const std::string &profilePath, const std::string &choicesPath,
    const std::string &chosenProfileName );
  void applyProfileViaAPI( const std::string &chosenProfileName );

  // ----- ODM Power Limit internals -----

  void logLine( const std::string &message );
  void applyODMPowerLimits();

  // ----- Charging internals -----

  std::string m_currentChargingProfile;
  std::string m_currentChargingPriority;

  void initializeChargingSettings() noexcept;

  // ----- YCbCr 4:2:0 internals -----

  TccSettings &m_settings;
  std::atomic< bool > &m_modeReapplyPending;
  bool m_ycbcr420Available = false;

  // ----- NVIDIA Power Control internals -----

  int32_t m_lastAppliedNVIDIAOffset = 0;
  std::atomic< int32_t > &m_nvidiaPowerCTRLDefaultPowerLimit;
  std::atomic< int32_t > &m_nvidiaPowerCTRLMaxPowerLimit;
  std::atomic< bool > &m_nvidiaPowerCTRLAvailable;
  std::atomic< bool > &m_cTGPAdjustmentSupported;

  bool fileExists( const std::string &path ) const
  {
    std::error_code ec;
    return fs::exists( path, ec ) && fs::is_regular_file( path, ec );
  }

  void checkYCbCr420Availability();
  void applyYCbCr420Workaround();

  // ----- NVIDIA Power Control private methods -----

  void initNVIDIAPowerCTRL();
  int32_t getNVIDIAProfileOffset() const;
  void applyNVIDIACTGPOffset();
  void queryNVIDIAPowerLimits();

  bool checkNVIDIAAvailability() const
  {
    std::error_code ec;
    return fs::exists( NVIDIA_CTGP_OFFSET, ec ) && fs::is_regular_file( NVIDIA_CTGP_OFFSET, ec );
  }


  static int32_t executeNvidiaSmi( const std::string &command );
};
