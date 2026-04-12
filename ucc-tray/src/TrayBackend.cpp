/*
 * Copyright (C) 2026 Unified Control Center Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "TrayBackend.hpp"

#include "GuiLauncher.hpp"
#include "UccDbusTypes.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

#include <algorithm>

namespace {

/// Parse a QVariantList of { id, name } maps into parallel name/id lists.
bool parseProfileArray( const QVariantList &list,
                        QStringList &outNames, QStringList &outIds )
{
  for ( const auto &val : list )
  {
    const auto map = val.toMap();
    QString id   = map.value( "id" ).toString();
    QString name = map.value( "name" ).toString();
    if ( id.isEmpty() ) continue;
    outIds.append( id );
    outNames.append( name );
  }
  return !list.isEmpty();
}

bool parseProfileArray( const ucc::dbus::ProfileSummaryDtoList &list,
                        QStringList &outNames, QStringList &outIds )
{
  for ( const auto &item : list )
  {
    if ( item.id.isEmpty() ) continue;
    outIds.append( item.id );
    outNames.append( item.name );
  }
  return !list.isEmpty();
}

} // anonymous namespace

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
  connect( m_client.get(), &ucc::UccdClient::profilesListChanged,
           this, &TrayBackend::onProfilesListChanged );
  connect( m_client.get(), &ucc::UccdClient::connectionStatusChanged,
           this, &TrayBackend::onConnectionStatusChanged );

  // Initial data load
  loadCapabilities();

  if ( !m_deviceSupported )
  {
    fprintf( stderr, "[TrayBackend] Device not supported — tray disabled\n" );
    return;
  }

  loadProfiles();
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
  // Fetch the fan profile zones/sources and apply directly
  auto zones   = m_client->getFanProfileZones( fanProfileId.toStdString() );
  auto sources = m_client->getFanProfileSources( fanProfileId.toStdString() );
  if ( zones )
    m_client->applyFanProfiles( *zones, sources.value_or( ucc::dbus::ThermalSourceDtoList{} ), fanProfileId );

  // Mark override so pollSlowState() doesn't revert to the daemon's stored value
  m_fanProfileOverride = true;
  m_activeProfileFanId = fanProfileId;
  m_activeProfileFanName = resolveFanProfileName( fanProfileId );
  emit activeProfileChanged();
}

void TrayBackend::setActiveKeyboardProfile( const QString &keyboardProfileId )
{
  if ( auto profileData = m_client->getKeyboardProfile( keyboardProfileId.toStdString() ) )
  {
    QJsonObject obj = QJsonObject::fromVariantMap( *profileData );
    // Inject the keyboard profile ID so uccd can notify subscribers
    obj[ "keyboardProfileId" ] = keyboardProfileId;
    m_client->setKeyboardBacklight(
      QJsonDocument( obj ).toJson( QJsonDocument::Compact ).toStdString() );
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

  if ( auto map = m_client->getGpuProfile( gpuProfileId.toStdString() ) )
  {
    applyWithProfileId( QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( *map ) ).toJson( QJsonDocument::Compact ) ) );
  }
  else
  {
    qWarning() << "[TrayBackend] GPU profile not found on daemon:" << gpuProfileId;
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

  if ( changed )
    emit metricsUpdated();
}

void TrayBackend::pollSlowState()
{
  // Power state
  if ( auto ps = m_client->getPowerState() )
  {
    auto raw = QString::fromStdString( *ps );
    using namespace Qt::StringLiterals;
    auto s = raw == "power_ac"_L1  ? u"AC"_s
           : raw == "power_bat"_L1 ? u"Battery"_s
           : raw;
    if ( s != m_powerState )
    {
      m_powerState = s;
      emit powerStateChanged();
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
  }
}

void TrayBackend::onProfilesListChanged()
{
  loadProfiles();
}

void TrayBackend::onSettingsFileChanged( const QString & /*path*/ )
{
  // Legacy handler — no longer watching uccrc.
  // All profile data comes from the daemon now.
  fprintf( stderr, "[TrayBackend] Settings file changed (legacy) — reloading from daemon\n" );
  loadProfiles();
}

