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
#include <map>
#include "UccdClient.hpp"

namespace ucc
{

/**
 * @brief Profile management for QML interface
 *
 * Provides profile operations with Qt signals/slots integration.
 * All profile references use IDs; names are used only for display.
 */
class ProfileManager : public QObject
{
  Q_OBJECT
  Q_PROPERTY( QStringList defaultProfiles READ defaultProfiles NOTIFY defaultProfilesChanged )
  Q_PROPERTY( QStringList customProfiles READ customProfiles NOTIFY customProfilesChanged )
  Q_PROPERTY( QStringList allProfiles READ allProfiles NOTIFY allProfilesChanged )
  Q_PROPERTY( QString activeProfile READ activeProfileName NOTIFY activeProfileChanged )
  Q_PROPERTY( QString powerState READ powerState NOTIFY powerStateChanged )
  Q_PROPERTY( int activeProfileIndex READ activeProfileIndex NOTIFY activeProfileIndexChanged )
  Q_PROPERTY( bool connected READ isConnected NOTIFY connectedChanged )

public:
  explicit ProfileManager( QObject *parent = nullptr );
  ~ProfileManager() override = default;

  // Name lists for display (parallel to ID lists)
  QStringList defaultProfiles() const { return m_defaultProfiles; }
  QStringList customProfiles() const { return m_customProfiles; }
  QStringList allProfiles() const { return m_allProfiles; }

  // Active profile
  QString activeProfileId() const { return m_activeProfileId; }
  QString activeProfileName() const;
  QString powerState() const { return m_powerState; }
  int activeProfileIndex() const { return m_activeProfileIndex; }
  bool isConnected() const { return m_connected; }
  UccdClient* getClient() const { return m_client.get(); }

  // Data accessors for combos (id + name)
  const QJsonArray& defaultProfilesData() const { return m_defaultProfilesData; }
  const QJsonArray& customProfilesData() const { return m_customProfilesData; }

  // Fan profile data (built-in + custom, each with id + name)
  const QJsonArray& builtinFanProfilesData() const { return m_builtinFanProfilesData; }
  const QJsonArray& customFanProfilesData() const { return m_customFanProfilesData; }
  const QJsonArray& customKeyboardProfilesData() const { return m_customKeyboardProfilesData; }

public slots:
  void refresh();
  void setActiveProfile( const QString &profileId );
  void setActiveProfileByIndex( int index );
  void saveProfile( const QString &profileJSON );
  void deleteProfile( const QString &profileId );
  QString getProfileDetails( const QString &profileId );
  QString createProfileFromDefault( const QString &name );
  std::vector< int > getHardwarePowerLimits();
  bool isCustomProfile( const QString &profileId ) const;

  // ID-based lookups
  QString profileNameById( const QString &profileId ) const;
  QString profileIdByName( const QString &profileName ) const;

  // Fan profiles (by ID)
  QString getFanProfile( const QString &fanProfileId );
  bool setFanProfile( const QString &fanProfileId, const QString &name, const QString &json );
  QStringList customFanProfiles() const { return m_customFanProfiles; }
  bool deleteFanProfile( const QString &fanProfileId );
  bool renameFanProfile( const QString &fanProfileId, const QString &newName );

  // Keyboard profiles (by ID)
  QString getKeyboardProfile( const QString &keyboardProfileId );
  bool setKeyboardProfile( const QString &keyboardProfileId, const QString &name, const QString &json );
  QStringList customKeyboardProfiles() const { return m_customKeyboardProfiles; }
  bool deleteKeyboardProfile( const QString &keyboardProfileId );
  bool renameKeyboardProfile( const QString &keyboardProfileId, const QString &newName );

  QString getSettingsJSON();
  bool setStateMap( const QString &state, const QString &profileId );
  bool setBatchStateMap( const std::map< QString, QString > &entries );

signals:
  void defaultProfilesChanged();
  void customProfilesChanged();
  void allProfilesChanged();
  void activeProfileChanged();
  void powerStateChanged();
  void activeProfileIndexChanged();
  void connectedChanged();
  void customKeyboardProfilesChanged();
  void customFanProfilesChanged();
  void error( const QString &message );

private:
  void updateProfiles();
  void onProfileChanged( const std::string &profileId );
  void onPowerStateChanged( const QString &state );
  QString resolveStateMapToProfileId( const QString &state );
  void updateAllProfiles();
  void updateActiveProfileIndex();
  void loadCustomProfilesFromSettings();
  void saveCustomProfilesToSettings();
  void loadBuiltinFanProfiles();
  void loadCustomFanProfilesFromSettings();
  void saveCustomFanProfilesToSettings();
  void loadCustomKeyboardProfilesFromSettings();
  void saveCustomKeyboardProfilesToSettings();
  void migrateFanProfileIds( QJsonArray &arr );
  void migrateKeyboardProfileIds( QJsonArray &arr );

  std::unique_ptr< UccdClient > m_client;
  std::unique_ptr< QSettings > m_settings;

  // Profile names (for display) and parallel ID lists
  QStringList m_defaultProfiles;
  QStringList m_customProfiles;
  QStringList m_allProfiles;
  QStringList m_allProfileIds;      ///< parallel to m_allProfiles

  // Fan profile names (for display)
  QStringList m_builtinFanProfiles;
  QStringList m_customFanProfiles;
  QStringList m_customKeyboardProfiles;

  QString m_activeProfileId;
  QString m_powerState;
  int m_activeProfileIndex = -1;
  bool m_connected = false;
  std::vector< int > m_hardwarePowerLimits;

  QJsonArray m_defaultProfilesData;
  QJsonArray m_customProfilesData;
  QJsonArray m_builtinFanProfilesData;   ///< [{id, name}, ...] from daemon
  QJsonArray m_customFanProfilesData;    ///< [{id, name, json}, ...] local
  QJsonArray m_customKeyboardProfilesData;
  QJsonObject m_stateMap;
};

} // namespace ucc
