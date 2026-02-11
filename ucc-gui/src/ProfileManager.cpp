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
#include <QDir>

namespace ucc
{

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
    
    // No need to send profiles to uccd; UCC handles profile application
    
    // No need to set stateMap in uccd; UCC handles power state changes
    
    // DO NOT load profiles here - defer to after signals are connected
  }
  emit connectedChanged();


}

void ProfileManager::refresh()
{
  updateProfiles();
}


void ProfileManager::updateProfiles()
{
  
  // Fetch default profiles if not already loaded
  if (m_defaultProfilesData.isEmpty()) {
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
    } catch (const std::exception &e) {
      qWarning() << "Failed to get default profiles:" << e.what();
    }
  }
  
  // Default profiles are cached
  emit defaultProfilesChanged();

  // Custom profiles are loaded from local storage, no need to fetch from server
  emit customProfilesChanged();

  // Ensure combined list is up-to-date before deciding which profile to activate
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

  // Query daemon for the currently active profile so the GUI shows the
  // correct selection on startup (we may have missed the ProfileChanged
  // signal that was emitted before the GUI connected).
  if ( m_activeProfile.isEmpty() )
  {
    try {
      if ( auto json = m_client->getActiveProfileJSON() )
      {
        QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *json ).toUtf8() );
        if ( doc.isObject() )
        {
          QJsonObject obj = doc.object();
          QString name = obj["name"].toString();
          QString id = obj["id"].toString();
          if ( !name.isEmpty() )
          {
            m_activeProfile = name;
            emit activeProfileChanged();
          }
        }
      }
    } catch ( const std::exception &e ) {
      qWarning() << "Failed to get active profile:" << e.what();
    }
  }

  // Update combined list and index
  updateAllProfiles();
  updateActiveProfileIndex();

}


void ProfileManager::setActiveProfile( const QString &profileId )
{
  // profileId might actually be a profile name (from QML modelData)
  // Try to find the actual ID from the profile name
  QString actualId = profileId;
  
  // Search in default profiles
  for ( const auto &profile : m_defaultProfilesData )
  {

    if ( profile.isObject() )
    {
      QJsonObject obj = profile.toObject();

      if ( obj["name"].toString() == profileId )
      {
        actualId = obj["id"].toString();
        break;
      }
    }
  }
  
  // Search in custom profiles if not found

  if ( actualId == profileId )
  {
    for ( const auto &profile : m_customProfilesData )
    {

      if ( profile.isObject() )
      {
        QJsonObject obj = profile.toObject();

        if ( obj["name"].toString() == profileId )
        {
          actualId = obj["id"].toString();
          break;
        }
      }
    }
  }
  
  // Check if this is a custom profile
  bool isCustom = false;
  QString profileData;
  for (const auto &profile : m_customProfilesData) {
    QJsonObject obj = profile.toObject();
    if (obj.value("id").toString() == actualId) {
      isCustom = true;
      profileData = QJsonDocument(obj).toJson(QJsonDocument::Compact);
      break;
    }
  }
  
  bool success = false;
  if (isCustom && !profileData.isEmpty()) {
    // Send full profile data for custom profiles
    try {
      success = m_client->applyProfile(profileData.toStdString());
    } catch (const std::exception &e) {
      qWarning() << "Failed to apply custom profile:" << e.what();
    }
    qDebug() << "Custom profile applied:" << profileId << "(ID:" << actualId << ")";
  } else {
    // Use ID for default profiles
    try {
      success = m_client->setActiveProfile(actualId.toStdString());
    } catch (const std::exception &e) {
      qWarning() << "Failed to set active profile:" << e.what();
    }
    qDebug() << "Default profile activated:" << profileId << "(ID:" << actualId << ")";
  }
  
  // Always update local state and emit signals, regardless of DBus success
  // The UI should reflect the selected profile even if application to system fails
  if (m_activeProfile != profileId) {
    m_activeProfile = profileId;
    emit activeProfileChanged();
  }
  updateAllProfiles();
  updateActiveProfileIndex();
  
  if (!success) {
    emit error("Failed to activate profile on system: " + profileId);
  }
}

