/*
 * Copyright (C) 2026 Uniwill Control Center Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "TrayBackend.hpp"

#include "GuiLauncher.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QDebug>

#include <algorithm>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TrayBackend::TrayBackend( QObject *parent )
  : QObject( parent )
  , m_client( std::make_unique< ucc::UccdClient >() )
{
  // Fast timer: temperatures, fan speeds (every 1.5 s)
  m_fastTimer = new QTimer( this );
  m_fastTimer->setInterval( 1500 );
  connect( m_fastTimer, &QTimer::timeout, this, &TrayBackend::pollMetrics );

  // Slow timer: profiles, hw toggles (every 5 s)
  m_slowTimer = new QTimer( this );
  m_slowTimer->setInterval( 5000 );
  connect( m_slowTimer, &QTimer::timeout, this, &TrayBackend::pollSlowState );

  // Daemon signals
  connect( m_client.get(), &ucc::UccdClient::profileChanged,
           this, &TrayBackend::onDaemonProfileChanged );
  connect( m_client.get(), &ucc::UccdClient::connectionStatusChanged,
           this, &TrayBackend::onConnectionStatusChanged );

  // Watch the shared settings file so we pick up changes from the GUI immediately
  m_settingsWatcher = new QFileSystemWatcher( this );
  QString uccrcPath = QDir::homePath() + "/.config/uccrc";
  if ( QFile::exists( uccrcPath ) )
    m_settingsWatcher->addPath( uccrcPath );
  connect( m_settingsWatcher, &QFileSystemWatcher::fileChanged,
           this, &TrayBackend::onSettingsFileChanged );

  // Initial data load
  loadCapabilities();

  if ( !m_deviceSupported )
  {
    fprintf( stderr, "[TrayBackend] Device not supported — tray disabled\n" );
    return;
  }

  loadProfiles();
  loadLocalProfiles();
  pollMetrics();
  pollSlowState();

  m_fastTimer->start();
  m_slowTimer->start();
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

bool TrayBackend::connected() const
{
  return m_client && m_client->isConnected();
}

bool TrayBackend::deviceSupported() const
{
  return m_deviceSupported;
}

// ---------------------------------------------------------------------------
// Monitoring getters
// ---------------------------------------------------------------------------
// System info getters
// ---------------------------------------------------------------------------

QString TrayBackend::laptopModel() const { return m_laptopModel; }
QString TrayBackend::cpuModel()    const { return m_cpuModel; }
QString TrayBackend::dGpuModel()   const { return m_dGpuModel; }
QString TrayBackend::iGpuModel()   const { return m_iGpuModel; }

// ---------------------------------------------------------------------------
// Monitoring getters
// ---------------------------------------------------------------------------

int    TrayBackend::cpuTemp()      const { return m_cpuTemp; }
int    TrayBackend::gpuTemp()      const { return m_gpuTemp; }
int    TrayBackend::cpuFreqMHz()   const { return m_cpuFreqMHz; }
int    TrayBackend::gpuFreqMHz()   const { return m_gpuFreqMHz; }
double TrayBackend::cpuPowerW()    const { return m_cpuPowerW; }
double TrayBackend::gpuPowerW()    const { return m_gpuPowerW; }
int    TrayBackend::cpuFanRPM()    const { return m_cpuFanRPM; }
int    TrayBackend::gpuFanRPM()    const { return m_gpuFanRPM; }
int    TrayBackend::cpuFanPercent() const { return m_cpuFanPercent; }
int    TrayBackend::gpuFanPercent() const { return m_gpuFanPercent; }
int    TrayBackend::wcFanSpeed()   const { return m_wcFanSpeed; }
int    TrayBackend::wcPumpLevel()  const { return m_wcPumpLevel; }
int    TrayBackend::gpuComputeUtilPct()   const { return m_gpuComputeUtilPct; }
int    TrayBackend::gpuMemoryUtilPct()    const { return m_gpuMemoryUtilPct; }
int    TrayBackend::gpuVramUsedMiB()      const { return m_gpuVramUsedMiB; }
int    TrayBackend::gpuVramTotalMiB()     const { return m_gpuVramTotalMiB; }
QString TrayBackend::gpuPerfLimitReason() const { return m_gpuPerfLimitReason; }
int    TrayBackend::gpuEncoderUtilPct()   const { return m_gpuEncoderUtilPct; }
int    TrayBackend::gpuDecoderUtilPct()   const { return m_gpuDecoderUtilPct; }
int    TrayBackend::gpuCurrentPstate()    const { return m_gpuCurrentPstate; }
int    TrayBackend::gpuGrClockOffsetMHz() const { return m_gpuGrClockOffsetMHz; }
int    TrayBackend::gpuMemClockOffsetMHz() const { return m_gpuMemClockOffsetMHz; }
int    TrayBackend::gpuVramFreqMHz() const { return m_gpuVramFreqMHz; }
int    TrayBackend::gpuCoreVoltageMv() const { return m_gpuCoreVoltageMv; }

// ---------------------------------------------------------------------------
// Profile getters
// ---------------------------------------------------------------------------

QStringList TrayBackend::profileNames() const { return m_profileNames; }
QStringList TrayBackend::profileIds()   const { return m_profileIds; }
QString TrayBackend::activeProfileId() const { return m_activeProfileId; }
QString TrayBackend::activeProfileName() const { return m_activeProfileName; }
QString TrayBackend::powerState() const { return m_powerState; }

// ---------------------------------------------------------------------------
// Hardware toggles
// ---------------------------------------------------------------------------

bool TrayBackend::webcamEnabled() const { return m_webcamEnabled; }

void TrayBackend::setWebcamEnabled( bool v )
{
  if ( m_client->setWebcamEnabled( v ) )
  {
    m_webcamEnabled = v;
    emit webcamEnabledChanged();
  }
}

bool TrayBackend::fnLock() const { return m_fnLock; }

void TrayBackend::setFnLock( bool v )
{
  if ( m_client->setFnLock( v ) )
  {
    m_fnLock = v;
    emit fnLockChanged();
  }
}

int TrayBackend::displayBrightness() const { return m_displayBrightness; }

void TrayBackend::setDisplayBrightness( int v )
{
  if ( m_client->setDisplayBrightness( v ) )
  {
    m_displayBrightness = v;
    emit displayBrightnessChanged();
  }
}

// ---------------------------------------------------------------------------
// Water cooler
// ---------------------------------------------------------------------------

bool TrayBackend::waterCoolerSupported() const { return m_waterCoolerSupported; }
bool TrayBackend::wcConnected()           const { return m_powerState == QStringLiteral( "AC w/ Water Cooler" ); }
bool TrayBackend::wcAutoControl()         const { return m_wcAutoControl; }
bool TrayBackend::wcEnabled()             const { return m_wcEnabled; }
int  TrayBackend::wcFanPercent()      const { return m_wcFanPercent; }
int  TrayBackend::wcPumpVoltageCode() const { return m_wcPumpVoltageCode; }
bool TrayBackend::wcLedEnabled()      const { return m_wcLedEnabled; }
int  TrayBackend::wcLedMode()         const { return m_wcLedMode; }
int  TrayBackend::wcLedRed()          const { return m_wcLedRed; }
int  TrayBackend::wcLedGreen()        const { return m_wcLedGreen; }
int  TrayBackend::wcLedBlue()         const { return m_wcLedBlue; }

void TrayBackend::setWcFanSpeed( int percent )
{
  if ( m_client->setWaterCoolerFanSpeed( percent ) )
  {
    m_wcFanPercent = percent;
    emit wcControlStateChanged();
  }
}

void TrayBackend::setWcEnabled( bool enabled )
{
  m_client->enableWaterCooler( enabled );
  m_wcEnabled = enabled;
  m_wcEnabledOverride = true;
  emit wcEnabledChanged();
}

void TrayBackend::setWcPumpVoltageCode( int voltageCode )
{
  if ( m_client->setWaterCoolerPumpVoltage( voltageCode ) )
  {
    m_wcPumpVoltageCode = voltageCode;
    emit wcControlStateChanged();
  }
}

void TrayBackend::setWcLedEnabled( bool enabled )
{
  if ( !enabled )
  {
    if ( m_client->turnOffWaterCoolerLED() )
    {
      m_wcLedEnabled = false;
      emit wcControlStateChanged();
    }
  }
  else
  {
    // Re-send the current colour/mode to turn the LED back on
    if ( m_client->setWaterCoolerLEDColor( m_wcLedRed, m_wcLedGreen, m_wcLedBlue, m_wcLedMode ) )
    {
      m_wcLedEnabled = true;
      emit wcControlStateChanged();
    }
  }
}

void TrayBackend::setWcLed( int r, int g, int b, int mode )
{
  if ( m_client->setWaterCoolerLEDColor( r, g, b, mode ) )
  {
    m_wcLedMode  = mode;
    m_wcLedRed   = r;
    m_wcLedGreen = g;
    m_wcLedBlue  = b;
    emit wcControlStateChanged();
  }
}

// ---------------------------------------------------------------------------
// ODM profiles
// ---------------------------------------------------------------------------

QStringList TrayBackend::availableODMProfiles() const { return m_availableODMProfiles; }
QString TrayBackend::odmPerformanceProfile() const { return m_odmPerformanceProfile; }

// ---------------------------------------------------------------------------
// Fan profiles
// ---------------------------------------------------------------------------

QStringList TrayBackend::fanProfileNames() const { return m_fanProfileNames; }
QStringList TrayBackend::fanProfileIds()   const { return m_fanProfileIds; }

// ---------------------------------------------------------------------------
// Active profile sub-profile info
// ---------------------------------------------------------------------------

QString TrayBackend::activeProfileFanName() const { return m_activeProfileFanName; }
QString TrayBackend::activeProfileFanId() const { return m_activeProfileFanId; }
QString TrayBackend::activeProfileKeyboardName() const { return m_activeProfileKeyboardName; }
QString TrayBackend::activeProfileKeyboardId() const { return m_activeProfileKeyboardId; }
QString TrayBackend::activeProfileGpuName() const { return m_activeProfileGpuName; }
QString TrayBackend::activeProfileGpuId() const { return m_activeProfileGpuId; }

// ---------------------------------------------------------------------------
// Keyboard profiles (from local settings)
// ---------------------------------------------------------------------------

QStringList TrayBackend::keyboardProfileNames() const { return m_keyboardProfileNames; }
QStringList TrayBackend::keyboardProfileIds()   const { return m_keyboardProfileIds; }
QStringList TrayBackend::gpuProfileNames() const { return m_gpuProfileNames; }
QStringList TrayBackend::gpuProfileIds()   const { return m_gpuProfileIds; }

// ---------------------------------------------------------------------------
// Invokable actions
// ---------------------------------------------------------------------------

void TrayBackend::setActiveProfile( const QString &profileId )
{
  if ( m_client->setActiveProfile( profileId.toStdString() ) )
  {
    m_activeProfileId = profileId;
    // Resolve the name from our cached list
    int idx = m_profileIds.indexOf( profileId );
    m_activeProfileName = ( idx >= 0 ) ? m_profileNames[ idx ] : profileId;
    emit activeProfileChanged();

    // Refresh power limits – they may change with the profile
    pollSlowState();
  }
}

void TrayBackend::setActiveFanProfile( const QString &fanProfileId )
{
  // Fetch the fan profile JSON and apply its curves to hardware.
  // GetFanProfile returns keys "tableCPU"/"tableGPU"/"tablePump"/"tableWaterCoolerFan"
  // but ApplyFanProfiles expects "cpu"/"gpu"/"pump"/"waterCoolerFan".
  if ( auto json = m_client->getFanProfile( fanProfileId.toStdString() ) )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isObject() )
    {
      QJsonObject src = doc.object();
      QJsonObject dst;
      if ( src.contains( "tableCPU" ) )         dst[ "cpu" ]            = src[ "tableCPU" ];
      if ( src.contains( "tableGPU" ) )         dst[ "gpu" ]            = src[ "tableGPU" ];
      if ( src.contains( "tablePump" ) )        dst[ "pump" ]           = src[ "tablePump" ];
      if ( src.contains( "tableWaterCoolerFan" ) ) dst[ "waterCoolerFan" ] = src[ "tableWaterCoolerFan" ];
      dst[ "fanProfileId" ] = fanProfileId;
      const std::string applyJson =
        QJsonDocument( dst ).toJson( QJsonDocument::Compact ).toStdString();
      m_client->applyFanProfiles( applyJson );
    }
  }
  // Mark override so pollSlowState() doesn't revert to the daemon's stored value
  m_fanProfileOverride = true;
  m_activeProfileFanId = fanProfileId;
  m_activeProfileFanName = resolveFanProfileName( fanProfileId );
  emit activeProfileChanged();
}

void TrayBackend::setActiveKeyboardProfile( const QString &keyboardProfileId )
{
  auto it = std::ranges::find_if( m_keyboardProfilesData, [&]( const QJsonValue &val ) {
    return val.toObject()[ "id" ].toString() == keyboardProfileId;
  } );
  if ( it != m_keyboardProfilesData.end() )
  {
    auto profileData = it->toObject()[ "json" ].toString();
    if ( !profileData.isEmpty() )
    {
      QJsonDocument doc = QJsonDocument::fromJson( profileData.toUtf8() );
      if ( doc.isObject() )
      {
        QJsonObject obj = doc.object();
        // Inject the keyboard profile ID so uccd can notify subscribers
        obj[ "keyboardProfileId" ] = keyboardProfileId;
        m_client->setKeyboardBacklight(
          QJsonDocument( obj ).toJson( QJsonDocument::Compact ).toStdString() );
      }
    }
  }
  m_keyboardProfileOverride = true;
  m_activeProfileKeyboardId = keyboardProfileId;
  m_activeProfileKeyboardName = resolveKeyboardProfileName( keyboardProfileId );
  emit activeProfileChanged();
}

void TrayBackend::setActiveGpuProfile( const QString &gpuProfileId )
{
  auto applyWithProfileId = [this, &gpuProfileId]( const QString &jsonText ) {
    QJsonDocument doc = QJsonDocument::fromJson( jsonText.toUtf8() );
    if ( !doc.isObject() )
      return;

    QJsonObject obj = doc.object();

    if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj["nvidiaPowerCTRLProfile"].isObject() )
    {
      QJsonObject nvidiaObj = obj["nvidiaPowerCTRLProfile"].toObject();
      int ctgpOffset = nvidiaObj["cTGPOffset"].toInt( 0 );
      if ( m_client->getCTGPAdjustmentSupported().value_or( false ) )
        (void)m_client->setNVIDIAPowerOffset( ctgpOffset );
    }

    obj[ "gpuProfileId" ] = gpuProfileId;
    m_client->applyNvidiaGpuOCProfile(
      QJsonDocument( obj ).toJson( QJsonDocument::Compact ).toStdString(), 0 );
  };

  if ( auto json = m_client->getGpuProfile( gpuProfileId.toStdString() ) )
  {
    applyWithProfileId( QString::fromStdString( *json ) );
  }
  else
  {
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    QByteArray gpuRaw = settings.value( "customGpuProfiles", "[]" ).toByteArray();
    QJsonDocument doc = QJsonDocument::fromJson( gpuRaw );
    if ( doc.isArray() )
    {
      for ( const auto &val : doc.array() )
      {
        const QJsonObject obj = val.toObject();
        if ( obj.value( "id" ).toString() == gpuProfileId )
        {
          const QString profileJson = obj.value( "json" ).toString();
          if ( !profileJson.isEmpty() )
            applyWithProfileId( profileJson );
          break;
        }
      }
    }
  }

  m_gpuProfileOverride = true;
  m_activeProfileGpuId = gpuProfileId;
  m_activeProfileGpuName = resolveGpuProfileName( gpuProfileId );
  emit activeProfileChanged();
}

void TrayBackend::setODMPerformanceProfile( const QString &profile )
{
  if ( m_client->setODMPerformanceProfile( profile.toStdString() ) )
  {
    m_odmPerformanceProfile = profile;
    emit odmPerformanceProfileChanged();
  }
}

void TrayBackend::openControlCenter()
{
  ucc::launchGui();
}

void TrayBackend::refreshAll()
{
  loadCapabilities();
  loadProfiles();
  loadLocalProfiles();
  pollMetrics();
  pollSlowState();
}

// ---------------------------------------------------------------------------
// Polling slots
// ---------------------------------------------------------------------------

void TrayBackend::pollMetrics()
{
  bool changed = false;

  auto update = [&]<typename F, typename O>( F &field, O optVal ) {
    if ( optVal )
    {
      auto val = static_cast< std::decay_t< F > >( *optVal );
      if ( field != val )
      {
        field = val;
        changed = true;
      }
    }
  };

  update( m_cpuTemp,       m_client->getCpuTemperature() );
  update( m_gpuTemp,       m_client->getGpuTemperature() );
  update( m_cpuFreqMHz,    m_client->getCpuFrequency() );
  update( m_gpuFreqMHz,    m_client->getGpuFrequency() );
  update( m_cpuPowerW,     m_client->getCpuPower() );
  update( m_gpuPowerW,     m_client->getGpuPower() );
  update( m_cpuFanRPM,     m_client->getFanSpeedRPM() );
  update( m_gpuFanRPM,     m_client->getGpuFanSpeedRPM() );
  update( m_cpuFanPercent, m_client->getFanSpeedPercent() );
  update( m_gpuFanPercent, m_client->getGpuFanSpeedPercent() );

  // Extended NVIDIA dGPU metrics
  update( m_gpuComputeUtilPct,   m_client->getDGpuComputeUtilPct() );
  update( m_gpuMemoryUtilPct,    m_client->getDGpuMemoryUtilPct() );
  update( m_gpuVramUsedMiB,      m_client->getDGpuVramUsedMiB() );
  update( m_gpuVramTotalMiB,     m_client->getDGpuVramTotalMiB() );

  if ( auto v = m_client->getDGpuPerfLimitReason() )
  {
    QString reason = QString::fromStdString( *v );
    if ( m_gpuPerfLimitReason != reason )
    {
      m_gpuPerfLimitReason = reason;
      changed = true;
    }
  }
  else if ( !m_gpuPerfLimitReason.isEmpty() )
  {
    m_gpuPerfLimitReason.clear();
    changed = true;
  }

  update( m_gpuEncoderUtilPct,   m_client->getDGpuEncoderUtilPct() );
  update( m_gpuDecoderUtilPct,   m_client->getDGpuDecoderUtilPct() );
  update( m_gpuCurrentPstate,    m_client->getDGpuCurrentPstate() );
  update( m_gpuGrClockOffsetMHz,  m_client->getDGpuGrClockOffsetMHz() );
  update( m_gpuMemClockOffsetMHz, m_client->getDGpuMemClockOffsetMHz() );
  update( m_gpuVramFreqMHz,      m_client->getDGpuVramFrequencyMHz() );
  update( m_gpuCoreVoltageMv,    m_client->getDGpuCoreVoltageMv() );

  if ( m_waterCoolerSupported )
  {
    update( m_wcFanSpeed,  m_client->getWaterCoolerFanSpeed() );
    update( m_wcPumpLevel, m_client->getWaterCoolerPumpLevel() );
  }

  if ( changed )
    emit metricsUpdated();
}

void TrayBackend::pollSlowState()
{
  // Active profile
  if ( auto json = m_client->getActiveProfileJSON() )
  {
    auto doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isObject() )
    {
      auto obj = doc.object();
      auto newId = obj[ "id" ].toString();
      bool profileSwitched = ( newId != m_activeProfileId );
      m_activeProfileId = newId;
      m_activeProfileName = obj[ "name" ].toString();
      bool changed = profileSwitched;

      // Reset overrides when the system profile itself changes
      if ( profileSwitched )
      {
        m_fanProfileOverride = false;
        m_keyboardProfileOverride = false;
        m_gpuProfileOverride = false;
        m_wcEnabledOverride = false;
      }

      // Extract fan profile reference — skip if user manually overrode it
      auto fanObj = obj[ "fan" ].toObject();
      auto fanId = fanObj[ "fanProfile" ].toString();

      // Extract water cooler auto-control flag
      bool autoWC = fanObj[ "autoControlWC" ].toBool( true );
      if ( autoWC != m_wcAutoControl )
      {
        m_wcAutoControl = autoWC;
        emit wcAutoControlChanged();
      }

      // Query daemon directly for the runtime water-cooler enable state
      if ( !m_wcEnabledOverride )
      {
        bool wcEn = m_client->isWaterCoolerEnabled().value_or( m_wcEnabled );
        if ( wcEn != m_wcEnabled )
        {
          m_wcEnabled = wcEn;
          emit wcEnabledChanged();
        }
      }
      if ( !m_fanProfileOverride && fanId != m_activeProfileFanId )
      {
        m_activeProfileFanId = fanId;
        m_activeProfileFanName = resolveFanProfileName( fanId );
        changed = true;
      }

      // Extract keyboard profile reference — skip if user manually overrode it
      auto kbId = obj[ "selectedKeyboardProfile" ].toString();
      if ( !m_keyboardProfileOverride && kbId != m_activeProfileKeyboardId )
      {
        m_activeProfileKeyboardId = kbId;
        m_activeProfileKeyboardName = resolveKeyboardProfileName( kbId );
        changed = true;
      }

      const QString gpuId = obj[ "gpuProfileId" ].toString();
      if ( !m_gpuProfileOverride && gpuId != m_activeProfileGpuId )
      {
        m_activeProfileGpuId = gpuId;
        m_activeProfileGpuName = resolveGpuProfileName( gpuId );
        changed = true;
      }

      if ( changed )
        emit activeProfileChanged();
    }
  }

  // Power state
  if ( auto ps = m_client->getPowerState() )
  {
    auto raw = QString::fromStdString( *ps );
    using namespace Qt::StringLiterals;
    auto s = raw == "power_ac"_L1  ? u"AC"_s
           : raw == "power_bat"_L1 ? u"Battery"_s
           : raw == "power_wc"_L1  ? u"AC w/ Water Cooler"_s
           : raw;
    if ( s != m_powerState )
    {
      m_powerState = s;
      emit powerStateChanged();
      emit wcConnectedChanged();  // derived from powerState
    }
  }

  // Hardware toggles
  if ( auto v = m_client->getWebcamEnabled(); v && *v != m_webcamEnabled )
  {
    m_webcamEnabled = *v;
    emit webcamEnabledChanged();
  }
  if ( auto v = m_client->getFnLock(); v && *v != m_fnLock )
  {
    m_fnLock = *v;
    emit fnLockChanged();
  }
  if ( auto v = m_client->getDisplayBrightness(); v && *v != m_displayBrightness )
  {
    m_displayBrightness = *v;
    emit displayBrightnessChanged();
  }

  // ODM Performance Profile
  if ( auto v = m_client->getODMPerformanceProfile() )
  {
    QString s = QString::fromStdString( *v );
    if ( s != m_odmPerformanceProfile )
    {
      m_odmPerformanceProfile = s;
      emit odmPerformanceProfileChanged();
    }
  }

}

// ---------------------------------------------------------------------------
// Daemon signal handlers
// ---------------------------------------------------------------------------

void TrayBackend::onDaemonProfileChanged( const QString &profileId,
                                          const QString &keyboardProfileId,
                                          const QString &fanProfileId,
                                          const QString &gpuProfileId )
{
  bool changed = false;

  if ( profileId != m_activeProfileId )
  {
    m_activeProfileId = profileId;
    int idx = m_profileIds.indexOf( profileId );
    m_activeProfileName = ( idx >= 0 ) ? m_profileNames[ idx ] : profileId;
    // Reset overrides when the system profile itself changes
    m_fanProfileOverride = false;
    m_keyboardProfileOverride = false;
    m_gpuProfileOverride = false;
    m_wcEnabledOverride = false;
    changed = true;
  }

  if ( !keyboardProfileId.isEmpty() && keyboardProfileId != m_activeProfileKeyboardId )
  {
    m_keyboardProfileOverride = false;
    m_activeProfileKeyboardId = keyboardProfileId;
    m_activeProfileKeyboardName = resolveKeyboardProfileName( keyboardProfileId );
    changed = true;
  }

  if ( !fanProfileId.isEmpty() && fanProfileId != m_activeProfileFanId )
  {
    m_fanProfileOverride = false;
    m_activeProfileFanId = fanProfileId;
    m_activeProfileFanName = resolveFanProfileName( fanProfileId );
    changed = true;
  }

  if ( !gpuProfileId.isEmpty() && gpuProfileId != m_activeProfileGpuId )
  {
    m_gpuProfileOverride = false;
    m_activeProfileGpuId = gpuProfileId;
    m_activeProfileGpuName = resolveGpuProfileName( gpuProfileId );
    changed = true;
  }

  if ( changed )
  {
    emit activeProfileChanged();
    pollSlowState();
  }
}

void TrayBackend::onSettingsFileChanged( const QString &path )
{
  // Some editors replace the file (delete + create) rather than writing in-place,
  // which removes it from the watch list.  Re-add after a short delay.
  QTimer::singleShot( 500, this, [this, path]() {
    if ( QFile::exists( path ) && !m_settingsWatcher->files().contains( path ) )
      m_settingsWatcher->addPath( path );

    fprintf( stderr, "[TrayBackend] Settings file changed, reloading profiles...\n" );
    loadProfiles();
    loadLocalProfiles();
  } );
}

void TrayBackend::onConnectionStatusChanged( bool connected )
{
  emit connectedChanged();

  if ( connected )
  {
    qInfo() << "[TrayBackend] Reconnected to uccd — refreshing all state";
    loadCapabilities();
    loadProfiles();
    loadLocalProfiles();
    pollMetrics();
    pollSlowState();

    // Ensure timers are running (they may have been started already, but
    // calling start() on a running QTimer simply resets the interval which
    // is harmless).
    m_fastTimer->start();
    m_slowTimer->start();
  }
  else
  {
    qWarning() << "[TrayBackend] Lost connection to uccd";
  }
}

// ---------------------------------------------------------------------------
// One-time loaders
// ---------------------------------------------------------------------------

void TrayBackend::loadProfiles()
{
  // Default (built-in) profiles come from the daemon
  QStringList names, ids;

  // Built-in profiles from daemon
  if ( auto json = m_client->getDefaultProfilesJSON() )
  {
    auto doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isArray() )
    {
      for ( const auto &val : doc.array() )
      {
        auto obj = val.toObject();
        QString id   = obj[ "id" ].toString();
        QString name = obj[ "name" ].toString();
        if ( id.isEmpty() ) continue;
        ids.append( id );
        names.append( name );
      }
    }
  }

  // Custom profiles are stored client-side in ~/.config/uccrc (same as GUI).
  // The values are QByteArray-encoded JSON, so we must use toByteArray().
  QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
  QByteArray customRaw = settings.value( "customProfiles", "[]" ).toByteArray();
  fprintf( stderr, "[TrayBackend] loadProfiles: uccrc exists=%d customProfiles bytes=%d\n",
           QFile::exists( QDir::homePath() + "/.config/uccrc" ), (int)customRaw.size() );
  auto customDoc = QJsonDocument::fromJson( customRaw );
  fprintf( stderr, "[TrayBackend] customProfiles isArray=%d count=%d\n",
           (int)customDoc.isArray(), customDoc.isArray() ? (int)customDoc.array().size() : 0 );
  if ( customDoc.isArray() )
  {
    for ( const auto &val : customDoc.array() )
    {
      auto obj = val.toObject();
      QString id   = obj[ "id" ].toString();
      QString name = obj[ "name" ].toString();
      if ( id.isEmpty() ) continue;
      ids.append( id );
      names.append( name );
    }
  }

  if ( ids != m_profileIds || names != m_profileNames )
  {
    m_profileIds   = ids;
    m_profileNames = names;
    fprintf( stderr, "[TrayBackend] Profiles emitted: %d profiles\n", (int)ids.size() );
    emit profilesChanged();
  }

  // Active profile
  if ( auto json = m_client->getActiveProfileJSON() )
  {
    auto doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isObject() )
    {
      auto obj = doc.object();
      m_activeProfileId = obj[ "id" ].toString();
      m_activeProfileName = obj[ "name" ].toString();
      fprintf( stderr, "[TrayBackend] Active profile: %s / %s\n",
               qPrintable( m_activeProfileId ), qPrintable( m_activeProfileName ) );
      emit activeProfileChanged();
    }
  }

  // Fan profiles
  if ( auto json = m_client->getFanProfilesJSON() )
  {
    auto doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isArray() )
    {
      QStringList fpNames, fpIds;
      for ( const auto &val : doc.array() )
      {
        auto obj = val.toObject();
        QString id   = obj[ "id" ].toString();
        QString name = obj[ "name" ].toString();
        if ( id.isEmpty() ) continue;
        fpIds.append( id );
        fpNames.append( name );
      }
      if ( fpIds != m_fanProfileIds || fpNames != m_fanProfileNames )
      {
        m_fanProfileIds   = fpIds;
        m_fanProfileNames = fpNames;
        emit fanProfilesChanged();
      }
    }
  }

  if ( auto json = m_client->getGpuProfilesJSON() )
  {
    auto doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isArray() )
    {
      QStringList gpNames, gpIds;
      for ( const auto &val : doc.array() )
      {
        auto obj = val.toObject();
        QString id   = obj[ "id" ].toString();
        QString name = obj[ "name" ].toString();
        if ( id.isEmpty() ) continue;
        gpIds.append( id );
        gpNames.append( name );
      }
      if ( gpIds != m_gpuProfileIds || gpNames != m_gpuProfileNames )
      {
        m_gpuProfileIds = gpIds;
        m_gpuProfileNames = gpNames;
        emit gpuProfilesChanged();
      }
    }
  }

  // ODM profiles
  if ( auto profs = m_client->getAvailableODMProfiles() )
  {
    QStringList sl;
    for ( const auto &s : *profs )
      sl.append( QString::fromStdString( s ) );
    if ( sl != m_availableODMProfiles )
    {
      m_availableODMProfiles = sl;
      emit odmProfilesAvailableChanged();
    }
  }
}

void TrayBackend::loadCapabilities()
{
  if ( auto supported = m_client->isDeviceSupported() )
  {
    bool was = m_deviceSupported;
    m_deviceSupported = *supported;
    if ( was != m_deviceSupported )
      emit deviceSupportedChanged();
  }

  if ( !m_deviceSupported )
    return;

  if ( auto v = m_client->getWaterCoolerSupported() )
  {
    bool was = m_waterCoolerSupported;
    m_waterCoolerSupported = *v;
    if ( was != m_waterCoolerSupported )
      emit waterCoolerSupportedChanged();
  }

  if ( auto sysInfoJson = m_client->getSystemInfoJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *sysInfoJson ) );
    if ( doc.isObject() )
    {
      const QJsonObject obj = doc.object();
      m_laptopModel = obj.value( "laptopModel" ).toString();
      m_cpuModel    = obj.value( "cpuModel" ).toString();
      m_dGpuModel   = obj.value( "dGpuModel" ).toString();
      m_iGpuModel   = obj.value( "iGpuModel" ).toString();
      emit systemInfoChanged();
    }
  }
}

// ---------------------------------------------------------------------------
// Local settings loaders (custom fan + keyboard profiles from ~/.config/uccrc)
// ---------------------------------------------------------------------------

void TrayBackend::loadLocalProfiles()
{
  QString uccrcPath = QDir::homePath() + "/.config/uccrc";
  bool fileExists = QFile::exists( uccrcPath );
  fprintf( stderr, "[TrayBackend] loadLocalProfiles: path=%s exists=%d\n",
           qPrintable( uccrcPath ), (int)fileExists );

  QSettings settings( uccrcPath, QSettings::IniFormat );
  fprintf( stderr, "[TrayBackend] QSettings keys: %s\n",
           qPrintable( settings.allKeys().join(", ") ) );

  // Merge custom fan profiles — append to the daemon-loaded lists (avoid duplicates on re-load)
  {
    m_customFanProfileNames.clear();
    m_customFanProfileIds.clear();
    QByteArray fanRaw = settings.value( "customFanProfiles", "[]" ).toByteArray();
    auto doc = QJsonDocument::fromJson( fanRaw );
    if ( doc.isArray() )
    {
      for ( const auto &val : doc.array() )
      {
        auto obj = val.toObject();
        QString id   = obj[ "id" ].toString();
        QString name = obj[ "name" ].toString();
        if ( id.isEmpty() ) continue;
        m_customFanProfileIds.append( id );
        m_customFanProfileNames.append( name );
        // Only append if not already present (built-ins already loaded by loadProfiles())
        if ( !m_fanProfileIds.contains( id ) )
        {
          m_fanProfileIds.append( id );
          m_fanProfileNames.append( name );
        }
      }
    }
    emit fanProfilesChanged();
  }

  // Re-resolve active fan profile name after loading local data
  if ( !m_activeProfileFanId.isEmpty() )
    m_activeProfileFanName = resolveFanProfileName( m_activeProfileFanId );

  // Custom keyboard profiles from QSettings
  {
    QStringList kpNames, kpIds;
    QJsonArray  kpData;
    QByteArray kbRaw = settings.value( "customKeyboardProfiles", "[]" ).toByteArray();
    auto doc = QJsonDocument::fromJson( kbRaw );
    if ( doc.isArray() )
    {
      for ( const auto &val : doc.array() )
      {
        auto obj = val.toObject();
        QString id   = obj[ "id" ].toString();
        QString name = obj[ "name" ].toString();
        if ( id.isEmpty() ) continue;
        kpIds.append( id );
        kpNames.append( name );
        kpData.append( obj );
      }
    }

    if ( kpIds != m_keyboardProfileIds || kpNames != m_keyboardProfileNames )
    {
      m_keyboardProfileIds   = kpIds;
      m_keyboardProfileNames = kpNames;
      m_keyboardProfilesData = kpData;
      fprintf( stderr, "[TrayBackend] keyboardProfiles updated: %d entries — [%s]\n",
               (int)kpIds.size(), qPrintable( kpNames.join(", ") ) );
      emit keyboardProfilesChanged();
    }
  }

  if ( !m_activeProfileKeyboardId.isEmpty() )
    m_activeProfileKeyboardName = resolveKeyboardProfileName( m_activeProfileKeyboardId );

  // Merge custom GPU OC profiles from local settings
  {
    QByteArray gpuRaw = settings.value( "customGpuProfiles", "[]" ).toByteArray();
    auto doc = QJsonDocument::fromJson( gpuRaw );
    if ( doc.isArray() )
    {
      for ( const auto &val : doc.array() )
      {
        auto obj = val.toObject();
        QString id   = obj[ "id" ].toString();
        QString name = obj[ "name" ].toString();
        if ( id.isEmpty() )
          continue;
        if ( !m_gpuProfileIds.contains( id ) )
        {
          m_gpuProfileIds.append( id );
          m_gpuProfileNames.append( name );
        }
      }
      emit gpuProfilesChanged();
    }
  }

  if ( !m_activeProfileGpuId.isEmpty() )
    m_activeProfileGpuName = resolveGpuProfileName( m_activeProfileGpuId );
}

// ---------------------------------------------------------------------------
// Resolvers: fan & keyboard profile ID → display name
// ---------------------------------------------------------------------------

QString TrayBackend::resolveFanProfileName( const QString &fanProfileId ) const
{
  if ( fanProfileId.isEmpty() )
    return {};

  if ( auto idx = m_fanProfileIds.indexOf( fanProfileId ); idx >= 0 )
    return m_fanProfileNames[ idx ];

  return fanProfileId;
}

QString TrayBackend::resolveKeyboardProfileName( const QString &kbProfileId ) const
{
  if ( kbProfileId.isEmpty() )
    return {};

  if ( auto idx = m_keyboardProfileIds.indexOf( kbProfileId ); idx >= 0 )
    return m_keyboardProfileNames[ idx ];

  return kbProfileId;
}

QString TrayBackend::resolveGpuProfileName( const QString &gpuProfileId ) const
{
  if ( gpuProfileId.isEmpty() )
    return {};

  if ( auto idx = m_gpuProfileIds.indexOf( gpuProfileId ); idx >= 0 )
    return m_gpuProfileNames[ idx ];

  return gpuProfileId;
}
