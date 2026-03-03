/*
 * Copyright (C) 2026 Uniwill Control Center Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QVariantList>
#include <QVariantMap>
#include <QFileSystemWatcher>
#include <QtQmlIntegration/qqmlintegration.h>
#include <memory>

#include "UccdClient.hpp"

/**
 * @brief QML-facing backend that wraps UccdClient for the tray popup.
 *
 * Exposes profiles, system monitoring, hardware toggles, water cooler
 * controls and keyboard backlight state via Q_PROPERTY / Q_INVOKABLE
 * so that the QML UI can bind to them declaratively.
 */
class TrayBackend : public QObject
{
  Q_OBJECT
  QML_ELEMENT

  // ── Connection ──
  Q_PROPERTY( bool connected READ connected NOTIFY connectedChanged )

  // ── Device support ──
  Q_PROPERTY( bool deviceSupported READ deviceSupported NOTIFY deviceSupportedChanged )

  // ── System Info (static, loaded once) ──
  Q_PROPERTY( QString laptopModel  READ laptopModel  NOTIFY systemInfoChanged )
  Q_PROPERTY( QString cpuModel     READ cpuModel     NOTIFY systemInfoChanged )
  Q_PROPERTY( QString dGpuModel    READ dGpuModel    NOTIFY systemInfoChanged )
  Q_PROPERTY( QString iGpuModel    READ iGpuModel    NOTIFY systemInfoChanged )

  // ── Dashboard / Monitoring ──
  Q_PROPERTY( int cpuTemp        READ cpuTemp        NOTIFY metricsUpdated )
  Q_PROPERTY( int gpuTemp        READ gpuTemp        NOTIFY metricsUpdated )
  Q_PROPERTY( int cpuFreqMHz     READ cpuFreqMHz     NOTIFY metricsUpdated )
  Q_PROPERTY( int gpuFreqMHz     READ gpuFreqMHz     NOTIFY metricsUpdated )
  Q_PROPERTY( double cpuPowerW   READ cpuPowerW      NOTIFY metricsUpdated )
  Q_PROPERTY( double gpuPowerW   READ gpuPowerW      NOTIFY metricsUpdated )
  Q_PROPERTY( int cpuFanRPM      READ cpuFanRPM      NOTIFY metricsUpdated )
  Q_PROPERTY( int gpuFanRPM      READ gpuFanRPM      NOTIFY metricsUpdated )
  Q_PROPERTY( int cpuFanPercent  READ cpuFanPercent   NOTIFY metricsUpdated )
  Q_PROPERTY( int gpuFanPercent  READ gpuFanPercent   NOTIFY metricsUpdated )
  Q_PROPERTY( int wcFanSpeed     READ wcFanSpeed      NOTIFY metricsUpdated )
  Q_PROPERTY( int wcPumpLevel    READ wcPumpLevel     NOTIFY metricsUpdated )
  // Extended NVIDIA dGPU metrics
  Q_PROPERTY( int  gpuComputeUtilPct   READ gpuComputeUtilPct   NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuMemoryUtilPct    READ gpuMemoryUtilPct    NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuVramUsedMiB      READ gpuVramUsedMiB      NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuVramTotalMiB     READ gpuVramTotalMiB     NOTIFY metricsUpdated )
  Q_PROPERTY( QString gpuPerfLimitReason READ gpuPerfLimitReason NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuEncoderUtilPct   READ gpuEncoderUtilPct   NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuDecoderUtilPct   READ gpuDecoderUtilPct   NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuCurrentPstate    READ gpuCurrentPstate    NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuGrClockOffsetMHz READ gpuGrClockOffsetMHz NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuMemClockOffsetMHz READ gpuMemClockOffsetMHz NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuVramFreqMHz READ gpuVramFreqMHz NOTIFY metricsUpdated )
  Q_PROPERTY( int  gpuCoreVoltageMv READ gpuCoreVoltageMv NOTIFY metricsUpdated )

  // ── Profiles ──
  Q_PROPERTY( QString      activeProfileId READ activeProfileId NOTIFY activeProfileChanged )
  Q_PROPERTY( QString      activeProfileName READ activeProfileName NOTIFY activeProfileChanged )
  Q_PROPERTY( QString      powerState      READ powerState      NOTIFY powerStateChanged )

