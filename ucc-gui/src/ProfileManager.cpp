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

// Construction

ProfileManager::ProfileManager( QObject *parent )
  : QObject( parent )
  , m_client( std::make_unique< UccdClient >( this ) )
{
  m_connected = m_client->isConnected();

  // Load custom fan profiles via D-Bus (best-effort if disconnected)
  loadCustomFanProfilesFromSettings();

  // Load custom keyboard profiles via D-Bus
  loadCustomKeyboardProfilesFromSettings();

  // Always wire daemon signals - they won't fire while disconnected but will
  // start arriving as soon as uccd appears (see connectionStatusChanged below).
  connect( m_client.get(), &UccdClient::profileChanged,
           this, [this]( const QString &profileId,
                         const QString &keyboardProfileId,
                         const QString &fanProfileId ) {
    onProfileChanged( profileId.toStdString(),
                      keyboardProfileId.toStdString(),
                      fanProfileId.toStdString() );
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
      qInfo() << "[ProfileManager] uccd reconnected — reloading data";
      m_hardwarePowerLimits = m_client->getODMPowerLimits().value_or( std::vector< int >() );
      loadBuiltinFanProfiles();
      loadCustomFanProfilesFromSettings();
      loadCustomProfilesFromSettings();
      loadCustomKeyboardProfilesFromSettings();
    }
    emit connectedChanged();
  } );

  if ( m_connected )
  {
    // Fetch hardware power limits immediately
    m_hardwarePowerLimits = m_client->getODMPowerLimits().value_or( std::vector< int >() );

    // Fetch built-in fan profiles from daemon (id + name)
    loadBuiltinFanProfiles();

    // Load custom profiles from daemon
    loadCustomProfilesFromSettings();
    loadCustomKeyboardProfilesFromSettings();
  }
  emit connectedChanged();
}

// Refresh / update

void ProfileManager::refresh()
{
  updateProfiles();
}

void ProfileManager::updateProfiles()
{
  // Fetch default profiles if not already loaded
  if ( m_defaultProfilesData.isEmpty() )
  {
    try {
      if ( auto json = m_client->getDefaultProfilesJSON() )
      {
        QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *json ).toUtf8() );
        if ( doc.isArray() )
        {
          m_defaultProfilesData = doc.array();
          m_defaultProfiles.clear();
          for ( const auto &profile : m_defaultProfilesData )
          {
            if ( profile.isObject() )
            {
              QString name = profile.toObject()["name"].toString();
              if ( !name.isEmpty() )
              {
                m_defaultProfiles.append( name );
              }
            }
          }
        }
      }
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to get default profiles:" << e.what();
    }
  }

  emit defaultProfilesChanged();
  emit customProfilesChanged();

  // Ensure combined list is up-to-date
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
  // This is the authoritative source: if the user set a built-in profile via
  // SetStateMap, the stateMap reflects that - whereas the daemon's
  // GetActiveProfileJSON only reports the *running* profile which may differ.
  if ( m_activeProfileId.isEmpty() && !m_powerState.isEmpty() )
  {
    if ( QString mapped = resolveStateMapToProfileId( m_powerState ); !mapped.isEmpty() )
    {
      m_activeProfileId = mapped;
      emit activeProfileChanged();
    }
  }

  // Fallback: ask daemon for currently running profile (e.g. fresh install, no stateMap yet)
  if ( m_activeProfileId.isEmpty() )
  {
    try
    {
      if ( auto json = m_client->getActiveProfileJSON() )
      {
        QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *json ).toUtf8() );
        if ( doc.isObject() )
        {
          QJsonObject obj = doc.object();
          QString id = obj["id"].toString();

          if ( !id.isEmpty() )
          {
            m_activeProfileId = id;
            emit activeProfileChanged();
          }
        }
      }
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to get active profile:" << e.what();
    }
  }

  // Query the daemon's live active profile to learn the current keyboard and
  // fan sub-profile IDs. These may differ from the stored profile if a remote
  // client (e.g. the tray applet) changed them at runtime.
  try
  {
    if ( auto json = m_client->getActiveProfileJSON() )
    {
      QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *json ).toUtf8() );
      if ( doc.isObject() )
      {
        QJsonObject obj = doc.object();

        // Keyboard profile ID
        QString kbId = obj[ "selectedKeyboardProfile" ].toString();
        if ( !kbId.isEmpty() )
          m_activeKeyboardProfileId = kbId;

        // Fan profile ID
        auto fanObj = obj[ "fan" ].toObject();
        QString fpId = fanObj[ "fanProfile" ].toString();
        if ( !fpId.isEmpty() )
          m_activeFanProfileId = fpId;
      }
    }
  }
  catch ( ... ) {}

  updateAllProfiles();
  updateActiveProfileIndex();
}