void TrayBackend::onConnectionStatusChanged( bool connected )
{
  emit connectedChanged();

  if ( connected )
  {
    qInfo() << "[TrayBackend] Reconnected to uccd — refreshing all state";
    loadCapabilities();
    loadProfiles();
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
  // All profiles (built-in + custom) come from the daemon now
  QStringList names, ids;

  if ( auto json = m_client->getProfiles() )
    parseProfileArray( *json, names, ids );

  if ( ids != m_profileIds || names != m_profileNames )
  {
    m_profileIds   = ids;
    m_profileNames = names;
    fprintf( stderr, "[TrayBackend] Profiles emitted: %d profiles\n", (int)ids.size() );
    emit profilesChanged();
  }

  // Active profile
  if ( auto applied = m_client->getAppliedProfiles() )
  {
    m_activeProfileId = applied->value( "profileId" ).toString();
    m_activeProfileName = applied->value( "profileName" ).toString();
    m_activeProfileFanId = applied->value( "fanProfileId" ).toString();
    m_activeProfileFanName = resolveFanProfileName( m_activeProfileFanId );
    m_activeProfileKeyboardId = applied->value( "keyboardProfileId" ).toString();
    m_activeProfileKeyboardName = resolveKeyboardProfileName( m_activeProfileKeyboardId );
    m_activeProfileGpuId = applied->value( "appliedGpuProfileId" ).toString();
    m_activeProfileGpuName = resolveGpuProfileName( m_activeProfileGpuId );
    fprintf( stderr, "[TrayBackend] Active profile: %s / %s\n",
             qPrintable( m_activeProfileId ), qPrintable( m_activeProfileName ) );
    emit activeProfileChanged();
  }

  // Fan profiles
  {
    QStringList fpNames, fpIds;
    if ( auto fanList = m_client->getFanProfiles() )
      parseProfileArray( *fanList, fpNames, fpIds );
    if ( fpIds != m_fanProfileIds || fpNames != m_fanProfileNames )
    {
      m_fanProfileIds   = fpIds;
      m_fanProfileNames = fpNames;
      emit fanProfilesChanged();
    }
  }

  // GPU profiles
  {
    QStringList gpNames, gpIds;
    if ( auto json = m_client->getGpuProfiles() )
      parseProfileArray( *json, gpNames, gpIds );
    if ( gpIds != m_gpuProfileIds || gpNames != m_gpuProfileNames )
    {
      m_gpuProfileIds = gpIds;
      m_gpuProfileNames = gpNames;
      emit gpuProfilesChanged();
    }
  }

  // Keyboard profiles from daemon
  {
    QStringList kpNames, kpIds;
    if ( auto json = m_client->getKeyboardProfiles() )
      parseProfileArray( *json, kpNames, kpIds );
    if ( kpIds != m_keyboardProfileIds || kpNames != m_keyboardProfileNames )
    {
      m_keyboardProfileIds   = kpIds;
      m_keyboardProfileNames = kpNames;
      emit keyboardProfilesChanged();
    }
  }

  // Re-resolve sub-profile display names
  if ( !m_activeProfileFanId.isEmpty() )
    m_activeProfileFanName = resolveFanProfileName( m_activeProfileFanId );
  if ( !m_activeProfileKeyboardId.isEmpty() )
    m_activeProfileKeyboardName = resolveKeyboardProfileName( m_activeProfileKeyboardId );
  if ( !m_activeProfileGpuId.isEmpty() )
    m_activeProfileGpuName = resolveGpuProfileName( m_activeProfileGpuId );

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
  // Capability-based detection: the daemon always runs, capability flags
  // determine what the UI shows.  deviceSupported is true if the HAL found
  // *any* controllable hardware.
  if ( auto caps = m_client->getCapabilities() )
  {
    bool hasAnyCaps = !caps->isEmpty();
    bool was = m_deviceSupported;
    m_deviceSupported = hasAnyCaps;
    if ( was != m_deviceSupported )
      emit deviceSupportedChanged();
  }

  if ( !m_deviceSupported )
    return;

  if ( auto sysInfo = m_client->getSystemInfo() )
  {
    m_laptopModel = sysInfo->value( "laptopModel" ).toString();
    m_cpuModel    = sysInfo->value( "cpuModel" ).toString();
    m_dGpuModel   = sysInfo->value( "dGpuModel" ).toString();
    m_iGpuModel   = sysInfo->value( "iGpuModel" ).toString();
    emit systemInfoChanged();
  }
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