  // ── Hardware toggles ──
  Q_PROPERTY( bool webcamEnabled  READ webcamEnabled  WRITE setWebcamEnabled  NOTIFY webcamEnabledChanged )
  Q_PROPERTY( bool fnLock         READ fnLock         WRITE setFnLock         NOTIFY fnLockChanged )
  Q_PROPERTY( int  displayBrightness READ displayBrightness WRITE setDisplayBrightness NOTIFY displayBrightnessChanged )

  // ── Water cooler ──
  Q_PROPERTY( bool waterCoolerSupported READ waterCoolerSupported NOTIFY waterCoolerSupportedChanged )
  Q_PROPERTY( bool wcConnected          READ wcConnected          NOTIFY wcConnectedChanged )
  Q_PROPERTY( bool wcAutoControl        READ wcAutoControl        NOTIFY wcAutoControlChanged )
  Q_PROPERTY( bool wcEnabled            READ wcEnabled            NOTIFY wcEnabledChanged )

  // Water cooler control state (local cache)
  Q_PROPERTY( int  wcFanPercent      READ wcFanPercent      NOTIFY wcControlStateChanged )
  Q_PROPERTY( int  wcPumpVoltageCode READ wcPumpVoltageCode NOTIFY wcControlStateChanged )
  Q_PROPERTY( bool wcLedEnabled      READ wcLedEnabled      NOTIFY wcControlStateChanged )
  Q_PROPERTY( int  wcLedMode         READ wcLedMode         NOTIFY wcControlStateChanged )
  Q_PROPERTY( int  wcLedRed          READ wcLedRed          NOTIFY wcControlStateChanged )
  Q_PROPERTY( int  wcLedGreen        READ wcLedGreen        NOTIFY wcControlStateChanged )
  Q_PROPERTY( int  wcLedBlue         READ wcLedBlue         NOTIFY wcControlStateChanged )

  // ── ODM Performance Profile ──
  Q_PROPERTY( QStringList availableODMProfiles READ availableODMProfiles NOTIFY odmProfilesAvailableChanged )
  Q_PROPERTY( QString     odmPerformanceProfile READ odmPerformanceProfile NOTIFY odmPerformanceProfileChanged )

  // ── Profile lists as parallel name/id QStringLists (reliable ComboBox model) ──
  Q_PROPERTY( QStringList profileNames        READ profileNames        NOTIFY profilesChanged )
  Q_PROPERTY( QStringList profileIds          READ profileIds          NOTIFY profilesChanged )
  Q_PROPERTY( QStringList fanProfileNames     READ fanProfileNames     NOTIFY fanProfilesChanged )
  Q_PROPERTY( QStringList fanProfileIds       READ fanProfileIds       NOTIFY fanProfilesChanged )
  Q_PROPERTY( QStringList keyboardProfileNames READ keyboardProfileNames NOTIFY keyboardProfilesChanged )
  Q_PROPERTY( QStringList keyboardProfileIds   READ keyboardProfileIds   NOTIFY keyboardProfilesChanged )
  Q_PROPERTY( QStringList gpuProfileNames READ gpuProfileNames NOTIFY gpuProfilesChanged )
  Q_PROPERTY( QStringList gpuProfileIds   READ gpuProfileIds   NOTIFY gpuProfilesChanged )

  // ── Active profile sub-profile info ──
  Q_PROPERTY( QString activeProfileFanName      READ activeProfileFanName      NOTIFY activeProfileChanged )
  Q_PROPERTY( QString activeProfileFanId        READ activeProfileFanId        NOTIFY activeProfileChanged )
  Q_PROPERTY( QString activeProfileKeyboardName READ activeProfileKeyboardName NOTIFY activeProfileChanged )
  Q_PROPERTY( QString activeProfileKeyboardId   READ activeProfileKeyboardId   NOTIFY activeProfileChanged )
  Q_PROPERTY( QString activeProfileGpuName      READ activeProfileGpuName      NOTIFY activeProfileChanged )
  Q_PROPERTY( QString activeProfileGpuId        READ activeProfileGpuId        NOTIFY activeProfileChanged )

public:
  explicit TrayBackend( QObject *parent = nullptr );
  ~TrayBackend() override = default;

  // ── Connection ──
  bool connected() const;

  // ── Device support ──
  bool deviceSupported() const;

  // ── System Info ──
  QString laptopModel() const;
  QString cpuModel() const;
  QString dGpuModel() const;
  QString iGpuModel() const;