void ProfileManager::saveProfile( const QString &profileJSON )
{
  QJsonDocument doc = QJsonDocument::fromJson(profileJSON.toUtf8());
  if (!doc.isObject()) {
    emit error("Invalid profile JSON");
    return;
  }
  
  QJsonObject profileObj = doc.object();
  QString profileId = profileObj.value("id").toString();
  QString profileName = profileObj.value("name").toString();
  
  if (profileId.isEmpty() || profileName.isEmpty()) {
    emit error("Profile missing id or name");
    return;
  }
  
  // Check if profile already exists and remember old name
  int foundIndex = -1;
  QString oldName;
  for (int i = 0; i < m_customProfilesData.size(); ++i) {
    QJsonObject existingProfile = m_customProfilesData[i].toObject();
    if (existingProfile.value("id").toString() == profileId) {
      // remember index and old name before replacing
      foundIndex = i;
      oldName = existingProfile.value("name").toString();
      break;
    }
  }
  
  if (foundIndex == -1) {
    // Add new profile
    m_customProfilesData.append(profileObj);
    m_customProfiles.append(profileName);
  } else {
    // Update existing profile object
    m_customProfilesData[foundIndex] = profileObj;

    // If the name changed, update the names list so UI widgets refresh
    if (!oldName.isEmpty() && oldName != profileName) {
      int nameIndex = m_customProfiles.indexOf(oldName);
      if (nameIndex != -1) {
        m_customProfiles.replace(nameIndex, profileName);
      } else {
        // Fallback: ensure the new name is present
        if (!m_customProfiles.contains(profileName))
          m_customProfiles.append(profileName);
      }
    }
  }
  
  // Save to local storage
  saveCustomProfilesToSettings();
  
  // Send to daemon for persistence
  if (m_connected) {
    bool success = m_client->saveCustomProfile(profileJSON.toStdString());
    if (!success) {
      qWarning() << "Failed to save profile to daemon:" << profileName;
    } else {
      qDebug() << "Profile saved to daemon:" << profileName;
    }
  } else {
    qWarning() << "Not connected to daemon, profile not persisted:" << profileName;
  }
  
  // Update the UI
  updateAllProfiles();
  
  qDebug() << "Profile saved locally:" << profileName;
}

void ProfileManager::deleteProfile( const QString &profileId )
{
  // Find and remove the profile
  for (int i = 0; i < m_customProfilesData.size(); ++i) {
    QJsonObject profileObj = m_customProfilesData[i].toObject();
    if (profileObj.value("id").toString() == profileId) {
      QString profileName = profileObj.value("name").toString();
      m_customProfilesData.removeAt(i);
      m_customProfiles.removeAll(profileName);
      
      // Save to local storage
      saveCustomProfilesToSettings();
      
      // Update the UI
      updateAllProfiles();
      
      qDebug() << "Profile deleted locally:" << profileName;
      return;
    }
  }
  
  emit error("Profile not found: " + profileId);
}

QString ProfileManager::createProfileFromDefault( const QString &name )
{
  // Get default profile template from server
  if (auto defaultJson = m_client->getDefaultValuesProfileJSON()) {
    QJsonDocument doc = QJsonDocument::fromJson(QString::fromStdString(*defaultJson).toUtf8());
    if (doc.isObject()) {
      QJsonObject profileObj = doc.object();
      
      // Generate a unique ID
      QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
      
      // Set the name and ID
      profileObj["name"] = name;
      profileObj["id"] = id;
      
      // Add to custom profiles
      m_customProfilesData.append(profileObj);
      m_customProfiles.append(name);
      
      // Save to local storage
      saveCustomProfilesToSettings();
      
      // Update the UI
      updateAllProfiles();
      
      qDebug() << "Created new profile from default:" << name;
      
      // Return the profile JSON
      return QJsonDocument(profileObj).toJson(QJsonDocument::Compact);
    }
  }
  
  emit error("Failed to get default profile template");
  return QString();
}