// Active profile name (for display)

QString ProfileManager::activeProfileName() const
{
  return profileNameById( m_activeProfileId );
}

// ID <-> name helpers

QString ProfileManager::profileNameById( const QString &profileId ) const
{
  if ( profileId.isEmpty() ) return QString();

  // Search custom profiles first (they take precedence)
  for ( const auto &p : m_customProfilesData )
  {
    if ( p.isObject() && p.toObject()["id"].toString() == profileId )
      return p.toObject()["name"].toString();
  }
  for ( const auto &p : m_defaultProfilesData )
  {
    if ( p.isObject() && p.toObject()["id"].toString() == profileId )
      return p.toObject()["name"].toString();
  }
  return QString();
}

QString ProfileManager::profileIdByName( const QString &profileName ) const
{
  if ( profileName.isEmpty() ) return QString();

  // Search custom profiles first
  for ( const auto &p : m_customProfilesData )
  {
    if ( p.isObject() && p.toObject()["name"].toString() == profileName )
      return p.toObject()["id"].toString();
  }
  for ( const auto &p : m_defaultProfilesData )
  {
    if ( p.isObject() && p.toObject()["name"].toString() == profileName )
      return p.toObject()["id"].toString();
  }
  return QString();
}

// Set active profile by ID

void ProfileManager::setActiveProfile( const QString &profileId )
{
  // Check if this is a custom profile
  bool isCustom = false;
  QString profileData;
  for ( const auto &profile : m_customProfilesData )
  {
    QJsonObject obj = profile.toObject();
    if ( obj.value( "id" ).toString() == profileId )
    {
      isCustom = true;
      profileData = QJsonDocument( obj ).toJson( QJsonDocument::Compact );
      break;
    }
  }

  bool success = false;
  if ( isCustom && !profileData.isEmpty() )
  {
    try {
      success = m_client->applyProfile( profileData.toStdString() );
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to apply custom profile:" << e.what();
    }
    qDebug() << "Custom profile applied:" << profileId;
  }
  else
  {
    try {
      success = m_client->setActiveProfile( profileId.toStdString() );
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to set active profile:" << e.what();
    }
    qDebug() << "Default profile activated:" << profileId;
  }

  if ( m_activeProfileId != profileId )
  {
    m_activeProfileId = profileId;
    emit activeProfileChanged();
  }
  updateAllProfiles();
  updateActiveProfileIndex();

  if ( !success )
  {
    emit error( "Failed to activate profile: " + profileId );
  }
}

// Save / delete / create profiles

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

  if ( !m_connected || !m_client->saveCustomProfile( profileJSON.toStdString() ) )
  {
    emit error( "Failed to save profile via uccd" );
    return;
  }

  loadCustomProfilesFromSettings();
  updateAllProfiles();
  qDebug() << "Profile saved:" << profileName;
}

void ProfileManager::deleteProfile( const QString &profileId )
{
  if ( !m_connected || !m_client->deleteCustomProfile( profileId.toStdString() ) )
  {
    emit error( "Failed to delete profile via uccd: " + profileId );
    return;
  }

  loadCustomProfilesFromSettings();
  updateAllProfiles();
  qDebug() << "Profile deleted:" << profileId;
}

QString ProfileManager::createProfileFromDefault( const QString &name )
{
  if ( !m_connected )
  {
    emit error( "Not connected to uccd" );
    return QString();
  }

  auto defaultJson = m_client->getDefaultValuesProfileJSON();
  if ( !defaultJson )
  {
    emit error( "Failed to get default profile template" );
    return QString();
  }

  QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *defaultJson ).toUtf8() );
  if ( !doc.isObject() )
  {
    emit error( "Invalid default profile template" );
    return QString();
  }

  QJsonObject profileObj = doc.object();
  QString id = QUuid::createUuid().toString( QUuid::WithoutBraces );
  profileObj["name"] = name;
  profileObj["id"] = id;
  QString profileJSON = QJsonDocument( profileObj ).toJson( QJsonDocument::Compact );

  if ( !m_client->saveCustomProfile( profileJSON.toStdString() ) )
  {
    emit error( "Failed to save new profile via uccd" );
    return QString();
  }

  loadCustomProfilesFromSettings();
  updateAllProfiles();
  qDebug() << "Created new profile from default:" << name;
  return profileJSON;
}

// Profile details

