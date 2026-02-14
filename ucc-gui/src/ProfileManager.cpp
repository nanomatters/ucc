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
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
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
  , m_settings( std::make_unique< QSettings >( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat, this ) )
{
  m_connected = m_client->isConnected();

  // Load local custom fan profiles regardless of DBus connection
  loadCustomFanProfilesFromSettings();

  // Load local custom keyboard profiles
  loadCustomKeyboardProfilesFromSettings();

  if ( m_connected )
  {
    // Fetch hardware power limits immediately
    m_hardwarePowerLimits = m_client->getODMPowerLimits().value_or( std::vector< int >() );

    // Fetch built-in fan profiles from daemon (id + name)
    loadBuiltinFanProfiles();

    // Connect to profile changed signal
    connect( m_client.get(), &UccdClient::profileChanged,
             this, [this]( const QString &profileId ) {
      onProfileChanged( profileId.toStdString() );
    } );

    // Connect to power state changed signal
    connect( m_client.get(), &UccdClient::powerStateChanged,
             this, [this]( const QString &state ) {
      onPowerStateChanged( state );
    } );

    // Load custom profiles from local storage
    loadCustomProfilesFromSettings();
  }
  emit connectedChanged();
}

// ---------------------------------------------------------------------------
// Refresh / update
// ---------------------------------------------------------------------------

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
  // SetStateMap, the stateMap reflects that — whereas the daemon's
  // GetActiveProfileJSON only reports the *running* profile which may differ.
  if ( m_activeProfileId.isEmpty() && !m_powerState.isEmpty() )
  {
    QString mapped = resolveStateMapToProfileId( m_powerState );
    if ( !mapped.isEmpty() )
    {
      m_activeProfileId = mapped;
      emit activeProfileChanged();
    }
  }

  // Fallback: ask daemon for currently running profile (e.g. fresh install, no stateMap yet)
  if ( m_activeProfileId.isEmpty() )
  {
    try {
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

// ---------------------------------------------------------------------------
// Set active profile by ID
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Save / delete / create profiles
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

  int foundIndex = -1;
  QString oldName;
  for ( int i = 0; i < m_customProfilesData.size(); ++i )
  {
    QJsonObject existingProfile = m_customProfilesData[i].toObject();
    if ( existingProfile.value( "id" ).toString() == profileId )
    {
      foundIndex = i;
      oldName = existingProfile.value( "name" ).toString();
      break;
    }
  }

  if ( foundIndex == -1 )
  {
    m_customProfilesData.append( profileObj );
    m_customProfiles.append( profileName );
  }
  else
  {
    m_customProfilesData[foundIndex] = profileObj;
    if ( !oldName.isEmpty() && oldName != profileName )
    {
      int nameIndex = m_customProfiles.indexOf( oldName );
      if ( nameIndex != -1 )
        m_customProfiles.replace( nameIndex, profileName );
      else if ( !m_customProfiles.contains( profileName ) )
        m_customProfiles.append( profileName );
    }
  }

  saveCustomProfilesToSettings();

  if ( m_connected )
  {
    bool success = m_client->saveCustomProfile( profileJSON.toStdString() );
    if ( !success )
      qWarning() << "Failed to save profile to daemon:" << profileName;
    else
      qDebug() << "Profile saved to daemon:" << profileName;
  }

  updateAllProfiles();
  qDebug() << "Profile saved locally:" << profileName;
}

void ProfileManager::deleteProfile( const QString &profileId )
{
  for ( int i = 0; i < m_customProfilesData.size(); ++i )
  {
    QJsonObject profileObj = m_customProfilesData[i].toObject();
    if ( profileObj.value( "id" ).toString() == profileId )
    {
      QString profileName = profileObj.value( "name" ).toString();
      m_customProfilesData.removeAt( i );
      m_customProfiles.removeAll( profileName );
      saveCustomProfilesToSettings();
      updateAllProfiles();
      qDebug() << "Profile deleted locally:" << profileName;
      return;
    }
  }
  emit error( "Profile not found: " + profileId );
}

QString ProfileManager::createProfileFromDefault( const QString &name )
{
  if ( auto defaultJson = m_client->getDefaultValuesProfileJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *defaultJson ).toUtf8() );
    if ( doc.isObject() )
    {
      QJsonObject profileObj = doc.object();
      QString id = QUuid::createUuid().toString( QUuid::WithoutBraces );
      profileObj["name"] = name;
      profileObj["id"] = id;

      m_customProfilesData.append( profileObj );
      m_customProfiles.append( name );
      saveCustomProfilesToSettings();
      updateAllProfiles();

      qDebug() << "Created new profile from default:" << name;
      return QJsonDocument( profileObj ).toJson( QJsonDocument::Compact );
    }
  }

  emit error( "Failed to get default profile template" );
  return QString();
}