QString ProfileManager::getProfileDetails( const QString &profileId )
{
  // Search in default profiles
  for ( const auto &profile : m_defaultProfilesData )
  {
    if ( profile.toObject()["id"].toString() == profileId )
    {
      return QJsonDocument( profile.toObject() ).toJson( QJsonDocument::Compact );
    }
  }

  // Search in custom profiles
  for ( const auto &profile : m_customProfilesData )
  {
    if ( profile.toObject()["id"].toString() == profileId )
    {
      return QJsonDocument( profile.toObject() ).toJson( QJsonDocument::Compact );
    }
  }

  return QString();
}

QString ProfileManager::getProfileIdByName( const QString &profileName )
{
  // Search in default profiles
  for ( const auto &profile : m_defaultProfilesData )
  {
    if ( profile.toObject()["name"].toString() == profileName )
    {
      return profile.toObject()["id"].toString();
    }
  }

  // Search in custom profiles
  for ( const auto &profile : m_customProfilesData )
  {
    if ( profile.toObject()["name"].toString() == profileName )
    {
      return profile.toObject()["id"].toString();
    }
  }

  return QString();
}

void ProfileManager::onProfileChanged( const std::string &profileId )
{
  qDebug() << "Profile changed signal:" << QString::fromStdString( profileId );

  // Resolve the profile ID to a name so we can update m_activeProfile.
  // The daemon sends the profile ID in the signal, but the GUI tracks by name.
  QString resolvedName;
  const QString qId = QString::fromStdString( profileId );

  // Search default profiles
  for ( const auto &p : m_defaultProfilesData )
  {
    if ( p.isObject() && p.toObject()["id"].toString() == qId )
    {
      resolvedName = p.toObject()["name"].toString();
      break;
    }
  }

  // Search custom profiles if not found
  if ( resolvedName.isEmpty() )
  {
    for ( const auto &p : m_customProfilesData )
    {
      if ( p.isObject() && p.toObject()["id"].toString() == qId )
      {
        resolvedName = p.toObject()["name"].toString();
        break;
      }
    }
  }

  // If the ID itself matches a known profile name (e.g. custom profile applied via JSON),
  // use it directly.
  if ( resolvedName.isEmpty() && ( m_allProfiles.contains( qId ) ) )
  {
    resolvedName = qId;
  }

  if ( !resolvedName.isEmpty() && m_activeProfile != resolvedName )
  {
    m_activeProfile = resolvedName;
    emit activeProfileChanged();
    qDebug() << "Active profile updated from signal:" << m_activeProfile;
  }

  // Refresh profiles list in case new profiles were added
  updateProfiles();
}

void ProfileManager::onPowerStateChanged( const QString &state )
{
  qDebug() << "Power state changed:" << state;

  // Update internal power state and notify GUI
  m_powerState = state;
  emit powerStateChanged();

  // Resolve the mapped profile name for display purposes only.
  // The daemon (uccd) is responsible for applying the correct profile
  // when the power state changes — the GUI should not override that.
  QString desiredProfile = resolveStateMapToProfileName( state );
  if ( desiredProfile.isEmpty() )
  {
    qDebug() << "No profile mapped for state:" << state;
    return;
  }

  // Update active profile display if it changed
  if ( m_activeProfile != desiredProfile ) {
    m_activeProfile = desiredProfile;
    emit activeProfileChanged();
    updateAllProfiles();
    updateActiveProfileIndex();
  }
}

void ProfileManager::updateAllProfiles()
{
  
  QStringList newAllProfiles;
  newAllProfiles.append( m_defaultProfiles );
  newAllProfiles.append( m_customProfiles );
  if ( m_allProfiles != newAllProfiles )
  {
    m_allProfiles = newAllProfiles;
    emit allProfilesChanged();
  }
}

void ProfileManager::updateActiveProfileIndex()
{
  
  int newIndex = m_allProfiles.indexOf( m_activeProfile );

  if ( m_activeProfileIndex != newIndex )
  {
    m_activeProfileIndex = newIndex;
    emit activeProfileIndexChanged();
  }
}

