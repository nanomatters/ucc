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

#include "ProfileManager.hpp"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <QFile>

namespace ucc
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProfileManager::ProfileManager( QObject *parent )
  : QObject( parent )
  , m_client( std::make_unique< UccdClient >( this ) )
{
  m_connected = m_client->isConnected();

  // Always wire daemon signals — they won't fire while disconnected but will
  // start arriving as soon as uccd appears (see connectionStatusChanged below).
  connect( m_client.get(), &UccdClient::profileChanged,
           this, [this]( const QString &profileId,
                         const QString &keyboardProfileId,
                         const QString &fanProfileId,
                         const QString &gpuProfileId ) {
    onProfileChanged( profileId.toStdString(),
                      keyboardProfileId.toStdString(),
                      fanProfileId.toStdString(),
                      gpuProfileId.toStdString() );
  } );
  connect( m_client.get(), &UccdClient::powerStateChanged,
           this, [this]( const QString &state ) {
    onPowerStateChanged( state );
  } );

  // Re-init when uccd appears or disappears
  connect( m_client.get(), &UccdClient::connectionStatusChanged,
           this, [this]( bool connected ) {
    const bool wasConnected = m_connected;
    m_connected = connected;
    if ( connected && !wasConnected )
    {
      qInfo() << "[ProfileManager] uccd reconnected — reloading data from daemon";
      if ( auto odmList = m_client->getODMPowerLimits() )
      {
        m_hardwarePowerLimits.clear();
        for ( const auto &v : *odmList )
          m_hardwarePowerLimits.push_back( v.toMap().value( "max" ).toInt() );
      }
      loadProfilesFromDaemon();
      loadFanProfilesFromDaemon();
      loadGpuProfilesFromDaemon();
      loadKeyboardProfilesFromDaemon();
      loadHardwareFanDevicesFromDaemon();
      loadHardwareSensorsFromDaemon();
      loadThermalSourcesFromDaemon();
      loadFanZonesFromDaemon();
    }
    emit connectedChanged();
  } );

  if ( m_connected )
  {
    if ( auto odmList = m_client->getODMPowerLimits() )
    {
      m_hardwarePowerLimits.clear();
      for ( const auto &v : *odmList )
        m_hardwarePowerLimits.push_back( v.toMap().value( "max" ).toInt() );
    }
    loadProfilesFromDaemon();
    loadFanProfilesFromDaemon();
    loadGpuProfilesFromDaemon();
    loadKeyboardProfilesFromDaemon();
    loadHardwareFanDevicesFromDaemon();
    loadHardwareSensorsFromDaemon();
    loadThermalSourcesFromDaemon();
    loadFanZonesFromDaemon();
  }

  emit connectedChanged();
}

// ---------------------------------------------------------------------------
// Load all profiles from daemon (replaces loadCustomProfilesFromSettings +
// loadBuiltinFanProfiles + loadBuiltinGpuProfiles)
// ---------------------------------------------------------------------------

void ProfileManager::loadProfilesFromDaemon()
{
  m_allProfilesData = QJsonArray();

  // GetProfilesJSON returns ALL profiles with "editable" flag
  if ( auto list = m_client->getProfiles() )
    m_allProfilesData = QJsonArray::fromVariantList( *list );

  m_activeProfileId.clear();

  updateAllProfiles();
}

void ProfileManager::loadFanProfilesFromDaemon()
{
  m_fanProfilesData = QJsonArray();

  if ( auto list = m_client->getFanProfiles() )
    m_fanProfilesData = QJsonArray::fromVariantList( *list );

  emit fanProfilesChanged();
}

void ProfileManager::loadGpuProfilesFromDaemon()
{
  m_gpuProfilesData = QJsonArray();

  if ( auto list = m_client->getGpuProfiles() )
    m_gpuProfilesData = QJsonArray::fromVariantList( *list );

  emit gpuProfilesChanged();
}