// ---------------------------------------------------------------------------
// Profile details
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Profile changed signal (from daemon)
// ---------------------------------------------------------------------------

void ProfileManager::onProfileChanged( const std::string &profileId )
{
  const QString qId = QString::fromStdString( profileId );

  if ( !qId.isEmpty() && m_activeProfileId != qId )
  {
    m_activeProfileId = qId;
    emit activeProfileChanged();
    qDebug() << "Active profile updated from signal:" << qId;
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

// ---------------------------------------------------------------------------
// Profile list management
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// State map
// ---------------------------------------------------------------------------

QString ProfileManager::resolveStateMapToProfileId( const QString &state )
{
  if ( !m_stateMap.contains( state ) ) return QString();
  return m_stateMap[state].toString();
}

bool ProfileManager::setStateMap( const QString &state, const QString &profileId )
{
  m_stateMap[state] = profileId;
  saveCustomProfilesToSettings();
  return m_client->setStateMap( state.toStdString(), profileId.toStdString() );
}

bool ProfileManager::setBatchStateMap( const std::map< QString, QString > &entries )
{
  // Update local stateMap first
  for ( const auto &[state, profileId] : entries )
    m_stateMap[state] = profileId;
  saveCustomProfilesToSettings();

  // Convert to std::string map for D-Bus client
  std::map< std::string, std::string > stdEntries;
  for ( const auto &[state, profileId] : entries )
    stdEntries[state.toStdString()] = profileId.toStdString();
  return m_client->setBatchStateMap( stdEntries );
}

// ---------------------------------------------------------------------------
// Local settings persistence (profiles + stateMap)
// ---------------------------------------------------------------------------

void ProfileManager::loadCustomProfilesFromSettings()
{
  m_customProfilesData = QJsonArray();
  m_customProfiles.clear();

  QString profilesJson = m_settings->value( "customProfiles", "{}" ).toString();
  QJsonDocument doc = QJsonDocument::fromJson( profilesJson.toUtf8() );

  if ( doc.isArray() )
  {
    m_customProfilesData = doc.array();
    for ( const QJsonValue &value : m_customProfilesData )
    {
      if ( value.isObject() )
      {
        QString name = value.toObject().value( "name" ).toString();
        if ( !name.isEmpty() )
          m_customProfiles.append( name );
      }
    }
  }

  m_activeProfileId = "";

  // Load stateMap
  QString stateMapJson = m_settings->value( "stateMap", "{}" ).toString();
  QJsonDocument stateMapDoc = QJsonDocument::fromJson( stateMapJson.toUtf8() );
  if ( stateMapDoc.isObject() )
    m_stateMap = stateMapDoc.object();
  else
  {
    qWarning() << "Failed to parse stateMap JSON, using empty map";
    m_stateMap = QJsonObject();
  }
}

void ProfileManager::saveCustomProfilesToSettings()
{
  QJsonDocument doc( m_customProfilesData );
  m_settings->setValue( "customProfiles", doc.toJson( QJsonDocument::Compact ) );

  QJsonDocument stateMapDoc( m_stateMap );
  m_settings->setValue( "stateMap", stateMapDoc.toJson( QJsonDocument::Compact ) );

  m_settings->sync();
}

// ---------------------------------------------------------------------------
// Built-in fan profiles (from daemon)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Custom fan profiles (local storage, by ID)
// ---------------------------------------------------------------------------

void ProfileManager::migrateFanProfileIds( QJsonArray &arr )
{
  // Ensure every entry has an "id" field; generate UUIDs for legacy entries
  for ( int i = 0; i < arr.size(); ++i )
  {
    if ( arr[i].isObject() )
    {
      QJsonObject o = arr[i].toObject();
      if ( o.value( "id" ).toString().isEmpty() )
      {
        o["id"] = QUuid::createUuid().toString( QUuid::WithoutBraces );
        arr[i] = o;
      }
    }
  }
}

void ProfileManager::loadCustomFanProfilesFromSettings()
{
  m_customFanProfilesData = QJsonArray();
  m_customFanProfiles.clear();

  QString fanJson = m_settings->value( "customFanProfiles", "[]" ).toString();
  QJsonDocument doc = QJsonDocument::fromJson( fanJson.toUtf8() );

  if ( doc.isArray() )
  {
    m_customFanProfilesData = doc.array();
    migrateFanProfileIds( m_customFanProfilesData );
    for ( const auto &val : m_customFanProfilesData )
    {
      if ( val.isObject() )
      {
        QString name = val.toObject().value( "name" ).toString();
        if ( !name.isEmpty() )
          m_customFanProfiles.append( name );
      }
    }
    // Persist any migration changes (new IDs)
    saveCustomFanProfilesToSettings();
  }
}

void ProfileManager::saveCustomFanProfilesToSettings()
{
  QJsonDocument doc( m_customFanProfilesData );
  m_settings->setValue( "customFanProfiles", doc.toJson( QJsonDocument::Compact ) );
  m_settings->sync();
}

QString ProfileManager::getFanProfile( const QString &fanProfileId )
{
  // Check custom fan profiles first (by ID)
  for ( const auto &v : m_customFanProfilesData )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      if ( o.value( "id" ).toString() == fanProfileId )
      {
        QString jsonStr = o.value( "json" ).toString();
        if ( !jsonStr.trimmed().isEmpty() )
          return jsonStr;

        qWarning() << "[ProfileManager] Custom fan profile" << fanProfileId << "has empty JSON, falling back to built-in";
      }
    }
  }

  // Fall back to daemon-provided built-in profiles
  if ( auto json = m_client->getFanProfile( fanProfileId.toStdString() ) )
    return QString::fromStdString( *json );

  return "{}";
}