void ProfileManager::setActiveProfileByIndex( int index )
{
  if ( index >= 0 && index < m_allProfiles.size() )
  {
    setActiveProfile( m_allProfiles.at( index ) );
  }
}

std::vector< int > ProfileManager::getHardwarePowerLimits()
{
  // Return cached hardware limits
  qDebug() << "ProfileManager::getHardwarePowerLimits() returning:" << m_hardwarePowerLimits.size() << "values";
  for ( size_t i = 0; i < m_hardwarePowerLimits.size(); ++i )
  {
    qDebug() << "  Limit" << (int)i << "=" << m_hardwarePowerLimits[i];
  }
  return m_hardwarePowerLimits;
}

bool ProfileManager::isCustomProfile( const QString &profileName ) const
{
  // A profile is custom if it's in the custom profiles list
  return m_customProfiles.contains( profileName );
}

void ProfileManager::loadCustomProfilesFromSettings()
{
  m_customProfilesData = QJsonArray();
  m_customProfiles.clear();
  
  // Load custom profiles from QSettings
  QString profilesJson = m_settings->value("customProfiles", "{}").toString();
  qDebug() << "Loading custom profiles from settings, JSON:" << profilesJson;
  QJsonDocument doc = QJsonDocument::fromJson(profilesJson.toUtf8());
  
  if (doc.isArray()) {
    m_customProfilesData = doc.array();
    for (const QJsonValue &value : m_customProfilesData) {
      if (value.isObject()) {
        QJsonObject profileObj = value.toObject();
        QString name = profileObj.value("name").toString();
        if (!name.isEmpty()) {
          m_customProfiles.append(name);
          qDebug() << "Loaded custom profile:" << name;
        }
      }
    }
  }
  
  // Do not load or persist an 'activeProfile' in settings anymore.
  // Active profile will be determined from the stateMap + current power state.
  m_activeProfile = "";
  
  // Load stateMap from settings
  QString stateMapJson = m_settings->value("stateMap", "{}").toString();
  qDebug() << "Loading stateMap from settings, JSON:" << stateMapJson;
  QJsonDocument stateMapDoc = QJsonDocument::fromJson(stateMapJson.toUtf8());
  if (stateMapDoc.isObject()) {
    m_stateMap = stateMapDoc.object();
    qDebug() << "Loaded stateMap:" << m_stateMap;
  } else {
    // Default stateMap
    m_stateMap["power_ac"] = "__default_custom_profile__";
    m_stateMap["power_bat"] = "__default_custom_profile__";
    m_stateMap["power_wc"] = "__default_custom_profile__";
  }
  
  qDebug() << "Loaded" << m_customProfiles.size() << "custom profiles from local storage";
}


void ProfileManager::loadCustomFanProfilesFromSettings()
{
  m_customFanProfilesData = QJsonArray();
  m_customFanProfiles.clear();

  QString fanJson = m_settings->value("customFanProfiles", "[]").toString();
  qDebug() << "Loading custom fan profiles from settings, JSON:" << fanJson;
  QJsonDocument doc = QJsonDocument::fromJson( fanJson.toUtf8() );

  if ( doc.isArray() )
  {
    m_customFanProfilesData = doc.array();
    for ( const auto &val : m_customFanProfilesData )
    {
      if ( val.isObject() )
      {
        QJsonObject o = val.toObject();
        QString name = o.value( "name" ).toString();
        if ( !name.isEmpty() )
        {
          m_customFanProfiles.append( name );
          qDebug() << "Loaded custom fan profile:" << name;
        }
      }
    }
  }
}

void ProfileManager::saveCustomFanProfilesToSettings()
{
  QJsonDocument doc( m_customFanProfilesData );
  QString jsonStr = doc.toJson( QJsonDocument::Compact );
  qDebug() << "Saving custom fan profiles to settings file:" << m_settings->fileName();
  qDebug() << "JSON:" << jsonStr;
  m_settings->setValue( "customFanProfiles", jsonStr );
  m_settings->sync();
}

