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
#include <QDBusServiceWatcher>
#include "UccDbusTypes.hpp"
#include <string>
#include <memory>
#include <optional>
#include <functional>
#include <vector>
#include <map>
#include <chrono>

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

  // System Information
  std::optional< QVariantMap > getSystemInfo();
  std::optional< bool > isDeviceSupported();
  std::optional< QStringList > getCapabilities();

  // Profile Management — unified API (profiles have "editable" flag)
  std::optional< QVariantList > getProfiles();       // All profiles with editable flag
  std::optional< QVariantList > getDefaultProfiles(); // Backward-compat: built-in only
  std::optional< QVariantMap > getCpuFrequencyLimits();
  std::optional< QVariantMap > getDefaultValuesProfile();
  std::optional< QVariantList > getCustomProfiles();  // Backward-compat: custom only
  std::optional< QVariantMap > getActiveProfile();
  std::optional< QVariantMap > getAppliedProfiles();
  std::optional< QVariantMap > getSettings();
  std::optional< std::string > getPowerState();
  bool setStateMap( const std::string &state, const std::string &profileId );
  bool setBatchStateMap( const QMap< QString, QString > &stateMap );
  bool setActiveProfile( const std::string &profileId );
  bool applyProfile( const std::string &profileJSON );
  bool saveProfile( const std::string &profileJSON );         // Unified save
  bool deleteProfile( const std::string &profileId );         // Unified delete
  bool saveCustomProfile( const std::string &profileJSON );   // Backward-compat → saveProfile
  bool deleteCustomProfile( const std::string &profileId );   // Backward-compat → deleteProfile

  // Hardware zone model
  std::optional< ucc::dbus::HardwareFanDeviceDtoList > getHardwareFanDevices();
  std::optional< ucc::dbus::HardwareSensorDtoList > getHardwareSensors();
  std::optional< ucc::dbus::ThermalSourceDtoList > getThermalSources();
  std::optional< QVariantMap > getSensorReadings();
  std::optional< ucc::dbus::FanZoneDtoList > getFanZones();

  // Fan sub-profiles (built-in + custom, with editable flag)
  std::optional< ucc::dbus::ProfileSummaryDtoList > getFanProfiles();
  std::optional< ucc::dbus::FanZoneCurveDtoList > getFanProfileZones( const std::string &fanProfileId );
  std::optional< ucc::dbus::ThermalSourceDtoList > getFanProfileSources( const std::string &fanProfileId );
  bool saveFanProfile( const std::string &id, const std::string &name,
                       const ucc::dbus::FanZoneCurveDtoList &zones,
                       const ucc::dbus::ThermalSourceDtoList &thermalSources );
  bool deleteFanProfile( const std::string &id );

  // GPU sub-profiles (built-in + custom, with editable flag)
  std::optional< ucc::dbus::ProfileSummaryDtoList > getGpuProfiles();
  std::optional< QVariantMap > getGpuProfile( const std::string &gpuProfileId );
  bool saveGpuProfile( const std::string &id, const std::string &name, const std::string &json );
  bool deleteGpuProfile( const std::string &id );

  // Keyboard sub-profiles (custom only, all editable)
  std::optional< ucc::dbus::ProfileSummaryDtoList > getKeyboardProfiles();
  std::optional< QVariantMap > getKeyboardProfile( const std::string &keyboardProfileId );
  bool saveKeyboardProfile( const std::string &id, const std::string &name, const std::string &json );
  bool deleteKeyboardProfile( const std::string &id );

  // Legacy alias (deprecated)
  std::optional< bool > setFanProfile( const std::string &fanProfileId, const std::string &json );

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
  std::optional< std::vector< std::string > > getAvailableEPPs();
  std::optional< int > getCpuCoreCount();

  // Fan Control
  bool applyFanProfiles( const ucc::dbus::FanZoneCurveDtoList &zones,
                         const ucc::dbus::ThermalSourceDtoList &thermalSources,
                         const QString &fanProfileId );
  bool revertFanProfiles();
  std::optional< QVariantMap > getFanZoneTelemetry();
  std::optional< std::string > getCurrentFanSpeed();
  std::optional< std::string > getFanTemperatures();

  // Power Management
  bool setODMPowerLimits( const std::vector< int > &limits );
  std::optional< QVariantList > getODMPowerLimits();

  // Charging Profile (firmware-level charging modes)
  std::optional< QStringList > getChargingProfilesAvailable();
  std::optional< std::string > getCurrentChargingProfile();
  bool setChargingProfile( const std::string &profileDescriptor );

  // Charging Priority (USB-C PD priority)
  std::optional< QStringList > getChargingPrioritiesAvailable();
  std::optional< std::string > getCurrentChargingPriority();
  bool setChargingPriority( const std::string &priorityDescriptor );

  // Battery Charge Thresholds
  std::optional< QVariantList > getChargeStartAvailableThresholds();
  std::optional< QVariantList > getChargeEndAvailableThresholds();
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
  std::optional< int > getNVIDIAPowerCTRLMinPowerLimit();
  std::optional< int > getNVIDIAPowerCTRLDefaultPowerLimit();
  std::optional< bool > getNVIDIAPowerCTRLAvailable();
  bool setPrimeProfile( const std::string &profile );
  std::optional< std::string > getPrimeProfile();
  std::optional< std::string > getGpuInfo();

  // NVIDIA GPU OC Control
  std::optional< bool > getNvidiaOCAvailable();
  std::optional< QVariantMap > getNvidiaOCState( int deviceIndex = 0 );
  bool setNvidiaClockOffset( int deviceIndex, int clockType, int pstate, int offsetMHz );
  bool setNvidiaGpuLockedClocks( int deviceIndex, int minMHz, int maxMHz );
  bool setNvidiaVramLockedClocks( int deviceIndex, int minMHz, int maxMHz );
  bool resetNvidiaGpuLockedClocks( int deviceIndex );
  bool resetNvidiaVramLockedClocks( int deviceIndex );
  bool resetNvidiaAllClockOffsets( int deviceIndex );
  bool setNvidiaGpuPowerLimit( int deviceIndex, double watts );
  bool resetNvidiaGpuPowerLimit( int deviceIndex );
  bool applyNvidiaGpuOCProfile( const std::string &profileJSON, int deviceIndex = 0 );
  bool resetNvidiaGpuOCAll( int deviceIndex = 0 );

  // NVIDIA Auto-OC
  bool startAutoOC( const std::string &component = "both", int deviceIndex = 0,
                    int stepSizeMHz = 0, int maxOffsetMHz = 0, int stabilityMs = 0 );
  bool resumeAutoOC( const std::string &mode,
                     const std::string &component = "both", int deviceIndex = 0,
                     int stepSizeMHz = 0, int maxOffsetMHz = 0, int stabilityMs = 0 );
  bool pauseAutoOC();
  bool stopAutoOC();
  std::optional< bool > getAutoOCRunning();
  std::optional< QVariantMap > getAutoOCProgress();
  bool hasAutoOCCheckpoint();
  bool clearAutoOCCheckpoint();

  // NVIDIA Auto-Undervolt
  bool startAutoUndervolt( int deviceIndex = 0,
                           bool targetFpsEnabled = false,
                           int  targetFps = 0,
                           bool extendedValidation = false,
                           bool powerLimitMode = false,
                           int  stepSizeMHz = 0,
                           int  maxOffsetMHz = 0,
                           int  stabilityMs = 0 );
  bool resumeAutoUndervolt( const std::string &mode,
                            int deviceIndex = 0,
                            bool targetFpsEnabled = false,
                            int  targetFps = 0,
                            bool extendedValidation = false,
                            bool powerLimitMode = false,
                            int  stepSizeMHz = 0,
                            int  maxOffsetMHz = 0,
                            int  stabilityMs = 0 );

  bool pauseAutoUndervolt() const
  { return callMethod< bool >( "PauseAutoUndervolt" ).value_or( false ); }

  bool stopAutoUndervolt() const
  { return callMethod< bool >( "StopAutoUndervolt" ).value_or( false ); }

  bool getAutoUndervoltRunning() const
  { return callMethod< bool >( "GetAutoUndervoltRunning" ).value_or( false ); }

  std::optional< QVariantMap > getAutoUndervoltProgress();
  std::optional< std::string > getAutoUndervoltProfiles();
  bool hasAutoUndervoltCheckpoint();
  bool clearAutoUndervoltCheckpoint();

  // Device Capability Queries
  std::optional< bool > getWaterCoolerSupported();
  std::optional< bool > getCTGPAdjustmentSupported();

  // Keyboard Control
  bool setKeyboardBacklight( const std::string &config );
  std::optional< QVariantMap > getKeyboardBacklightInfo();
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
  std::optional< int > getIGpuTemperature();
  std::optional< int > getCpuFrequency();
  std::optional< int > getGpuFrequency();
  std::optional< int > getIGpuFrequency();
  std::optional< double > getCpuPower();
  std::optional< double > getGpuPower();
  std::optional< double > getIGpuPower();
  // Extended discrete GPU metrics (NVIDIA, -1 when unavailable)
  std::optional< int > getDGpuComputeUtilPct();       ///< Compute utilization 0–100 %
  std::optional< int > getDGpuMemoryUtilPct();        ///< Memory-controller utilization 0–100 %
  std::optional< int > getDGpuVramUsedMiB();          ///< Used VRAM in MiB
  std::optional< int > getDGpuVramTotalMiB();         ///< Total VRAM in MiB
  std::optional< std::string > getDGpuPerfLimitReason(); ///< Perf-cap/throttle reason
  std::optional< int > getDGpuEncoderUtilPct();       ///< NVENC utilization 0–100 %
  std::optional< int > getDGpuDecoderUtilPct();       ///< NVDEC utilization 0–100 %
  std::optional< int > getDGpuCurrentPstate();        ///< Current P-state index (0–15)
  std::optional< int > getDGpuGrClockOffsetMHz();     ///< Graphics-clock offset at current P-state
  std::optional< int > getDGpuMemClockOffsetMHz();    ///< Memory-clock offset at current P-state
  std::optional< int > getDGpuVramFrequencyMHz();     ///< VRAM frequency in MHz
  std::optional< int > getDGpuCoreVoltageMv();        ///< Core voltage in mV
  std::optional< int > getFanSpeedRPM();
  std::optional< int > getGpuFanSpeedRPM();
  std::optional< int > getFanSpeedPercent();
  std::optional< int > getGpuFanSpeedPercent();
  // Water cooler readings (if available from daemon)
  std::optional< int > getWaterCoolerFanSpeed();
  std::optional< int > getWaterCoolerPumpLevel();

  // Water cooler control
  bool enableWaterCooler( bool enable );
  std::optional< bool > isWaterCoolerEnabled();
  bool setWaterCoolerFanSpeed( int dutyCyclePercent );
  bool setWaterCoolerPumpVoltage( int voltageCode );   // PumpVoltage enum cast to int
  bool setWaterCoolerLEDColor( int r, int g, int b, int mode );  // RGBState enum cast to int
  bool turnOffWaterCoolerLED();

  // Monitoring history
  std::optional< QByteArray > getMonitorDataSince( qint64 sinceTimestampMs );
  bool setMonitorHistoryHorizon( int seconds );
  std::optional< int > getMonitorHistoryHorizon();
  std::optional< QVariantList > getMonitorSources();
  std::optional< QVariantMap > getFpsSources();
  std::optional< QVariantMap > getAutoUvAutoApplyStatus();
  bool setFpsSourceApp( const std::string &appName );
  std::optional< std::string > getFpsSourceApp();

  // Signal Subscription
  using ProfileChangedCallback = std::function< void( const std::string &profileId ) >;
  using PowerStateChangedCallback = std::function< void( const std::string &state ) >;

  void subscribeProfileChanged( ProfileChangedCallback callback );
  void subscribePowerStateChanged( PowerStateChangedCallback callback );

  // Auto-OC signal subscription
  using AutoOCProgressCallback = std::function< void( const QVariantMap &progress ) >;
  using AutoOCFinishedCallback = std::function< void( int coreOffset, int vramOffset, bool success, const std::string &msg ) >;

  void subscribeAutoOCProgress( AutoOCProgressCallback callback );
  void subscribeAutoOCFinished( AutoOCFinishedCallback callback );

  // Auto-Undervolt signal subscription
  using AutoUndervoltProgressCallback = std::function< void( const QVariantMap &progress ) >;
  using AutoUndervoltFinishedCallback = std::function< void( int gpuFreqCapMHz, bool success, const std::string &msg, const std::string &appName ) >;

  void subscribeAutoUndervoltProgress( AutoUndervoltProgressCallback callback );
  void subscribeAutoUndervoltFinished( AutoUndervoltFinishedCallback callback );

  // Connection status
  bool isConnected() const;

