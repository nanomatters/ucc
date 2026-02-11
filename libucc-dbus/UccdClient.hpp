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

#include <QObject>
#include <QString>
#include <QDBusInterface>
#include <QDBusConnection>
#include <QDBusReply>
#include <string>
#include <memory>
#include <optional>
#include <functional>
#include <vector>

namespace ucc
{

/**
 * @brief DBus client for communicating with uccd daemon
 *
 * Provides a Qt6-native interface to all uccd DBus methods.
 */
class UccdClient : public QObject
{
  Q_OBJECT

public:
  /**
   * @brief Construct client connected to system bus
   */
  explicit UccdClient( QObject *parent = nullptr );

  ~UccdClient() override = default;

  // Profile Management
  std::optional< std::string > getDefaultProfilesJSON();
  std::optional< std::string > getCpuFrequencyLimitsJSON();
  std::optional< std::string > getDefaultValuesProfileJSON();
  std::optional< std::string > getCustomProfilesJSON();
  std::optional< std::string > getActiveProfileJSON();
  std::optional< std::string > getSettingsJSON();
  std::optional< std::string > getPowerState();
  bool setStateMap( const std::string &state, const std::string &profileId );
  bool setActiveProfile( const std::string &profileId );
  bool setActiveProfileByName( const std::string &profileName );
  bool applyProfile( const std::string &profileJSON );
  std::optional< std::string > getProfileIdByName( const std::string &profileName );
  bool saveCustomProfile( const std::string &profileJSON );
  bool deleteCustomProfile( const std::string &profileId );
  std::optional< std::string > getFanProfile( const std::string &name );
  std::optional< std::vector< std::string > > getFanProfileNames();
  std::optional< bool > setFanProfile( const std::string &name, const std::string &json );

  // Display Control
  bool setDisplayBrightness( int brightness );
  std::optional< int > getDisplayBrightness();
  bool setYCbCr420Workaround( bool enabled );
  std::optional< bool > getYCbCr420Workaround();
  bool setDisplayRefreshRate( const std::string &display, int refreshRate );

  // CPU Control
  bool setCpuScalingGovernor( const std::string &governor );
  std::optional< std::string > getCpuScalingGovernor();
  std::optional< std::vector< std::string > > getAvailableCpuGovernors();
  bool setCpuFrequency( int minFreq, int maxFreq );
  bool setEnergyPerformancePreference( const std::string &preference );

  // Fan Control
  bool setFanProfile( const std::string &profileJSON );
  bool setFanProfileCPU( const std::string &pointsJSON );
  bool setFanProfileDGPU( const std::string &pointsJSON );
  bool applyFanProfiles( const std::string &fanProfilesJSON );
  bool revertFanProfiles();
  std::optional< std::string > getCurrentFanSpeed();
  std::optional< std::string > getFanTemperatures();

  // Power Management
  bool setODMPowerLimits( const std::vector< int > &limits );
  std::optional< std::vector< int > > getODMPowerLimits();

  // Charging Profile (firmware-level charging modes)
  std::optional< std::string > getChargingProfilesAvailable();
  std::optional< std::string > getCurrentChargingProfile();
  bool setChargingProfile( const std::string &profileDescriptor );

  // Charging Priority (USB-C PD priority)
  std::optional< std::string > getChargingPrioritiesAvailable();
  std::optional< std::string > getCurrentChargingPriority();
  bool setChargingPriority( const std::string &priorityDescriptor );

  // Battery Charge Thresholds
  std::optional< std::string > getChargeStartAvailableThresholds();
  std::optional< std::string > getChargeEndAvailableThresholds();
  std::optional< int > getChargeStartThreshold();
  std::optional< int > getChargeEndThreshold();
  bool setChargeStartThreshold( int value );
  bool setChargeEndThreshold( int value );

  // Charge Type
  std::optional< std::string > getChargeType();
  bool setChargeType( const std::string &type );

  // GPU Control
  bool setNVIDIAPowerOffset( int offset );
  std::optional< int > getNVIDIAPowerOffset();
  std::optional< int > getNVIDIAPowerCTRLMaxPowerLimit();
  bool setPrimeProfile( const std::string &profile );
  std::optional< std::string > getPrimeProfile();
  std::optional< std::string > getGpuInfo();

  // Device Capability Queries
  std::optional< bool > getWaterCoolerSupported();
  std::optional< bool > getCTGPAdjustmentSupported();

  // Keyboard Control
  bool setKeyboardBacklight( const std::string &config );
  std::optional< std::string > getKeyboardBacklightInfo();
  std::optional< std::string > getKeyboardBacklightStates();
  bool setFnLock( bool enabled );
  std::optional< bool > getFnLock();

  // Webcam Control
  bool setWebcamEnabled( bool enabled );
  std::optional< bool > getWebcamEnabled();

  // ODM Profile
  bool setODMPerformanceProfile( const std::string &profile );
  std::optional< std::string > getODMPerformanceProfile();
  std::optional< std::vector< std::string > > getAvailableODMProfiles();

  // System Monitoring
  std::optional< int > getCpuTemperature();
  std::optional< int > getGpuTemperature();
  std::optional< int > getCpuFrequency();
  std::optional< int > getGpuFrequency();
  std::optional< double > getCpuPower();
  std::optional< double > getGpuPower();
  std::optional< int > getFanSpeedRPM();
  std::optional< int > getGpuFanSpeedRPM();
  std::optional< int > getFanSpeedPercent();
  std::optional< int > getGpuFanSpeedPercent();
  // Water cooler readings (if available from daemon)
  std::optional< int > getWaterCoolerFanSpeed();
  std::optional< int > getWaterCoolerPumpLevel();

  // Signal Subscription
  using ProfileChangedCallback = std::function< void( const std::string &profileId ) >;
  using PowerStateChangedCallback = std::function< void( const std::string &state ) >;

  void subscribeProfileChanged( ProfileChangedCallback callback );
  void subscribePowerStateChanged( PowerStateChangedCallback callback );

  // Connection status
  bool isConnected() const;

signals:
  void profileChanged( const QString &profileId );
  void powerStateChanged( const QString &state );
  void connectionStatusChanged( bool connected );

private slots:
  void onProfileChangedSignal( const QString &profileId );
  void onPowerStateChangedSignal( const QString &state );

private:
  std::unique_ptr< QDBusInterface > m_interface;
  bool m_connected = false;

  static constexpr const char *DBUS_SERVICE = "com.uniwill.uccd";
  static constexpr const char *DBUS_PATH = "/com/uniwill/uccd";
  static constexpr const char *DBUS_INTERFACE = "com.uniwill.uccd";

  // Helper for DBus calls
  template< typename T >
  std::optional< T > callMethod( const QString &method ) const;

  template< typename T, typename... Args >
  std::optional< T > callMethod( const QString &method, const Args &...args ) const;

  bool callVoidMethod( const QString &method ) const;

  template< typename... Args >
  bool callVoidMethod( const QString &method, const Args &...args ) const;
};

} // namespace ucc
