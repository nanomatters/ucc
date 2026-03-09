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
#include <memory>
#include <map>
#include "UccdClient.hpp"

namespace ucc
{

/**
 * @brief Profile management for QML interface
 *
 * Provides profile operations with Qt signals/slots integration.
 * All profile data is fetched from / saved to the uccd daemon — no local
 * persistence of profiles in ~/.config/uccrc anymore.
 */
class ProfileManager : public QObject
{
  Q_OBJECT
  Q_PROPERTY( QStringList allProfiles READ allProfiles NOTIFY allProfilesChanged )
  Q_PROPERTY( QString activeProfile READ activeProfileName NOTIFY activeProfileChanged )
  Q_PROPERTY( QString powerState READ powerState NOTIFY powerStateChanged )
  Q_PROPERTY( int activeProfileIndex READ activeProfileIndex NOTIFY activeProfileIndexChanged )
  Q_PROPERTY( bool connected READ isConnected NOTIFY connectedChanged )

public:
  explicit ProfileManager( QObject *parent = nullptr );
  ~ProfileManager() override = default;

  QStringList allProfiles() const { return m_allProfiles; }

  // Active profile
  QString activeProfileId() const { return m_activeProfileId; }
  QString activeProfileName() const;
  QString activeKeyboardProfileId() const { return m_activeKeyboardProfileId; }
  QString activeFanProfileId() const { return m_activeFanProfileId; }
  QString activeGpuProfileId() const { return m_activeGpuProfileId; }
  QString powerState() const { return m_powerState; }
  int activeProfileIndex() const { return m_activeProfileIndex; }
  bool isConnected() const { return m_connected; }
  UccdClient* getClient() const { return m_client.get(); }

  /// All system profiles (built-in + custom) with editable flag
  const QJsonArray& allProfilesData() const { return m_allProfilesData; }

  // Sub-profile data: built-in + custom combined, each with editable flag
  const QJsonArray& fanProfilesData() const { return m_fanProfilesData; }
  const QJsonArray& gpuProfilesData() const { return m_gpuProfilesData; }
  const QJsonArray& keyboardProfilesData() const { return m_keyboardProfilesData; }

  /// Available thermal sources from hardware (fetched once at startup)
  const QJsonArray& thermalSourcesData() const { return m_thermalSourcesData; }

  /// Hardware fan zones (topology: id, name, deviceType, fanIds, thermalSourceId)
  const QJsonArray& fanZonesData() const { return m_fanZonesData; }

public slots:
  void refresh();
  void setActiveProfile( const QString &profileId );
  void setActiveProfileByIndex( int index );
  void saveProfile( const QString &profileJSON );
  void deleteProfile( const QString &profileId );
  QString getProfileDetails( const QString &profileId );
  QString createProfileFromDefault( const QString &name );
  std::vector< int > getHardwarePowerLimits();
  // ID-based lookups
  QString profileNameById( const QString &profileId ) const;
  QString profileIdByName( const QString &profileName ) const;

  // Fan profiles (by ID) — all go through daemon
  QString getFanProfile( const QString &fanProfileId );
  bool setFanProfile( const QString &fanProfileId, const QString &name, const QString &json );
  bool deleteFanProfile( const QString &fanProfileId );
  bool renameFanProfile( const QString &fanProfileId, const QString &newName );

  // Keyboard profiles (by ID) — all go through daemon
  QString getKeyboardProfile( const QString &keyboardProfileId );
  bool setKeyboardProfile( const QString &keyboardProfileId, const QString &name, const QString &json );
  bool deleteKeyboardProfile( const QString &keyboardProfileId );
  bool renameKeyboardProfile( const QString &keyboardProfileId, const QString &newName );

  // GPU OC profiles (by ID) — all go through daemon
  QString getGpuProfile( const QString &gpuProfileId );
  bool setGpuProfile( const QString &gpuProfileId, const QString &name, const QString &json );
  bool deleteGpuProfile( const QString &gpuProfileId );
  bool renameGpuProfile( const QString &gpuProfileId, const QString &newName );

  /// Check whether a system profile ID is user-created (editable)
  bool isProfileEditable( const QString &profileId ) const;
  /// Check whether a sub-profile ID is editable (returns false for built-ins)
  bool isProfileEditable( const QString &profileId, const QJsonArray &profilesData ) const;

  QString getSettingsJSON();
  bool setStateMap( const QString &state, const QString &profileId );
  bool setBatchStateMap( const std::map< QString, QString > &entries );

signals:
  void allProfilesChanged();
  void activeProfileChanged();
  void activeKeyboardProfileChanged( const QString &keyboardProfileId );
  void activeFanProfileChanged( const QString &fanProfileId );
  void activeGpuProfileChanged( const QString &gpuProfileId );
  void powerStateChanged();
  void activeProfileIndexChanged();
  void connectedChanged();
  void keyboardProfilesChanged();
  void fanProfilesChanged();
  void gpuProfilesChanged();
  void error( const QString &message );

private:
  void updateProfiles();
  void onProfileChanged( const std::string &profileId,
                         const std::string &keyboardProfileId,
                         const std::string &fanProfileId,
                         const std::string &gpuProfileId );
  void onPowerStateChanged( const QString &state );
  QString resolveStateMapToProfileId( const QString &state );
  void updateAllProfiles();
  void updateActiveProfileIndex();
  void loadProfilesFromDaemon();
  void loadFanProfilesFromDaemon();
  void loadGpuProfilesFromDaemon();
  void loadKeyboardProfilesFromDaemon();
  void loadThermalSourcesFromDaemon();
  void loadFanZonesFromDaemon();

  std::unique_ptr< UccdClient > m_client;

  QStringList m_allProfiles;
  QStringList m_allProfileIds;      ///< parallel to m_allProfiles

  QString m_activeProfileId;
  QString m_activeKeyboardProfileId;
  QString m_activeFanProfileId;
  QString m_activeGpuProfileId;
  QString m_powerState;
  int m_activeProfileIndex = -1;
  bool m_connected = false;
  std::vector< int > m_hardwarePowerLimits;

  QJsonArray m_allProfilesData;          ///< all system profiles (built-in + custom) with editable flag
  QJsonArray m_fanProfilesData;          ///< all fan profiles (built-in + custom)
  QJsonArray m_gpuProfilesData;          ///< all GPU profiles (built-in + custom)
  QJsonArray m_keyboardProfilesData;     ///< all keyboard profiles (all custom)
  QJsonArray m_thermalSourcesData;        ///< available thermal sources from hardware
  QJsonArray m_fanZonesData;               ///< hardware fan zones (topology)
};

} // namespace ucc