void ProfileManager::saveCustomProfilesToSettings()
{
  QJsonDocument doc(m_customProfilesData);
  QString jsonStr = doc.toJson(QJsonDocument::Compact);
  qDebug() << "Saving custom profiles to settings file:" << m_settings->fileName();
  qDebug() << "JSON:" << jsonStr;
  m_settings->setValue("customProfiles", jsonStr);
  
  // Save stateMap
  QJsonDocument stateMapDoc(m_stateMap);
  QString stateMapJson = stateMapDoc.toJson(QJsonDocument::Compact);
  qDebug() << "Saving stateMap:" << stateMapJson;
  m_settings->setValue("stateMap", stateMapJson);
  
  m_settings->sync();
  qDebug() << "QSettings sync completed";
  
  qDebug() << "Saved" << m_customProfilesData.size() << "custom profiles to local storage";
}

// Helper: resolve a stateMap entry (which may be an id or name) to a profile name
QString ProfileManager::resolveStateMapToProfileName( const QString &state )
{
  if ( !m_stateMap.contains( state ) ) return QString();
  QString mapped = m_stateMap[state].toString();
  if ( mapped.isEmpty() ) return QString();

  // If mapped is already a profile name in our list, return it
  if ( m_allProfiles.contains( mapped ) || m_customProfiles.contains( mapped ) || m_defaultProfiles.contains( mapped ) )
    return mapped;

  // Otherwise, try to resolve as an ID
  for ( const auto &p : m_defaultProfilesData ) {
    if ( p.isObject() && p.toObject()["id"].toString() == mapped )
      return p.toObject()["name"].toString();
  }
  for ( const auto &p : m_customProfilesData ) {
    if ( p.isObject() && p.toObject()["id"].toString() == mapped )
      return p.toObject()["name"].toString();
  }

  return QString();
}

QString ProfileManager::getFanProfile( const QString &name )
{
  // If it's a custom fan profile stored locally, return it
  for ( const auto &v : m_customFanProfilesData )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      if ( o.value( "name" ).toString() == name )
      {
        QString jsonStr = o.value( "json" ).toString();

        // If the stored custom entry is empty, fall back to built-in instead of returning empty.
        if ( jsonStr.trimmed().isEmpty() ) {
          qWarning() << "[ProfileManager] CUSTOM fan profile" << name << "has empty JSON — falling back to built-in";
          // Do not return here; allow fallback to daemon-provided built-in profile
        } else {
          // Diagnostic: inspect temperatures and spacing
          QJsonDocument doc = QJsonDocument::fromJson( jsonStr.toUtf8() );
          if ( doc.isObject() ) {
            QJsonObject obj = doc.object();
            if ( obj.contains("tableCPU") && obj["tableCPU"].isArray() ) {
              QJsonArray arr = obj["tableCPU"].toArray();
              QStringList temps;
              for ( int i = 0; i < arr.size() && i < 8; ++i ) {
                if ( arr[i].isObject() ) temps << QString::number( arr[i].toObject()["temp"].toInt() );
              }
              qDebug() << "[ProfileManager] Returning CUSTOM fan profile" << name << "CPU points:" << arr.size() << "sample temps:" << temps;

              // check spacing
              if ( arr.size() > 1 ) {
                int prev = arr[0].toObject()["temp"].toInt();
                for ( int i = 1; i < arr.size(); ++i ) {
                  int t = arr[i].toObject()["temp"].toInt();
                  int diff = t - prev;
                  if ( diff % 5 != 0 || t < 20 || t > 100 ) {
                    qWarning() << "[ProfileManager] CUSTOM fan profile" << name << "has non-5°C spacing or out-of-range temp:" << t << "(diff" << diff << ")";
                    break;
                  }
                  prev = t;
                }
              }
            }
          }

          return jsonStr;
        }
      }
    }
  }

  // Otherwise, fall back to daemon-provided built-in profiles via DBus
  if ( auto json = m_client->getFanProfile( name.toStdString() ) )
  {
    QString s = QString::fromStdString( *json );

    // Diagnostics: inspect built-in JSON we got from daemon
    QJsonDocument doc = QJsonDocument::fromJson( s.toUtf8() );
    if ( doc.isObject() ) {
      QJsonObject obj = doc.object();
      if ( obj.contains("tableCPU") && obj["tableCPU"].isArray() ) {
        QJsonArray arr = obj["tableCPU"].toArray();
        QStringList temps;
        for ( int i = 0; i < arr.size() && i < 8; ++i ) {
          if ( arr[i].isObject() ) temps << QString::number( arr[i].toObject()["temp"].toInt() );
        }
        qDebug() << "[ProfileManager] Returning BUILT-IN fan profile" << name << "CPU points:" << arr.size() << "sample temps:" << temps;

        // check spacing
        if ( arr.size() > 1 ) {
          int prev = arr[0].toObject()["temp"].toInt();
          for ( int i = 1; i < arr.size(); ++i ) {
            int t = arr[i].toObject()["temp"].toInt();
            int diff = t - prev;
            if ( diff % 5 != 0 || t < 20 || t > 100 ) {
              qWarning() << "[ProfileManager] BUILT-IN fan profile" << name << "has non-5°C spacing or out-of-range temp:" << t << "(diff" << diff << ")";
              break;
            }
            prev = t;
          }
        }
      }
    }

    return s;
  }
  return "{}";
}

