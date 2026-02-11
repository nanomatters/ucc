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
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QThread>
#include <QFile>
#include <QVariantMap>
#include <QRegularExpression>
#include <QSet>

namespace ucc
{

namespace
{
QSet< QString > loadSupportedMethods( QDBusInterface *interface, bool &ok )
{
  ok = false;
  QSet< QString > methods;
  if ( !interface || !interface->isValid() )
  {
    return methods;
  }

  QDBusInterface introspectIface(
    interface->service(),
    interface->path(),
    "org.freedesktop.DBus.Introspectable",
    interface->connection() );

  QDBusMessage reply = introspectIface.call( "Introspect" );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
  {
    return methods;
  }

  const QString xml = reply.arguments().at( 0 ).toString();
  QRegularExpression re( R"(<method name=\"([^\"]+)\")" );
  auto it = re.globalMatch( xml );
  while ( it.hasNext() )
  {
    methods.insert( it.next().captured( 1 ) );
  }

  ok = true;
  return methods;
}

bool hasMethod( QDBusInterface *interface, const QString &method )
{
  if ( !interface || !interface->isValid() )
    return false;

  // Introspect the interface on each call to avoid stale results from a
  // previous failed introspection attempt. The DBus service may become
  // available after the client is constructed, so a static cache causes
  // false negatives.
  bool ok = false;
  QSet< QString > methods = loadSupportedMethods( interface, ok );
  if ( !ok )
    return false;
  return methods.contains( method );
}
} // namespace

UccdClient::UccdClient( QObject *parent )
  : QObject( parent )
  , m_interface( std::make_unique< QDBusInterface >(
      DBUS_SERVICE,
      DBUS_PATH,
      DBUS_INTERFACE,
      QDBusConnection::systemBus(),
      this ) )
{
  m_connected = m_interface->isValid();

  if ( m_connected )
  {
    // Subscribe to signals
    QDBusConnection::systemBus().connect(
      DBUS_SERVICE,
      DBUS_PATH,
      DBUS_INTERFACE,
      "ProfileChanged",
      this,
      SLOT( onProfileChangedSignal( QString ) )
    );

    QDBusConnection::systemBus().connect(
      DBUS_SERVICE,
      DBUS_PATH,
      DBUS_INTERFACE,
      "PowerStateChanged",
      this,
      SLOT( onPowerStateChangedSignal( QString ) )
    );
  }
  else
  {
    qWarning() << "Failed to connect to uccd DBus service:"
               << m_interface->lastError().message();
  }

  emit connectionStatusChanged( m_connected );
}

bool UccdClient::isConnected() const
{
  return m_connected && m_interface->isValid();
}

// Signal handlers
void UccdClient::onProfileChangedSignal( const QString &profileId )
{
  emit profileChanged( profileId );
}

void UccdClient::onPowerStateChangedSignal( const QString &state )
{
  emit powerStateChanged( state );
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

// Profile Management
std::optional< std::string > UccdClient::getDefaultProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetDefaultProfilesJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getCpuFrequencyLimitsJSON()
{
  if ( auto result = callMethod< QString >( "GetCpuFrequencyLimitsJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getDefaultValuesProfileJSON()
{
  if ( auto result = callMethod< QString >( "GetDefaultValuesProfileJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getCustomProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetCustomProfilesJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getActiveProfileJSON()
{
  if ( auto result = callMethod< QString >( "GetActiveProfileJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getSettingsJSON()
{
  if ( auto result = callMethod< QString >( "GetSettingsJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getPowerState()
{
  if ( auto result = callMethod< QString >( "GetPowerState" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

bool UccdClient::setStateMap( const std::string &state, const std::string &profileId )
{
  const QString qState = QString::fromStdString( state );
  const QString qProfileId = QString::fromStdString( profileId );
  return callVoidMethod( "SetStateMap", qState, qProfileId );
}

bool UccdClient::setActiveProfile( const std::string &profileId )
{
  const QString id = QString::fromStdString( profileId );
  if ( hasMethod( m_interface.get(), "SetActiveProfile" ) )
  {
    return callVoidMethod( "SetActiveProfile", id );
  }
  if ( hasMethod( m_interface.get(), "SetTempProfileById" ) )
  {
    return callVoidMethod( "SetTempProfileById", id );
  }

  return false;
}

bool UccdClient::applyProfile( const std::string &profileJSON )
{
  return callVoidMethod( "ApplyProfile", QString::fromStdString( profileJSON ) );
}

bool UccdClient::saveCustomProfile( [[maybe_unused]] [[maybe_unused]] const std::string &profileJSON )
{
  return callVoidMethod( "SaveCustomProfile", QString::fromStdString( profileJSON ) );
}

bool UccdClient::deleteCustomProfile( [[maybe_unused]] const std::string &profileId )
{
  return callVoidMethod( "DeleteCustomProfile", QString::fromStdString( profileId ) );
}

std::optional< std::string > UccdClient::getFanProfile( const std::string &name )
{
  if ( auto result = callMethod< QString >( "GetFanProfile", QString::fromStdString( name ) ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::vector< std::string > > UccdClient::getFanProfileNames()
{
  if ( auto result = callMethod< QString >( "GetFanProfileNames" ) )
  {
    // Parse JSON array
    QJsonDocument doc = QJsonDocument::fromJson( result->toUtf8() );
    if ( doc.isArray() )
    {
      std::vector< std::string > names;
      for ( const auto &value : doc.array() )
      {
        if ( value.isString() )
        {
          names.push_back( value.toString().toStdString() );
        }
      }
      return names;
    }
  }
  return std::nullopt;
}

std::optional< bool > UccdClient::setFanProfile( const std::string &name, const std::string &json )
{ return callMethod< bool >( "SetFanProfile", QString::fromStdString( name ), QString::fromStdString( json ) ); }

bool UccdClient::setDisplayBrightness( int brightness )
{ return callVoidMethod( "SetDisplayBrightness", brightness ); }

std::optional< int > UccdClient::getDisplayBrightness()
{
  if ( hasMethod( m_interface.get(), "GetDisplayBrightness" ) )
  {
    return callMethod< int >( "GetDisplayBrightness" );
  }

  return std::nullopt;
}

bool UccdClient::setWebcamEnabled( bool enabled )
{
  if ( hasMethod( m_interface.get(), "SetWebcam" ) )
  {
    return callVoidMethod( "SetWebcam", enabled );
  }

  return false;
}

std::optional< bool > UccdClient::getWebcamEnabled()
{
  if ( hasMethod( m_interface.get(), "GetWebcamSWStatus" ) )
  {
    return callMethod< bool >( "GetWebcamSWStatus" );
  }
  if ( hasMethod( m_interface.get(), "GetWebcam" ) )
  {
    return callMethod< bool >( "GetWebcam" );
  }

  return std::nullopt;
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
  if ( hasMethod( m_interface.get(), "SetFnLockStatus" ) )
  {
    return callVoidMethod( "SetFnLockStatus", enabled );
  }
  if ( hasMethod( m_interface.get(), "SetFnLock" ) )
  {
    return callVoidMethod( "SetFnLock", enabled );
  }

  return false;
}

std::optional< bool > UccdClient::getFnLock()
{
  if ( hasMethod( m_interface.get(), "GetFnLockStatus" ) )
  {
    return callMethod< bool >( "GetFnLockStatus" );
  }
  if ( hasMethod( m_interface.get(), "GetFnLock" ) )
  {
    return callMethod< bool >( "GetFnLock" );
  }

  return std::nullopt;
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

bool UccdClient::setFanProfile( [[maybe_unused]] [[maybe_unused]] const std::string &profileJSON )
{
  return false;
}

bool UccdClient::setFanProfileCPU( const std::string &pointsJSON )
{
  const QString js = QString::fromStdString( pointsJSON );
  if ( hasMethod( m_interface.get(), "SetFanProfileCPU" ) )
  {
    return callMethod< bool, QString >( "SetFanProfileCPU", js ).value_or( false );
  }
  return false;
}

bool UccdClient::setFanProfileDGPU( const std::string &pointsJSON )
{
  const QString js = QString::fromStdString( pointsJSON );
  if ( hasMethod( m_interface.get(), "SetFanProfileDGPU" ) )
  {
    return callMethod< bool, QString >( "SetFanProfileDGPU", js ).value_or( false );
  }
  return false;
}

bool UccdClient::applyFanProfiles( const std::string &fanProfilesJSON )
{
  const QString js = QString::fromStdString( fanProfilesJSON );
  if ( hasMethod( m_interface.get(), "ApplyFanProfiles" ) )
  {
    return callMethod< bool, QString >( "ApplyFanProfiles", js ).value_or( false );
  }
  return false;
}

bool UccdClient::revertFanProfiles()
{
  if ( hasMethod( m_interface.get(), "RevertFanProfiles" ) )
  {
    return callMethod< bool >( "RevertFanProfiles" ).value_or( false );
  }
  return false;
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

std::optional< std::vector< int > > UccdClient::getODMPowerLimits()
{
  auto jsonStr = callMethod< QString >( "ODMPowerLimitsJSON" );
  if ( !jsonStr )
    return std::nullopt;

  QJsonDocument doc = QJsonDocument::fromJson( jsonStr->toUtf8() );
  if ( !doc.isArray() )
    return std::nullopt;

  QJsonArray array = doc.array();
  std::vector< int > limits;
  for ( const QJsonValue &value : array )
  {
    if ( value.isObject() )
    {
      QJsonObject obj = value.toObject();
      if ( obj.contains( "max" ) && obj["max"].isDouble() )
      {
        limits.push_back( obj["max"].toInt() );
      }
    }
  }
  return limits;
}

bool UccdClient::setChargingProfile( const std::string &profileDescriptor )
{
  if ( !hasMethod( m_interface.get(), "SetChargingProfile" ) )
    return false;

  return callVoidMethod( "SetChargingProfile", QString::fromStdString( profileDescriptor ) );
}

std::optional< std::string > UccdClient::getChargingProfilesAvailable()
{
  if ( !hasMethod( m_interface.get(), "GetChargingProfilesAvailable" ) )
    return std::nullopt;

  if ( auto result = callMethod< QString >( "GetChargingProfilesAvailable" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getCurrentChargingProfile()
{
  if ( !hasMethod( m_interface.get(), "GetCurrentChargingProfile" ) )
    return std::nullopt;

  if ( auto result = callMethod< QString >( "GetCurrentChargingProfile" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getChargingPrioritiesAvailable()
{
  if ( !hasMethod( m_interface.get(), "GetChargingPrioritiesAvailable" ) )
    return std::nullopt;

  if ( auto result = callMethod< QString >( "GetChargingPrioritiesAvailable" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getCurrentChargingPriority()
{
  if ( !hasMethod( m_interface.get(), "GetCurrentChargingPriority" ) )
    return std::nullopt;

  if ( auto result = callMethod< QString >( "GetCurrentChargingPriority" ) )
    return result->toStdString();

  return std::nullopt;
}

bool UccdClient::setChargingPriority( const std::string &priorityDescriptor )
{
  if ( !hasMethod( m_interface.get(), "SetChargingPriority" ) )
    return false;

  return callVoidMethod( "SetChargingPriority", QString::fromStdString( priorityDescriptor ) );
}

std::optional< std::string > UccdClient::getChargeStartAvailableThresholds()
{
  if ( !hasMethod( m_interface.get(), "GetChargeStartAvailableThresholds" ) )
    return std::nullopt;

  if ( auto result = callMethod< QString >( "GetChargeStartAvailableThresholds" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getChargeEndAvailableThresholds()
{
  if ( !hasMethod( m_interface.get(), "GetChargeEndAvailableThresholds" ) )
    return std::nullopt;

  if ( auto result = callMethod< QString >( "GetChargeEndAvailableThresholds" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< int > UccdClient::getChargeStartThreshold()
{
  if ( !hasMethod( m_interface.get(), "GetChargeStartThreshold" ) )
    return std::nullopt;

  return callMethod< int >( "GetChargeStartThreshold" );
}

std::optional< int > UccdClient::getChargeEndThreshold()
{
  if ( !hasMethod( m_interface.get(), "GetChargeEndThreshold" ) )
    return std::nullopt;

  return callMethod< int >( "GetChargeEndThreshold" );
}

bool UccdClient::setChargeStartThreshold( int value )
{
  if ( !hasMethod( m_interface.get(), "SetChargeStartThreshold" ) )
    return false;

  return callVoidMethod( "SetChargeStartThreshold", value );
}

bool UccdClient::setChargeEndThreshold( int value )
{
  if ( !hasMethod( m_interface.get(), "SetChargeEndThreshold" ) )
    return false;

  return callVoidMethod( "SetChargeEndThreshold", value );
}

std::optional< std::string > UccdClient::getChargeType()
{
  if ( !hasMethod( m_interface.get(), "GetChargeType" ) )
    return std::nullopt;

  if ( auto result = callMethod< QString >( "GetChargeType" ) )
    return result->toStdString();

  return std::nullopt;
}

bool UccdClient::setChargeType( const std::string &type )
{
  if ( !hasMethod( m_interface.get(), "SetChargeType" ) )
    return false;

  return callVoidMethod( "SetChargeType", QString::fromStdString( type ) );
}

bool UccdClient::setNVIDIAPowerOffset( [[maybe_unused]] int offset )
{
  return false;
}

std::optional< int > UccdClient::getNVIDIAPowerOffset()
{
  return std::nullopt;
}

std::optional< int > UccdClient::getNVIDIAPowerCTRLMaxPowerLimit()
{
  return callMethod< int >( "GetNVIDIAPowerCTRLMaxPowerLimit" );
}

bool UccdClient::setPrimeProfile( [[maybe_unused]] const std::string &profile )
{
  return false;
}

std::optional< std::string > UccdClient::getPrimeProfile()
{
  return std::nullopt;
}

bool UccdClient::setKeyboardBacklight( const std::string &config )
{
  if ( hasMethod( m_interface.get(), "SetKeyboardBacklightStatesJSON" ) )
  {
    return callMethod< bool, QString >( "SetKeyboardBacklightStatesJSON", QString::fromStdString( config ) ).value_or( false );
  }

  return false;
}

std::optional< std::string > UccdClient::getKeyboardBacklightInfo()
{
  if ( hasMethod( m_interface.get(), "GetKeyboardBacklightCapabilitiesJSON" ) )
  {
    auto caps = callMethod< QString >( "GetKeyboardBacklightCapabilitiesJSON" );
    if ( caps )
    {
      return caps->toStdString();
    }
  }

  return std::nullopt;
}

std::optional< std::string > UccdClient::getKeyboardBacklightStates()
{
  if ( hasMethod( m_interface.get(), "GetKeyboardBacklightStatesJSON" ) )
  {
    auto states = callMethod< QString >( "GetKeyboardBacklightStatesJSON" );
    if ( states )
    {
      return states->toStdString();
    }
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

  const QDBusArgument arg = reply.arguments().at( 0 ).value< QDBusArgument >();
  arg.beginMap();
  while ( !arg.atEnd() )
  {
    arg.beginMapEntry();
    QString entryKey;
    QVariantMap innerMap;
    arg >> entryKey;
    arg >> innerMap;
    arg.endMapEntry();

    if ( entryKey == key )
    {
      if ( innerMap.contains( "data" ) )
      {
        int value = innerMap.value( "data" ).toInt();        // Treat entries with a zero timestamp as missing data (timestamp==0 means not available)
        if ( innerMap.contains( "timestamp" ) )
        {
          qint64 ts = innerMap.value( "timestamp" ).toLongLong();
          if ( ts == 0 )
          {
            qDebug() << "[UccdClient] readFanDataValue: key" << key << "has timestamp 0 - treating as missing";
            return std::nullopt;
          }
        }
        if ( value >= 0 )
        {
          return value;
        }
      }
      return std::nullopt;
    }
  }
  arg.endMap();

  return std::nullopt;
}

std::optional< int > readJsonInt( QDBusInterface *iface, const QString &method, const QString &key )
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

  const QString json = reply.arguments().at( 0 ).toString();
  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( doc.isNull() || !doc.isObject() )
  {
    return std::nullopt;
  }

  QJsonObject obj = doc.object();
  if ( !obj.contains( key ) )
  {
    return std::nullopt;
  }

  int val = obj[ key ].toInt();
  return ( val >= 0 ) ? std::optional< int >( val ) : std::nullopt;
}

std::optional< double > readJsonDouble( QDBusInterface *iface, const QString &method, const QString &key )
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

  const QString json = reply.arguments().at( 0 ).toString();
  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( doc.isNull() || !doc.isObject() )
  {
    return std::nullopt;
  }

  QJsonObject obj = doc.object();
  if ( !obj.contains( key ) )
  {
    return std::nullopt;
  }

  double val = obj[ key ].toDouble();
  return ( val >= 0.0 ) ? std::optional< double >( val ) : std::nullopt;
}
} // namespace

// System Monitoring implementations
std::optional< int > UccdClient::getCpuTemperature()
{
  return readFanDataValue( m_interface.get(), "GetFanDataCPU", "temp" );
}

std::optional< int > UccdClient::getGpuTemperature()
{
  if ( auto temp = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "temp" ) )
    return temp;

  return readJsonInt( m_interface.get(), "GetIGpuInfoValuesJSON", "temp" );
}

std::optional< int > UccdClient::getCpuFrequency()
{
  QFile file( "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq" );
  if ( file.open( QIODevice::ReadOnly ) )
  {
    QString content = QString::fromLatin1( file.readAll() ).trimmed();
    bool ok;
    int freq = content.toInt( &ok );
    if ( ok )
    {
      return freq / 1000;
    }
  }
  return std::nullopt;
}

std::optional< int > UccdClient::getGpuFrequency()
{
  if ( auto freq = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "coreFrequency" ) )
  {
    return freq;
  }
  return readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "coreFreq" );
}

std::optional< double > UccdClient::getCpuPower()
{
  return readJsonDouble( m_interface.get(), "GetCpuPowerValuesJSON", "powerDraw" );
}

std::optional< double > UccdClient::getGpuPower()
{
  if ( auto power = readJsonDouble( m_interface.get(), "GetDGpuInfoValuesJSON", "powerDraw" ) )
  {
    return power;
  }
  return readJsonDouble( m_interface.get(), "GetIGpuInfoValuesJSON", "powerDraw" );
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

// Water cooler readings
std::optional< int > UccdClient::getWaterCoolerFanSpeed()
{
  if ( hasMethod( m_interface.get(), "GetWaterCoolerFanSpeed" ) )
    return callMethod< int >( "GetWaterCoolerFanSpeed" );

  return std::nullopt;
}

std::optional< int > UccdClient::getWaterCoolerPumpLevel()
{
  if ( hasMethod( m_interface.get(), "GetWaterCoolerPumpLevel" ) )
    return callMethod< int >( "GetWaterCoolerPumpLevel" );

  // No method available
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

} // namespace ucc