void ProfileManager::loadKeyboardProfilesFromDaemon()
{
  m_keyboardProfilesData = QJsonArray();

  if ( auto list = m_client->getKeyboardProfiles() )
    m_keyboardProfilesData = QJsonArray::fromVariantList( *list );

  emit keyboardProfilesChanged();
}

void ProfileManager::loadThermalSourcesFromDaemon()
{
  m_thermalSourcesData = QJsonArray();

  if ( auto list = m_client->getThermalSources() )
    m_thermalSourcesData = QJsonArray::fromVariantList( *list );
}

void ProfileManager::loadHardwareFanDevicesFromDaemon()
{
  m_hardwareFanDevicesData = QJsonArray();

  if ( auto list = m_client->getHardwareFanDevices() )
    m_hardwareFanDevicesData = QJsonArray::fromVariantList( *list );
}

void ProfileManager::loadHardwareSensorsFromDaemon()
{
  m_hardwareSensorsData = QJsonArray();

  if ( auto list = m_client->getHardwareSensors() )
    m_hardwareSensorsData = QJsonArray::fromVariantList( *list );
}

void ProfileManager::loadFanZonesFromDaemon()
{
  m_fanZonesData = QJsonArray();

  if ( auto list = m_client->getFanZones() )
    m_fanZonesData = QJsonArray::fromVariantList( *list );
}

// ---------------------------------------------------------------------------
// Refresh / update
// ---------------------------------------------------------------------------

void ProfileManager::refresh()
{
  updateProfiles();
}

void ProfileManager::refreshGpuProfiles()
{
  loadGpuProfilesFromDaemon();
}

void ProfileManager::updateProfiles()
{
  // Fetch profiles from daemon if not already loaded
  if ( m_allProfilesData.isEmpty() )
    loadProfilesFromDaemon();

  updateAllProfiles();

  // Query daemon for current power state
  if ( m_powerState.isEmpty() )
  {
    try {
      if ( auto state = m_client->getPowerState() )
      {
        m_powerState = QString::fromStdString( *state );
        emit powerStateChanged();
      }
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to get power state:" << e.what();
    }
  }

  // Resolve the active profile for the current power state from the stateMap.
  if ( m_activeProfileId.isEmpty() && !m_powerState.isEmpty() )
  {
    if ( QString mapped = resolveStateMapToProfileId( m_powerState ); !mapped.isEmpty() )
    {
      m_activeProfileId = mapped;
      emit activeProfileChanged();
    }
  }

  // Fallback: ask daemon for currently running profile
  if ( m_activeProfileId.isEmpty() )
  {
    try
    {
      if ( auto map = m_client->getActiveProfile() )
      {
        QString id = map->value( "id" ).toString();
        if ( !id.isEmpty() )
        {
          m_activeProfileId = id;
          emit activeProfileChanged();
        }
      }
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to get active profile:" << e.what();
    }
  }

  // Query the daemon's live active profile for current sub-profile IDs
  try
  {
    if ( auto map = m_client->getActiveProfile() )
    {
      QJsonObject obj = QJsonObject::fromVariantMap( *map );

      QString kbId = obj[ "selectedKeyboardProfile" ].toString();
      if ( !kbId.isEmpty() )
        m_activeKeyboardProfileId = kbId;

      auto fanObj = obj[ "fan" ].toObject();
      QString fpId = fanObj[ "fanProfile" ].toString();
      if ( !fpId.isEmpty() )
        m_activeFanProfileId = fpId;

      QString gpId = obj[ "gpuProfileId" ].toString();
      if ( !gpId.isEmpty() )
        m_activeGpuProfileId = gpId;
    }
  }
  catch ( ... ) {}

  updateAllProfiles();
  updateActiveProfileIndex();
}

// ---------------------------------------------------------------------------
// Active profile name (for display)
// ---------------------------------------------------------------------------

QString ProfileManager::activeProfileName() const
{
  return profileNameById( m_activeProfileId );
}

// ---------------------------------------------------------------------------
// ID <-> name helpers
// ---------------------------------------------------------------------------

