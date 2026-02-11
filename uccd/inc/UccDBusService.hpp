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

#include <sdbus-c++/sdbus-c++.h>
#include <atomic>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <optional>
#include "workers/DaemonWorker.hpp"
#include "workers/HardwareMonitorWorker.hpp"
#include "workers/DisplayWorker.hpp"
#include "workers/CpuWorker.hpp"
#include "workers/FanControlWorker.hpp"
#include "workers/KeyboardBacklightListener.hpp"
#include "workers/ProfileSettingsWorker.hpp"
#include "workers/LCTWaterCoolerWorker.hpp"
#include "FnLockController.hpp"
#include "profiles/UccProfile.hpp"
#include "profiles/DefaultProfiles.hpp"
#include "ProfileManager.hpp"
#include "SettingsManager.hpp"
#include "AutosaveManager.hpp"
#include "TccSettings.hpp"
#include "tuxedo_io_lib/tuxedo_io_api.hh"
#include <atomic>

// Forward declarations
class HardwareMonitorWorker;
class UccDBusService;

// helper functions for JSON serialization
std::string dgpuInfoToJSON( const DGpuInfo &info );
std::string igpuInfoToJSON( const IGpuInfo &info );

/**
 * @brief Time-stamped data structure
 *
 * Holds a value along with its timestamp for DBus transmission.
 */
template< typename T >
struct TimeData
{
  int64_t timestamp;
  T data;

  TimeData() : timestamp( 0 ), data{} {}

  TimeData( int64_t ts, const T &value ) : timestamp( ts ), data( value ) {}

  void set( int64_t ts, const T &value )
  {
    timestamp = ts;
    data = value;
  }
};

/**
 * @brief Fan data structure
 *
 * Contains timestamped speed and temperature data for a fan.
 */
struct FanData
{
  TimeData< int32_t > speed;
  TimeData< int32_t > temp;

  FanData() : speed(), temp() {}
};

/**
 * @brief DBus data container
 *
 * Contains all data that is exposed via the DBus interface.
 * This structure mirrors the UccDBusData TypeScript class.
 */
class UccDBusData
{
public:
  std::string device;
  std::string displayModes;
  std::atomic< bool > isX11;
  std::atomic< bool > tuxedoWmiAvailable;
  std::atomic< bool > fanHwmonAvailable;
  std::string uccdVersion;
  std::vector< FanData > fans;
  std::atomic< bool > webcamSwitchAvailable;
  std::atomic< bool > webcamSwitchStatus;
  std::atomic< bool > forceYUV420OutputSwitchAvailable;
  std::string dGpuInfoValuesJSON;
  std::string iGpuInfoValuesJSON;
  std::string cpuPowerValuesJSON;
  std::string primeState;
  std::atomic< bool > modeReapplyPending;
  std::string tempProfileName;
  std::string tempProfileId;
  std::string activeProfileJSON;
  std::string profilesJSON;
  std::string customProfilesJSON;
  std::string defaultProfilesJSON;
  std::string defaultValuesProfileJSON;
  std::string settingsJSON;
  std::vector< std::string > odmProfilesAvailable;
  std::string odmPowerLimitsJSON;
  std::string keyboardBacklightCapabilitiesJSON;
  std::string keyboardBacklightStatesJSON;
  std::string keyboardBacklightStatesNewJSON;
  std::atomic< int32_t > fansMinSpeed;
  std::atomic< bool > fansOffAvailable;
  std::string chargingProfilesAvailable;
  std::string currentChargingProfile;
  std::string chargingPrioritiesAvailable;
  std::string currentChargingPriority;
  std::string chargeStartAvailableThresholds;
  std::string chargeEndAvailableThresholds;
  std::atomic< int32_t > chargeStartThreshold;
  std::atomic< int32_t > chargeEndThreshold;
  std::string chargeType;
  std::atomic< bool > fnLockSupported;
  std::atomic< bool > fnLockStatus;
  std::atomic< bool > sensorDataCollectionStatus;
  std::atomic< bool > d0MetricsUsage;
  std::atomic< int32_t > nvidiaPowerCTRLDefaultPowerLimit;
  std::atomic< int32_t > nvidiaPowerCTRLMaxPowerLimit;
  std::atomic< bool > nvidiaPowerCTRLAvailable;
  std::atomic< bool > waterCoolerAvailable;
  std::atomic< bool > waterCoolerConnected;
  std::atomic< bool > waterCoolerScanningEnabled;
  std::atomic< bool > waterCoolerSupported;
  std::atomic< bool > cTGPAdjustmentSupported;

  std::mutex dataMutex;