  // ── Monitoring ──
  int cpuTemp() const;
  int gpuTemp() const;
  int cpuFreqMHz() const;
  int gpuFreqMHz() const;
  double cpuPowerW() const;
  double gpuPowerW() const;
  int cpuFanRPM() const;
  int gpuFanRPM() const;
  int cpuFanPercent() const;
  int gpuFanPercent() const;
  int wcFanSpeed() const;
  int wcPumpLevel() const;
  // Extended NVIDIA dGPU metrics (-1 when unavailable)
  int gpuComputeUtilPct() const;
  int gpuMemoryUtilPct() const;
  int gpuVramUsedMiB() const;
  int gpuVramTotalMiB() const;
  QString gpuPerfLimitReason() const;
  int gpuEncoderUtilPct() const;
  int gpuDecoderUtilPct() const;
  int gpuCurrentPstate() const;
  int gpuGrClockOffsetMHz() const;
  int gpuMemClockOffsetMHz() const;
  int gpuVramFreqMHz() const;
  int gpuCoreVoltageMv() const;

  // ── Profiles ──
  QStringList profileNames() const;
  QStringList profileIds() const;
  QString activeProfileId() const;
  QString activeProfileName() const;
  QString powerState() const;

  // ── Hardware ──
  bool webcamEnabled() const;
  Q_INVOKABLE void setWebcamEnabled( bool v );
  bool fnLock() const;
  Q_INVOKABLE void setFnLock( bool v );
  int displayBrightness() const;
  Q_INVOKABLE void setDisplayBrightness( int v );

  // ── Water cooler ──
  bool waterCoolerSupported() const;
  bool wcConnected() const;
  bool wcAutoControl() const;
  bool wcEnabled() const;
  Q_INVOKABLE void setWcEnabled( bool enabled );
  int  wcFanPercent() const;
  int  wcPumpVoltageCode() const;
  bool wcLedEnabled() const;
  int  wcLedMode() const;
  int  wcLedRed() const;
  int  wcLedGreen() const;
  int  wcLedBlue() const;

  // ── ODM ──
  QStringList availableODMProfiles() const;
  QString odmPerformanceProfile() const;

  // ── Fan profiles ──
  QStringList fanProfileNames() const;
  QStringList fanProfileIds() const;

  // ── Keyboard profiles ──
  QStringList keyboardProfileNames() const;
  QStringList keyboardProfileIds() const;

  // ── GPU OC profiles ──
  QStringList gpuProfileNames() const;
  QStringList gpuProfileIds() const;

  // ── Active profile sub-profile info ──
  QString activeProfileFanName() const;
  QString activeProfileFanId() const;
  QString activeProfileKeyboardName() const;
  QString activeProfileKeyboardId() const;
  QString activeProfileGpuName() const;
  QString activeProfileGpuId() const;

  // ── Invokables for QML ──
  Q_INVOKABLE void setActiveProfile( const QString &profileId );
  Q_INVOKABLE void setActiveFanProfile( const QString &fanProfileId );
  Q_INVOKABLE void setActiveKeyboardProfile( const QString &keyboardProfileId );
  Q_INVOKABLE void setActiveGpuProfile( const QString &gpuProfileId );
  Q_INVOKABLE void setODMPerformanceProfile( const QString &profile );
  Q_INVOKABLE void openControlCenter();
  Q_INVOKABLE void refreshAll();
  // Water cooler control
  Q_INVOKABLE void setWcFanSpeed( int percent );
  Q_INVOKABLE void setWcPumpVoltageCode( int voltageCode );
  Q_INVOKABLE void setWcLedEnabled( bool enabled );
  Q_INVOKABLE void setWcLed( int r, int g, int b, int mode );

signals:
  void connectedChanged();
  void deviceSupportedChanged();
  void systemInfoChanged();
  void metricsUpdated();
  void profilesChanged();
  void activeProfileChanged();
  void powerStateChanged();
  void webcamEnabledChanged();
  void fnLockChanged();
  void displayBrightnessChanged();
  void waterCoolerSupportedChanged();
  void wcConnectedChanged();
  void wcAutoControlChanged();
  void wcEnabledChanged();
  void wcControlStateChanged();
  void odmProfilesAvailableChanged();
  void odmPerformanceProfileChanged();
  void fanProfilesChanged();
  void keyboardProfilesChanged();
  void gpuProfilesChanged();

private slots:
  void pollMetrics();
  void pollSlowState();
  void onDaemonProfileChanged( const QString &profileId,
                               const QString &keyboardProfileId,
                               const QString &fanProfileId,
                               const QString &gpuProfileId );
  void onSettingsFileChanged( const QString &path );
  void onConnectionStatusChanged( bool connected );

private:
  void loadProfiles();
  void loadCapabilities();
  void loadLocalProfiles();  // custom fan + keyboard profiles from QSettings
  QString resolveFanProfileName( const QString &fanProfileId ) const;
  QString resolveKeyboardProfileName( const QString &kbProfileId ) const;
  QString resolveGpuProfileName( const QString &gpuProfileId ) const;