signals:
  void profileChanged( const QString &profileId,
                       const QString &keyboardProfileId,
                       const QString &fanProfileId,
                       const QString &gpuProfileId );
  void profilesListChanged();
  void powerStateChanged( const QString &state );
  void connectionStatusChanged( bool connected );
  void autoOCProgressChanged( const QVariantMap &progress );
  void autoOCFinished( int coreOffsetMHz, int vramOffsetMHz,
                       bool success, const QString &message );
  void autoUndervoltProgressChanged( const QVariantMap &progress );
  void autoUndervoltFinished( int gpuFreqCapMHz, bool success,
                              const QString &message, const QString &appName );

private slots:
  void onProfileChangedSignal( const QString &profileId,
                               const QString &keyboardProfileId,
                               const QString &fanProfileId,
                               const QString &gpuProfileId );
  void onProfilesListChangedSignal();
  void onPowerStateChangedSignal( const QString &state );
  void onServiceRegistered( const QString &service );
  void onServiceUnregistered( const QString &service );

private:
  void connectToDaemon();          ///< (Re)create the interface and subscribe to D-Bus signals
  void subscribeDbusSignals();     ///< Connect D-Bus signals (idempotent — disconnects first)

  std::unique_ptr< QDBusInterface > m_interface;
  QDBusServiceWatcher *m_serviceWatcher = nullptr;
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

  // ── JSON snapshot caches (one D-Bus call per poll cycle) ──
  struct DGpuSnapshot
  {
    std::chrono::steady_clock::time_point ts{};
    bool valid = false;
    int temp = -1;
    int coreFrequency = -1;
    int vramFrequency = -1;
    double powerDraw = -1.0;
    int computeUtilPct = -1;
    int memoryUtilPct = -1;
    int vramUsedMiB = -1;
    int vramTotalMiB = -1;
    std::string perfLimitReason;
    int encoderUtilPct = -1;
    int decoderUtilPct = -1;
    int currentPstate = -1;
    int grClockOffsetMHz = -999;
    int memClockOffsetMHz = -999;
    int coreVoltageMv = -1;
    int fanSpeedPct = -1;
  };

  struct IGpuSnapshot
  {
    std::chrono::steady_clock::time_point ts{};
    bool valid = false;
    int temp = -1;
    int coreFrequency = -1;
    double powerDraw = -1.0;
  };

  struct CpuPowerSnapshot
  {
    std::chrono::steady_clock::time_point ts{};
    bool valid = false;
    double powerDraw = -1.0;
  };

  static constexpr int SNAPSHOT_TTL_MS = 100;
  DGpuSnapshot m_dGpuSnap;
  IGpuSnapshot m_iGpuSnap;
  CpuPowerSnapshot m_cpuPowerSnap;
  void refreshDGpuSnapshot();
  void refreshIGpuSnapshot();
  void refreshCpuPowerSnapshot();

  AutoOCProgressCallback m_autoOCProgressCallback;
  AutoOCFinishedCallback m_autoOCFinishedCallback;
  AutoUndervoltProgressCallback m_autoUndervoltProgressCallback;
  AutoUndervoltFinishedCallback m_autoUndervoltFinishedCallback;
};

} // namespace ucc