  explicit UccDBusData( int numberFans = 3 )
    : device( "unknown" ),
      displayModes( "[]" ),
      isX11( false ),
      tuxedoWmiAvailable( false ),
      fanHwmonAvailable( false ),
      uccdVersion( "0.0.0" ),
      fans( numberFans ),
      webcamSwitchAvailable( false ),
      webcamSwitchStatus( false ),
      forceYUV420OutputSwitchAvailable( false ),
      dGpuInfoValuesJSON( "{}" ),
      iGpuInfoValuesJSON( "{}" ),
      cpuPowerValuesJSON( "{}" ),
      primeState( "unknown" ),
      modeReapplyPending( false ),
      tempProfileName( "" ),
      tempProfileId( "" ),
      activeProfileJSON( "{}" ),
      profilesJSON( "[]" ),
      customProfilesJSON( "[]" ),
      defaultProfilesJSON( "[]" ),
      defaultValuesProfileJSON( "{}" ),
      settingsJSON( "{}" ),
      odmProfilesAvailable(),
      odmPowerLimitsJSON( "{}" ),
      keyboardBacklightCapabilitiesJSON( "{}" ),
      keyboardBacklightStatesJSON( "{}" ),
      keyboardBacklightStatesNewJSON( "{}" ),
      fansMinSpeed( 0 ),
      fansOffAvailable( false ),
      chargingProfilesAvailable( "[]" ),
      currentChargingProfile( "" ),
      chargingPrioritiesAvailable( "[]" ),
      currentChargingPriority( "" ),
      chargeStartAvailableThresholds( "[]" ),
      chargeEndAvailableThresholds( "[]" ),
      chargeStartThreshold( -1 ),
      chargeEndThreshold( -1 ),
      chargeType( "Unknown" ),
      fnLockSupported( false ),
      fnLockStatus( false ),
      sensorDataCollectionStatus( false ),
      d0MetricsUsage( false ),
      nvidiaPowerCTRLDefaultPowerLimit( 0 ),
      nvidiaPowerCTRLMaxPowerLimit( 1000 ),
      nvidiaPowerCTRLAvailable( false ),
        waterCoolerAvailable( false ),
        waterCoolerConnected( false ),
        waterCoolerScanningEnabled( true ),
        waterCoolerSupported( false ),
        cTGPAdjustmentSupported( true )
  {
  }
};

/**
 * @brief TCC DBus Interface Adaptor
 *
 * Implements the com.uniwill.uccd DBus interface.
 * Handles all method calls from DBus clients and provides access to daemon data.
 */
class UccDBusInterfaceAdaptor
{
public:
  static constexpr const char* INTERFACE_NAME = "com.uniwill.uccd";

  /**
   * @brief Constructor
   * @param object DBus object interface
   * @param data Shared data structure (includes mutex)
   * @param service Reference to UccDBusService for profile operations
   */
  explicit UccDBusInterfaceAdaptor( sdbus::IObject &object,
                                    UccDBusData &data,
                                    UccDBusService *service = nullptr );

  ~UccDBusInterfaceAdaptor() = default;

  // device and system information
  std::string GetDeviceName();
  std::string GetDisplayModesJSON();
  bool GetIsX11();
  bool TuxedoWmiAvailable();
  bool FanHwmonAvailable();
  std::string UccdVersion();

  // fan data methods
  std::map< std::string, std::map< std::string, sdbus::Variant > > GetFanDataCPU();
  std::map< std::string, std::map< std::string, sdbus::Variant > > GetFanDataGPU1();
  std::map< std::string, std::map< std::string, sdbus::Variant > > GetFanDataGPU2();

  // webcam and display methods
  bool WebcamSWAvailable();
  bool GetWebcamSWStatus();
  bool GetForceYUV420OutputSwitchAvailable();
  int32_t GetDisplayBrightness();
  bool SetDisplayBrightness( int32_t brightness );
  bool SetDisplayRefreshRate( const std::string &display, int refreshRate );

  // gpu information methods
  std::string GetDGpuInfoValuesJSON();
  std::string GetIGpuInfoValuesJSON();
  std::string GetCpuPowerValuesJSON();

  // graphics methods
  std::string GetPrimeState();
  bool ConsumeModeReapplyPending();

  // profile methods
  std::string GetActiveProfileJSON();
  std::string GetPowerState();
  bool SetTempProfile( const std::string &profileName );
  bool SetTempProfileById( const std::string &id );
  bool SetActiveProfile( const std::string &id );
  bool ApplyProfile( const std::string &profileJSON );
  std::string GetProfilesJSON();
  std::string GetCustomProfilesJSON();
  // Fan profile get/set for editable (custom) profiles only
  bool SetFanProfileCPU( const std::string &pointsJSON );
  bool SetFanProfileDGPU( const std::string &pointsJSON );
  bool ApplyFanProfiles( const std::string &fanProfilesJSON );
  bool RevertFanProfiles();
  std::string GetDefaultProfilesJSON();
  std::string GetCpuFrequencyLimitsJSON();
  std::string GetDefaultValuesProfileJSON();
  bool AddCustomProfile( const std::string &profileJSON );
  bool SaveCustomProfile( const std::string &profileJSON );
  bool DeleteCustomProfile( const std::string &profileId );
  bool UpdateCustomProfile( const std::string &profileJSON );
  std::string GetFanProfile( const std::string &name );
  std::string GetFanProfileNames();
  bool SetFanProfile( const std::string &name, const std::string &json );