bool ProfileManager::setFanProfile( const QString &fanProfileId, const QString &name, const QString &json )
{
  // Update existing entry or append new
  bool found = false;
  for ( int i = 0; i < m_customFanProfilesData.size(); ++i )
  {
    if ( m_customFanProfilesData[i].isObject() )
    {
      QJsonObject o = m_customFanProfilesData[i].toObject();
      if ( o.value( "id" ).toString() == fanProfileId )
      {
        o["name"] = name;
        o["json"] = json;
        m_customFanProfilesData[i] = o;
        found = true;
        break;
      }
    }
  }

  if ( !found )
  {
    QJsonObject o;
    o["id"] = fanProfileId;
    o["name"] = name;
    o["json"] = json;
    m_customFanProfilesData.append( o );
    m_customFanProfiles.append( name );
  }

  saveCustomFanProfilesToSettings();
  emit customFanProfilesChanged();
  return true;
}

bool ProfileManager::deleteFanProfile( const QString &fanProfileId )
{
  bool removed = false;
  QJsonArray newArr;
  for ( const auto &v : m_customFanProfilesData )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      if ( o.value( "id" ).toString() == fanProfileId )
      {
        m_customFanProfiles.removeAll( o.value( "name" ).toString() );
        removed = true;
        continue;
      }
    }
    newArr.append( v );
  }

  if ( removed )
  {
    m_customFanProfilesData = newArr;
    saveCustomFanProfilesToSettings();
    emit customFanProfilesChanged();
  }
  return removed;
}