QString ProfileManager::profileNameById( const QString &profileId ) const
{
  if ( profileId.isEmpty() ) return QString();

  for ( const auto &p : m_allProfilesData )
  {
    if ( p.isObject() && p.toObject()["id"].toString() == profileId )
      return p.toObject()["name"].toString();
  }
  return QString();
}

QString ProfileManager::profileIdByName( const QString &profileName ) const
{
  if ( profileName.isEmpty() ) return QString();

  for ( const auto &p : m_allProfilesData )
  {
    if ( p.isObject() && p.toObject()["name"].toString() == profileName )
      return p.toObject()["id"].toString();
  }
  return QString();
}

// ---------------------------------------------------------------------------
// Set active profile by ID
// ---------------------------------------------------------------------------

void ProfileManager::setActiveProfile( const QString &profileId )
{
  // Custom (editable) profiles are applied by sending data; built-in profiles by ID only
  QString profileData;
  bool isEditable = false;
  for ( const auto &profile : m_allProfilesData )
  {
    QJsonObject obj = profile.toObject();
    if ( obj.value( "id" ).toString() == profileId )
    {
      isEditable = obj.value( "editable" ).toBool( false );
      if ( isEditable )
        profileData = QJsonDocument( obj ).toJson( QJsonDocument::Compact );
      break;
    }
  }

  bool success = false;
  if ( isEditable && !profileData.isEmpty() )
  {
    try {
      success = m_client->applyProfile( profileData.toStdString() );
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to apply custom profile:" << e.what();
    }
  }
  else
  {
    try {
      success = m_client->setActiveProfile( profileId.toStdString() );
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to set active profile:" << e.what();
    }
  }

  if ( m_activeProfileId != profileId )
  {
    m_activeProfileId = profileId;
    emit activeProfileChanged();
  }
  updateAllProfiles();
  updateActiveProfileIndex();

  if ( !success )
    emit error( "Failed to activate profile: " + profileId );
}

// ---------------------------------------------------------------------------
// Save / delete / create profiles — all go through daemon
// ---------------------------------------------------------------------------

void ProfileManager::saveProfile( const QString &profileJSON )
{
  QJsonDocument doc = QJsonDocument::fromJson( profileJSON.toUtf8() );
  if ( !doc.isObject() )
  {
    emit error( "Invalid profile JSON" );
    return;
  }

  QJsonObject profileObj = doc.object();
  QString profileId = profileObj.value( "id" ).toString();
  QString profileName = profileObj.value( "name" ).toString();

  if ( profileId.isEmpty() || profileName.isEmpty() )
  {
    emit error( "Profile missing id or name" );
    return;
  }

  // Save to daemon (daemon now handles all persistence)
  if ( m_connected )
  {
    bool success = m_client->saveProfile( profileJSON.toStdString() );
    if ( !success )
    {
      qWarning() << "Failed to save profile to daemon:" << profileName;
      emit error( "Failed to save profile: " + profileName );
      return;
    }
    qDebug() << "Profile saved to daemon:" << profileName;
  }

  // Update local cache
  int foundIndex = -1;
  for ( int i = 0; i < m_allProfilesData.size(); ++i )
  {
    if ( m_allProfilesData[i].toObject().value( "id" ).toString() == profileId )
    {
      foundIndex = i;
      break;
    }
  }

  if ( foundIndex == -1 )
    m_allProfilesData.append( profileObj );
  else
    m_allProfilesData[foundIndex] = profileObj;

  updateAllProfiles();
}

void ProfileManager::deleteProfile( const QString &profileId )
{
  // Delete via daemon
  if ( m_connected )
  {
    bool success = m_client->deleteProfile( profileId.toStdString() );
    if ( !success )
    {
      qWarning() << "Failed to delete profile from daemon:" << profileId;
      emit error( "Failed to delete profile: " + profileId );
      return;
    }
  }

  // Update local cache
  for ( int i = 0; i < m_allProfilesData.size(); ++i )
  {
    if ( m_allProfilesData[i].toObject().value( "id" ).toString() == profileId )
    {
      m_allProfilesData.removeAt( i );
      updateAllProfiles();
      qDebug() << "Profile deleted:" << profileId;
      return;
    }
  }
  emit error( "Profile not found: " + profileId );
}