  // settings methods
  std::string GetSettingsJSON();
  bool SetStateMap( const std::string &state, const std::string &profileId );

  // odm methods
  std::vector< std::string > ODMProfilesAvailable();
  std::string ODMPowerLimitsJSON();

  // keyboard backlight methods
  std::string GetKeyboardBacklightCapabilitiesJSON();
  std::string GetKeyboardBacklightStatesJSON();
  bool SetKeyboardBacklightStatesJSON( const std::string &keyboardBacklightStatesJSON );

  // fan control methods
  int32_t GetFansMinSpeed();
  bool GetFansOffAvailable();

  // charging methods
  std::string GetChargingProfilesAvailable();
  std::string GetCurrentChargingProfile();
  bool SetChargingProfile( const std::string &profileDescriptor );
  std::string GetChargingPrioritiesAvailable();
  std::string GetCurrentChargingPriority();
  bool SetChargingPriority( const std::string &priorityDescriptor );
  std::string GetChargeStartAvailableThresholds();
  std::string GetChargeEndAvailableThresholds();
  int32_t GetChargeStartThreshold();
  int32_t GetChargeEndThreshold();
  bool SetChargeStartThreshold( int32_t value );
  bool SetChargeEndThreshold( int32_t value );
  std::string GetChargeType();
  bool SetChargeType( const std::string &type );

  // fn lock methods
  bool GetFnLockSupported();
  bool GetFnLockStatus();
  void SetFnLockStatus( bool status );

  // sensor data collection methods
  void SetSensorDataCollectionStatus( bool status );
  bool GetSensorDataCollectionStatus();
  void SetDGpuD0Metrics( bool status );

  // nvidia power control methods
  int32_t GetNVIDIAPowerCTRLDefaultPowerLimit();
  int32_t GetNVIDIAPowerCTRLMaxPowerLimit();
  bool GetNVIDIAPowerCTRLAvailable();
  std::string GetAvailableGovernors();

  // water cooler methods
  bool GetWaterCoolerAvailable();
  bool GetWaterCoolerConnected();
  int32_t GetWaterCoolerFanSpeed();
  int32_t GetWaterCoolerPumpLevel();
  bool EnableWaterCooler( bool enable );
  bool SetWaterCoolerFanSpeed( int32_t dutyCyclePercent );
  bool SetWaterCoolerPumpVoltage( int32_t voltage );
  bool SetWaterCoolerLEDColor( int32_t red, int32_t green, int32_t blue, int32_t mode );
  bool TurnOffWaterCoolerLED();
  bool TurnOffWaterCoolerFan();
  bool TurnOffWaterCoolerPump();
  bool IsWaterCoolerAutoControlEnabled();

  // device capability methods
  bool GetWaterCoolerSupported();
  bool GetCTGPAdjustmentSupported();

  // signal emitters
  void emitModeReapplyPendingChanged( bool pending );
  void emitProfileChanged( const std::string &profileId );

  // allow UccDBusService to access timeout handling
  friend class UccDBusService;

  // Required for manual vtable registration in sdbus-c++ 2.x
  void registerAdaptor();

private:
  sdbus::IObject &m_object;
  UccDBusData &m_data;
  UccDBusService *m_service;
  std::chrono::steady_clock::time_point m_lastDataCollectionAccess;

  void resetDataCollectionTimeout();
  std::map< std::string, std::map< std::string, sdbus::Variant > > exportFanData( const FanData &fanData );
};

/**
 * @brief TCC DBus Service Worker
 *
 * Manages the DBus service lifecycle as a daemon worker.
 * Exports the TCC interface on the system bus and handles periodic updates.
 */
class UccDBusService : public DaemonWorker
{
public:
  /**
   * @brief Constructor
   */
  UccDBusService();

  virtual ~UccDBusService() = default;

  // Prevent copy and move
  UccDBusService( const UccDBusService & ) = delete;
  UccDBusService( UccDBusService && ) = delete;
  UccDBusService &operator=( const UccDBusService & ) = delete;
  UccDBusService &operator=( UccDBusService && ) = delete;

  /**
   * @brief Get the DBus interface adaptor
   * @return Pointer to adaptor or nullptr if not initialized
   */
  UccDBusInterfaceAdaptor *getAdaptor() noexcept
  {
    return m_adaptor.get();
  }