QString ProfileManager::getProfileDetails( const QString &profileId )
{
  // Search in custom profiles first
  for ( const auto &profile : m_customProfilesData )
  {
    if ( profile.toObject()["id"].toString() == profileId )
      return QJsonDocument( profile.toObject() ).toJson( QJsonDocument::Compact );
  }
  // Then default profiles
  for ( const auto &profile : m_defaultProfilesData )
  {
    if ( profile.toObject()["id"].toString() == profileId )
      return QJsonDocument( profile.toObject() ).toJson( QJsonDocument::Compact );
  }
  return QString();
}

// Profile changed signal (from daemon)

void ProfileManager::onProfileChanged( const std::string &profileId,
                                       const std::string &keyboardProfileId,
                                       const std::string &fanProfileId )
{
  const QString qId = QString::fromStdString( profileId );

  if ( !qId.isEmpty() && m_activeProfileId != qId )
  {
    m_activeProfileId = qId;
    emit activeProfileChanged();
    qDebug() << "Active profile updated from signal:" << qId;
  }

  // Propagate keyboard / fan sub-profile changes regardless of whether the
  // top-level profile ID changed. Clients that only care about sub-profiles
  // (e.g. keyboard editor combo) listen to these dedicated signals.
  if ( const QString kbId = QString::fromStdString( keyboardProfileId );
       !kbId.isEmpty() && kbId != m_activeKeyboardProfileId )
  {
    m_activeKeyboardProfileId = kbId;
    emit activeKeyboardProfileChanged( kbId );
    qDebug() << "Active keyboard profile updated from signal:" << kbId;
  }

  if ( const QString fpId = QString::fromStdString( fanProfileId );
       !fpId.isEmpty() && fpId != m_activeFanProfileId )
  {
    m_activeFanProfileId = fpId;
    emit activeFanProfileChanged( fpId );
    qDebug() << "Active fan profile updated from signal:" << fpId;
  }

  updateProfiles();
}

void ProfileManager::onPowerStateChanged( const QString &state )
{
  qDebug() << "Power state changed:" << state;

  m_powerState = state;
  emit powerStateChanged();

  // Resolve the mapped profile ID for display purposes only.
  // The daemon is responsible for applying the correct profile.
  QString desiredProfileId = resolveStateMapToProfileId( state );
  if ( desiredProfileId.isEmpty() )
  {
    qDebug() << "No profile mapped for state:" << state;
    return;
  }

  if ( m_activeProfileId != desiredProfileId )
  {
    m_activeProfileId = desiredProfileId;
    emit activeProfileChanged();
    updateAllProfiles();
    updateActiveProfileIndex();
  }
}

// Profile list management

void ProfileManager::updateAllProfiles()
{
  QStringList newAllProfiles;
  QStringList newAllProfileIds;

  // Default profiles
  for ( const auto &p : m_defaultProfilesData )
  {
    if ( p.isObject() )
    {
      newAllProfiles.append( p.toObject()["name"].toString() );
      newAllProfileIds.append( p.toObject()["id"].toString() );
    }
  }
  // Custom profiles
  for ( const auto &p : m_customProfilesData )
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
  {
    setActiveProfile( m_allProfileIds.at( index ) );
  }
}

std::vector< int > ProfileManager::getHardwarePowerLimits()
{
  return m_hardwarePowerLimits;
}

bool ProfileManager::isCustomProfile( const QString &profileId ) const
{
  for ( const auto &p : m_customProfilesData )
  {
    if ( p.isObject() && p.toObject()["id"].toString() == profileId )
      return true;
  }
  return false;
}

// State map

QString ProfileManager::resolveStateMapToProfileId( const QString &state )
{
  if ( !m_stateMap.contains( state ) ) return QString();
  return m_stateMap[state].toString();
}

bool ProfileManager::setStateMap( const QString &state, const QString &profileId )
{
  if ( !m_client || !m_client->isConnected() ) return false;
  if ( !m_client->setStateMap( state.toStdString(), profileId.toStdString() ) )
    return false;
  m_stateMap[state] = profileId;
  return true;
}

bool ProfileManager::setBatchStateMap( const std::map< QString, QString > &entries )
{
  if ( !m_client || !m_client->isConnected() ) return false;

  std::map< std::string, std::string > stdEntries;
  for ( const auto &[state, profileId] : entries )
    stdEntries[state.toStdString()] = profileId.toStdString();

  if ( !m_client->setBatchStateMap( stdEntries ) )
    return false;

  for ( const auto &[state, profileId] : entries )
    m_stateMap[state] = profileId;
  return true;
}

// Custom profiles + stateMap (daemon-managed via D-Bus)

