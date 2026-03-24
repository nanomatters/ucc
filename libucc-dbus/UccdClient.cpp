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

#include "UccdClient.hpp"
#include <QDBusMessage>
#include <QDBusError>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QThread>
#include <QFile>
#include <QVariantMap>

namespace ucc
{

UccdClient::UccdClient( QObject *parent )
  : QObject( parent )
{
  // Initial connection attempt (uccd may not be running yet — that's fine)
  connectToDaemon();

  // Watch for uccd appearing or disappearing on the system bus
  m_serviceWatcher = new QDBusServiceWatcher(
    QLatin1String( DBUS_SERVICE ),
    QDBusConnection::systemBus(),
    QDBusServiceWatcher::WatchForOwnerChange,
    this );

  connect( m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered,
           this, &UccdClient::onServiceRegistered );
  connect( m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered,
           this, &UccdClient::onServiceUnregistered );

  emit connectionStatusChanged( m_connected );
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void UccdClient::subscribeDbusSignals()
{
  // Disconnect first so we never accumulate duplicate connections across
  // multiple reconnect cycles.
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "ProfileChanged", this,
                  SLOT( onProfileChangedSignal( QString, QString, QString, QString ) ) );
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "ProfilesListChanged", this,
                  SLOT( onProfilesListChangedSignal() ) );
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "PowerStateChanged", this,
                  SLOT( onPowerStateChangedSignal( QString ) ) );

  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "ProfileChanged", this,
               SLOT( onProfileChangedSignal( QString, QString, QString, QString ) ) );
  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "ProfilesListChanged", this,
               SLOT( onProfilesListChangedSignal() ) );
  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "PowerStateChanged", this,
               SLOT( onPowerStateChangedSignal( QString ) ) );

  // Auto-OC signals
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "AutoOCProgressChanged", this,
                  SIGNAL( autoOCProgressChanged( QVariantMap ) ) );
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "AutoOCFinished", this,
                  SIGNAL( autoOCFinished( int, int, bool, QString ) ) );

  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "AutoOCProgressChanged", this,
               SIGNAL( autoOCProgressChanged( QVariantMap ) ) );
  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "AutoOCFinished", this,
               SIGNAL( autoOCFinished( int, int, bool, QString ) ) );

  // Auto-Undervolt signals
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "AutoUndervoltProgressChanged", this,
                  SIGNAL( autoUndervoltProgressChanged( QVariantMap ) ) );
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "AutoUndervoltFinished", this,
                  SIGNAL( autoUndervoltFinished( int, bool, QString, QString ) ) );

  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "AutoUndervoltProgressChanged", this,
               SIGNAL( autoUndervoltProgressChanged( QVariantMap ) ) );
  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "AutoUndervoltFinished", this,
               SIGNAL( autoUndervoltFinished( int, bool, QString, QString ) ) );
}

void UccdClient::connectToDaemon()
{
  // Check if the service actually has an owner on the bus.  We must NOT
  // just create a QDBusInterface, because with a D-Bus activation .service
  // file the bus would auto-start uccd during the introspection call that
  // QDBusInterface performs in its constructor.
  if ( auto *busIface = QDBusConnection::systemBus().interface();
       !busIface || !busIface->isServiceRegistered( QLatin1String( DBUS_SERVICE ) ) )
  {
    m_connected = false;
    m_interface.reset();  // no interface while disconnected
    qWarning() << "[UccdClient] uccd D-Bus service not registered on the system bus";
    return;
  }

  // The service is running — safe to introspect without triggering activation.
  // Do NOT pass a parent to QDBusInterface — unique_ptr owns its lifetime.
  m_interface = std::make_unique< QDBusInterface >(
    DBUS_SERVICE,
    DBUS_PATH,
    DBUS_INTERFACE,
    QDBusConnection::systemBus() );

  m_connected = m_interface->isValid();

  if ( m_connected )
  {
    subscribeDbusSignals();
  }
  else
  {
    qWarning() << "[UccdClient] uccd D-Bus interface not valid:"
               << m_interface->lastError().message();
  }
}

// ---------------------------------------------------------------------------
// D-Bus service watcher slots
// ---------------------------------------------------------------------------

void UccdClient::onServiceRegistered( const QString &service )
{
  Q_UNUSED( service )
  qInfo() << "[UccdClient] uccd appeared on the system bus — reconnecting";
  connectToDaemon();
  emit connectionStatusChanged( m_connected );
}

void UccdClient::onServiceUnregistered( const QString &service )
{
  Q_UNUSED( service )
  qWarning() << "[UccdClient] uccd disappeared from the system bus";
  m_connected = false;
  emit connectionStatusChanged( false );
}

bool UccdClient::isConnected() const
{
  return m_connected && m_interface && m_interface->isValid();
}

// Signal handlers
void UccdClient::onProfileChangedSignal( const QString &profileId,
                                         const QString &keyboardProfileId,
                                         const QString &fanProfileId,
                                         const QString &gpuProfileId )
{
  emit profileChanged( profileId, keyboardProfileId, fanProfileId, gpuProfileId );
}

void UccdClient::onPowerStateChangedSignal( const QString &state )
{
  emit powerStateChanged( state );
}

void UccdClient::onProfilesListChangedSignal()
{
  emit profilesListChanged();
}

// Template implementations
template< typename T >
std::optional< T > UccdClient::callMethod( const QString &method ) const
{
  if ( !isConnected() )
  {
    return std::nullopt;
  }

  QDBusReply< T > reply = m_interface->call( method );
  if ( reply.isValid() )
  {
    return reply.value();
  }
  else
  {
    qWarning() << "DBus call failed:" << method << "-" << reply.error().message();
    return std::nullopt;
  }
}

template< typename T, typename... Args >
std::optional< T > UccdClient::callMethod( const QString &method, const Args &...args ) const
{
  if ( !isConnected() )
  {
    return std::nullopt;
  }

  QDBusReply< T > reply = m_interface->call( method, args... );
  if ( reply.isValid() )
  {
    return reply.value();
  }
  else
  {
    qWarning() << "DBus call failed:" << method << "-" << reply.error().message();
    return std::nullopt;
  }
}

bool UccdClient::callVoidMethod( const QString &method ) const
{
  if ( !isConnected() )
  {
    return false;
  }

  QDBusMessage reply = m_interface->call( method );
  if ( reply.type() == QDBusMessage::ErrorMessage )
  {
    qWarning() << "DBus call failed:" << method << "-" << reply.errorMessage();
    return false;
  }
  return true;
}