bool ProfileManager::setFanProfile( const QString &name, const QString &json )
{
  // Do not allow overwriting built-in profiles
  if ( m_defaultProfiles.contains( name ) )
  {
    qWarning() << "Attempt to overwrite built-in fan profile:" << name;
    return false;
  }

  // Update existing entry or append new
  bool found = false;
  for ( int i = 0; i < m_customFanProfilesData.size(); ++i )
  {
    if ( m_customFanProfilesData[i].isObject() )
    {
      QJsonObject o = m_customFanProfilesData[i].toObject();
      if ( o.value( "name" ).toString() == name )
      {
        o[ "json" ] = json;
        m_customFanProfilesData[i] = o;
        found = true;
        break;
      }
    }
  }

  if ( !found )
  {
    QJsonObject o;
    o[ "name" ] = name;
    o[ "json" ] = json;
    m_customFanProfilesData.append( o );
    m_customFanProfiles.append( name );
  }

  saveCustomFanProfilesToSettings();
  return true;
}

bool ProfileManager::deleteFanProfile( const QString &name )
{
  bool removed = false;
  QJsonArray newArr;
  for ( const auto &v : m_customFanProfilesData )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      if ( o.value( "name" ).toString() == name )
      {
        removed = true;
        continue;
      }
    }
    newArr.append( v );
  }

  if ( removed )
  {
    m_customFanProfilesData = newArr;
    m_customFanProfiles.removeAll( name );
    saveCustomFanProfilesToSettings();
  }

  return removed;
}

bool ProfileManager::renameFanProfile( const QString &oldName, const QString &newName )
{
  if ( oldName == newName ) return true;
  if ( newName.isEmpty() ) return false;

  QString json = getFanProfile( oldName );
  if ( json.isEmpty() || json == "{}" ) return false;

  if ( !deleteFanProfile( oldName ) ) return false;
  return setFanProfile( newName, json );
}

QString ProfileManager::getKeyboardProfile( const QString &name )
{
  // If it's a custom keyboard profile stored locally, return it
  for ( const auto &v : m_customKeyboardProfilesData )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      if ( o.value( "name" ).toString() == name )
      {
        QString jsonStr = o.value( "json" ).toString();
        if ( !jsonStr.trimmed().isEmpty() )
        {
          qDebug() << "[ProfileManager] Returning CUSTOM keyboard profile" << name;
          return jsonStr;
        }
        else
        {
          qWarning() << "[ProfileManager] CUSTOM keyboard profile" << name << "has empty JSON — falling back to default";
        }
      }
    }
  }

  // For now, return a default empty keyboard profile
  // In the future, this could be extended to support built-in keyboard profiles from the daemon
  qDebug() << "[ProfileManager] Returning default empty keyboard profile for" << name;
  return "{}";
}