void ProfileManager::loadCustomProfilesFromSettings()
{
  m_customProfilesData = QJsonArray();
  m_customProfiles.clear();
  m_stateMap = QJsonObject();
  m_activeProfileId = "";

  if ( !m_client || !m_client->isConnected() ) return;

  if ( auto json = m_client->getCustomProfilesJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isArray() )
    {
      m_customProfilesData = doc.array();
      for ( const auto &value : m_customProfilesData )
      {
        if ( !value.isObject() ) continue;
        QString name = value.toObject().value( "name" ).toString();
        if ( !name.isEmpty() )
          m_customProfiles.append( name );
      }
    }
  }

  // stateMap lives inside the settings object returned by GetSettingsJSON
  if ( auto settingsJson = m_client->getSettingsJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *settingsJson ) );
    if ( doc.isObject() )
    {
      QJsonValue sm = doc.object().value( "stateMap" );
      if ( sm.isObject() )
        m_stateMap = sm.toObject();
    }
  }
}

void ProfileManager::saveCustomProfilesToSettings()
{
  // No-op: profiles and stateMap are managed by uccd via D-Bus.
}

// Built-in fan profiles (from daemon)

void ProfileManager::loadBuiltinFanProfiles()
{
  m_builtinFanProfilesData = QJsonArray();
  m_builtinFanProfiles.clear();

  if ( auto json = m_client->getFanProfilesJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *json ).toUtf8() );
    if ( doc.isArray() )
    {
      m_builtinFanProfilesData = doc.array();
      for ( const auto &val : m_builtinFanProfilesData )
      {
        if ( val.isObject() )
        {
          QString name = val.toObject().value( "name" ).toString();
          if ( !name.isEmpty() )
            m_builtinFanProfiles.append( name );
        }
      }
    }
  }
}

// Custom fan profiles (daemon-managed via D-Bus)

void ProfileManager::loadCustomFanProfilesFromSettings()
{
  m_customFanProfilesData = QJsonArray();
  m_customFanProfiles.clear();

  if ( !m_client || !m_client->isConnected() )
    return;

  auto jsonOpt = m_client->getCustomFanProfiles();
  if ( !jsonOpt ) return;

  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *jsonOpt ) );
  if ( !doc.isArray() ) return;

  m_customFanProfilesData = doc.array();
  for ( const auto &val : m_customFanProfilesData )
  {
    if ( !val.isObject() ) continue;
    QString name = val.toObject().value( "name" ).toString();
    if ( !name.isEmpty() )
      m_customFanProfiles.append( name );
  }
}

void ProfileManager::saveCustomFanProfilesToSettings()
{
  // No-op: fan profiles are managed by uccd via D-Bus.
}

QString ProfileManager::getFanProfile( const QString &fanProfileId )
{
  // Check custom fan profiles first (by ID)
  for ( const auto &v : m_customFanProfilesData )
  {
    if ( !v.isObject() ) continue;

    QJsonObject o = v.toObject();
    if ( o.value( "id" ).toString() != fanProfileId ) continue;

    QJsonValue jv = o.value( "json" );
    if ( jv.isObject() )
      return QJsonDocument( jv.toObject() ).toJson( QJsonDocument::Compact );
    if ( jv.isString() )
    {
      QString s = jv.toString();
      if ( !s.trimmed().isEmpty() ) return s;
    }
  }

  // Fall back to daemon-provided built-in profiles
  if ( auto json = m_client->getFanProfile( fanProfileId.toStdString() ) )
    return QString::fromStdString( *json );

  return "{}";
}

bool ProfileManager::setFanProfile( const QString &fanProfileId, const QString &name, const QString &json )
{
  if ( !m_client || !m_client->isConnected() ) return false;

  if ( !m_client->saveCustomFanProfile( fanProfileId.toStdString(), name.toStdString(), json.toStdString() ) )
    return false;

  loadCustomFanProfilesFromSettings();
  emit customFanProfilesChanged();
  return true;
}

bool ProfileManager::deleteFanProfile( const QString &fanProfileId )
{
  if ( !m_client || !m_client->isConnected() ) return false;

  if ( !m_client->deleteCustomFanProfile( fanProfileId.toStdString() ) )
    return false;

  loadCustomFanProfilesFromSettings();
  emit customFanProfilesChanged();
  return true;
}