template< typename... Args >
bool UccdClient::callVoidMethod( const QString &method, const Args &...args ) const
{
  if ( !isConnected() )
  {
    return false;
  }

  QDBusMessage reply = m_interface->call( method, args... );
  if ( reply.type() == QDBusMessage::ErrorMessage )
  {
    qWarning() << "DBus call failed:" << method << "-" << reply.errorMessage();
    return false;
  }
  return true;
}

// System Information
std::optional< std::string > UccdClient::getSystemInfoJSON()
{
  if ( auto result = callMethod< QString >( "GetSystemInfoJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< bool > UccdClient::isDeviceSupported()
{
  return callMethod< bool >( "IsDeviceSupported" );
}

std::optional< std::string > UccdClient::getCapabilitiesJSON()
{
  if ( auto result = callMethod< QString >( "GetCapabilitiesJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Profile Management — unified API
// ---------------------------------------------------------------------------

std::optional< std::string > UccdClient::getProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetProfilesJSON" ) )
    return result->toStdString();
  return std::nullopt;
}
std::optional< std::string > UccdClient::getDefaultProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetDefaultProfilesJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< QVariantMap > UccdClient::getCpuFrequencyLimits()
{
  return callMethod< QVariantMap >( "GetCpuFrequencyLimits" );
}

std::optional< std::string > UccdClient::getDefaultValuesProfileJSON()
{
  if ( auto result = callMethod< QString >( "GetDefaultValuesProfileJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< std::string > UccdClient::getCustomProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetCustomProfilesJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< std::string > UccdClient::getActiveProfileJSON()
{
  if ( auto result = callMethod< QString >( "GetActiveProfileJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< QVariantMap > UccdClient::getAppliedProfiles()
{
  return callMethod< QVariantMap >( "GetAppliedProfiles" );
}

std::optional< std::string > UccdClient::getSettingsJSON()
{
  if ( auto result = callMethod< QString >( "GetSettingsJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< std::string > UccdClient::getPowerState()
{
  if ( auto result = callMethod< QString >( "GetPowerState" ) )
    return result->toStdString();
  return std::nullopt;
}

bool UccdClient::setStateMap( const std::string &state, const std::string &profileId )
{
  const QString qState = QString::fromStdString( state );
  const QString qProfileId = QString::fromStdString( profileId );
  return callVoidMethod( "SetStateMap", qState, qProfileId );
}

bool UccdClient::setBatchStateMap( const std::map< std::string, std::string > &entries )
{
  QJsonObject obj;
  for ( const auto &[state, profileId] : entries )
    obj[QString::fromStdString( state )] = QString::fromStdString( profileId );
  QString json = QJsonDocument( obj ).toJson( QJsonDocument::Compact );
  return callMethod< bool, QString >( "SetBatchStateMap", json ).value_or( false );
}

bool UccdClient::setActiveProfile( const std::string &profileId )
{
  return callMethod< bool, QString >( "SetActiveProfile", QString::fromStdString( profileId ) ).value_or( false );
}

bool UccdClient::applyProfile( const std::string &profileJSON )
{
  return callMethod< bool, QString >( "ApplyProfile", QString::fromStdString( profileJSON ) ).value_or( false );
}

bool UccdClient::saveProfile( const std::string &profileJSON )
{
  return callMethod< bool, QString >( "SaveProfile", QString::fromStdString( profileJSON ) ).value_or( false );
}

bool UccdClient::deleteProfile( const std::string &profileId )
{
  return callMethod< bool, QString >( "DeleteProfile", QString::fromStdString( profileId ) ).value_or( false );
}

// Backward-compat aliases
bool UccdClient::saveCustomProfile( const std::string &profileJSON ) { return saveProfile( profileJSON ); }
bool UccdClient::deleteCustomProfile( const std::string &profileId ) { return deleteProfile( profileId ); }

// ---------------------------------------------------------------------------
// Fan sub-profiles
// ---------------------------------------------------------------------------

std::optional< QVariantList > UccdClient::getFanProfiles()
{
  return callMethod< QVariantList >( "GetFanProfiles" );
}

std::optional< QVariantList > UccdClient::getThermalSources()
{
  return callMethod< QVariantList >( "GetThermalSources" );
}

std::optional< QVariantMap > UccdClient::getSensorReadings()
{
  if ( auto result = callMethod< QVariantMap >( "GetSensorReadings" ) )
    return *result;
  return std::nullopt;
}

std::optional< QVariantList > UccdClient::getHardwareFanDevices()
{
  return callMethod< QVariantList >( "GetHardwareFanDevices" );
}

std::optional< std::string > UccdClient::getHardwareSensorsJSON()
{
  if ( auto result = callMethod< QString >( "GetHardwareSensorsJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< QVariantList > UccdClient::getFanZones()
{
  return callMethod< QVariantList >( "GetFanZones" );
}

std::optional< std::string > UccdClient::getFanProfileJSON( const std::string &fanProfileId )
{
  if ( auto result = callMethod< QString >( "GetFanProfileJSON", QString::fromStdString( fanProfileId ) ) )
    return result->toStdString();
  return std::nullopt;
}

bool UccdClient::saveFanProfile( const std::string &id, const std::string &name, const std::string &json )
{
  auto result = callMethod< bool >( "SaveFanProfile", QString::fromStdString( id ),
                                    QString::fromStdString( name ), QString::fromStdString( json ) );
  return result.value_or( false );
}

bool UccdClient::deleteFanProfile( const std::string &id )
{
  return callMethod< bool >( "DeleteFanProfile", QString::fromStdString( id ) ).value_or( false );
}

// Legacy aliases
std::optional< std::string > UccdClient::getFanProfile( const std::string &fanProfileId )
{
  return getFanProfileJSON( fanProfileId );
}

std::optional< bool > UccdClient::setFanProfile( const std::string &fanProfileId, const std::string &json )
{
  return callMethod< bool >( "SetFanProfile", QString::fromStdString( fanProfileId ), QString::fromStdString( json ) );
}

// ---------------------------------------------------------------------------
// GPU sub-profiles
// ---------------------------------------------------------------------------

std::optional< std::string > UccdClient::getGpuProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetGpuProfilesJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< std::string > UccdClient::getGpuProfileJSON( const std::string &gpuProfileId )
{
  if ( auto result = callMethod< QString >( "GetGpuProfileJSON", QString::fromStdString( gpuProfileId ) ) )
  {
    if ( const std::string json = result->toStdString(); !json.empty() && json != "{}" )
      return json;
  }
  return std::nullopt;
}

bool UccdClient::saveGpuProfile( const std::string &id, const std::string &name, const std::string &json )
{
  auto result = callMethod< bool >( "SaveGpuProfile", QString::fromStdString( id ),
                                    QString::fromStdString( name ), QString::fromStdString( json ) );
  return result.value_or( false );
}

bool UccdClient::deleteGpuProfile( const std::string &id )
{
  return callMethod< bool >( "DeleteGpuProfile", QString::fromStdString( id ) ).value_or( false );
}

// Legacy alias
std::optional< std::string > UccdClient::getGpuProfile( const std::string &gpuProfileId )
{
  return getGpuProfileJSON( gpuProfileId );
}

// ---------------------------------------------------------------------------
// Keyboard sub-profiles
// ---------------------------------------------------------------------------

std::optional< std::string > UccdClient::getKeyboardProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetKeyboardProfilesJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< std::string > UccdClient::getKeyboardProfileJSON( const std::string &keyboardProfileId )
{
  if ( auto result = callMethod< QString >( "GetKeyboardProfileJSON", QString::fromStdString( keyboardProfileId ) ) )
    return result->toStdString();
  return std::nullopt;
}

bool UccdClient::saveKeyboardProfile( const std::string &id, const std::string &name, const std::string &json )
{
  auto result = callMethod< bool >( "SaveKeyboardProfile", QString::fromStdString( id ),
                                    QString::fromStdString( name ), QString::fromStdString( json ) );
  return result.value_or( false );
}

bool UccdClient::deleteKeyboardProfile( const std::string &id )
{
  return callMethod< bool >( "DeleteKeyboardProfile", QString::fromStdString( id ) ).value_or( false );
}

bool UccdClient::setDisplayBrightness( int brightness )
{ return callVoidMethod( "SetDisplayBrightness", brightness ); }

std::optional< int > UccdClient::getDisplayBrightness()
{
  return callMethod< int >( "GetDisplayBrightness" );
}

bool UccdClient::setWebcamEnabled( bool enabled )
{
  return callVoidMethod( "SetWebcam", enabled );
}

std::optional< bool > UccdClient::getWebcamEnabled()
{
  return callMethod< bool >( "GetWebcamSWStatus" );
}

// GPU Info
std::optional< std::string > UccdClient::getGpuInfo()
{
  if ( auto result = callMethod< QString >( "GetGpuInfo" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

// Device Capability Queries
std::optional< bool > UccdClient::getWaterCoolerSupported()
{
  return callMethod< bool >( "GetWaterCoolerSupported" );
}

std::optional< bool > UccdClient::getCTGPAdjustmentSupported()
{
  return callMethod< bool >( "GetCTGPAdjustmentSupported" );
}

// Fn Lock
bool UccdClient::setFnLock( bool enabled )
{
  return callVoidMethod( "SetFnLockStatus", enabled );
}

std::optional< bool > UccdClient::getFnLock()
{
  return callMethod< bool >( "GetFnLockStatus" );
}

// Stub implementations for remaining methods
// TODO: Implement these based on actual uccd DBus interface

bool UccdClient::setYCbCr420Workaround( [[maybe_unused]] bool enabled )
{
  // TODO: Implement when DBus method is available
  return false;
}

std::optional< bool > UccdClient::getYCbCr420Workaround()
{
  return std::nullopt;
}

bool UccdClient::setDisplayRefreshRate( const std::string &display, int refreshRate )
{
  const QString qDisplay = QString::fromStdString( display );
  return callVoidMethod( "SetDisplayRefreshRate", qDisplay, refreshRate );
}

bool UccdClient::setCpuScalingGovernor( [[maybe_unused]] const std::string &governor )
{
  return false;
}

std::optional< std::string > UccdClient::getCpuScalingGovernor()
{
  return std::nullopt;
}

std::optional< std::vector< std::string > > UccdClient::getAvailableCpuGovernors()
{
  auto jsonStr = callMethod< QString >( "GetAvailableGovernors" );
  if ( !jsonStr )
    return std::nullopt;

  QJsonDocument doc = QJsonDocument::fromJson( jsonStr->toUtf8() );
  if ( !doc.isArray() )
    return std::nullopt;

  QJsonArray array = doc.array();
  std::vector< std::string > governors;
  for ( const QJsonValue &value : array )
  {
    if ( value.isString() )
    {
      governors.push_back( value.toString().toStdString() );
    }
  }
  return governors;
}

bool UccdClient::setCpuFrequency( [[maybe_unused]] int minFreq, [[maybe_unused]] int maxFreq )
{
  return false;
}

bool UccdClient::setEnergyPerformancePreference( [[maybe_unused]] const std::string &preference )
{
  return false;
}

std::optional< std::vector< std::string > > UccdClient::getAvailableEPPs()
{
  auto jsonStr = callMethod< QString >( "GetAvailableEPPs" );
  if ( !jsonStr )
    return std::nullopt;

  QJsonDocument doc = QJsonDocument::fromJson( jsonStr->toUtf8() );
  if ( !doc.isArray() )
    return std::nullopt;

  QJsonArray array = doc.array();
  std::vector< std::string > epps;
  for ( const QJsonValue &value : array )
  {
    if ( value.isString() )
    {
      epps.push_back( value.toString().toStdString() );
    }
  }
  return epps;
}

std::optional< int > UccdClient::getCpuCoreCount()
{
  return callMethod< int >( "GetCpuCoreCount" );
}

bool UccdClient::enableWaterCooler( bool enable )
{
  return callMethod< bool, bool >( "EnableWaterCooler", enable ).value_or( false );
}

std::optional< bool > UccdClient::isWaterCoolerEnabled()
{
  return callMethod< bool >( "IsWaterCoolerEnabled" );
}

bool UccdClient::applyFanProfiles( const std::string &fanProfilesJSON )
{
  const QString js = QString::fromStdString( fanProfilesJSON );
  return callMethod< bool, QString >( "ApplyFanProfiles", js ).value_or( false );
}

bool UccdClient::revertFanProfiles()
{
  return callMethod< bool >( "RevertFanProfiles" ).value_or( false );
}

std::optional< QVariantMap > UccdClient::getFanZoneTelemetry()
{
  if ( auto result = callMethod< QVariantMap >( "GetFanZoneTelemetry" ) )
    return *result;
  return std::nullopt;
}

std::optional< std::string > UccdClient::getCurrentFanSpeed()
{
  return std::nullopt;
}

std::optional< std::string > UccdClient::getFanTemperatures()
{
  return std::nullopt;
}

bool UccdClient::setODMPowerLimits( [[maybe_unused]] const std::vector< int > &limits )
{
  return false;
}

std::optional< QVariantList > UccdClient::getODMPowerLimits()
{
  return callMethod< QVariantList >( "ODMPowerLimits" );
}

bool UccdClient::setChargingProfile( const std::string &profileDescriptor )
{
  return callVoidMethod( "SetChargingProfile", QString::fromStdString( profileDescriptor ) );
}

std::optional< std::string > UccdClient::getChargingProfilesAvailable()
{
  if ( auto result = callMethod< QString >( "GetChargingProfilesAvailable" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getCurrentChargingProfile()
{
  if ( auto result = callMethod< QString >( "GetCurrentChargingProfile" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getChargingPrioritiesAvailable()
{
  if ( auto result = callMethod< QString >( "GetChargingPrioritiesAvailable" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getCurrentChargingPriority()
{
  if ( auto result = callMethod< QString >( "GetCurrentChargingPriority" ) )
    return result->toStdString();

  return std::nullopt;
}

bool UccdClient::setChargingPriority( const std::string &priorityDescriptor )
{
  return callVoidMethod( "SetChargingPriority", QString::fromStdString( priorityDescriptor ) );
}

std::optional< std::string > UccdClient::getChargeStartAvailableThresholds()
{
  if ( auto result = callMethod< QString >( "GetChargeStartAvailableThresholds" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getChargeEndAvailableThresholds()
{
  if ( auto result = callMethod< QString >( "GetChargeEndAvailableThresholds" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< int > UccdClient::getChargeStartThreshold()
{
  return callMethod< int >( "GetChargeStartThreshold" );
}

std::optional< int > UccdClient::getChargeEndThreshold()
{
  return callMethod< int >( "GetChargeEndThreshold" );
}

bool UccdClient::setChargeStartThreshold( int value )
{
  return callVoidMethod( "SetChargeStartThreshold", value );
}

bool UccdClient::setChargeEndThreshold( int value )
{
  return callVoidMethod( "SetChargeEndThreshold", value );
}

std::optional< std::string > UccdClient::getChargeType()
{
  if ( auto result = callMethod< QString >( "GetChargeType" ) )
    return result->toStdString();

  return std::nullopt;
}

bool UccdClient::setChargeType( const std::string &type )
{
  return callVoidMethod( "SetChargeType", QString::fromStdString( type ) );
}

bool UccdClient::setNVIDIAPowerOffset( int offset )
{
  return callVoidMethod( "SetNVIDIAPowerOffset", offset );
}

std::optional< int > UccdClient::getNVIDIAPowerOffset()
{
  // Read the actual hardware-applied cTGP offset from the daemon (sysfs).
  return callMethod< int >( "GetNVIDIAPowerOffset" );
}

std::optional< int > UccdClient::getNVIDIAPowerCTRLMaxPowerLimit()
{
  return callMethod< int >( "GetNVIDIAPowerCTRLMaxPowerLimit" );
}

std::optional< int > UccdClient::getNVIDIAPowerCTRLMinPowerLimit()
{
  return callMethod< int >( "GetNVIDIAPowerCTRLMinPowerLimit" );
}

std::optional< int > UccdClient::getNVIDIAPowerCTRLDefaultPowerLimit()
{
  return callMethod< int >( "GetNVIDIAPowerCTRLDefaultPowerLimit" );
}

std::optional< bool > UccdClient::getNVIDIAPowerCTRLAvailable()
{
  return callMethod< bool >( "GetNVIDIAPowerCTRLAvailable" );
}

bool UccdClient::setPrimeProfile( [[maybe_unused]] const std::string &profile )
{
  // Prime profile switching is not supported as a standalone D-Bus call.
  // Prime state is detected automatically by the daemon's HardwareMonitorWorker.
  return false;
}

std::optional< std::string > UccdClient::getPrimeProfile()
{
  if ( auto result = callMethod< QString >( "GetPrimeState" ) )
    return result->toStdString();
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// NVIDIA GPU OC Control
// ---------------------------------------------------------------------------

std::optional< bool > UccdClient::getNvidiaOCAvailable()
{
  return callMethod< bool >( "GetNvidiaOCAvailable" );
}

std::optional< std::string > UccdClient::getNvidiaOCState( int deviceIndex )
{
  if ( auto result = callMethod< QString, int >( "GetNvidiaOCState", deviceIndex ) )
    return result->toStdString();
  return std::nullopt;
}

bool UccdClient::setNvidiaClockOffset( int deviceIndex, int clockType, int pstate, int offsetMHz )
{
  return callMethod< bool, int, int, int, int >( "SetNvidiaClockOffset",
      deviceIndex, clockType, pstate, offsetMHz ).value_or( false );
}

bool UccdClient::setNvidiaGpuLockedClocks( int deviceIndex, int minMHz, int maxMHz )
{
  return callMethod< bool, int, int, int >( "SetNvidiaGpuLockedClocks",
      deviceIndex, minMHz, maxMHz ).value_or( false );
}

bool UccdClient::setNvidiaVramLockedClocks( int deviceIndex, int minMHz, int maxMHz )
{
  return callMethod< bool, int, int, int >( "SetNvidiaVramLockedClocks",
      deviceIndex, minMHz, maxMHz ).value_or( false );
}

bool UccdClient::resetNvidiaGpuLockedClocks( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaGpuLockedClocks", deviceIndex ).value_or( false );
}

bool UccdClient::resetNvidiaVramLockedClocks( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaVramLockedClocks", deviceIndex ).value_or( false );
}

bool UccdClient::resetNvidiaAllClockOffsets( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaAllClockOffsets", deviceIndex ).value_or( false );
}

bool UccdClient::setNvidiaGpuPowerLimit( int deviceIndex, double watts )
{
  return callMethod< bool, int, double >( "SetNvidiaGpuPowerLimit",
      deviceIndex, watts ).value_or( false );
}

bool UccdClient::resetNvidiaGpuPowerLimit( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaGpuPowerLimit", deviceIndex ).value_or( false );
}

bool UccdClient::applyNvidiaGpuOCProfile( const std::string &profileJSON, int deviceIndex )
{
  return callMethod< bool, QString, int >( "ApplyNvidiaGpuOCProfile",
      QString::fromStdString( profileJSON ), deviceIndex ).value_or( false );
}

bool UccdClient::resetNvidiaGpuOCAll( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaGpuOCAll", deviceIndex ).value_or( false );
}

// ---------------------------------------------------------------------------
// Auto-OC
// ---------------------------------------------------------------------------

bool UccdClient::startAutoOC( const std::string &component, int deviceIndex,
                              int stepSizeMHz, int maxOffsetMHz, int stabilityMs )
{
  return callMethod< bool, QString, int, int, int, int >( "StartAutoOC",
      QString::fromStdString( component ), deviceIndex,
      stepSizeMHz, maxOffsetMHz, stabilityMs ).value_or( false );
}

bool UccdClient::resumeAutoOC( const std::string &mode,
                                const std::string &component, int deviceIndex,
                                int stepSizeMHz, int maxOffsetMHz, int stabilityMs )
{
  return callMethod< bool, QString, QString, int, int, int, int >( "ResumeAutoOC",
      QString::fromStdString( mode ),
      QString::fromStdString( component ), deviceIndex,
      stepSizeMHz, maxOffsetMHz, stabilityMs ).value_or( false );
}

bool UccdClient::pauseAutoOC()
{
  return callMethod< bool >( "PauseAutoOC" ).value_or( false );
}

bool UccdClient::stopAutoOC()
{
  return callMethod< bool >( "StopAutoOC" ).value_or( false );
}

std::optional< bool > UccdClient::getAutoOCRunning()
{
  return callMethod< bool >( "GetAutoOCRunning" );
}

std::optional< QVariantMap > UccdClient::getAutoOCProgress()
{
  return callMethod< QVariantMap >( "GetAutoOCProgress" );
}

bool UccdClient::hasAutoOCCheckpoint()
{
  return callMethod< bool >( "HasAutoOCCheckpoint" ).value_or( false );
}

bool UccdClient::clearAutoOCCheckpoint()
{
  return callMethod< bool >( "ClearAutoOCCheckpoint" ).value_or( false );
}

void UccdClient::subscribeAutoOCProgress( AutoOCProgressCallback callback )
{
  m_autoOCProgressCallback = std::move( callback );
  connect( this, &UccdClient::autoOCProgressChanged, this,
    [this]( const QVariantMap &progress ) {
      if ( m_autoOCProgressCallback )
        m_autoOCProgressCallback( progress );
    } );
}

void UccdClient::subscribeAutoOCFinished( AutoOCFinishedCallback callback )
{
  m_autoOCFinishedCallback = std::move( callback );
  connect( this, &UccdClient::autoOCFinished, this,
    [this]( int coreOffset, int vramOffset, bool success, const QString &msg ) {
      if ( m_autoOCFinishedCallback )
        m_autoOCFinishedCallback( coreOffset, vramOffset, success, msg.toStdString() );
    } );
}

// ---------------------------------------------------------------------------
// NVIDIA Auto-Undervolt
// ---------------------------------------------------------------------------

bool UccdClient::startAutoUndervolt( int deviceIndex,
                                     bool targetFpsEnabled,
                                     int  targetFps,
                                     bool extendedValidation,
                                     bool powerLimitMode,
                                     int  stepSizeMHz,
                                     int  maxOffsetMHz,
                                     int  stabilityMs )
{
  return callMethod< bool, int, bool, int, bool, bool, int, int, int >(
    "StartAutoUndervolt", deviceIndex, targetFpsEnabled, targetFps,
    extendedValidation, powerLimitMode, stepSizeMHz, maxOffsetMHz, stabilityMs ).value_or( false );
}

bool UccdClient::resumeAutoUndervolt( const std::string &mode,
                                       int deviceIndex,
                                       bool targetFpsEnabled,
                                       int  targetFps,
                                       bool extendedValidation,
                                       bool powerLimitMode,
                                       int  stepSizeMHz,
                                       int  maxOffsetMHz,
                                       int  stabilityMs )
{
  return callMethod< bool, QString, int, bool, int, bool, bool, int, int, int >(
    "ResumeAutoUndervolt", QString::fromStdString( mode ),
    deviceIndex, targetFpsEnabled, targetFps,
    extendedValidation, powerLimitMode, stepSizeMHz, maxOffsetMHz, stabilityMs ).value_or( false );
}

std::optional< QVariantMap > UccdClient::getAutoUndervoltProgress()
{
  return callMethod< QVariantMap >( "GetAutoUndervoltProgress" );
}

std::optional< std::string > UccdClient::getAutoUndervoltProfiles()
{
  if ( auto result = callMethod< QString >( "GetAutoUndervoltProfiles" ) )
    return result->toStdString();
  return std::nullopt;
}

bool UccdClient::hasAutoUndervoltCheckpoint()
{
  return callMethod< bool >( "HasAutoUndervoltCheckpoint" ).value_or( false );
}

bool UccdClient::clearAutoUndervoltCheckpoint()
{
  return callMethod< bool >( "ClearAutoUndervoltCheckpoint" ).value_or( false );
}

void UccdClient::subscribeAutoUndervoltProgress( AutoUndervoltProgressCallback callback )
{
  m_autoUndervoltProgressCallback = std::move( callback );
  connect( this, &UccdClient::autoUndervoltProgressChanged, this,
    [this]( const QVariantMap &progress ) {
      if ( m_autoUndervoltProgressCallback )
        m_autoUndervoltProgressCallback( progress );
    } );
}

void UccdClient::subscribeAutoUndervoltFinished( AutoUndervoltFinishedCallback callback )
{
  m_autoUndervoltFinishedCallback = std::move( callback );
  connect( this, &UccdClient::autoUndervoltFinished, this,
    [this]( int gpuFreqCapMHz, bool success, const QString &msg, const QString &appName ) {
      if ( m_autoUndervoltFinishedCallback )
        m_autoUndervoltFinishedCallback( gpuFreqCapMHz, success, msg.toStdString(), appName.toStdString() );
    } );
}

bool UccdClient::setKeyboardBacklight( const std::string &config )
{
  return callMethod< bool, QString >( "SetKeyboardBacklightStatesJSON", QString::fromStdString( config ) ).value_or( false );
}

std::optional< std::string > UccdClient::getKeyboardBacklightInfo()
{
  if ( auto caps = callMethod< QString >( "GetKeyboardBacklightCapabilitiesJSON" ); caps )
  {
    return caps->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getKeyboardBacklightStates()
{
  if ( auto states = callMethod< QString >( "GetKeyboardBacklightStatesJSON" ); states )
  {
    return states->toStdString();
  }
  return std::nullopt;
}

bool UccdClient::setODMPerformanceProfile( [[maybe_unused]] const std::string &profile )
{
  return false;
}

std::optional< std::string > UccdClient::getODMPerformanceProfile()
{
  return std::nullopt;
}

std::optional< std::vector< std::string > > UccdClient::getAvailableODMProfiles()
{
  return std::nullopt;
}

namespace
{
std::optional< int > readFanDataValue( QDBusInterface *iface, const QString &method, const QString &key )
{
  if ( !iface )
  {
    return std::nullopt;
  }

  QDBusMessage reply = iface->call( method );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
  {
    return std::nullopt;
  }

  // The adaptor returns QVariantMap (D-Bus a{sv}).
  // Each value in the outer map is a variant wrapping another a{sv}.
  // Demarshall the outer map first.
  QVariantMap outerMap = qdbus_cast< QVariantMap >( reply.arguments().at( 0 ).value< QDBusArgument >() );
  if ( !outerMap.contains( key ) )
    return std::nullopt;

  // The inner value is a variant containing a QDBusArgument for the nested a{sv}.
  QVariant innerVariant = outerMap.value( key );
  QVariantMap innerMap;
  if ( innerVariant.canConvert< QDBusArgument >() )
    innerMap = qdbus_cast< QVariantMap >( innerVariant.value< QDBusArgument >() );
  else
    innerMap = innerVariant.toMap();

  if ( !innerMap.contains( "data" ) )
    return std::nullopt;

  int value = innerMap.value( "data" ).toInt();

  // Treat entries with a zero timestamp as missing data
  if ( innerMap.contains( "timestamp" ) )
  {
    qint64 ts = innerMap.value( "timestamp" ).toLongLong();
    if ( ts == 0 )
    {
      // Silently treat as missing data - this is normal during startup
      // before fan monitoring worker has collected initial readings
      return std::nullopt;
    }
  }

  if ( value >= 0 )
    return value;

  return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// JSON snapshot caches — one D-Bus round-trip per poll cycle instead of N
// ---------------------------------------------------------------------------

void UccdClient::refreshDGpuSnapshot()
{
  using clock = std::chrono::steady_clock;
  auto now = clock::now();
  if ( m_dGpuSnap.valid &&
       std::chrono::duration_cast< std::chrono::milliseconds >( now - m_dGpuSnap.ts ).count() < SNAPSHOT_TTL_MS )
    return;

  m_dGpuSnap = {};
  if ( !m_interface )
    return;

  QDBusMessage reply = m_interface->call( QStringLiteral( "GetDGpuInfoValuesJSON" ) );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
    return;

  QJsonDocument doc = QJsonDocument::fromJson( reply.arguments().at( 0 ).toString().toUtf8() );
  if ( doc.isNull() || !doc.isObject() )
    return;

  const QJsonObject obj = doc.object();
  m_dGpuSnap.ts    = now;
  m_dGpuSnap.valid = true;
  m_dGpuSnap.temp            = obj[ "temp" ].toInt( -1 );
  m_dGpuSnap.coreFrequency   = obj.contains( "coreFrequency" ) ? obj[ "coreFrequency" ].toInt( -1 )
                                                                 : obj[ "coreFreq" ].toInt( -1 );
  m_dGpuSnap.vramFrequency   = obj[ "vramFrequency" ].toInt( -1 );
  m_dGpuSnap.powerDraw       = obj[ "powerDraw" ].toDouble( -1.0 );
  m_dGpuSnap.computeUtilPct  = obj[ "computeUtilPct" ].toInt( -1 );
  m_dGpuSnap.memoryUtilPct   = obj[ "memoryUtilPct" ].toInt( -1 );
  m_dGpuSnap.vramUsedMiB     = obj[ "vramUsedMiB" ].toInt( -1 );
  m_dGpuSnap.vramTotalMiB    = obj[ "vramTotalMiB" ].toInt( -1 );
  m_dGpuSnap.perfLimitReason = obj[ "perfLimitReason" ].toString().toStdString();
  m_dGpuSnap.encoderUtilPct  = obj[ "encoderUtilPct" ].toInt( -1 );
  m_dGpuSnap.decoderUtilPct  = obj[ "decoderUtilPct" ].toInt( -1 );
  m_dGpuSnap.currentPstate   = obj[ "currentPstate" ].toInt( -1 );
  m_dGpuSnap.grClockOffsetMHz  = obj[ "grClockOffsetMHz" ].toInt( -999 );
  m_dGpuSnap.memClockOffsetMHz = obj[ "memClockOffsetMHz" ].toInt( -999 );
  m_dGpuSnap.coreVoltageMv   = obj[ "coreVoltageMv" ].toInt( -1 );
  m_dGpuSnap.fanSpeedPct     = obj[ "fanSpeedPct" ].toInt( -1 );
}

void UccdClient::refreshIGpuSnapshot()
{
  using clock = std::chrono::steady_clock;
  auto now = clock::now();
  if ( m_iGpuSnap.valid &&
       std::chrono::duration_cast< std::chrono::milliseconds >( now - m_iGpuSnap.ts ).count() < SNAPSHOT_TTL_MS )
    return;

  m_iGpuSnap = {};
  if ( !m_interface )
    return;

  QDBusMessage reply = m_interface->call( QStringLiteral( "GetIGpuInfoValuesJSON" ) );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
    return;

  QJsonDocument doc = QJsonDocument::fromJson( reply.arguments().at( 0 ).toString().toUtf8() );
  if ( doc.isNull() || !doc.isObject() )
    return;

  const QJsonObject obj = doc.object();
  m_iGpuSnap.ts    = now;
  m_iGpuSnap.valid = true;
  m_iGpuSnap.temp          = obj[ "temp" ].toInt( -1 );
  m_iGpuSnap.coreFrequency = obj[ "coreFrequency" ].toInt( -1 );
  m_iGpuSnap.powerDraw     = obj[ "powerDraw" ].toDouble( -1.0 );
}

void UccdClient::refreshCpuPowerSnapshot()
{
  using clock = std::chrono::steady_clock;
  auto now = clock::now();
  if ( m_cpuPowerSnap.valid &&
       std::chrono::duration_cast< std::chrono::milliseconds >( now - m_cpuPowerSnap.ts ).count() < SNAPSHOT_TTL_MS )
    return;

  m_cpuPowerSnap = {};
  if ( !m_interface )
    return;

  QDBusMessage reply = m_interface->call( QStringLiteral( "GetCpuPowerValuesJSON" ) );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
    return;

  QJsonDocument doc = QJsonDocument::fromJson( reply.arguments().at( 0 ).toString().toUtf8() );
  if ( doc.isNull() || !doc.isObject() )
    return;

  const QJsonObject obj = doc.object();
  m_cpuPowerSnap.ts    = now;
  m_cpuPowerSnap.valid = true;
  m_cpuPowerSnap.powerDraw = obj[ "powerDraw" ].toDouble( -1.0 );
}

// ---------------------------------------------------------------------------
// System Monitoring implementations (using snapshot caches)
// ---------------------------------------------------------------------------

std::optional< int > UccdClient::getCpuTemperature()
{
  return readFanDataValue( m_interface.get(), "GetFanDataCPU", "temp" );
}

std::optional< int > UccdClient::getGpuTemperature()
{
  refreshDGpuSnapshot();
  if ( m_dGpuSnap.valid && m_dGpuSnap.temp >= 0 )
    return m_dGpuSnap.temp;

  refreshIGpuSnapshot();
  return ( m_iGpuSnap.valid && m_iGpuSnap.temp >= 0 )
    ? std::optional< int >( m_iGpuSnap.temp ) : std::nullopt;
}

std::optional< int > UccdClient::getIGpuTemperature()
{
  refreshIGpuSnapshot();
  return ( m_iGpuSnap.valid && m_iGpuSnap.temp >= 0 )
    ? std::optional< int >( m_iGpuSnap.temp ) : std::nullopt;
}

std::optional< int > UccdClient::getCpuFrequency()
{
  return callMethod< int >( "GetCpuFrequencyMHz" );
}

std::optional< int > UccdClient::getGpuFrequency()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.coreFrequency >= 0 )
    ? std::optional< int >( m_dGpuSnap.coreFrequency ) : std::nullopt;
}

std::optional< int > UccdClient::getIGpuFrequency()
{
  refreshIGpuSnapshot();
  return ( m_iGpuSnap.valid && m_iGpuSnap.coreFrequency >= 0 )
    ? std::optional< int >( m_iGpuSnap.coreFrequency ) : std::nullopt;
}

std::optional< double > UccdClient::getCpuPower()
{
  refreshCpuPowerSnapshot();
  return ( m_cpuPowerSnap.valid && m_cpuPowerSnap.powerDraw >= 0.0 )
    ? std::optional< double >( m_cpuPowerSnap.powerDraw ) : std::nullopt;
}

std::optional< double > UccdClient::getGpuPower()
{
  refreshDGpuSnapshot();
  if ( m_dGpuSnap.valid && m_dGpuSnap.powerDraw >= 0.0 )
    return m_dGpuSnap.powerDraw;

  refreshIGpuSnapshot();
  return ( m_iGpuSnap.valid && m_iGpuSnap.powerDraw >= 0.0 )
    ? std::optional< double >( m_iGpuSnap.powerDraw ) : std::nullopt;
}

std::optional< double > UccdClient::getIGpuPower()
{
  refreshIGpuSnapshot();
  return ( m_iGpuSnap.valid && m_iGpuSnap.powerDraw >= 0.0 )
    ? std::optional< double >( m_iGpuSnap.powerDraw ) : std::nullopt;
}

// ---- Extended discrete GPU metrics (all from cached dGPU snapshot) ----

std::optional< int > UccdClient::getDGpuComputeUtilPct()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.computeUtilPct >= 0 )
    ? std::optional< int >( m_dGpuSnap.computeUtilPct ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuMemoryUtilPct()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.memoryUtilPct >= 0 )
    ? std::optional< int >( m_dGpuSnap.memoryUtilPct ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuVramUsedMiB()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.vramUsedMiB >= 0 )
    ? std::optional< int >( m_dGpuSnap.vramUsedMiB ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuVramTotalMiB()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.vramTotalMiB >= 0 )
    ? std::optional< int >( m_dGpuSnap.vramTotalMiB ) : std::nullopt;
}

std::optional< std::string > UccdClient::getDGpuPerfLimitReason()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && !m_dGpuSnap.perfLimitReason.empty() )
    ? std::optional< std::string >( m_dGpuSnap.perfLimitReason ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuEncoderUtilPct()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.encoderUtilPct >= 0 )
    ? std::optional< int >( m_dGpuSnap.encoderUtilPct ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuDecoderUtilPct()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.decoderUtilPct >= 0 )
    ? std::optional< int >( m_dGpuSnap.decoderUtilPct ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuCurrentPstate()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.currentPstate >= 0 )
    ? std::optional< int >( m_dGpuSnap.currentPstate ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuGrClockOffsetMHz()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.grClockOffsetMHz != -999 )
    ? std::optional< int >( m_dGpuSnap.grClockOffsetMHz ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuMemClockOffsetMHz()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.memClockOffsetMHz != -999 )
    ? std::optional< int >( m_dGpuSnap.memClockOffsetMHz ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuVramFrequencyMHz()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.vramFrequency >= 0 )
    ? std::optional< int >( m_dGpuSnap.vramFrequency ) : std::nullopt;
}

std::optional< int > UccdClient::getDGpuCoreVoltageMv()
{
  refreshDGpuSnapshot();
  return ( m_dGpuSnap.valid && m_dGpuSnap.coreVoltageMv >= 0 )
    ? std::optional< int >( m_dGpuSnap.coreVoltageMv ) : std::nullopt;
}

std::optional< int > UccdClient::getFanSpeedRPM()
{
  if ( auto percentage = readFanDataValue( m_interface.get(), "GetFanDataCPU", "speed" ) )
  {
    return ( *percentage ) * 60;
  }
  return std::nullopt;
}

std::optional< int > UccdClient::getGpuFanSpeedRPM()
{
  auto gpu1 = readFanDataValue( m_interface.get(), "GetFanDataGPU1", "speed" );
  auto gpu2 = readFanDataValue( m_interface.get(), "GetFanDataGPU2", "speed" );

  if ( gpu1 && gpu2 )
  {
    return static_cast< int >( ( *gpu1 + *gpu2 ) / 2 ) * 60;
  }
  if ( gpu1 )
  {
    return ( *gpu1 ) * 60;
  }
  if ( gpu2 )
  {
    return ( *gpu2 ) * 60;
  }
  return std::nullopt;
}

// Return raw fan speed percentage (0-100) as reported by uccd
std::optional< int > UccdClient::getFanSpeedPercent()
{
  return readFanDataValue( m_interface.get(), "GetFanDataCPU", "speed" );
}

std::optional< int > UccdClient::getGpuFanSpeedPercent()
{
  auto gpu1 = readFanDataValue( m_interface.get(), "GetFanDataGPU1", "speed" );
  auto gpu2 = readFanDataValue( m_interface.get(), "GetFanDataGPU2", "speed" );

  if ( gpu1 && gpu2 )
  {
    return static_cast< int >( ( *gpu1 + *gpu2 ) / 2 );
  }
  if ( gpu1 )
  {
    return *gpu1;
  }
  if ( gpu2 )
  {
    return *gpu2;
  }
  return std::nullopt;
}

// Water cooler control
bool UccdClient::setWaterCoolerFanSpeed( int dutyCyclePercent )
{
  return callMethod< bool, int >( "SetWaterCoolerFanSpeed", dutyCyclePercent ).value_or( false );
}

bool UccdClient::setWaterCoolerPumpVoltage( int voltageCode )
{
  return callMethod< bool, int >( "SetWaterCoolerPumpVoltage", voltageCode ).value_or( false );
}

bool UccdClient::setWaterCoolerLEDColor( int r, int g, int b, int mode )
{
  return callMethod< bool, int, int, int, int >( "SetWaterCoolerLEDColor", r, g, b, mode ).value_or( false );
}

bool UccdClient::turnOffWaterCoolerLED()
{
  return callMethod< bool >( "TurnOffWaterCoolerLED" ).value_or( false );
}

// Water cooler readings
std::optional< int > UccdClient::getWaterCoolerFanSpeed()
{
  return callMethod< int >( "GetWaterCoolerFanSpeed" );
}

std::optional< int > UccdClient::getWaterCoolerPumpLevel()
{
  return callMethod< int >( "GetWaterCoolerPumpLevel" );
}

// --- Monitoring history ---

std::optional< QByteArray > UccdClient::getMonitorDataSince( qint64 sinceTimestampMs )
{
  return callMethod< QByteArray >( "GetMonitorDataSince", static_cast< qlonglong >( sinceTimestampMs ) );
}

bool UccdClient::setMonitorHistoryHorizon( int seconds )
{
  return callVoidMethod( "SetMonitorHistoryHorizon", seconds );
}

std::optional< int > UccdClient::getMonitorHistoryHorizon()
{
  return callMethod< int >( "GetMonitorHistoryHorizon" );
}

std::optional< std::string > UccdClient::getMonitorSourcesJSON()
{
  if ( auto result = callMethod< QString >( "GetMonitorSourcesJSON" ) )
    return result->toStdString();
  return std::nullopt;
}

std::optional< QVariantMap > UccdClient::getFpsSources()
{
  return callMethod< QVariantMap >( "GetFpsSources" );
}

std::optional< QVariantMap > UccdClient::getAutoUvAutoApplyStatus()
{
  return callMethod< QVariantMap >( "GetAutoUvAutoApplyStatus" );
}

bool UccdClient::setFpsSourceApp( const std::string &appName )
{
  return callMethod< bool, QString >( "SetFpsSourceApp", QString::fromStdString( appName ) ).value_or( false );
}

std::optional< std::string > UccdClient::getFpsSourceApp()
{
  if ( auto result = callMethod< QString >( "GetFpsSourceApp" ) )
    return result->toStdString();
  return std::nullopt;
}

void UccdClient::subscribeProfileChanged( [[maybe_unused]] ProfileChangedCallback callback )
{
  // Already handled via Qt signal connection
}

void UccdClient::subscribePowerStateChanged( [[maybe_unused]] PowerStateChangedCallback callback )
{
  // Already handled via Qt signal connection
}

// Explicit template instantiations — required because the template bodies are
// defined in this .cpp file rather than the header.  The linker (especially
// with LTO) needs these to satisfy external callers that invoke the methods
// via inline wrappers declared in UccdClient.hpp.
template std::optional< bool >    UccdClient::callMethod< bool >( const QString & ) const;
template std::optional< int >     UccdClient::callMethod< int  >( const QString & ) const;
template std::optional< QString > UccdClient::callMethod< QString >( const QString & ) const;
template std::optional< QVariantMap > UccdClient::callMethod< QVariantMap >( const QString & ) const;
template std::optional< QVariantList > UccdClient::callMethod< QVariantList >( const QString & ) const;

template std::optional< bool > UccdClient::callMethod< bool, QString >( const QString &, const QString & ) const;
template std::optional< bool > UccdClient::callMethod< bool, QString, QString >( const QString &, const QString &, const QString & ) const;
template std::optional< bool > UccdClient::callMethod< bool, QString, QString, QString >( const QString &, const QString &, const QString &, const QString & ) const;
template std::optional< bool > UccdClient::callMethod< bool, int >( const QString &, const int & ) const;
template std::optional< QString > UccdClient::callMethod< QString, QString >( const QString &, const QString & ) const;

template bool UccdClient::callVoidMethod< QString >( const QString &, const QString & ) const;
template bool UccdClient::callVoidMethod< int >( const QString &, const int & ) const;
template bool UccdClient::callVoidMethod< bool >( const QString &, const bool & ) const;
template bool UccdClient::callVoidMethod< QString, QString >( const QString &, const QString &, const QString & ) const;
template bool UccdClient::callVoidMethod< QString, QString, QString >( const QString &, const QString &, const QString &, const QString & ) const;

} // namespace ucc