QString ProfileManager::createProfileFromDefault( const QString &name )
{
  if ( auto defaultMap = m_client->getDefaultValuesProfile() )
  {
    QJsonObject profileObj = QJsonObject::fromVariantMap( *defaultMap );
    QString id = QUuid::createUuid().toString( QUuid::WithoutBraces );
    profileObj["name"] = name;
    profileObj["id"] = id;
    profileObj["editable"] = true;

    QString profileJSON = QJsonDocument( profileObj ).toJson( QJsonDocument::Compact );

    // Save through daemon
    if ( m_connected )
    {
      if ( !m_client->saveProfile( profileJSON.toStdString() ) )
      {
        emit error( "Failed to save new profile to daemon" );
        return QString();
      }
    }

    m_allProfilesData.append( profileObj );
    updateAllProfiles();

    qDebug() << "Created new profile from default:" << name;
    return profileJSON;
  }

  emit error( "Failed to get default profile template" );
  return QString();
}

// ---------------------------------------------------------------------------
// Profile details
// ---------------------------------------------------------------------------

QString ProfileManager::getProfileDetails( const QString &profileId )
{
  for ( const auto &profile : m_allProfilesData )
  {
    if ( profile.toObject()["id"].toString() == profileId )
      return QJsonDocument( profile.toObject() ).toJson( QJsonDocument::Compact );
  }
  return QString();
}

// ---------------------------------------------------------------------------
// Profile changed signal (from daemon)
// ---------------------------------------------------------------------------

void ProfileManager::onProfileChanged( const std::string &profileId,
                                       const std::string &keyboardProfileId,
                                       const std::string &fanProfileId,
                                       const std::string &gpuProfileId )
{
  const QString qId = QString::fromStdString( profileId );

  if ( !qId.isEmpty() && m_activeProfileId != qId )
  {
    m_activeProfileId = qId;
    emit activeProfileChanged();
    qDebug() << "Active profile updated from signal:" << qId;
  }

  if ( const QString kbId = QString::fromStdString( keyboardProfileId );
       !kbId.isEmpty() && kbId != m_activeKeyboardProfileId )
  {
    m_activeKeyboardProfileId = kbId;
    emit activeKeyboardProfileChanged( kbId );
  }

  if ( const QString fpId = QString::fromStdString( fanProfileId );
       !fpId.isEmpty() && fpId != m_activeFanProfileId )
  {
    m_activeFanProfileId = fpId;
    emit activeFanProfileChanged( fpId );
  }

  if ( const QString gpId = QString::fromStdString( gpuProfileId );
       !gpId.isEmpty() && gpId != m_activeGpuProfileId )
  {
    m_activeGpuProfileId = gpId;
    emit activeGpuProfileChanged( gpId );
  }

  updateProfiles();
}

void ProfileManager::onPowerStateChanged( const QString &state )
{
  m_powerState = state;
  emit powerStateChanged();

  QString desiredProfileId = resolveStateMapToProfileId( state );
  if ( desiredProfileId.isEmpty() ) return;

  if ( m_activeProfileId != desiredProfileId )
  {
    m_activeProfileId = desiredProfileId;
    emit activeProfileChanged();
    updateAllProfiles();
    updateActiveProfileIndex();
  }
}

// ---------------------------------------------------------------------------
// Profile list management
// ---------------------------------------------------------------------------

void ProfileManager::updateAllProfiles()
{
  QStringList newAllProfiles;
  QStringList newAllProfileIds;

  for ( const auto &p : m_allProfilesData )
  {
    if ( p.isObject() )
    {
      newAllProfiles.append( p.toObject()["name"].toString() );
      newAllProfileIds.append( p.toObject()["id"].toString() );
    }
  }

  if ( m_allProfiles != newAllProfiles || m_allProfileIds != newAllProfileIds )
  {
    m_allProfiles = newAllProfiles;
    m_allProfileIds = newAllProfileIds;
    emit allProfilesChanged();
  }
}

