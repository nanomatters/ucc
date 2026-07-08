/*
 * ucc-cli — Command-line interface for Uniwill Control Center
 *
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
#include "CommonTypes.hpp"
#include "version.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Helpers

static void printVersion()
{
  std::printf( "ucc-cli %s\n", UCC_VERSION_FULL );
}

/// Pretty-print a JSON string (compact -> indented).
static void printJSON( const std::string &json )
{
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( json ) );
  if ( doc.isNull() )
  {
    std::puts( json.c_str() );
  }
  else
  {
    std::puts( doc.toJson( QJsonDocument::Indented ).toStdString().c_str() );
  }
}

static void printVal( const char *label, const std::optional< int > &v, const char *unit = "" )
{
  if ( v )
    std::printf( "  %-24s %d %s\n", label, *v, unit );
  else
    std::printf( "  %-24s n/a\n", label );
}

static void printVal( const char *label, const std::optional< double > &v, const char *unit = "" )
{
  if ( v )
    std::printf( "  %-24s %.1f %s\n", label, *v, unit );
  else
    std::printf( "  %-24s n/a\n", label );
}

static void printVal( const char *label, const std::optional< bool > &v )
{
  if ( v )
    std::printf( "  %-24s %s\n", label, *v ? "yes" : "no" );
  else
    std::printf( "  %-24s n/a\n", label );
}

static void ok( bool success )
{
  if ( success )
    std::puts( "OK" );
  else
  {
    std::fputs( "FAILED\n", stderr );
    std::exit( 1 );
  }
}

static const char *tdpLabel( int idx )
{
  switch ( idx )
  {
    case 0: return "PL1 (Sustained):";
    case 1: return "PL2 (Boost):";
    case 2: return "PL4 (Peak):";
    default: return "Unknown:";
  }
}

/// Human-readable power state label.
static std::string powerStateLabel( const std::string &raw )
{
  if ( raw == "power_ac" )  return "AC";
  if ( raw == "power_bat" ) return "Battery";
  if ( raw == "power_wc" )  return "AC w/ Water Cooler";
  return raw;
}

/// Extract a human-readable name from a profile JSON string.
static std::string profileName( const std::string &json )
{
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( json ) );
  if ( doc.isObject() )
  {
    QJsonObject obj = doc.object();
    if ( obj.contains( "name" ) )
      return obj["name"].toString().toStdString();
  }
  return "(unknown)";
}

/// Extract the "id" from a profile JSON string.
static std::string profileId( const std::string &json )
{
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( json ) );
  if ( doc.isObject() )
  {
    QJsonObject obj = doc.object();
    if ( obj.contains( "id" ) )
      return obj["id"].toString().toStdString();
  }
  return "";
}

static bool jsonArrayHasEntries( const std::optional< std::string > &json )
{
  if ( !json )
    return false;

  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  return doc.isArray() && !doc.array().isEmpty();
}

// Local assignment helpers (stateMap + customProfiles cross-reference)

/// Per-profile assignment data loaded from the daemon settings.
struct LocalAssignments {
  QMap<QString, QStringList> profileStates;  ///< main profile id -> power state names
  QMap<QString, QStringList> fanStates;      ///< fan profile id -> power state names
};

/// Map power-state key (e.g. "power_ac") to a short display label ("AC").
static QString stateLabel( const QString &state )
{
  QString s = state.startsWith( "power_" ) ? state.mid( 6 ) : state;
  return s.toUpper();
}

/// Build an annotation string like " [AC, WC]" from a list of power state keys.
static QString assignmentTag( const QStringList &states )
{
  if ( states.isEmpty() ) return {};
  QStringList labels;
  for ( const QString &s : states )
    labels.append( stateLabel( s ) );
  return QString( " [%1]" ).arg( labels.join( ", " ) );
}

/// Load stateMap and customProfiles from the daemon settings (single source of truth)
/// and resolve which fan profiles are transitively referenced through
/// power-state-assigned main profiles.
static LocalAssignments loadLocalAssignments( ucc::UccdClient &c )
{
  LocalAssignments result;

  auto settingsJSON = c.getSettingsJSON();
  if ( !settingsJSON ) return result;

  QJsonDocument settingsDoc = QJsonDocument::fromJson( QByteArray::fromStdString( *settingsJSON ) );
  if ( !settingsDoc.isObject() ) return result;

  const QJsonObject settings = settingsDoc.object();

  // stateMap: { "power_ac": "main-profile-uuid", "power_wc": "...", ... }
  if ( settings.contains( "stateMap" ) && settings["stateMap"].isObject() )
  {
    const QJsonObject stateMap = settings["stateMap"].toObject();
    for ( const QString &state : stateMap.keys() )
    {
      QString profId = stateMap[state].toString();
      if ( !profId.isEmpty() && !result.profileStates[profId].contains( state ) )
        result.profileStates[profId].append( state );
    }
  }

  // customProfiles: resolve which fan profiles are used by assigned main profiles
  if ( settings.contains( "customProfiles" ) && settings["customProfiles"].isArray() )
  {
    for ( const QJsonValue &v : settings["customProfiles"].toArray() )
    {
      if ( !v.isObject() ) continue;
      QJsonObject prof = v.toObject();
      QString profId = prof["id"].toString();
      if ( !result.profileStates.contains( profId ) ) continue;  // not in stateMap
      const QStringList &states = result.profileStates[profId];

      // Fan profile referenced by this main profile
      if ( prof.contains( "fan" ) && prof["fan"].isObject() )
      {
        QString fanId = prof["fan"].toObject()["fanProfile"].toString();
        if ( !fanId.isEmpty() )
          for ( const QString &s : states )
            if ( !result.fanStates[fanId].contains( s ) )
              result.fanStates[fanId].append( s );
      }

    }
  }

  return result;
}

// Command implementations

static int cmdStatus( ucc::UccdClient &c )
{
  std::puts( "=== UCC System Status ===" );
  std::puts( "" );

  // System information
  if ( auto sysInfoJson = c.getSystemInfoJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *sysInfoJson ) );
    if ( doc.isObject() )
    {
      const QJsonObject obj = doc.object();
      const auto laptopModel = obj.value( "laptopModel" ).toString().toStdString();
      const auto cpuModel    = obj.value( "cpuModel" ).toString().toStdString();
      const auto dGpuModel   = obj.value( "dGpuModel" ).toString().toStdString();
      const auto iGpuModel   = obj.value( "iGpuModel" ).toString().toStdString();
      const auto ecFirmware  = obj.value( "ecFirmwareVersion" ).toString().toStdString();

      std::puts( "--- System ---" );
      if ( !laptopModel.empty() )
        std::printf( "  %-24s %s\n", "Laptop model:", laptopModel.c_str() );
      if ( !ecFirmware.empty() )
        std::printf( "  %-24s %s\n", "EC firmware:", ecFirmware.c_str() );
      if ( !cpuModel.empty() )
        std::printf( "  %-24s %s\n", "CPU:", cpuModel.c_str() );
      if ( !dGpuModel.empty() )
        std::printf( "  %-24s %s\n", "GPU (discrete):", dGpuModel.c_str() );
      if ( !iGpuModel.empty() )
        std::printf( "  %-24s %s\n", "GPU (integrated):", iGpuModel.c_str() );
      std::puts( "" );
    }
  }

  // Connection
  std::printf( "  %-24s %s\n", "Daemon connected:", c.isConnected() ? "yes" : "no" );

  // Power state
  auto ps = c.getPowerState();
  if ( ps )
    std::printf( "  %-24s %s\n", "Power state:", powerStateLabel( *ps ).c_str() );

  // Active profile
  auto prof = c.getActiveProfileJSON();
  if ( prof )
  {
    std::printf( "  %-24s %s\n", "Active profile:", profileName( *prof ).c_str() );
    std::printf( "  %-24s %s\n", "Profile ID:", profileId( *prof ).c_str() );
  }

  std::puts( "" );
  std::puts( "--- CPU ---" );
  printVal( "Temperature:",   c.getCpuTemperature(), "°C" );
  printVal( "Frequency:",     c.getCpuFrequency(),   "MHz" );
  printVal( "Power:",         c.getCpuPower(),        "W" );
  printVal( "Fan speed:",     c.getFanSpeedPercent(), "%" );
  printVal( "Fan RPM:",       c.getFanSpeedRPM(),     "RPM" );

  std::puts( "" );
  std::puts( "--- GPU ---" );
  printVal( "Temperature:",   c.getGpuTemperature(), "°C" );
  printVal( "Frequency:",     c.getGpuFrequency(),   "MHz" );
  printVal( "Power:",         c.getGpuPower(),        "W" );
  printVal( "Power allocation:", c.getDGpuPowerAllocation(), "W" );
  printVal( "Thermal budget:", c.getDGpuThermalBudget(), "W" );
  printVal( "Fan speed:",     c.getGpuFanSpeedPercent(), "%" );
  printVal( "Fan RPM:",       c.getGpuFanSpeedRPM(),     "RPM" );

  std::puts( "" );
  std::puts( "--- iGPU ---" );
  printVal( "Temperature:",   c.getIGpuTemperature(), "°C" );
  printVal( "Frequency:",     c.getIGpuFrequency(),   "MHz" );
  printVal( "Power:",         c.getIGpuPower(),        "W" );

  std::puts( "" );
  std::puts( "--- Hardware ---" );
  printVal( "Display brightness:", c.getDisplayBrightness(), "%" );
  printVal( "System power:", c.getSystemPower(), "W" );
  printVal( "Adapter current:", c.getAdapterCurrent(), "A" );
  printVal( "Battery temp:", c.getBatteryTemperature(), "°C" );
  printVal( "SSD temp:", c.getSsdTemperature(), "°C" );

  auto wcSupported = c.getWaterCoolerSupported();
  if ( wcSupported && *wcSupported )
  {
    std::puts( "" );
    std::puts( "--- Water Cooler ---" );
    auto wcEnabled = c.isWaterCoolerEnabled();
    printVal( "Enabled:",     wcEnabled );
    if ( wcEnabled && *wcEnabled )
    {
      auto wcFan  = c.getWaterCoolerFanSpeed();
      auto wcPump = c.getWaterCoolerPumpLevel();
      if ( wcFan && *wcFan >= 0 )
        printVal( "Fan speed:",   wcFan, "%" );
      if ( wcPump && *wcPump >= 0 )
        printVal( "Pump level:",  wcPump );
    }
  }

  // Charging info: uniwill exposes priority/type/thresholds without legacy profiles.
  {
    // Collect charging fields first, only print section if non-empty
    std::vector< std::pair< std::string, std::string > > chLines;

    auto chargingPriority = c.getCurrentChargingPriority();
    if ( chargingPriority && !chargingPriority->empty() )
      chLines.emplace_back( "Charging priority:", *chargingPriority );

    auto chargeType = c.getChargeType();
    if ( chargeType && *chargeType != "Unknown" && *chargeType != "N/A" && !chargeType->empty() )
      chLines.emplace_back( "Charge type:", *chargeType );

    auto batteryHealth = c.getBatteryHealth();
    if ( batteryHealth && !batteryHealth->empty() )
      chLines.emplace_back( "Battery health:", *batteryHealth );

    if ( jsonArrayHasEntries( c.getChargeEndAvailableThresholds() ) )
    {
      auto chargeStart = c.getChargeStartThreshold();
      auto chargeEnd   = c.getChargeEndThreshold();
      if ( chargeStart && *chargeStart >= 0 )
        chLines.emplace_back( "Charge start thr.:", std::to_string( *chargeStart ) + " %" );
      if ( chargeEnd && *chargeEnd >= 0 )
        chLines.emplace_back( "Charge end thr.:", std::to_string( *chargeEnd ) + " %" );
    }

    if ( !chLines.empty() )
    {
      std::puts( "" );
      std::puts( "--- Charging ---" );
      for ( const auto &[label, val] : chLines )
        std::printf( "  %-24s %s\n", label.c_str(), val.c_str() );
    }
  }

  std::puts( "" );
  return 0;
}

static int cmdProfileList( ucc::UccdClient &c )
{
  LocalAssignments assignments = loadLocalAssignments( c );

  // Default (built-in) profiles from daemon
  auto defJSON = c.getDefaultProfilesJSON();
  if ( defJSON )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *defJSON ) );
    if ( doc.isArray() )
    {
      std::puts( "Built-in profiles:" );
      for ( const QJsonValue &v : doc.array() )
      {
        if ( v.isObject() )
        {
          QJsonObject obj = v.toObject();
          std::printf( "  %-36s  %s\n",
                       obj["id"].toString().toStdString().c_str(),
                       obj["name"].toString().toStdString().c_str() );
        }
      }
    }
  }

  // Custom profiles - daemon is the authoritative source
  QList<QJsonObject> customProfiles;
  auto custJSON = c.getCustomProfilesJSON();
  if ( custJSON )
  {
    QJsonDocument dd = QJsonDocument::fromJson( QByteArray::fromStdString( *custJSON ) );
    if ( dd.isArray() )
      for ( const QJsonValue &v : dd.array() )
        if ( v.isObject() )
          customProfiles.append( v.toObject() );
  }

  if ( !customProfiles.isEmpty() )
  {
    std::puts( "\nCustom profiles:" );
    for ( const QJsonObject &obj : customProfiles )
    {
      QString id = obj["id"].toString();
      QString name = obj["name"].toString();
      QString tag = assignmentTag( assignments.profileStates.value( id ) );
      std::printf( "  %-36s  %s%s\n",
                   id.toStdString().c_str(),
                   name.toStdString().c_str(),
                   tag.toStdString().c_str() );
    }
  }

  // Active profile
  auto active = c.getActiveProfileJSON();
  if ( active )
    std::printf( "\nActive: %s (%s)\n", profileName( *active ).c_str(), profileId( *active ).c_str() );

  return 0;
}

/// Print a human-readable summary of a profile JSON object.
static void printProfileSummary( const QJsonObject &obj, bool showHeader = true )
{
  std::string name = obj["name"].toString().toStdString();
  std::string id = obj["id"].toString().toStdString();
  if ( showHeader )
    std::printf( "=== Profile: %s ===\n", name.c_str() );
  else
    std::printf( "  %-24s %s\n", "Name:", name.c_str() );
  std::printf( "  %-24s %s\n", "ID:", id.c_str() );

  QString desc = obj["description"].toString();
  if ( !desc.isEmpty() )
    std::printf( "  %-24s %s\n", "Description:", desc.toStdString().c_str() );

  // CPU settings
  if ( obj.contains( "cpu" ) && obj["cpu"].isObject() )
  {
    QJsonObject cpu = obj["cpu"].toObject();
    std::puts( "" );
    std::puts( "  CPU settings:" );
    QString gov = cpu["governor"].toString();
    if ( !gov.isEmpty() )
      std::printf( "    %-22s %s\n", "Governor:", gov.toStdString().c_str() );
    QString epp = cpu["energyPerformancePreference"].toString();
    if ( !epp.isEmpty() )
      std::printf( "    %-22s %s\n", "EPP:", epp.toStdString().c_str() );
    std::printf( "    %-22s %d\n", "Online cores:", cpu["onlineCores"].toInt() );
    std::printf( "    %-22s %s\n", "No turbo:", cpu["noTurbo"].toBool() ? "yes" : "no" );
    if ( cpu.contains( "hwpDynamicBoost" ) )
      std::printf( "    %-22s %s\n", "HWP Dynamic Boost:", cpu["hwpDynamicBoost"].toBool() ? "on" : "off" );
    int minFreq = cpu["scalingMinFrequency"].toInt();
    int maxFreq = cpu["scalingMaxFrequency"].toInt();
    if ( minFreq > 0 )
      std::printf( "    %-22s %d MHz\n", "Min frequency:", minFreq / 1000 );
    if ( maxFreq > 0 )
      std::printf( "    %-22s %d MHz\n", "Max frequency:", maxFreq / 1000 );
  }

  // Fan settings
  if ( obj.contains( "fan" ) && obj["fan"].isObject() )
  {
    QJsonObject fan = obj["fan"].toObject();
    std::puts( "" );
    std::puts( "  Fan settings:" );
    if ( fan.contains( "useControl" ) )
      std::printf( "    %-22s %s\n", "Fan control:", fan["useControl"].toBool() ? "yes" : "no" );
    QString fanProf = fan["fanProfile"].toString();
    if ( !fanProf.isEmpty() )
      std::printf( "    %-22s %s\n", "Fan profile:", fanProf.toStdString().c_str() );
    if ( fan.contains( "sameSpeed" ) )
      std::printf( "    %-22s %s\n", "Same speed:", fan["sameSpeed"].toBool() ? "yes" : "no" );
    if ( fan.contains( "enableWaterCooler" ) )
      std::printf( "    %-22s %s\n", "Water cooler:", fan["enableWaterCooler"].toBool() ? "yes" : "no" );
  }

  // Display settings
  if ( obj.contains( "display" ) && obj["display"].isObject() )
  {
    QJsonObject disp = obj["display"].toObject();
    bool hasBr  = disp.contains( "useBrightness" ) && disp["useBrightness"].toBool();
    bool hasRR  = disp.contains( "useRefRate" ) && disp["useRefRate"].toBool();
    if ( hasBr || hasRR )
    {
      std::puts( "" );
      std::puts( "  Display settings:" );
      if ( hasBr )
        std::printf( "    %-22s %d %%\n", "Brightness:", disp["brightness"].toInt() );
      if ( hasRR )
        std::printf( "    %-22s %d Hz\n", "Refresh rate:", disp["refreshRate"].toInt() );
    }
  }

  if ( obj.contains( "platformProfile" ) )
  {
    QString platformProfile = obj[ "platformProfile" ].toString();
    if ( !platformProfile.isEmpty() )
      std::printf( "  %-24s %s\n", "Platform profile:", platformProfile.toStdString().c_str() );
  }

  // ODM power limits
  if ( obj.contains( "odmPowerLimits" ) && obj["odmPowerLimits"].isObject() )
  {
    QJsonObject odm = obj["odmPowerLimits"].toObject();
    if ( odm.contains( "tdpValues" ) && odm["tdpValues"].isArray() )
    {
      QJsonArray tdp = odm["tdpValues"].toArray();
      std::string s;
      for ( int i = 0; i < tdp.size(); ++i )
      {
        if ( i > 0 ) s += ", ";
        s += std::to_string( tdp[i].toInt() ) + " W";
      }
      std::printf( "  %-24s %s\n", "ODM power limits:", s.c_str() );
      // Detailed breakdown
      for ( int i = 0; i < tdp.size(); ++i )
        std::printf( "    %-22s %d W\n", tdpLabel( i ), tdp[i].toInt() );
    }
  }

  // NVIDIA cTGP (stored directly in profile)
  if ( obj.contains( "nvidiaCTGPOffset" ) )
  {
    int ctgp = obj["nvidiaCTGPOffset"].toInt();
    std::printf( "  %-24s %d W\n", "cTGP offset:", ctgp );
  }

}

static int cmdProfileGet( ucc::UccdClient &c )
{
  auto json = c.getActiveProfileJSON();
  if ( !json )
  {
    std::fputs( "Error: Could not retrieve active profile\n", stderr );
    return 1;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( !doc.isObject() )
  {
    std::puts( json->c_str() );
    return 0;
  }
  printProfileSummary( doc.object() );
  return 0;
}

static int cmdProfileSet( ucc::UccdClient &c, const char *profileId )
{
  ok( c.setActiveProfile( profileId ) );
  return 0;
}

static int cmdProfileGetDefault( ucc::UccdClient &c )
{
  auto json = c.getDefaultProfilesJSON();
  if ( !json )
  {
    std::fputs( "Error: Could not retrieve default profiles\n", stderr );
    return 1;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( !doc.isArray() )
  {
    std::puts( json->c_str() );
    return 0;
  }
  QJsonArray arr = doc.array();
  std::printf( "Built-in profiles (%d):\n", (int)arr.size() );
  for ( int i = 0; i < arr.size(); ++i )
  {
    if ( i > 0 ) std::puts( "" );
    QJsonObject obj = arr[i].toObject();
    printProfileSummary( obj, false );
    if ( i < arr.size() - 1 )
      std::puts( "  ────────────────────────────" );
  }
  return 0;
}

static int cmdProfileGetCustom( ucc::UccdClient &c )
{
  auto json = c.getCustomProfilesJSON();
  if ( !json )
  {
    std::fputs( "Error: Could not retrieve custom profiles\n", stderr );
    return 1;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( !doc.isArray() || doc.array().isEmpty() )
  {
    std::puts( "No custom profiles." );
    return 0;
  }
  QJsonArray arr = doc.array();
  std::printf( "Custom profiles (%d):\n", (int)arr.size() );
  for ( int i = 0; i < arr.size(); ++i )
  {
    if ( i > 0 ) std::puts( "" );
    QJsonObject obj = arr[i].toObject();
    printProfileSummary( obj, false );
    if ( i < arr.size() - 1 )
      std::puts( "  ────────────────────────────" );
  }
  return 0;
}

static int cmdProfileApply( ucc::UccdClient &c, const char *jsonStr )
{
  ok( c.applyProfile( jsonStr ) );
  return 0;
}

static int cmdProfileSave( ucc::UccdClient &c, const char *jsonStr )
{
  ok( c.saveCustomProfile( jsonStr ) );
  return 0;
}

static int cmdProfileDelete( ucc::UccdClient &c, const char *id )
{
  ok( c.deleteCustomProfile( id ) );
  return 0;
}

// Fan

static int cmdFanList( ucc::UccdClient &c )
{
  LocalAssignments assignments = loadLocalAssignments( c );

  auto json = c.getFanProfilesJSON();
  if ( !json )
  {
    std::fputs( "Error: Could not retrieve fan profiles\n", stderr );
    return 1;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( doc.isArray() )
  {
    std::puts( "Fan profiles:" );
    for ( const QJsonValue &v : doc.array() )
    {
      if ( v.isObject() )
      {
        QJsonObject obj = v.toObject();
        QString id = obj["id"].toString();
        QString tag = assignmentTag( assignments.fanStates.value( id ) );
        std::printf( "  %-36s  %s%s\n",
                     id.toStdString().c_str(),
                     obj["name"].toString().toStdString().c_str(),
                     tag.toStdString().c_str() );
      }
    }
  }
  else
  {
    printJSON( *json );
  }

  // Custom fan profiles from daemon (single source of truth)
  auto customFPJson = c.getCustomFanProfiles();
  if ( customFPJson )
  {
    QJsonDocument cdoc = QJsonDocument::fromJson( QByteArray::fromStdString( *customFPJson ) );
    if ( cdoc.isArray() && !cdoc.array().isEmpty() )
    {
      std::puts( "\nCustom fan profiles:" );
      for ( const QJsonValue &v : cdoc.array() )
      {
        if ( v.isObject() )
        {
          QJsonObject obj = v.toObject();
          QString id = obj["id"].toString();
          QString tag = assignmentTag( assignments.fanStates.value( id ) );
          std::printf( "  %-36s  %s%s\n",
                       id.toStdString().c_str(),
                       obj["name"].toString().toStdString().c_str(),
                       tag.toStdString().c_str() );
        }
      }
    }
  }

  return 0;
}

/// Print a fan curve table from a JSON array of {temp, speed} objects.
static void printFanCurve( const char *label, const QJsonArray &arr )
{
  if ( arr.isEmpty() ) return;
  std::printf( "\n  %s:\n", label );
  std::printf( "    %-10s %s\n", "Temp (°C)", "Speed (%)" );
  std::printf( "    %-10s %s\n", "--------", "---------" );
  for ( const QJsonValue &v : arr )
  {
    QJsonObject pt = v.toObject();
    std::printf( "    %-10d %d\n", pt["temp"].toInt(), pt["speed"].toInt() );
  }
}

static int cmdFanGet( ucc::UccdClient &c, const char *fanProfileId )
{
  auto json = c.getFanProfile( fanProfileId );
  QString customName, customId;

  // Check if daemon returned an empty object (invalid response for custom profiles)
  if ( json && *json == "{}" )
    json = std::nullopt;

  // Fall back to custom fan profiles from daemon (GUI-managed)
  if ( !json )
  {
    auto customFPJson = c.getCustomFanProfiles();
    if ( customFPJson )
    {
      QJsonDocument cdoc = QJsonDocument::fromJson( QByteArray::fromStdString( *customFPJson ) );
      if ( cdoc.isArray() )
      {
        for ( const QJsonValue &v : cdoc.array() )
        {
          if ( v.isObject() )
          {
            QJsonObject obj = v.toObject();
            if ( obj["id"].toString() == QString::fromUtf8( fanProfileId ) )
            {
              json = obj["json"].toString().toStdString();
              customName = obj["name"].toString();
              customId = obj["id"].toString();
              break;
            }
          }
        }
      }
    }
  }

  if ( !json )
  {
    std::fputs( "Error: Could not retrieve fan profile\n", stderr );
    return 1;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( !doc.isObject() )
  {
    std::fprintf( stderr, "Error: Invalid fan profile JSON\n" );
    return 1;
  }
  QJsonObject obj = doc.object();
  
  // Use custom name/id if retrieved from local storage, otherwise from JSON, fallback to argument
  QString displayName = !customName.isEmpty() ? customName : 
                        (!obj["name"].toString().isEmpty() ? obj["name"].toString() : QString::fromUtf8( fanProfileId ));
  QString displayId = !customId.isEmpty() ? customId : 
                      (!obj["id"].toString().isEmpty() ? obj["id"].toString() : QString::fromUtf8( fanProfileId ));
  
  std::printf( "=== Fan Profile: %s ===\n", displayName.toStdString().c_str() );
  std::printf( "  %-24s %s\n", "ID:", displayId.toStdString().c_str() );

  if ( obj.contains( "tableCPU" ) && obj["tableCPU"].isArray() )
    printFanCurve( "CPU fan curve", obj["tableCPU"].toArray() );

  if ( obj.contains( "tableGPU" ) && obj["tableGPU"].isArray() )
    printFanCurve( "GPU fan curve", obj["tableGPU"].toArray() );

  if ( obj.contains( "tablePump" ) && obj["tablePump"].isArray() )
    printFanCurve( "Pump curve", obj["tablePump"].toArray() );

  if ( obj.contains( "tableWaterCoolerFan" ) && obj["tableWaterCoolerFan"].isArray() )
    printFanCurve( "Water cooler fan curve", obj["tableWaterCoolerFan"].toArray() );

  return 0;
}

static int cmdFanApply( ucc::UccdClient &c, const char *jsonStr )
{
  // The apply method expects keys: cpu, gpu, pump, waterCoolerFan
  ok( c.applyFanProfiles( jsonStr ) );
  return 0;
}

static int cmdFanRevert( ucc::UccdClient &c )
{
  ok( c.revertFanProfiles() );
  return 0;
}

/// Activate a fan profile by ID: fetch its curves, remap keys and apply.
static int cmdFanSet( ucc::UccdClient &c, const char *fanProfileId )
{
  // Try daemon built-in profiles first
  auto json = c.getFanProfile( fanProfileId );

  // Check if daemon returned an empty object (invalid response for custom profiles)
  if ( json && *json == "{}" )
    json = std::nullopt;

  // Fall back to custom fan profiles from daemon (GUI-managed)
  if ( !json )
  {
    auto customFPJson = c.getCustomFanProfiles();
    if ( customFPJson )
    {
      QJsonDocument cdoc = QJsonDocument::fromJson( QByteArray::fromStdString( *customFPJson ) );
      if ( cdoc.isArray() )
      {
        for ( const QJsonValue &v : cdoc.array() )
        {
          if ( v.isObject() && v.toObject()["id"].toString() == QString::fromUtf8( fanProfileId ) )
          {
            json = v.toObject()["json"].toString().toStdString();
            break;
          }
        }
      }
    }
  }

  if ( !json )
  {
    std::fputs( "Error: Fan profile not found\n", stderr );
    return 1;
  }

  // Remap keys: tableCPU->cpu, tableGPU->gpu, etc. (same as TrayBackend)
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( !doc.isObject() )
  {
    std::fputs( "Error: Invalid fan profile JSON\n", stderr );
    return 1;
  }
  QJsonObject src = doc.object();
  QJsonObject dst;
  if ( src.contains( "tableCPU" ) )            dst["cpu"]            = src["tableCPU"];
  if ( src.contains( "tableGPU" ) )            dst["gpu"]            = src["tableGPU"];
  if ( src.contains( "tablePump" ) )           dst["pump"]           = src["tablePump"];
  if ( src.contains( "tableWaterCoolerFan" ) ) dst["waterCoolerFan"] = src["tableWaterCoolerFan"];
  // Pass through if already in apply-format
  if ( src.contains( "cpu" ) )            dst["cpu"]            = src["cpu"];
  if ( src.contains( "gpu" ) )            dst["gpu"]            = src["gpu"];
  if ( src.contains( "pump" ) )           dst["pump"]           = src["pump"];
  if ( src.contains( "waterCoolerFan" ) ) dst["waterCoolerFan"] = src["waterCoolerFan"];

  std::string applyJson = QJsonDocument( dst ).toJson( QJsonDocument::Compact ).toStdString();
  ok( c.applyFanProfiles( applyJson ) );
  return 0;
}

// Dashboard / Monitor

static int cmdMonitor( ucc::UccdClient &c, int count, int interval )
{
  // If count == 0, run indefinitely
  int remaining = count;
  bool first = true;

  while ( count == 0 || remaining > 0 )
  {
    if ( !first )
    {
      // Use QCoreApplication event loop for timer
      QEventLoop loop;
      QTimer::singleShot( interval * 1000, &loop, &QEventLoop::quit );
      loop.exec();
    }
    first = false;

    auto cpuTemp = c.getCpuTemperature();
    auto gpuTemp = c.getGpuTemperature();
    auto cpuFreq = c.getCpuFrequency();
    auto gpuFreq = c.getGpuFrequency();
    auto cpuPow  = c.getCpuPower();
    auto gpuPow  = c.getGpuPower();
    auto cpuFan  = c.getFanSpeedPercent();
    auto gpuFan  = c.getGpuFanSpeedPercent();

    std::printf( "CPU: %3d°C  %5dMHz  %5.1fW  Fan:%3d%%  |  "
                 "GPU: %3d°C  %5dMHz  %5.1fW  Fan:%3d%%\n",
                 cpuTemp.value_or( 0 ), cpuFreq.value_or( 0 ),
                 cpuPow.value_or( 0.0 ), cpuFan.value_or( 0 ),
                 gpuTemp.value_or( 0 ), gpuFreq.value_or( 0 ),
                 gpuPow.value_or( 0.0 ), gpuFan.value_or( 0 ) );
    std::fflush( stdout );

    if ( count > 0 )
      --remaining;
  }

  return 0;
}

// Hardware controls

static int cmdBrightnessGet( ucc::UccdClient &c )
{
  auto v = c.getDisplayBrightness();
  if ( !v )
  {
    std::fputs( "Error: Could not read display brightness\n", stderr );
    return 1;
  }
  std::printf( "%d\n", *v );
  return 0;
}

static int cmdBrightnessSet( ucc::UccdClient &c, int val )
{
  ok( c.setDisplayBrightness( val ) );
  return 0;
}

// Water Cooler

static int cmdWaterCoolerStatus( ucc::UccdClient &c )
{
  auto supported = c.getWaterCoolerSupported();
  if ( !supported || !*supported )
  {
    std::puts( "Water cooler: not supported" );
    return 0;
  }

  std::puts( "=== Water Cooler ===" );
  printVal( "Supported:",     supported );
  auto wcEnabled = c.isWaterCoolerEnabled();
  printVal( "Enabled:",       wcEnabled );
  if ( wcEnabled && *wcEnabled )
  {
    auto wcFan  = c.getWaterCoolerFanSpeed();
    auto wcPump = c.getWaterCoolerPumpLevel();
    if ( wcFan && *wcFan >= 0 )
      printVal( "Fan speed:",   wcFan, "%" );
    else
      std::printf( "  %-24s not connected\n", "Fan speed:" );
    if ( wcPump && *wcPump >= 0 )
      printVal( "Pump level:",  wcPump );
    else
      std::printf( "  %-24s not connected\n", "Pump level:" );
  }
  return 0;
}

static int cmdWaterCoolerEnable( ucc::UccdClient &c, bool enable )
{
  ok( c.enableWaterCooler( enable ) );
  return 0;
}

static int cmdWaterCoolerFanSet( ucc::UccdClient &c, int percent )
{
  ok( c.setWaterCoolerFanSpeed( percent ) );
  return 0;
}

static int cmdWaterCoolerPumpSet( ucc::UccdClient &c, int voltageCode )
{
  ok( c.setWaterCoolerPumpVoltage( voltageCode ) );
  return 0;
}

static int cmdWaterCoolerLed( ucc::UccdClient &c, int r, int g, int b, int mode )
{
  ok( c.setWaterCoolerLEDColor( r, g, b, mode ) );
  return 0;
}

static int cmdWaterCoolerLedOff( ucc::UccdClient &c )
{
  ok( c.turnOffWaterCoolerLED() );
  return 0;
}

// Charging

/// Parse a JSON array of strings and return a comma-separated list or empty string.
static std::string jsonArrayToList( const std::string &json )
{
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( json ) );
  if ( !doc.isArray() ) return {};
  QStringList items;
  for ( const QJsonValue &v : doc.array() )
    if ( v.isString() ) items << v.toString();
  if ( items.isEmpty() ) return {};
  return items.join( ", " ).toStdString();
}

static int cmdChargingStatus( ucc::UccdClient &c )
{
  std::vector< std::pair< std::string, std::string > > lines;

  auto chargingPriority = c.getCurrentChargingPriority();
  if ( chargingPriority && !chargingPriority->empty() )
    lines.emplace_back( "Charging priority:", *chargingPriority );

  auto prioritiesAvail = c.getChargingPrioritiesAvailable();
  if ( prioritiesAvail )
  {
    std::string plist = jsonArrayToList( *prioritiesAvail );
    if ( !plist.empty() )
      lines.emplace_back( "Available priorities:", plist );
  }

  auto chargeType = c.getChargeType();
  if ( chargeType && *chargeType != "Unknown" && *chargeType != "N/A" && !chargeType->empty() )
    lines.emplace_back( "Charge type:", *chargeType );

  auto batteryHealth = c.getBatteryHealth();
  if ( batteryHealth && !batteryHealth->empty() )
    lines.emplace_back( "Battery health:", *batteryHealth );

  if ( jsonArrayHasEntries( c.getChargeEndAvailableThresholds() ) )
  {
    auto chargeStart = c.getChargeStartThreshold();
    auto chargeEnd   = c.getChargeEndThreshold();
    if ( chargeStart && *chargeStart >= 0 )
      lines.emplace_back( "Charge start thr.:", std::to_string( *chargeStart ) + " %" );
    if ( chargeEnd && *chargeEnd >= 0 )
      lines.emplace_back( "Charge end thr.:", std::to_string( *chargeEnd ) + " %" );
  }

  if ( lines.empty() )
  {
    std::puts( "Charging control: not available on this hardware" );
    return 0;
  }

  std::puts( "=== Charging ===" );
  for ( const auto &[label, value] : lines )
    std::printf( "  %-24s %s\n", label.c_str(), value.c_str() );

  return 0;
}

static int cmdChargingSetPriority( ucc::UccdClient &c, const char *priority )
{
  ok( c.setChargingPriority( priority ) );
  return 0;
}

static int cmdChargingSetThresholds( ucc::UccdClient &c, int start, int end )
{
  bool s = c.setChargeStartThreshold( start );
  bool e = c.setChargeEndThreshold( end );
  ok( s && e );
  return 0;
}

// GPU

static int cmdGpuInfo( ucc::UccdClient &c )
{
  std::puts( "=== GPU (dGPU) ===" );
  std::puts( "" );

  // Live sensors
  printVal( "Temperature:",         c.getGpuTemperature(), "°C" );
  printVal( "Frequency:",           c.getGpuFrequency(), "MHz" );
  printVal( "Power:",               c.getGpuPower(), "W" );
  printVal( "Power allocation:",    c.getDGpuPowerAllocation(), "W" );
  printVal( "Thermal budget:",      c.getDGpuThermalBudget(), "W" );
  printVal( "Fan speed:",           c.getGpuFanSpeedPercent(), "%" );
  printVal( "Fan RPM:",             c.getGpuFanSpeedRPM(), "RPM" );

  auto ctgpAvail = c.getNVIDIAPowerCTRLAvailable();
  auto ctgpMax   = c.getNVIDIAPowerCTRLMaxPowerLimit();
  auto ctgpDef   = c.getNVIDIAPowerCTRLDefaultPowerLimit();
  auto ctgpOff   = c.getNVIDIAPowerOffset();

  std::puts( "\n--- NVIDIA Power Control ---" );
  printVal( "cTGP available:",      ctgpAvail );
  printVal( "Max power limit:",     ctgpMax, "W" );
  printVal( "Default power limit:", ctgpDef, "W" );
  printVal( "cTGP offset:",         ctgpOff, "W" );

  std::puts( "\n=== iGPU ===" );
  std::puts( "" );
  printVal( "Temperature:",         c.getIGpuTemperature(), "°C" );
  printVal( "Frequency:",           c.getIGpuFrequency(), "MHz" );
  printVal( "Power:",               c.getIGpuPower(), "W" );

  return 0;
}

// State Map

static int cmdStateMapGet( ucc::UccdClient &c )
{
  auto settings = c.getSettingsJSON();
  if ( !settings )
  {
    std::puts( "No settings available" );
    return 0;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *settings ) );
  if ( !doc.isObject() )
  {
    std::puts( settings->c_str() );
    return 0;
  }
  QJsonObject obj = doc.object();

  std::puts( "=== Settings ===" );

  // State map (profile-per-power-state)
  if ( obj.contains( "stateMap" ) && obj["stateMap"].isObject() )
  {
    std::puts( "\n  Power state → Profile mapping:" );
    QJsonObject sm = obj["stateMap"].toObject();
    for ( auto it = sm.begin(); it != sm.end(); ++it )
    {
      std::string state = it.key().toStdString();
      std::string profId = it.value().toString().toStdString();
      std::printf( "    %-24s %s\n", powerStateLabel( state ).c_str(), profId.c_str() );
    }
  }

  // Feature toggles
  std::puts( "\n  Feature controls:" );
  if ( obj.contains( "cpuSettingsEnabled" ) )
    std::printf( "    %-24s %s\n", "CPU settings:", obj["cpuSettingsEnabled"].toBool() ? "enabled" : "disabled" );
  if ( obj.contains( "fanControlEnabled" ) )
    std::printf( "    %-24s %s\n", "Fan control:", obj["fanControlEnabled"].toBool() ? "enabled" : "disabled" );
  if ( obj.contains( "fahrenheit" ) )
    std::printf( "    %-24s %s\n", "Temperature unit:", obj["fahrenheit"].toBool() ? "Fahrenheit" : "Celsius" );

  // Charging
  if ( obj.contains( "chargingPriority" ) && !obj["chargingPriority"].isNull() )
  {
    QString cp = obj["chargingPriority"].toString();
    if ( !cp.isEmpty() )
      std::printf( "    %-24s %s\n", "Charging priority:", cp.toStdString().c_str() );
  }

  return 0;
}

static int cmdStateMapSet( ucc::UccdClient &c, const char *state, const char *profileId )
{
  ok( c.setStateMap( state, profileId ) );
  return 0;
}

// CPU Info

static int cmdCpuInfo( ucc::UccdClient &c )
{
  auto cores = c.getCpuCoreCount();
  auto freqLimits = c.getCpuFrequencyLimitsJSON();
  auto govs = c.getAvailableCpuGovernors();
  auto epps = c.getAvailableEPPs();

  std::puts( "=== CPU Info ===" );
  printVal( "Core count:",          cores );
  printVal( "Temperature:",         c.getCpuTemperature(), "°C" );
  printVal( "Frequency:",           c.getCpuFrequency(), "MHz" );
  printVal( "Power:",               c.getCpuPower(), "W" );
  printVal( "System power:",        c.getSystemPower(), "W" );
  printVal( "Adapter current:",     c.getAdapterCurrent(), "A" );
  printVal( "Battery temp:",        c.getBatteryTemperature(), "°C" );
  printVal( "SSD temp:",            c.getSsdTemperature(), "°C" );

  if ( govs && !govs->empty() )
  {
    std::string list;
    for ( size_t i = 0; i < govs->size(); ++i )
    {
      if ( i > 0 ) list += ", ";
      list += ( *govs )[i];
    }
    std::printf( "  %-24s %s\n", "Available governors:", list.c_str() );
  }

  if ( epps && !epps->empty() )
  {
    std::string list;
    for ( size_t i = 0; i < epps->size(); ++i )
    {
      if ( i > 0 ) list += ", ";
      list += ( *epps )[i];
    }
    std::printf( "  %-24s %s\n", "Available EPPs:", list.c_str() );
  }

  if ( freqLimits )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *freqLimits ) );
    if ( doc.isObject() )
    {
      QJsonObject obj = doc.object();
      int minKHz = obj["min"].toInt();
      int maxKHz = obj["max"].toInt();
      std::printf( "  %-24s %d MHz\n", "Min frequency:", minKHz / 1000 );
      std::printf( "  %-24s %d MHz\n", "Max frequency:", maxKHz / 1000 );
    }
  }

  return 0;
}

// Power Limits

static int cmdPowerLimits( ucc::UccdClient &c )
{
  auto limits = c.getODMPowerLimits();
  if ( !limits )
  {
    std::puts( "No ODM power limits available" );
    return 0;
  }
  std::puts( "ODM Power Limits:" );
  for ( size_t i = 0; i < limits->size(); ++i )
    std::printf( "  %-24s %d W\n", tdpLabel( (int)i ), ( *limits )[i] );
  return 0;
}

// Usage / Help

static void printUsage()
{
  std::puts(
    "Usage: ucc-cli <command> [options]\n"
    "\n"
    "Commands:\n"
    "  status                        Show full system status (dashboard)\n"
    "  monitor [-n COUNT] [-i SECS]  Live monitor (like top). Default: continuous, 2s\n"
    "\n"
    "Profile management:\n"
    "  profile list                  List all profiles (built-in + custom)\n"
    "  profile get                   Show active profile (JSON)\n"
    "  profile set <ID>              Set active profile by ID\n"
    "  profile defaults              Show default profiles (JSON)\n"
    "  profile customs               Show custom profiles (JSON)\n"
    "  profile apply <JSON>          Apply a profile from JSON\n"
    "  profile save <JSON>           Save a custom profile\n"
    "  profile delete <ID>           Delete a custom profile\n"
    "\n"
    "State map (auto-switch on power state change):\n"
    "  statemap get                  Show current settings/state map\n"
    "  statemap set <STATE> <ID>     Set profile for power state\n"
    "                                States: power_ac, power_bat, power_wc\n"
    "\n"
    "Fan control:\n"
    "  fan list                      List fan profiles\n"
    "  fan get <ID>                  Show fan profile curves (JSON)\n"
    "  fan set <ID>                  Activate a fan profile by ID\n"
    "  fan apply <JSON>              Apply fan curves (keys: cpu, gpu, pump, waterCoolerFan)\n"
    "  fan revert                    Revert to saved fan profile\n"
    "\n"
    "Hardware controls:\n"
    "  brightness get                Get display brightness (0-100)\n"
    "  brightness set <VALUE>        Set display brightness (0-100)\n"
    "\n"
    "Water cooler:\n"
    "  watercooler status            Show water cooler status\n"
    "  watercooler enable            Enable water cooler (BLE scanning)\n"
    "  watercooler disable           Disable water cooler\n"
    "  watercooler fan <PERCENT>     Set water cooler fan speed (0-100)\n"
    "  watercooler pump <CODE>       Set pump voltage (0=11V, 1=12V, 2=7V, 3=8V, 4=off)\n"
    "  watercooler led <R> <G> <B> <MODE>\n"
    "                                Set LED color (0-255) + mode\n"
    "                                Modes: 0=static, 1=breathe, 2=colorful, 3=breathe-color\n"
    "  watercooler led-off           Turn off water cooler LED\n"
    "\n"
    "Charging:\n"
    "  charging status               Show charging info\n"
    "  charging set-priority <DESC>  Set charging priority\n"
    "  charging set-thresholds <START> <END>\n"
    "                                Set charge start/end thresholds (%)\n"
    "\n"
    "System info:\n"
    "  cpu                           Show CPU info and capabilities\n"
    "  gpu                           Show GPU info and NVIDIA power control\n"
    "  power-limits                  Show ODM power limits\n"
    "\n"
    "General:\n"
    "  --help, -h                    Show this help\n"
    "  --version, -v                 Show version\n"
    "  --json                        Force JSON output for status commands\n"
  );
}

// Argument parsing helpers

static bool matchArg( const char *arg, const char *name )
{
  return std::strcmp( arg, name ) == 0;
}

// Status JSON mode

static int cmdStatusJSON( ucc::UccdClient &c )
{
  QJsonObject root;

  root["connected"] = c.isConnected();

  if ( auto sysInfo = c.getSystemInfoJSON() )
  {
    QJsonDocument sysDoc = QJsonDocument::fromJson( QByteArray::fromStdString( *sysInfo ) );
    if ( sysDoc.isObject() )
      root["system"] = sysDoc.object();
  }

  auto ps = c.getPowerState();
  if ( ps ) root["powerState"] = QString::fromStdString( *ps );

  auto prof = c.getActiveProfileJSON();
  if ( prof )
  {
    QJsonDocument pd = QJsonDocument::fromJson( QByteArray::fromStdString( *prof ) );
    if ( pd.isObject() )
      root["activeProfile"] = pd.object();
  }

  // CPU
  QJsonObject cpu;
  auto v1 = c.getCpuTemperature();  if ( v1 ) cpu["temperature"] = *v1;
  auto v2 = c.getCpuFrequency();    if ( v2 ) cpu["frequency"]   = *v2;
  auto v3 = c.getCpuPower();        if ( v3 ) cpu["power"]       = *v3;
  auto v4 = c.getFanSpeedPercent();  if ( v4 ) cpu["fanPercent"]  = *v4;
  auto v5 = c.getFanSpeedRPM();     if ( v5 ) cpu["fanRPM"]      = *v5;
  root["cpu"] = cpu;

  // GPU
  QJsonObject gpu;
  auto g1 = c.getGpuTemperature();      if ( g1 ) gpu["temperature"] = *g1;
  auto g2 = c.getGpuFrequency();        if ( g2 ) gpu["frequency"]   = *g2;
  auto g3 = c.getGpuPower();            if ( g3 ) gpu["power"]       = *g3;
  auto g4 = c.getGpuFanSpeedPercent();  if ( g4 ) gpu["fanPercent"]  = *g4;
  auto g5 = c.getGpuFanSpeedRPM();      if ( g5 ) gpu["fanRPM"]      = *g5;
  auto g6 = c.getDGpuPowerAllocation(); if ( g6 ) gpu["powerAllocation"] = *g6;
  auto g7 = c.getDGpuThermalBudget();   if ( g7 ) gpu["thermalBudget"] = *g7;
  root["gpu"] = gpu;

  // iGPU
  QJsonObject igpu;
  auto i1 = c.getIGpuTemperature(); if ( i1 ) igpu["temperature"] = *i1;
  auto i2 = c.getIGpuFrequency();   if ( i2 ) igpu["frequency"]   = *i2;
  auto i3 = c.getIGpuPower();       if ( i3 ) igpu["power"]       = *i3;
  root["igpu"] = igpu;

  // Hardware
  QJsonObject hw;
  auto b = c.getDisplayBrightness(); if ( b ) hw["displayBrightness"] = *b;
  auto sp = c.getSystemPower();      if ( sp ) hw["systemPower"] = *sp;
  auto ac = c.getAdapterCurrent();   if ( ac ) hw["adapterCurrent"] = *ac;
  auto bt = c.getBatteryTemperature(); if ( bt ) hw["batteryTemperature"] = *bt;
  auto st = c.getSsdTemperature();   if ( st ) hw["ssdTemperature"] = *st;
  root["hardware"] = hw;

  // Water cooler
  auto wcSupported = c.getWaterCoolerSupported();
  if ( wcSupported && *wcSupported )
  {
    QJsonObject wc;
    wc["supported"] = true;
    auto we = c.isWaterCoolerEnabled();   if ( we ) wc["enabled"]   = *we;
    if ( we && *we )
    {
      auto wf = c.getWaterCoolerFanSpeed();
      auto wp = c.getWaterCoolerPumpLevel();
      if ( wf && *wf >= 0 ) wc["fanSpeed"]  = *wf;
      if ( wp && *wp >= 0 ) wc["pumpLevel"] = *wp;
    }
    root["waterCooler"] = wc;
  }

  // Charging
  {
    QJsonObject ch;
    auto cr = c.getCurrentChargingPriority();
    if ( cr && !cr->empty() ) ch["priority"] = QString::fromStdString( *cr );
    auto ct = c.getChargeType();
    if ( ct && *ct != "Unknown" && *ct != "N/A" && !ct->empty() )
      ch["type"] = QString::fromStdString( *ct );
    auto bh = c.getBatteryHealth();
    if ( bh && !bh->empty() )
      ch["health"] = QString::fromStdString( *bh );

    if ( jsonArrayHasEntries( c.getChargeEndAvailableThresholds() ) )
    {
      auto cs = c.getChargeStartThreshold(); if ( cs && *cs >= 0 ) ch["startThreshold"] = *cs;
      auto ce = c.getChargeEndThreshold();   if ( ce && *ce >= 0 ) ch["endThreshold"]   = *ce;
    }
    if ( !ch.isEmpty() )
      root["charging"] = ch;
  }

  std::puts( QJsonDocument( root ).toJson( QJsonDocument::Indented ).toStdString().c_str() );
  return 0;
}

// Main

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  app.setApplicationName( "ucc-cli" );
  app.setApplicationVersion( UCC_VERSION_FULL );
  app.setOrganizationName( "Uniwill" );

  if ( argc < 2 )
  {
    printUsage();
    return 1;
  }

  // Check for global flags
  bool jsonMode = false;
  std::vector< const char * > args;
  for ( int i = 1; i < argc; ++i )
  {
    if ( matchArg( argv[i], "--json" ) )
      jsonMode = true;
    else
      args.push_back( argv[i] );
  }

  if ( args.empty() )
  {
    printUsage();
    return 1;
  }

  const char *cmd = args[0];

  // Help / version (no daemon needed)
  if ( matchArg( cmd, "--help" ) || matchArg( cmd, "-h" ) || matchArg( cmd, "help" ) )
  {
    printUsage();
    return 0;
  }
  if ( matchArg( cmd, "--version" ) || matchArg( cmd, "-v" ) || matchArg( cmd, "version" ) )
  {
    printVersion();
    return 0;
  }

  // Create D-Bus client
  ucc::UccdClient client;

  if ( !client.isConnected() )
  {
    std::fputs( "Error: Cannot connect to uccd daemon (com.uniwill.uccd on system bus).\n"
                "Make sure uccd is running: systemctl status uccd\n", stderr );
    return 2;
  }

  // Dispatch commands

  // status
  if ( matchArg( cmd, "status" ) )
    return jsonMode ? cmdStatusJSON( client ) : cmdStatus( client );

  // monitor [-n COUNT] [-i INTERVAL]
  if ( matchArg( cmd, "monitor" ) || matchArg( cmd, "mon" ) )
  {
    int count = 0;       // 0 = infinite
    int interval = 2;    // seconds
    for ( size_t i = 1; i < args.size(); ++i )
    {
      if ( matchArg( args[i], "-n" ) && i + 1 < args.size() )
        count = std::atoi( args[++i] );
      else if ( matchArg( args[i], "-i" ) && i + 1 < args.size() )
        interval = std::atoi( args[++i] );
    }
    return cmdMonitor( client, count, interval );
  }

  // profile ...
  if ( matchArg( cmd, "profile" ) || matchArg( cmd, "prof" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli profile <list|get|set|defaults|customs|apply|save|delete>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "list" ) || matchArg( sub, "ls" ) )
      return cmdProfileList( client );
    if ( matchArg( sub, "get" ) || matchArg( sub, "show" ) || matchArg( sub, "active" ) )
      return cmdProfileGet( client );
    if ( matchArg( sub, "set" ) || matchArg( sub, "activate" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli profile set <PROFILE_ID>\n", stderr ); return 1; }
      return cmdProfileSet( client, args[2] );
    }
    if ( matchArg( sub, "defaults" ) || matchArg( sub, "default" ) )
      return cmdProfileGetDefault( client );
    if ( matchArg( sub, "customs" ) || matchArg( sub, "custom" ) )
      return cmdProfileGetCustom( client );
    if ( matchArg( sub, "apply" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli profile apply <JSON>\n", stderr ); return 1; }
      return cmdProfileApply( client, args[2] );
    }
    if ( matchArg( sub, "save" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli profile save <JSON>\n", stderr ); return 1; }
      return cmdProfileSave( client, args[2] );
    }
    if ( matchArg( sub, "delete" ) || matchArg( sub, "del" ) || matchArg( sub, "rm" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli profile delete <PROFILE_ID>\n", stderr ); return 1; }
      return cmdProfileDelete( client, args[2] );
    }
    std::fprintf( stderr, "Unknown profile subcommand: %s\n", sub );
    return 1;
  }

  // statemap ...
  if ( matchArg( cmd, "statemap" ) || matchArg( cmd, "state-map" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli statemap <get|set>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "get" ) || matchArg( sub, "show" ) )
      return cmdStateMapGet( client );
    if ( matchArg( sub, "set" ) )
    {
      if ( args.size() < 4 ) { std::fputs( "Usage: ucc-cli statemap set <STATE> <PROFILE_ID>\n", stderr ); return 1; }
      return cmdStateMapSet( client, args[2], args[3] );
    }
    std::fprintf( stderr, "Unknown statemap subcommand: %s\n", sub );
    return 1;
  }

  // fan ...
  if ( matchArg( cmd, "fan" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli fan <list|get|set|apply|revert>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "list" ) || matchArg( sub, "ls" ) )
      return cmdFanList( client );
    if ( matchArg( sub, "get" ) || matchArg( sub, "show" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli fan get <FAN_PROFILE_ID>\n", stderr ); return 1; }
      return cmdFanGet( client, args[2] );
    }
    if ( matchArg( sub, "set" ) || matchArg( sub, "activate" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli fan set <FAN_PROFILE_ID>\n", stderr ); return 1; }
      return cmdFanSet( client, args[2] );
    }
    if ( matchArg( sub, "apply" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli fan apply <JSON>\n", stderr ); return 1; }
      return cmdFanApply( client, args[2] );
    }
    if ( matchArg( sub, "revert" ) )
      return cmdFanRevert( client );
    std::fprintf( stderr, "Unknown fan subcommand: %s\n", sub );
    return 1;
  }

  // brightness ...
  if ( matchArg( cmd, "brightness" ) || matchArg( cmd, "br" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli brightness <get|set>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "get" ) )
      return cmdBrightnessGet( client );
    if ( matchArg( sub, "set" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli brightness set <0-100>\n", stderr ); return 1; }
      return cmdBrightnessSet( client, std::atoi( args[2] ) );
    }
    std::fprintf( stderr, "Unknown brightness subcommand: %s\n", sub );
    return 1;
  }

  // watercooler ...
  if ( matchArg( cmd, "watercooler" ) || matchArg( cmd, "wc" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli watercooler <status|enable|disable|fan|pump|led|led-off>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "status" ) )
      return cmdWaterCoolerStatus( client );
    if ( matchArg( sub, "enable" ) || matchArg( sub, "on" ) )
      return cmdWaterCoolerEnable( client, true );
    if ( matchArg( sub, "disable" ) || matchArg( sub, "off" ) )
      return cmdWaterCoolerEnable( client, false );
    if ( matchArg( sub, "fan" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli watercooler fan <0-100>\n", stderr ); return 1; }
      return cmdWaterCoolerFanSet( client, std::atoi( args[2] ) );
    }
    if ( matchArg( sub, "pump" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli watercooler pump <CODE>\n", stderr ); return 1; }
      return cmdWaterCoolerPumpSet( client, std::atoi( args[2] ) );
    }
    if ( matchArg( sub, "led" ) )
    {
      if ( args.size() < 6 )
      {
        std::fputs( "Usage: ucc-cli watercooler led <R> <G> <B> <MODE>\n", stderr );
        return 1;
      }
      return cmdWaterCoolerLed( client, std::atoi( args[2] ), std::atoi( args[3] ),
                                std::atoi( args[4] ), std::atoi( args[5] ) );
    }
    if ( matchArg( sub, "led-off" ) )
      return cmdWaterCoolerLedOff( client );
    std::fprintf( stderr, "Unknown watercooler subcommand: %s\n", sub );
    return 1;
  }

  // charging ...
  if ( matchArg( cmd, "charging" ) || matchArg( cmd, "charge" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli charging <status|set-priority|set-thresholds>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "status" ) )
      return cmdChargingStatus( client );
    if ( matchArg( sub, "set-priority" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli charging set-priority <DESCRIPTOR>\n", stderr ); return 1; }
      return cmdChargingSetPriority( client, args[2] );
    }
    if ( matchArg( sub, "set-thresholds" ) )
    {
      if ( args.size() < 4 ) { std::fputs( "Usage: ucc-cli charging set-thresholds <START> <END>\n", stderr ); return 1; }
      return cmdChargingSetThresholds( client, std::atoi( args[2] ), std::atoi( args[3] ) );
    }
    std::fprintf( stderr, "Unknown charging subcommand: %s\n", sub );
    return 1;
  }

  // cpu
  if ( matchArg( cmd, "cpu" ) )
    return cmdCpuInfo( client );

  // gpu
  if ( matchArg( cmd, "gpu" ) )
    return cmdGpuInfo( client );

  // power-limits
  if ( matchArg( cmd, "power-limits" ) || matchArg( cmd, "odm" ) )
    return cmdPowerLimits( client );

  std::fprintf( stderr, "Unknown command: %s\nTry: ucc-cli --help\n", cmd );
  return 1;
}