  // Control water cooler scanning (can be called by DBus adaptor)
  void setWaterCoolerScanningEnabled( bool enable );

  /**
   * @brief Get the CPU worker
   * @return Pointer to CPU worker or nullptr if not initialized
   */
  CpuWorker *getCpuWorker() noexcept
  {
    return m_cpuWorker.get();
  }

  /**
   * @brief Get the DBus data reference for testing
   */
  const UccDBusData &getDbusData() const noexcept
  {
    return m_dbusData;
  }

  // profile management methods
  UccProfile getCurrentProfile() const;
  bool setCurrentProfileByName( const std::string &profileName );
  bool setCurrentProfileById( const std::string &id );
  bool applyProfileJSON( const std::string &profileJSON );
  std::vector< UccProfile > getAllProfiles() const;
  std::vector< UccProfile > getDefaultProfiles() const;
  std::vector< UccProfile > getCustomProfiles() const;
  UccProfile getDefaultProfile() const;
  void updateDBusActiveProfileData();
  void updateDBusSettingsData();
  
  // profile manipulation methods
  bool addCustomProfile( const UccProfile &profile );
  bool deleteCustomProfile( const std::string &profileId );
  bool updateCustomProfile( const UccProfile &profile );

  // Allow UccDBusInterfaceAdaptor to access private members
  friend class UccDBusInterfaceAdaptor;

public:
  void onStart() override;

protected:
  void onWork() override;
  void onExit() override;

private:
  static constexpr const char* INTERFACE_NAME = "com.uniwill.uccd";
  UccDBusData m_dbusData;
  TuxedoIOAPI m_io;
  std::unique_ptr< sdbus::IConnection > m_connection;
  std::unique_ptr< sdbus::IObject > m_object;
  std::unique_ptr< UccDBusInterfaceAdaptor > m_adaptor;
  bool m_started;

  // profile management
  ProfileManager m_profileManager;
  SettingsManager m_settingsManager;
  AutosaveManager m_autosaveManager;
  TccSettings m_settings;
  TccAutosave m_autosave;
  UccProfile m_activeProfile;
  std::vector< UccProfile > m_defaultProfiles;
  std::vector< UccProfile > m_customProfiles;
  
  // state switching
  ProfileState m_currentState;
  std::string m_currentStateProfileId;
  
  // water cooler state tracking
  bool m_previousWaterCoolerConnected;
  std::atomic< int32_t > m_waterCoolerLedMode{ 0 };  // Tracks the GUI-requested LED mode (may be Temperature)

  // Water cooler debounce – avoids reacting to brief BLE connect/disconnect
  // glitches that cause rapid power-state oscillation.
  bool m_wcDebouncePending = false;
  bool m_wcDebouncedTarget = false;                           // the state we are debouncing towards
  std::chrono::steady_clock::time_point m_wcDebounceStart{};  // when the pending change was first seen
  static constexpr int WC_CONNECT_DEBOUNCE_S    = 3;          // seconds stable before accepting "connected"
  static constexpr int WC_DISCONNECT_DEBOUNCE_S = 10;         // seconds stable before accepting "disconnected"

  void setupGpuDataCallback();
  void updateFanData();
  void loadProfiles();
  void loadSettings();
  void applyStartupProfile();
  void loadAutosave();
  void saveAutosave();
  void initializeProfiles();
  void initializeDisplayModes();
  void serializeProfilesJSON();
  void applyProfileForCurrentState();
  void fillDeviceSpecificDefaults( std::vector< UccProfile > &profiles );
  std::optional< UniwillDeviceID > identifyDevice();
  void computeDeviceCapabilities();
  bool syncOutputPortsSetting();
  std::vector< std::vector< std::string > > getOutputPorts();

  // workers
  std::unique_ptr< HardwareMonitorWorker > m_hardwareMonitorWorker;
  std::unique_ptr< DisplayWorker > m_displayWorker;
  std::unique_ptr< CpuWorker > m_cpuWorker;
  std::unique_ptr< ProfileSettingsWorker > m_profileSettingsWorker;
  std::unique_ptr< FanControlWorker > m_fanControlWorker;
  std::unique_ptr< KeyboardBacklightListener > m_keyboardBacklightListener;
  std::unique_ptr< LCTWaterCoolerWorker > m_waterCoolerWorker;

  // identified device
  std::optional< UniwillDeviceID > m_deviceId;

  // periodic validation counters
  uint32_t m_nvidiaValidationCounter = 0;

  // controllers
  FnLockController m_fnLockController;

  static constexpr const char *SERVICE_NAME = "com.uniwill.uccd";
  static constexpr const char *OBJECT_PATH = "/com/uniwill/uccd";
};