void ProfileManager::updateActiveProfileIndex()
{
  int newIndex = m_allProfileIds.indexOf( m_activeProfileId );
  if ( m_activeProfileIndex != newIndex )
  {
    m_activeProfileIndex = newIndex;
    emit activeProfileIndexChanged();
  }
}

void ProfileManager::setActiveProfileByIndex( int index )
{
  if ( index >= 0 && index < m_allProfileIds.size() )
    setActiveProfile( m_allProfileIds.at( index ) );
}

std::vector< int > ProfileManager::getHardwarePowerLimits()
{
  if ( auto odmList = m_client->getODMPowerLimits(); odmList && !odmList->empty() )
  {
    m_hardwarePowerLimits.clear();
    for ( const auto &v : *odmList )
      m_hardwarePowerLimits.push_back( v.toMap().value( "max" ).toInt() );
  }

  return m_hardwarePowerLimits;
}

bool ProfileManager::isProfileEditable( const QString &profileId ) const
{
  return isProfileEditable( profileId, m_allProfilesData );
}

// ---------------------------------------------------------------------------
// State map
// ---------------------------------------------------------------------------

QString ProfileManager::resolveStateMapToProfileId( const QString &state )
{
  const QVariantMap settings = getSettings();
  if ( settings.isEmpty() )
    return QString();

  const QVariantMap stateMap = settings.value( "stateMap" ).toMap();
  if ( stateMap.isEmpty() || !stateMap.contains( state ) )
    return QString();

  return stateMap.value( state ).toString();
}

bool ProfileManager::setStateMap( const QString &state, const QString &profileId )
{
  return m_client->setStateMap( state.toStdString(), profileId.toStdString() );
}

bool ProfileManager::setBatchStateMap( const std::map< QString, QString > &entries )
{
  std::map< std::string, std::string > stdEntries;
  for ( const auto &[state, profileId] : entries )
    stdEntries[state.toStdString()] = profileId.toStdString();
  return m_client->setBatchStateMap( stdEntries );
}

// ---------------------------------------------------------------------------
// Fan profiles — all go through daemon
// ---------------------------------------------------------------------------

QString ProfileManager::getFanProfile( const QString &fanProfileId )
{
  // Fetch from daemon (supports both built-in and custom)
  if ( auto map = m_client->getFanProfile( fanProfileId.toStdString() ) )
    return QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( *map ) ).toJson( QJsonDocument::Compact ) );

  return "{}";
}

bool ProfileManager::setFanProfile( const QString &fanProfileId, const QString &name, const QString &json )
{
  bool success = m_client->saveFanProfile( fanProfileId.toStdString(), name.toStdString(), json.toStdString() );
  if ( success )
  {
    loadFanProfilesFromDaemon();
    qDebug() << "Fan profile saved via daemon:" << name;
  }
  else
  {
    qWarning() << "Failed to save fan profile via daemon:" << name;
  }
  return success;
}

bool ProfileManager::deleteFanProfile( const QString &fanProfileId )
{
  bool success = m_client->deleteFanProfile( fanProfileId.toStdString() );
  if ( success )
  {
    loadFanProfilesFromDaemon();
    qDebug() << "Fan profile deleted via daemon:" << fanProfileId;
  }
  return success;
}

bool ProfileManager::renameFanProfile( const QString &fanProfileId, const QString &newName )
{
  if ( newName.isEmpty() ) return false;

  // Fetch current data, update name, re-save
  if ( auto map = m_client->getFanProfile( fanProfileId.toStdString() ) )
  {
    return setFanProfile( fanProfileId, newName,
      QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( *map ) ).toJson( QJsonDocument::Compact ) ) );
  }
  return false;
}

// ---------------------------------------------------------------------------
// Keyboard profiles — all go through daemon
// ---------------------------------------------------------------------------

QString ProfileManager::getKeyboardProfile( const QString &keyboardProfileId )
{
  if ( auto map = m_client->getKeyboardProfile( keyboardProfileId.toStdString() ) )
    return QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( *map ) ).toJson( QJsonDocument::Compact ) );

  return "{}";
}