  std::unique_ptr< ucc::UccdClient > m_client;
  QTimer *m_fastTimer = nullptr;   // ~1 s  — temps, fans
  QTimer *m_slowTimer = nullptr;   // ~5 s  — profiles, hw toggles
  QFileSystemWatcher *m_settingsWatcher = nullptr;

  // Cached monitoring values
  int m_cpuTemp = 0;
  int m_gpuTemp = 0;
  int m_cpuFreqMHz = 0;
  int m_gpuFreqMHz = 0;
  double m_cpuPowerW = 0.0;
  double m_gpuPowerW = 0.0;
  int m_cpuFanRPM = 0;
  int m_gpuFanRPM = 0;
  int m_cpuFanPercent = 0;
  int m_gpuFanPercent = 0;
  int m_wcFanSpeed = 0;
  int m_wcPumpLevel = -1;
  // Extended NVIDIA dGPU metrics
  int m_gpuComputeUtilPct  = -1;
  int m_gpuMemoryUtilPct   = -1;
  int m_gpuVramUsedMiB     = -1;
  int m_gpuVramTotalMiB    = -1;
  QString m_gpuPerfLimitReason;
  int m_gpuEncoderUtilPct  = -1;
  int m_gpuDecoderUtilPct  = -1;
  int m_gpuCurrentPstate   = -1;
  int m_gpuGrClockOffsetMHz  = -999;
  int m_gpuMemClockOffsetMHz = -999;
  int m_gpuVramFreqMHz = -1;
  int m_gpuCoreVoltageMv = -1;

  // Profiles (parallel lists: names[i] ↔ ids[i])
  QStringList m_profileNames;
  QStringList m_profileIds;
  QString m_activeProfileId;
  QString m_activeProfileName;
  QString m_powerState;

  // Hardware toggles
  bool m_webcamEnabled = true;
  bool m_fnLock = false;
  int m_displayBrightness = 50;

  // Water cooler control state cache
  int  m_wcFanPercent = 50;
  int  m_wcPumpVoltageCode = 4;  // PumpVoltage::Off
  bool m_wcLedEnabled = true;
  int  m_wcLedMode = 0;          // RGBState::Static
  int  m_wcLedRed = 255;
  int  m_wcLedGreen = 0;
  int  m_wcLedBlue = 0;

  // System info
  QString m_laptopModel;
  QString m_cpuModel;
  QString m_dGpuModel;
  QString m_iGpuModel;

  // Device capabilities
  bool m_deviceSupported = true;
  bool m_waterCoolerSupported = false;
  bool m_wcAutoControl = true;
  bool m_wcEnabled = true;  // true = daemon controls fan/pump automatically
  bool m_wcEnabledOverride = false;  // user explicitly toggled — don't overwrite from poll

  // ODM profiles
  QStringList m_availableODMProfiles;
  QString m_odmPerformanceProfile;

  // Fan profiles (parallel lists)
  QStringList m_fanProfileNames;
  QStringList m_fanProfileIds;

  // Active profile sub-profile info
  QString m_activeProfileFanId;
  QString m_activeProfileFanName;
  QString m_activeProfileKeyboardId;
  QString m_activeProfileKeyboardName;
  bool m_fanProfileOverride = false;
  bool m_keyboardProfileOverride = false;

  // Custom fan & keyboard profiles (parallel lists, from local settings)
  QStringList m_customFanProfileNames;
  QStringList m_customFanProfileIds;
  QStringList m_keyboardProfileNames;
  QStringList m_keyboardProfileIds;
  QStringList m_gpuProfileNames;
  QStringList m_gpuProfileIds;
  QJsonArray  m_keyboardProfilesData;

  QString m_activeProfileGpuId;
  QString m_activeProfileGpuName;
  bool m_gpuProfileOverride = false;
};