bool ProfileManager::renameFanProfile( const QString &fanProfileId, const QString &newName )
{
  if ( newName.isEmpty() ) return false;

  for ( int i = 0; i < m_customFanProfilesData.size(); ++i )
  {
    if ( m_customFanProfilesData[i].isObject() )
    {
      QJsonObject o = m_customFanProfilesData[i].toObject();
      if ( o.value( "id" ).toString() == fanProfileId )
      {
        QString oldName = o.value( "name" ).toString();
        o["name"] = newName;
        m_customFanProfilesData[i] = o;

        int nameIdx = m_customFanProfiles.indexOf( oldName );
        if ( nameIdx != -1 )
          m_customFanProfiles.replace( nameIdx, newName );

        saveCustomFanProfilesToSettings();
        emit customFanProfilesChanged();
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Custom keyboard profiles (local storage, by ID)
// ---------------------------------------------------------------------------

void ProfileManager::migrateKeyboardProfileIds( QJsonArray &arr )
{
  for ( int i = 0; i < arr.size(); ++i )
  {
    if ( arr[i].isObject() )
    {
      QJsonObject o = arr[i].toObject();
      if ( o.value( "id" ).toString().isEmpty() )
      {
        o["id"] = QUuid::createUuid().toString( QUuid::WithoutBraces );
        arr[i] = o;
      }
    }
  }
}

void ProfileManager::loadCustomKeyboardProfilesFromSettings()
{
  m_customKeyboardProfilesData = QJsonArray();
  m_customKeyboardProfiles.clear();

  QString keyboardJson = m_settings->value( "customKeyboardProfiles", "[]" ).toString();
  QJsonDocument doc = QJsonDocument::fromJson( keyboardJson.toUtf8() );

  if ( doc.isArray() )
  {
    m_customKeyboardProfilesData = doc.array();
    migrateKeyboardProfileIds( m_customKeyboardProfilesData );
    for ( const auto &val : m_customKeyboardProfilesData )
    {
      if ( val.isObject() )
      {
        QString name = val.toObject().value( "name" ).toString();
        if ( !name.isEmpty() )
          m_customKeyboardProfiles.append( name );
      }
    }
    // Persist any migration changes (new IDs)
    saveCustomKeyboardProfilesToSettings();
  }
}

void ProfileManager::saveCustomKeyboardProfilesToSettings()
{
  QJsonDocument doc( m_customKeyboardProfilesData );
  m_settings->setValue( "customKeyboardProfiles", doc.toJson( QJsonDocument::Compact ) );
  m_settings->sync();
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
        QString jsonStr = o.value( "json" ).toString();
        if ( !jsonStr.trimmed().isEmpty() )
          return jsonStr;

        qWarning() << "[ProfileManager] Keyboard profile" << keyboardProfileId << "has empty JSON";
      }
    }
  }
  return "{}";
}

bool ProfileManager::setKeyboardProfile( const QString &keyboardProfileId, const QString &name, const QString &json )
{
  bool found = false;
  for ( int i = 0; i < m_customKeyboardProfilesData.size(); ++i )
  {
    if ( m_customKeyboardProfilesData[i].isObject() )
    {
      QJsonObject o = m_customKeyboardProfilesData[i].toObject();
      if ( o.value( "id" ).toString() == keyboardProfileId )
      {
        o["name"] = name;
        o["json"] = json;
        m_customKeyboardProfilesData[i] = o;
        found = true;
        break;
      }
    }
  }

  if ( !found )
  {
    QJsonObject o;
    o["id"] = keyboardProfileId;
    o["name"] = name;
    o["json"] = json;
    m_customKeyboardProfilesData.append( o );
    m_customKeyboardProfiles.append( name );
  }

  saveCustomKeyboardProfilesToSettings();
  emit customKeyboardProfilesChanged();
  return true;
}

bool ProfileManager::deleteKeyboardProfile( const QString &keyboardProfileId )
{
  bool removed = false;
  QJsonArray newArr;
  for ( const auto &v : m_customKeyboardProfilesData )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      if ( o.value( "id" ).toString() == keyboardProfileId )
      {
        m_customKeyboardProfiles.removeAll( o.value( "name" ).toString() );
        removed = true;
        continue;
      }
    }
    newArr.append( v );
  }

  if ( removed )
  {
    m_customKeyboardProfilesData = newArr;
    saveCustomKeyboardProfilesToSettings();
    emit customKeyboardProfilesChanged();
  }
  return removed;
}

bool ProfileManager::renameKeyboardProfile( const QString &keyboardProfileId, const QString &newName )
{
  if ( newName.isEmpty() ) return false;

  for ( int i = 0; i < m_customKeyboardProfilesData.size(); ++i )
  {
    if ( m_customKeyboardProfilesData[i].isObject() )
    {
      QJsonObject o = m_customKeyboardProfilesData[i].toObject();
      if ( o.value( "id" ).toString() == keyboardProfileId )
      {
        QString oldName = o.value( "name" ).toString();
        o["name"] = newName;
        m_customKeyboardProfilesData[i] = o;

        int nameIdx = m_customKeyboardProfiles.indexOf( oldName );
        if ( nameIdx != -1 )
          m_customKeyboardProfiles.replace( nameIdx, newName );

        saveCustomKeyboardProfilesToSettings();
        emit customKeyboardProfilesChanged();
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Settings JSON
// ---------------------------------------------------------------------------

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
