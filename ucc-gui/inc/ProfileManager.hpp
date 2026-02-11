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
#include <QStringList>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <memory>
#include "UccdClient.hpp"

namespace ucc
{

/**
 * @brief Profile management for QML interface
 *
 * Provides profile operations with Qt signals/slots integration
 */
class ProfileManager : public QObject
{
  Q_OBJECT
  Q_PROPERTY( QStringList defaultProfiles READ defaultProfiles NOTIFY defaultProfilesChanged )
  Q_PROPERTY( QStringList customProfiles READ customProfiles NOTIFY customProfilesChanged )
  Q_PROPERTY( QStringList allProfiles READ allProfiles NOTIFY allProfilesChanged )
  Q_PROPERTY( QString activeProfile READ activeProfile NOTIFY activeProfileChanged )
  Q_PROPERTY( QString powerState READ powerState NOTIFY powerStateChanged )
  Q_PROPERTY( int activeProfileIndex READ activeProfileIndex NOTIFY activeProfileIndexChanged )
  Q_PROPERTY( bool connected READ isConnected NOTIFY connectedChanged )

public:
  explicit ProfileManager( QObject *parent = nullptr );
  ~ProfileManager() override = default;

  QStringList defaultProfiles() const { return m_defaultProfiles; }
  QStringList customProfiles() const { return m_customProfiles; }
  QStringList allProfiles() const { return m_allProfiles; }
  QString activeProfile() const { return m_activeProfile; }
  QString powerState() const { return m_powerState; }
  int activeProfileIndex() const { return m_activeProfileIndex; }
  bool isConnected() const { return m_connected; }
  UccdClient* getClient() const { return m_client.get(); }

public slots:
  void refresh();
  void setActiveProfile( const QString &profileId );
  void setActiveProfileByIndex( int index );
  void saveProfile( const QString &profileJSON );
  void deleteProfile( const QString &profileId );
  QString getProfileDetails( const QString &profileId );
  QString getProfileIdByName( const QString &profileName );
  QString createProfileFromDefault( const QString &name );
  std::vector< int > getHardwarePowerLimits();
  bool isCustomProfile( const QString &profileName ) const;
  QString getFanProfile( const QString &name );
  bool setFanProfile( const QString &name, const QString &json );
  QStringList customFanProfiles() const { return m_customFanProfiles; }
  bool deleteFanProfile( const QString &name );
  bool renameFanProfile( const QString &oldName, const QString &newName );
  QString getKeyboardProfile( const QString &name );
  bool setKeyboardProfile( const QString &name, const QString &json );
  QStringList customKeyboardProfiles() const { return m_customKeyboardProfiles; }
  bool deleteKeyboardProfile( const QString &name );
  bool renameKeyboardProfile( const QString &oldName, const QString &newName );
  QString getSettingsJSON();

  bool setStateMap( const QString &state, const QString &profileId );
  
signals:
  void defaultProfilesChanged();
  void customProfilesChanged();
  void allProfilesChanged();
  void activeProfileChanged();
  void powerStateChanged();
  void activeProfileIndexChanged();
  void connectedChanged();
  void customKeyboardProfilesChanged();
  void error( const QString &message );

private:
  void updateProfiles();
  void onProfileChanged( const std::string &profileId );
  void onPowerStateChanged( const QString &state );
  // Local detection removed; daemon provides current power state via DBus
  QString resolveStateMapToProfileName( const QString &state );
  void updateAllProfiles();
  void updateActiveProfileIndex();
  void loadCustomProfilesFromSettings();
  void saveCustomProfilesToSettings();
  void loadCustomFanProfilesFromSettings();
  void saveCustomFanProfilesToSettings();
  void loadCustomKeyboardProfilesFromSettings();
  void saveCustomKeyboardProfilesToSettings();

  std::unique_ptr< UccdClient > m_client;
  std::unique_ptr< QSettings > m_settings;
  QStringList m_defaultProfiles;
  QStringList m_customProfiles;
  QStringList m_allProfiles;
  QStringList m_customFanProfiles;
  QStringList m_customKeyboardProfiles;
  QString m_activeProfile;
  QString m_powerState;
  int m_activeProfileIndex = -1;
  bool m_connected = false;
  std::vector< int > m_hardwarePowerLimits;

  QJsonArray m_defaultProfilesData;
  QJsonArray m_customProfilesData;
  QJsonArray m_customFanProfilesData;
  QJsonArray m_customKeyboardProfilesData;
  QJsonObject m_stateMap;
};

} // namespace ucc