bool ProfileManager::renameFanProfile( const QString &fanProfileId, const QString &newName )
{
  if ( newName.isEmpty() || !m_client || !m_client->isConnected() ) return false;

  // Fetch existing JSON, then re-save with new name.
  QString existingJson;
  for ( const auto &v : m_customFanProfilesData )
  {
    if ( !v.isObject() ) continue;
    QJsonObject o = v.toObject();
    if ( o.value( "id" ).toString() != fanProfileId ) continue;

    QJsonValue jv = o.value( "json" );
    if ( jv.isObject() )
      existingJson = QJsonDocument( jv.toObject() ).toJson( QJsonDocument::Compact );
    else if ( jv.isString() )
      existingJson = jv.toString();
    break;
  }
  if ( existingJson.isEmpty() ) return false;

  if ( !m_client->saveCustomFanProfile( fanProfileId.toStdString(), newName.toStdString(), existingJson.toStdString() ) )
    return false;

  loadCustomFanProfilesFromSettings();
  emit customFanProfilesChanged();
  return true;
}

// Custom keyboard profiles (local storage, by ID)

void ProfileManager::loadCustomKeyboardProfilesFromSettings()
{
  m_customKeyboardProfilesData = QJsonArray();
  m_customKeyboardProfiles.clear();

  if ( !m_client || !m_client->isConnected() )
  {
    emit customKeyboardProfilesChanged();
    return;
  }

  if ( auto jsonOpt = m_client->getCustomKeyboardProfiles() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *jsonOpt ) );
    if ( doc.isArray() )
    {
      m_customKeyboardProfilesData = doc.array();
      for ( const auto &val : m_customKeyboardProfilesData )
      {
        if ( val.isObject() )
        {
          QString name = val.toObject().value( "name" ).toString();
          if ( !name.isEmpty() )
            m_customKeyboardProfiles.append( name );
        }
      }
    }
  }
}

void ProfileManager::saveCustomKeyboardProfilesToSettings()
{
  // Now managed directly via D-Bus per-profile additions/deletions.
}

QString ProfileManager::getKeyboardProfile( const QString &keyboardProfileId )
{
  for ( const auto &v : m_customKeyboardProfilesData )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      if ( o.value( "id" ).toString() == keyboardProfileId )
      {
        QJsonValue jsonVal = o.value( "json" );
        if ( jsonVal.isString() )
        {
          QString jsonStr = jsonVal.toString().trimmed();
          if ( !jsonStr.isEmpty() )
            return jsonStr;
        }
        else if ( jsonVal.isObject() )
        {
          // json field is a nested object - serialize back to compact string
          return QString::fromUtf8( QJsonDocument( jsonVal.toObject() ).toJson( QJsonDocument::Compact ) );
        }

        qWarning() << "[ProfileManager] Keyboard profile" << keyboardProfileId << "has empty JSON";
      }
    }
  }
  return "{}";
}

bool ProfileManager::setKeyboardProfile( const QString &keyboardProfileId, const QString &name, const QString &json )
{
  if ( !m_client || !m_client->isConnected() ) return false;

  bool success = m_client->saveCustomKeyboardProfile( keyboardProfileId.toStdString(), name.toStdString(), json.toStdString() );
  if ( success )
  {
    loadCustomKeyboardProfilesFromSettings();
    emit customKeyboardProfilesChanged();
  }
  return success;
}

bool ProfileManager::deleteKeyboardProfile( const QString &keyboardProfileId )
{
  if ( !m_client || !m_client->isConnected() ) return false;

  bool success = m_client->deleteCustomKeyboardProfile( keyboardProfileId.toStdString() );
  if ( success )
  {
    loadCustomKeyboardProfilesFromSettings();
    emit customKeyboardProfilesChanged();
  }
  return success;
}

bool ProfileManager::renameKeyboardProfile( const QString &keyboardProfileId, const QString &newName )
{
  if ( newName.isEmpty() || !m_client || !m_client->isConnected() ) return false;

  // Retrieve current json directly from our array before saving
  QString existingJson = getKeyboardProfile( keyboardProfileId );
  if ( existingJson.isEmpty() || existingJson == "{}" ) return false;
  
  bool success = m_client->saveCustomKeyboardProfile( keyboardProfileId.toStdString(), newName.toStdString(), existingJson.toStdString() );
  if ( success )
  {
    loadCustomKeyboardProfilesFromSettings();
    emit customKeyboardProfilesChanged();
  }
  return success;
}

// Settings JSON

QString ProfileManager::getSettingsJSON()
{
  try {
    if ( auto json = m_client->getSettingsJSON() )
      return QString::fromStdString( *json );
  } catch ( const std::exception &e ) {
    qWarning() << "Failed to get settings JSON:" << e.what();
  }
  return "{}";
}

} // namespace ucc