bool ProfileManager::setKeyboardProfile( const QString &name, const QString &json )
{
  // Update existing entry or append new
  bool found = false;
  for ( int i = 0; i < m_customKeyboardProfilesData.size(); ++i )
  {
    if ( m_customKeyboardProfilesData[i].isObject() )
    {
      QJsonObject o = m_customKeyboardProfilesData[i].toObject();
      if ( o.value( "name" ).toString() == name )
      {
        o[ "json" ] = json;
        m_customKeyboardProfilesData[i] = o;
        found = true;
        break;
      }
    }
  }

  if ( !found )
  {
    QJsonObject o;
    o[ "name" ] = name;
    o[ "json" ] = json;
    m_customKeyboardProfilesData.append( o );
    m_customKeyboardProfiles.append( name );
  }

  saveCustomKeyboardProfilesToSettings();
  emit customKeyboardProfilesChanged();
  return true;
}

bool ProfileManager::deleteKeyboardProfile( const QString &name )
{
  bool removed = false;
  QJsonArray newArr;
  for ( const auto &v : m_customKeyboardProfilesData )
  {
    if ( v.isObject() )
    {
      QJsonObject o = v.toObject();
      if ( o.value( "name" ).toString() == name )
      {
        removed = true;
        continue;
      }
    }
    newArr.append( v );
  }

  if ( removed )
  {
    m_customKeyboardProfilesData = newArr;
    m_customKeyboardProfiles.removeAll( name );
    saveCustomKeyboardProfilesToSettings();
    emit customKeyboardProfilesChanged();
  }

  return removed;
}

bool ProfileManager::renameKeyboardProfile( const QString &oldName, const QString &newName )
{
  if ( oldName == newName ) return true;
  if ( newName.isEmpty() ) return false;

  QString json = getKeyboardProfile( oldName );
  if ( json.isEmpty() || json == "{}" ) return false;

  if ( !deleteKeyboardProfile( oldName ) ) return false;
  return setKeyboardProfile( newName, json );
}

void ProfileManager::loadCustomKeyboardProfilesFromSettings()
{
  m_customKeyboardProfilesData = QJsonArray();
  m_customKeyboardProfiles.clear();

  QString keyboardJson = m_settings->value("customKeyboardProfiles", "[]").toString();
  qDebug() << "Loading custom keyboard profiles from settings, JSON:" << keyboardJson;
  QJsonDocument doc = QJsonDocument::fromJson( keyboardJson.toUtf8() );

  if ( doc.isArray() )
  {
    m_customKeyboardProfilesData = doc.array();
    for ( const auto &val : m_customKeyboardProfilesData )
    {
      if ( val.isObject() )
      {
        QJsonObject o = val.toObject();
        QString name = o.value( "name" ).toString();
        if ( !name.isEmpty() )
        {
          m_customKeyboardProfiles.append( name );
          qDebug() << "Loaded custom keyboard profile:" << name;
        }
      }
    }
  }
}

void ProfileManager::saveCustomKeyboardProfilesToSettings()
{
  QJsonDocument doc( m_customKeyboardProfilesData );
  QString jsonStr = doc.toJson( QJsonDocument::Compact );
  jsonStr.replace(",", ", ").replace(":", ": ");  // Add spaces after commas and colons
  qDebug() << "Saving custom keyboard profiles to settings file:" << m_settings->fileName();
  qDebug() << "JSON:" << jsonStr;
  m_settings->setValue( "customKeyboardProfiles", jsonStr );
  m_settings->sync();
}

QString ProfileManager::getSettingsJSON()
{
  try {
    if ( auto json = m_client->getSettingsJSON() )
    {
      return QString::fromStdString( *json );
    }
  } catch (const std::exception &e) {
    qWarning() << "Failed to get settings JSON:" << e.what();
  }
  return "{}";
}

bool ProfileManager::setStateMap( const QString &state, const QString &profileId )
{
  // Update local stateMap
  m_stateMap[state] = profileId;
  saveCustomProfilesToSettings();
  
  // Update uccd
  return m_client->setStateMap( state.toStdString(), profileId.toStdString() );
}

} // namespace ucc