bool ProfileManager::setKeyboardProfile( const QString &keyboardProfileId, const QString &name, const QString &json )
{
  bool success = m_client->saveKeyboardProfile( keyboardProfileId.toStdString(), name.toStdString(), json.toStdString() );
  if ( success )
  {
    loadKeyboardProfilesFromDaemon();
    qDebug() << "Keyboard profile saved via daemon:" << name;
  }
  return success;
}

bool ProfileManager::deleteKeyboardProfile( const QString &keyboardProfileId )
{
  bool success = m_client->deleteKeyboardProfile( keyboardProfileId.toStdString() );
  if ( success )
  {
    loadKeyboardProfilesFromDaemon();
    qDebug() << "Keyboard profile deleted via daemon:" << keyboardProfileId;
  }
  return success;
}

bool ProfileManager::renameKeyboardProfile( const QString &keyboardProfileId, const QString &newName )
{
  if ( newName.isEmpty() ) return false;

  if ( auto map = m_client->getKeyboardProfile( keyboardProfileId.toStdString() ) )
    return setKeyboardProfile( keyboardProfileId, newName,
      QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( *map ) ).toJson( QJsonDocument::Compact ) ) );

  return false;
}

// ---------------------------------------------------------------------------
// GPU OC profiles — all go through daemon
// ---------------------------------------------------------------------------

QString ProfileManager::getGpuProfile( const QString &gpuProfileId )
{
  if ( auto map = m_client->getGpuProfile( gpuProfileId.toStdString() ) )
    return QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( *map ) ).toJson( QJsonDocument::Compact ) );

  return "{}";
}

bool ProfileManager::setGpuProfile( const QString &gpuProfileId, const QString &name, const QString &json )
{
  // Do not allow overwriting built-in profiles
  if ( !isProfileEditable( gpuProfileId, m_gpuProfilesData ) )
    return false;

  bool success = m_client->saveGpuProfile( gpuProfileId.toStdString(), name.toStdString(), json.toStdString() );
  if ( success )
  {
    loadGpuProfilesFromDaemon();
    qDebug() << "GPU profile saved via daemon:" << name;
  }
  return success;
}

bool ProfileManager::deleteGpuProfile( const QString &gpuProfileId )
{
  if ( !isProfileEditable( gpuProfileId, m_gpuProfilesData ) )
    return false;

  bool success = m_client->deleteGpuProfile( gpuProfileId.toStdString() );
  if ( success )
  {
    loadGpuProfilesFromDaemon();
    qDebug() << "GPU profile deleted via daemon:" << gpuProfileId;
  }
  return success;
}

bool ProfileManager::renameGpuProfile( const QString &gpuProfileId, const QString &newName )
{
  if ( newName.isEmpty() ) return false;

  if ( !isProfileEditable( gpuProfileId, m_gpuProfilesData ) )
    return false;

  if ( auto map = m_client->getGpuProfile( gpuProfileId.toStdString() ) )
    return setGpuProfile( gpuProfileId, newName,
      QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( *map ) ).toJson( QJsonDocument::Compact ) ) );

  return false;
}

// ---------------------------------------------------------------------------
// Editable helper
// ---------------------------------------------------------------------------

bool ProfileManager::isProfileEditable( const QString &profileId, const QJsonArray &profilesData ) const
{
  for ( const auto &v : profilesData )
  {
    if ( v.isObject() && v.toObject().value( "id" ).toString() == profileId )
      return v.toObject().value( "editable" ).toBool( false );
  }
  return true;  // unknown ID → treat as editable (new profile being created)
}

// ---------------------------------------------------------------------------
// Settings JSON
// ---------------------------------------------------------------------------

QVariantMap ProfileManager::getSettings()
{
  try {
    if ( auto settings = m_client->getSettings() )
      return *settings;
  } catch ( const std::exception &e ) {
    qWarning() << "Failed to get settings JSON:" << e.what();
  }
  return QVariantMap();
}

} // namespace ucc
