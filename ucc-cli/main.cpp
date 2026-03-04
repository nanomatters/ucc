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
#include <QSettings>
#include <QDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void printVersion()
{
  std::printf( "ucc-cli %s\n", UCC_VERSION_FULL );
}

/// Pretty-print a JSON string (compact → indented).
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

static void printVal( const char *label, const std::optional< std::string > &v )
{
  if ( v )
    std::printf( "  %-24s %s\n", label, v->c_str() );
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

// ---------------------------------------------------------------------------
// Local settings helper (same path as GUI/tray: ~/.config/uccrc)
// ---------------------------------------------------------------------------

static QSettings localSettings()
{
  return QSettings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
}

// ---------------------------------------------------------------------------
// Local assignment helpers (stateMap + customProfiles cross-reference)
// ---------------------------------------------------------------------------

/// Per-profile assignment data loaded from local uccrc settings.
struct LocalAssignments {
  QMap<QString, QStringList> profileStates;  ///< main profile id  -> power state names
  QMap<QString, QStringList> fanStates;      ///< fan profile id   -> power state names
  QMap<QString, QStringList> kbStates;       ///< keyboard profile id -> power state names
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

/// Load stateMap from uccrc and profiles from daemon to resolve which fan/keyboard profiles
/// are transitively referenced through power-state-assigned main profiles.
static LocalAssignments loadLocalAssignments( ucc::UccdClient &c )
{
  LocalAssignments result;
  QSettings settings = localSettings();

  // stateMap: { "power_ac": "main-profile-uuid", "power_wc": "...", ... }
  QByteArray smData = settings.value( "stateMap", "{}" ).toByteArray();
  QJsonDocument smDoc = QJsonDocument::fromJson( smData );
  if ( smDoc.isObject() )
  {
    QJsonObject stateMap = smDoc.object();
    for ( const QString &state : stateMap.keys() )
    {
      QString profId = stateMap[state].toString();
      if ( !profId.isEmpty() && !result.profileStates[profId].contains( state ) )
        result.profileStates[profId].append( state );
    }
  }

  // Profiles from daemon (all with editable flag) — resolve sub-profile references
  if ( auto json = c.getProfilesJSON() )
  {
    QJsonDocument cpDoc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( cpDoc.isArray() )
    {
      for ( const QJsonValue &v : cpDoc.array() )
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

        // Keyboard profile referenced by this main profile
        QString kbId = prof["selectedKeyboardProfile"].toString();
        if ( !kbId.isEmpty() )
          for ( const QString &s : states )
            if ( !result.kbStates[kbId].contains( s ) )
              result.kbStates[kbId].append( s );
      }
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

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

      std::puts( "--- System ---" );
      if ( !laptopModel.empty() )
        std::printf( "  %-24s %s\n", "Laptop model:", laptopModel.c_str() );
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
  printVal( "Fan speed:",     c.getGpuFanSpeedPercent(), "%" );
  printVal( "Fan RPM:",       c.getGpuFanSpeedRPM(),     "RPM" );

  // Extended NVIDIA metrics (if available)
  {
    auto computeUtil = c.getDGpuComputeUtilPct();
    auto memUtil     = c.getDGpuMemoryUtilPct();
    auto vramUsed    = c.getDGpuVramUsedMiB();
    auto vramTotal   = c.getDGpuVramTotalMiB();
    auto pstate      = c.getDGpuCurrentPstate();
    auto coreVoltage = c.getDGpuCoreVoltageMv();
    auto grOffset    = c.getDGpuGrClockOffsetMHz();
    auto memOffset   = c.getDGpuMemClockOffsetMHz();

    bool hasExtended = ( computeUtil && *computeUtil >= 0 ) || ( pstate && *pstate >= 0 );
    if ( hasExtended )
    {
      printVal( "GPU load:",          computeUtil, "%" );
      printVal( "Memory load:",       memUtil, "%" );
      if ( vramUsed && *vramUsed >= 0 && vramTotal && *vramTotal > 0 )
        std::printf( "  %-24s %d / %d MiB\n", "VRAM:", *vramUsed, *vramTotal );
      if ( pstate && *pstate >= 0 )
        std::printf( "  %-24s P%d\n", "P-State:", *pstate );
      printVal( "Core voltage:",      coreVoltage, "mV" );
      printVal( "Core clock offset:", grOffset, "MHz" );
      printVal( "Mem clock offset:",  memOffset, "MHz" );
    }
  }

  std::puts( "" );
  std::puts( "--- iGPU ---" );
  printVal( "Temperature:",   c.getIGpuTemperature(), "°C" );
  printVal( "Frequency:",     c.getIGpuFrequency(),   "MHz" );
  printVal( "Power:",         c.getIGpuPower(),        "W" );

  std::puts( "" );
  std::puts( "--- Hardware ---" );
  printVal( "Display brightness:", c.getDisplayBrightness(), "%" );
  printVal( "Webcam enabled:",     c.getWebcamEnabled() );
  printVal( "Fn Lock:",            c.getFnLock() );

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

  // Charging info — mirror GUI logic: only show if hardware provides data
  auto chargingProfilesAvail = c.getChargingProfilesAvailable();
  bool hasChargingHW = false;
  if ( chargingProfilesAvail )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *chargingProfilesAvail ) );
    hasChargingHW = doc.isArray() && !doc.array().isEmpty();
  }

  if ( hasChargingHW )
  {
    // Collect charging fields first, only print section if non-empty
    std::vector< std::pair< std::string, std::string > > chLines;

    auto chargingProfile = c.getCurrentChargingProfile();
    if ( chargingProfile && !chargingProfile->empty() )
      chLines.emplace_back( "Charging profile:", *chargingProfile );

    auto chargingPriority = c.getCurrentChargingPriority();
    if ( chargingPriority && !chargingPriority->empty() )
      chLines.emplace_back( "Charging priority:", *chargingPriority );

    auto chargeType = c.getChargeType();
    if ( chargeType && *chargeType != "Unknown" && *chargeType != "N/A" && !chargeType->empty() )
      chLines.emplace_back( "Charge type:", *chargeType );

    auto endAvail = c.getChargeEndAvailableThresholds();
    bool hasThr = false;
    if ( endAvail )
    {
      QJsonDocument td = QJsonDocument::fromJson( QByteArray::fromStdString( *endAvail ) );
      hasThr = td.isArray() && !td.array().isEmpty();
    }
    if ( hasThr )
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

  // Custom profiles from daemon (editable ones)
  auto custJSON = c.getCustomProfilesJSON();
  QList<QJsonObject> customProfiles;
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
static void printProfileSummary( const QJsonObject &obj, bool showHeader = true, ucc::UccdClient *client = nullptr )
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

  // Webcam
  if ( obj.contains( "webcam" ) && obj["webcam"].isObject() )
  {
    QJsonObject wc = obj["webcam"].toObject();
    if ( wc.contains( "useStatus" ) && wc["useStatus"].toBool() )
      std::printf( "  %-24s %s\n", "Webcam:", wc["status"].toBool() ? "enabled" : "disabled" );
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

  // NVIDIA cTGP (resolved from GPU profile by ID)
  if ( obj.contains( "gpuProfileId" ) )
  {
    QString gpuId = obj["gpuProfileId"].toString();
    if ( !gpuId.isEmpty() )
    {
      std::printf( "  %-24s %s\n", "GPU OC profile:", gpuId.toStdString().c_str() );
      if ( client )
      {
        if ( auto gpuJson = client->getGpuProfileJSON( gpuId.toStdString() ) )
        {
          QJsonDocument gpuDoc = QJsonDocument::fromJson( QByteArray::fromStdString( *gpuJson ) );
          if ( gpuDoc.isObject() )
          {
            QJsonObject gpuObj = gpuDoc.object();
            if ( gpuObj.contains( "nvidiaPowerCTRLProfile" ) && gpuObj["nvidiaPowerCTRLProfile"].isObject() )
            {
              int ctgp = gpuObj["nvidiaPowerCTRLProfile"].toObject()["cTGPOffset"].toInt();
              std::printf( "  %-24s %d W\n", "cTGP offset:", ctgp );
            }
          }
        }
      }
    }
  }

  // ODM profile
  if ( obj.contains( "odmProfile" ) && obj["odmProfile"].isObject() )
  {
    QString odmName = obj["odmProfile"].toObject()["name"].toString();
    if ( !odmName.isEmpty() )
      std::printf( "  %-24s %s\n", "ODM profile:", odmName.toStdString().c_str() );
  }

  // Charging
  if ( obj.contains( "chargingProfile" ) )
  {
    QString cp = obj["chargingProfile"].toString();
    if ( !cp.isEmpty() )
      std::printf( "  %-24s %s\n", "Charging profile:", cp.toStdString().c_str() );
  }

  // Selected keyboard profile
  if ( obj.contains( "selectedKeyboardProfile" ) )
  {
    QString kp = obj["selectedKeyboardProfile"].toString();
    if ( !kp.isEmpty() )
      std::printf( "  %-24s %s\n", "Keyboard profile:", kp.toStdString().c_str() );
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
  printProfileSummary( doc.object(), true, &c );
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
    printProfileSummary( obj, false, &c );
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
    printProfileSummary( obj, false, &c );
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

// --- Fan ---

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

  // All fan profiles (built-in + custom) are now in the daemon response above
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
  auto json = c.getFanProfileJSON( fanProfileId );

  // Check if daemon returned an empty object
  if ( json && *json == "{}" )
    json = std::nullopt;

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
  
  // Use name/id from JSON, fallback to argument
  QString displayName = !obj["name"].toString().isEmpty() ? obj["name"].toString() : QString::fromUtf8( fanProfileId );
  QString displayId = !obj["id"].toString().isEmpty() ? obj["id"].toString() : QString::fromUtf8( fanProfileId );
  
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

/// Activate a fan profile by ID: fetch its curves, remap keys, and apply.
static int cmdFanSet( ucc::UccdClient &c, const char *fanProfileId )
{
  // All fan profiles (built-in + custom) are now on the daemon
  auto json = c.getFanProfileJSON( fanProfileId );

  // Check if daemon returned an empty object
  if ( json && *json == "{}" )
    json = std::nullopt;

  if ( !json )
  {
    std::fputs( "Error: Fan profile not found\n", stderr );
    return 1;
  }

  // Remap keys: tableCPU→cpu, tableGPU→gpu, etc. (same as TrayBackend)
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

// --- Dashboard / Monitor ---

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
    auto gpuLoad = c.getDGpuComputeUtilPct();
    auto pstate  = c.getDGpuCurrentPstate();

    // Base metrics
    std::printf( "CPU: %3d°C  %5dMHz  %5.1fW  Fan:%3d%%  |  "
                 "GPU: %3d°C  %5dMHz  %5.1fW  Fan:%3d%%",
                 cpuTemp.value_or( 0 ), cpuFreq.value_or( 0 ),
                 cpuPow.value_or( 0.0 ), cpuFan.value_or( 0 ),
                 gpuTemp.value_or( 0 ), gpuFreq.value_or( 0 ),
                 gpuPow.value_or( 0.0 ), gpuFan.value_or( 0 ) );

    // Extended NVIDIA metrics if available
    if ( gpuLoad && *gpuLoad >= 0 )
      std::printf( "  Load:%3d%%", *gpuLoad );
    if ( pstate && *pstate >= 0 )
      std::printf( "  P%d", *pstate );
    std::puts( "" );
    std::fflush( stdout );

    if ( count > 0 )
      --remaining;
  }

  return 0;
}

// --- Keyboard ---

static int cmdKeyboardInfo( ucc::UccdClient &c )
{
  auto info = c.getKeyboardBacklightInfo();
  if ( !info )
  {
    std::fputs( "Error: Could not retrieve keyboard backlight info\n", stderr );
    return 1;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *info ) );
  if ( !doc.isObject() )
  {
    std::puts( info->c_str() );
    return 0;
  }
  QJsonObject obj = doc.object();
  std::puts( "=== Keyboard Backlight ===" );
  std::printf( "  %-24s %d\n",  "Zones:", obj["zones"].toInt() );
  std::printf( "  %-24s %d\n",  "Max brightness:", obj["maxBrightness"].toInt() );
  std::printf( "  %-24s %d\n",  "Max red:", obj["maxRed"].toInt() );
  std::printf( "  %-24s %d\n",  "Max green:", obj["maxGreen"].toInt() );
  std::printf( "  %-24s %d\n",  "Max blue:", obj["maxBlue"].toInt() );
  if ( obj.contains( "modes" ) && obj["modes"].isArray() )
  {
    QJsonArray modes = obj["modes"].toArray();
    std::string mstr;
    for ( int i = 0; i < modes.size(); ++i )
    {
      if ( i > 0 ) mstr += ", ";
      int m = modes[i].toInt();
      switch ( m )
      {
        case 0: mstr += "static"; break;
        case 1: mstr += "breathe"; break;
        case 2: mstr += "colorful"; break;
        case 3: mstr += "breathe-color"; break;
        default: mstr += std::to_string( m ); break;
      }
    }
    std::printf( "  %-24s %s\n", "Modes:", mstr.c_str() );
  }
  return 0;
}

static int cmdKeyboardGet( ucc::UccdClient &c )
{
  auto states = c.getKeyboardBacklightStates();
  if ( !states )
  {
    std::fputs( "Error: Could not retrieve keyboard backlight states\n", stderr );
    return 1;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *states ) );
  if ( !doc.isObject() )
  {
    std::puts( states->c_str() );
    return 0;
  }
  QJsonObject root = doc.object();
  int brightness = root["brightness"].toInt();
  QJsonArray arr = root["states"].toArray();

  std::puts( "=== Keyboard Backlight State ===" );
  std::printf( "  %-24s %d\n", "Global brightness:", brightness );
  std::printf( "  %-24s %d\n", "Zones:", (int)arr.size() );

  if ( arr.isEmpty() )
    return 0;

  // Check if all zones are uniform
  QJsonObject first = arr[0].toObject();
  bool uniform = true;
  for ( int i = 1; i < arr.size(); ++i )
  {
    QJsonObject z = arr[i].toObject();
    if ( z["red"].toInt() != first["red"].toInt() ||
         z["green"].toInt() != first["green"].toInt() ||
         z["blue"].toInt() != first["blue"].toInt() ||
         z["mode"].toInt() != first["mode"].toInt() ||
         z["brightness"].toInt() != first["brightness"].toInt() )
    {
      uniform = false;
      break;
    }
  }

  if ( uniform )
  {
    std::printf( "  %-24s uniform\n", "Pattern:" );
    std::printf( "  %-24s rgb(%d, %d, %d)\n", "Color:",
                 first["red"].toInt(), first["green"].toInt(), first["blue"].toInt() );
    std::printf( "  %-24s %d\n", "Zone brightness:", first["brightness"].toInt() );
    int m = first["mode"].toInt();
    const char *modeName = m == 0 ? "static" : m == 1 ? "breathe" :
                           m == 2 ? "colorful" : m == 3 ? "breathe-color" : "unknown";
    std::printf( "  %-24s %s\n", "Mode:", modeName );
  }
  else
  {
    // Print a compact table for differing zones
    std::puts( "" );
    std::printf( "  %-6s %-6s %-14s %-12s %s\n", "Zone", "Mode", "Color", "Brightness", "" );
    std::printf( "  %-6s %-6s %-14s %-12s %s\n", "----", "----", "-----------", "----------", "" );
    for ( int i = 0; i < arr.size(); ++i )
    {
      QJsonObject z = arr[i].toObject();
      int m = z["mode"].toInt();
      const char *modeName = m == 0 ? "static" : m == 1 ? "breathe" :
                             m == 2 ? "color" : m == 3 ? "br-color" : "?";
      char color[32];
      std::snprintf( color, sizeof(color), "(%d,%d,%d)",
                     z["red"].toInt(), z["green"].toInt(), z["blue"].toInt() );
      std::printf( "  %-6d %-6s %-14s %-12d\n", i + 1, modeName, color, z["brightness"].toInt() );
    }
  }
  return 0;
}

static int cmdKeyboardSet( ucc::UccdClient &c, const char *jsonStr )
{
  ok( c.setKeyboardBacklight( jsonStr ) );
  return 0;
}

/// List keyboard profiles from the daemon.
static int cmdKeyboardProfileList( ucc::UccdClient &c )
{
  LocalAssignments assignments = loadLocalAssignments( c );

  auto json = c.getKeyboardProfilesJSON();
  if ( !json )
  {
    std::puts( "No keyboard profiles found." );
    return 0;
  }
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( !doc.isArray() || doc.array().isEmpty() )
  {
    std::puts( "No keyboard profiles found." );
    return 0;
  }
  std::puts( "Keyboard profiles:" );
  for ( const QJsonValue &v : doc.array() )
  {
    if ( v.isObject() )
    {
      QJsonObject obj = v.toObject();
      QString id = obj["id"].toString();
      QString tag = assignmentTag( assignments.kbStates.value( id ) );
      std::printf( "  %-36s  %s%s\n",
                   id.toStdString().c_str(),
                   obj["name"].toString().toStdString().c_str(),
                   tag.toStdString().c_str() );
    }
  }
  return 0;
}

/// Set a keyboard profile by ID from the daemon.
static int cmdKeyboardProfileSet( ucc::UccdClient &c, const char *profileId )
{
  auto json = c.getKeyboardProfileJSON( profileId );
  if ( !json || *json == "{}" )
  {
    std::fputs( "Error: Keyboard profile not found\n", stderr );
    return 1;
  }

  // The daemon returns the full profile JSON; extract the "json" sub-field if present
  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( doc.isObject() )
  {
    QString innerJson = doc.object()["json"].toString();
    if ( !innerJson.isEmpty() )
    {
      ok( c.setKeyboardBacklight( innerJson.toStdString() ) );
      return 0;
    }
  }
  // Fall back to applying the whole response as-is
  ok( c.setKeyboardBacklight( *json ) );
  return 0;
}

static int cmdKeyboardColor( ucc::UccdClient &c, int r, int g, int b, int brightness )
{
  // Get current capabilities to determine zone count
  auto info = c.getKeyboardBacklightInfo();
  if ( !info )
  {
    std::fputs( "Error: Could not retrieve keyboard capabilities\n", stderr );
    return 1;
  }

  QJsonDocument cap = QJsonDocument::fromJson( QByteArray::fromStdString( *info ) );
  int zones = 1;
  if ( cap.isObject() && cap.object().contains( "zones" ) )
    zones = cap.object()["zones"].toInt( 1 );

  // Build a per-zone array with uniform color
  QJsonArray states;
  for ( int i = 0; i < zones; ++i )
  {
    QJsonObject zone;
    zone["mode"]       = 0;  // static
    zone["brightness"] = brightness;
    zone["red"]        = r;
    zone["green"]      = g;
    zone["blue"]       = b;
    states.append( zone );
  }

  QString json = QJsonDocument( states ).toJson( QJsonDocument::Compact );
  ok( c.setKeyboardBacklight( json.toStdString() ) );
  return 0;
}

// --- Hardware controls ---

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

static int cmdWebcamGet( ucc::UccdClient &c )
{
  auto v = c.getWebcamEnabled();
  if ( !v )
  {
    std::fputs( "Error: Could not read webcam status\n", stderr );
    return 1;
  }
  std::puts( *v ? "enabled" : "disabled" );
  return 0;
}

static int cmdWebcamSet( ucc::UccdClient &c, bool enabled )
{
  ok( c.setWebcamEnabled( enabled ) );
  return 0;
}

static int cmdFnLockGet( ucc::UccdClient &c )
{
  auto v = c.getFnLock();
  if ( !v )
  {
    std::fputs( "Error: Could not read Fn Lock status\n", stderr );
    return 1;
  }
  std::puts( *v ? "on" : "off" );
  return 0;
}

static int cmdFnLockSet( ucc::UccdClient &c, bool enabled )
{
  ok( c.setFnLock( enabled ) );
  return 0;
}

// --- Water Cooler ---

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

// --- Charging ---

/// Parse a JSON array of strings and return a comma-separated list, or empty string.
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
  auto profilesAvail = c.getChargingProfilesAvailable();
  std::string profilesList;
  if ( profilesAvail )
    profilesList = jsonArrayToList( *profilesAvail );

  if ( profilesList.empty() )
  {
    std::puts( "Charging control: not available on this hardware" );
    return 0;
  }

  std::puts( "=== Charging ===" );

  auto chargingProfile = c.getCurrentChargingProfile();
  if ( chargingProfile && !chargingProfile->empty() )
    printVal( "Charging profile:",  chargingProfile );

  std::printf( "  %-24s %s\n", "Available profiles:", profilesList.c_str() );

  auto chargingPriority = c.getCurrentChargingPriority();
  if ( chargingPriority && !chargingPriority->empty() )
    printVal( "Charging priority:", chargingPriority );

  auto prioritiesAvail = c.getChargingPrioritiesAvailable();
  if ( prioritiesAvail )
  {
    std::string plist = jsonArrayToList( *prioritiesAvail );
    if ( !plist.empty() )
      std::printf( "  %-24s %s\n", "Available priorities:", plist.c_str() );
  }

  auto chargeType = c.getChargeType();
  if ( chargeType && *chargeType != "Unknown" && *chargeType != "N/A" && !chargeType->empty() )
    printVal( "Charge type:",       chargeType );

  auto endAvail = c.getChargeEndAvailableThresholds();
  bool hasThr = false;
  if ( endAvail )
  {
    QJsonDocument td = QJsonDocument::fromJson( QByteArray::fromStdString( *endAvail ) );
    hasThr = td.isArray() && !td.array().isEmpty();
  }
  if ( hasThr )
  {
    auto chargeStart = c.getChargeStartThreshold();
    auto chargeEnd   = c.getChargeEndThreshold();
    if ( chargeStart && *chargeStart >= 0 )
      printVal( "Charge start thr.:", chargeStart, "%" );
    if ( chargeEnd && *chargeEnd >= 0 )
      printVal( "Charge end thr.:",   chargeEnd,   "%" );
  }

  return 0;
}

static int cmdChargingSetProfile( ucc::UccdClient &c, const char *profile )
{
  ok( c.setChargingProfile( profile ) );
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

// --- GPU ---

static int cmdGpuInfo( ucc::UccdClient &c )
{
  std::puts( "=== GPU (dGPU) ===" );

  std::optional< std::string > dGpuName;
  if ( auto sysInfoJson = c.getSystemInfoJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *sysInfoJson ) );
    if ( doc.isObject() )
    {
      const auto name = doc.object().value( "dGpuModel" ).toString().toStdString();
      if ( !name.empty() )
        dGpuName = name;
    }
  }
  printVal( "GPU name:", dGpuName );
  std::puts( "" );

  // Live sensors
  std::puts( "--- Sensors ---" );
  printVal( "Temperature:",         c.getGpuTemperature(), "°C" );
  printVal( "Frequency:",           c.getGpuFrequency(), "MHz" );
  printVal( "Power:",               c.getGpuPower(), "W" );
  printVal( "Fan speed:",           c.getGpuFanSpeedPercent(), "%" );
  printVal( "Fan RPM:",             c.getGpuFanSpeedRPM(), "RPM" );

  // Extended NVIDIA metrics
  auto computeUtil = c.getDGpuComputeUtilPct();
  auto memUtil     = c.getDGpuMemoryUtilPct();
  auto vramUsed    = c.getDGpuVramUsedMiB();
  auto vramTotal   = c.getDGpuVramTotalMiB();
  auto perfLimit   = c.getDGpuPerfLimitReason();
  auto encoderUtil = c.getDGpuEncoderUtilPct();
  auto decoderUtil = c.getDGpuDecoderUtilPct();
  auto pstate      = c.getDGpuCurrentPstate();
  auto grOffset    = c.getDGpuGrClockOffsetMHz();
  auto memOffset   = c.getDGpuMemClockOffsetMHz();
  auto vramFreq    = c.getDGpuVramFrequencyMHz();
  auto coreVoltage = c.getDGpuCoreVoltageMv();

  bool hasExtended = ( computeUtil && *computeUtil >= 0 ) || ( pstate && *pstate >= 0 );
  if ( hasExtended )
  {
    std::puts( "\n--- NVIDIA Extended Metrics ---" );
    printVal( "GPU load:",            computeUtil, "%" );
    printVal( "Memory load:",         memUtil, "%" );
    if ( vramUsed && *vramUsed >= 0 && vramTotal && *vramTotal > 0 )
      std::printf( "  %-24s %d / %d MiB\n", "VRAM usage:", *vramUsed, *vramTotal );
    else
    {
      printVal( "VRAM used:",          vramUsed, "MiB" );
      printVal( "VRAM total:",         vramTotal, "MiB" );
    }
    printVal( "VRAM frequency:",      vramFreq, "MHz" );
    if ( pstate && *pstate >= 0 )
      std::printf( "  %-24s P%d\n", "P-State:", *pstate );
    else
      std::printf( "  %-24s n/a\n", "P-State:" );
    printVal( "Core voltage:",        coreVoltage, "mV" );
    printVal( "Core clock offset:",   grOffset, "MHz" );
    printVal( "Memory clock offset:", memOffset, "MHz" );
    printVal( "NVENC utilization:",   encoderUtil, "%" );
    printVal( "NVDEC utilization:",   decoderUtil, "%" );
    printVal( "Perf limit reason:",   perfLimit );
  }

  auto ctgpAvail = c.getNVIDIAPowerCTRLAvailable();
  auto ctgpMax   = c.getNVIDIAPowerCTRLMaxPowerLimit();
  auto ctgpDef   = c.getNVIDIAPowerCTRLDefaultPowerLimit();
  auto ctgpOff   = c.getNVIDIAPowerOffset();

  std::puts( "\n--- NVIDIA Power Control ---" );
  printVal( "cTGP available:",      ctgpAvail );
  printVal( "Max power limit:",     ctgpMax, "W" );
  printVal( "Default power limit:", ctgpDef, "W" );
  printVal( "cTGP offset:",         ctgpOff, "W" );

  // NVIDIA OC state
  auto ocAvail = c.getNvidiaOCAvailable();
  if ( ocAvail && *ocAvail )
  {
    auto ocState = c.getNvidiaOCState( 0 );
    if ( ocState )
    {
      std::puts( "\n--- NVIDIA OC State ---" );
      QJsonDocument ocDoc = QJsonDocument::fromJson( QByteArray::fromStdString( *ocState ) );
      if ( !ocDoc.isObject() )
      {
        std::printf( "  %-24s unavailable\n", "State details:" );
      }
      else
      {
        QJsonObject root = ocDoc.object();

        printVal( "Offsets supported:", root.contains( "offsetsSupported" )
                                       ? std::optional< bool >( root["offsetsSupported"].toBool() )
                                       : std::nullopt );
        printVal( "Locked clocks:", root.contains( "lockedClocksSupported" )
                                    ? std::optional< bool >( root["lockedClocksSupported"].toBool() )
                                    : std::nullopt );

        if ( root.contains( "gpuClockRange" ) && root["gpuClockRange"].isObject() )
        {
          QJsonObject r = root["gpuClockRange"].toObject();
          std::printf( "  %-24s %d - %d MHz\n", "GPU clock range:",
                       r["min"].toInt(), r["max"].toInt() );
        }

        if ( root.contains( "powerMinW" ) || root.contains( "powerMaxW" ) )
        {
          std::printf( "  %-24s %d - %d W\n", "Power limit range:",
                       root["powerMinW"].toInt(), root["powerMaxW"].toInt() );
        }
        int defaultPower = root["powerDefaultW"].toInt( 0 );
        if ( ctgpDef )
          defaultPower = *ctgpDef;
        if ( defaultPower > 0 )
          std::printf( "  %-24s %d W\n", "Default power limit:", defaultPower );

        int minPower = root["powerMinW"].toInt( 0 );
        int maxPower = root["powerMaxW"].toInt( 0 );

        int currentPower = root["powerLimitW"].toInt( 0 );
        if ( ctgpDef && ctgpOff )
          currentPower = *ctgpDef + *ctgpOff;
        else if ( currentPower <= 0 )
          currentPower = defaultPower;

        if ( currentPower > 0 )
        {
          if ( minPower > 0 && currentPower < minPower )
            currentPower = minPower;
          if ( maxPower > 0 && currentPower > maxPower )
            currentPower = maxPower;
          std::printf( "  %-24s %d W\n", "Current power limit:", currentPower );
        }
        else
        {
          std::printf( "  %-24s n/a\n", "Current power limit:" );
        }

        if ( root.contains( "pstates" ) && root["pstates"].isArray() )
        {
          QJsonArray pstates = root["pstates"].toArray();
          if ( !pstates.isEmpty() )
          {
            QJsonObject selected;
            bool found = false;
            if ( pstate && *pstate >= 0 )
            {
              for ( const QJsonValue &v : pstates )
              {
                if ( v.isObject() && v.toObject()["pstate"].toInt( -1 ) == *pstate )
                {
                  selected = v.toObject();
                  found = true;
                  break;
                }
              }
            }
            if ( !found )
            {
              for ( const QJsonValue &v : pstates )
              {
                if ( v.isObject() )
                {
                  selected = v.toObject();
                  found = true;
                  break;
                }
              }
            }

            if ( found )
            {
              int selectedP = selected["pstate"].toInt( -1 );
              if ( selectedP >= 0 )
                std::printf( "  %-24s P%d\n", "OC profile state:", selectedP );

              if ( selected.contains( "gpu" ) && selected["gpu"].isObject() )
              {
                QJsonObject g = selected["gpu"].toObject();
                std::printf( "  %-24s %d MHz\n", "Core clock offset:", g["currentOffset"].toInt() );
                std::printf( "  %-24s %d to %d MHz\n", "Core offset range:",
                             g["minOffset"].toInt(), g["maxOffset"].toInt() );
              }

              if ( selected.contains( "vram" ) && selected["vram"].isObject() )
              {
                QJsonObject v = selected["vram"].toObject();
                std::printf( "  %-24s %d MHz\n", "Memory clock offset:", v["currentOffset"].toInt() );
                std::printf( "  %-24s %d to %d MHz\n", "Memory offset range:",
                             v["minOffset"].toInt(), v["maxOffset"].toInt() );
              }
            }
          }
        }
      }
    }
  }

  std::puts( "\n=== iGPU ===" );
  std::puts( "" );
  printVal( "Temperature:",         c.getIGpuTemperature(), "°C" );
  printVal( "Frequency:",           c.getIGpuFrequency(), "MHz" );
  printVal( "Power:",               c.getIGpuPower(), "W" );

  return 0;
}

// --- GPU OC Profiles ---

static int cmdGpuProfileList( ucc::UccdClient &c )
{
  // All GPU profiles (built-in + custom) from daemon
  auto json = c.getGpuProfilesJSON();
  if ( json )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
    if ( doc.isArray() && !doc.array().isEmpty() )
    {
      std::puts( "GPU OC profiles:" );
      for ( const QJsonValue &v : doc.array() )
      {
        if ( v.isObject() )
        {
          QJsonObject obj = v.toObject();
          bool editable = obj["editable"].toBool( false );
          std::printf( "  %-36s  %s%s\n",
                       obj["id"].toString().toStdString().c_str(),
                       obj["name"].toString().toStdString().c_str(),
                       editable ? " (custom)" : "" );
        }
      }
    }
  }

  return 0;
}

static int cmdGpuProfileGet( ucc::UccdClient &c, const char *gpuProfileId )
{
  // All GPU profiles (built-in + custom) are now on the daemon
  auto json = c.getGpuProfileJSON( gpuProfileId );
  if ( json && *json == "{}" )
    json = std::nullopt;

  if ( !json )
  {
    std::fputs( "Error: GPU OC profile not found\n", stderr );
    return 1;
  }

  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( !doc.isObject() )
  {
    std::puts( json->c_str() );
    return 0;
  }
  QJsonObject obj = doc.object();
  QString displayName = !obj["name"].toString().isEmpty() ? obj["name"].toString() : QString::fromUtf8( gpuProfileId );
  QString displayId = !obj["id"].toString().isEmpty() ? obj["id"].toString() : QString::fromUtf8( gpuProfileId );

  std::printf( "=== GPU OC Profile: %s ===\n", displayName.toStdString().c_str() );
  std::printf( "  %-24s %s\n", "ID:", displayId.toStdString().c_str() );

  // Show cTGP offset if present
  if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj["nvidiaPowerCTRLProfile"].isObject() )
  {
    int ctgp = obj["nvidiaPowerCTRLProfile"].toObject()["cTGPOffset"].toInt();
    std::printf( "  %-24s %d W\n", "cTGP offset:", ctgp );
  }

  // Show clock offsets, locked clocks, power limit from OC data
  if ( obj.contains( "grClockOffsetMHz" ) )
    std::printf( "  %-24s %d MHz\n", "Core clock offset:", obj["grClockOffsetMHz"].toInt() );
  if ( obj.contains( "memClockOffsetMHz" ) )
    std::printf( "  %-24s %d MHz\n", "Mem clock offset:", obj["memClockOffsetMHz"].toInt() );
  if ( obj.contains( "gpuLockedClockMinMHz" ) || obj.contains( "gpuLockedClockMaxMHz" ) )
    std::printf( "  %-24s %d - %d MHz\n", "GPU locked clocks:",
                 obj["gpuLockedClockMinMHz"].toInt(), obj["gpuLockedClockMaxMHz"].toInt() );
  if ( obj.contains( "vramLockedClockMinMHz" ) || obj.contains( "vramLockedClockMaxMHz" ) )
    std::printf( "  %-24s %d - %d MHz\n", "VRAM locked clocks:",
                 obj["vramLockedClockMinMHz"].toInt(), obj["vramLockedClockMaxMHz"].toInt() );
  if ( obj.contains( "powerLimitWatts" ) )
    std::printf( "  %-24s %.1f W\n", "Power limit:", obj["powerLimitWatts"].toDouble() );

  std::puts( "\nRaw JSON:" );
  printJSON( *json );

  return 0;
}

static int cmdGpuProfileSet( ucc::UccdClient &c, const char *gpuProfileId )
{
  // All GPU profiles (built-in + custom) are now on the daemon
  auto json = c.getGpuProfileJSON( gpuProfileId );
  if ( json && *json == "{}" )
    json = std::nullopt;

  if ( !json )
  {
    std::fputs( "Error: GPU OC profile not found\n", stderr );
    return 1;
  }

  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
  if ( !doc.isObject() )
  {
    std::fputs( "Error: Invalid GPU OC profile JSON\n", stderr );
    return 1;
  }
  QJsonObject obj = doc.object();

  // Apply cTGP offset if present and supported (same logic as TrayBackend)
  if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj["nvidiaPowerCTRLProfile"].isObject() )
  {
    auto ctgpSupported = c.getCTGPAdjustmentSupported();
    if ( ctgpSupported && *ctgpSupported )
    {
      int ctgpOffset = obj["nvidiaPowerCTRLProfile"].toObject()["cTGPOffset"].toInt();
      c.setNVIDIAPowerOffset( ctgpOffset );
    }
  }

  // Inject gpuProfileId into JSON before applying
  obj["gpuProfileId"] = QString::fromUtf8( gpuProfileId );
  std::string applyJson = QJsonDocument( obj ).toJson( QJsonDocument::Compact ).toStdString();

  ok( c.applyNvidiaGpuOCProfile( applyJson, 0 ) );
  return 0;
}

static int cmdGpuProfileReset( ucc::UccdClient &c )
{
  ok( c.resetNvidiaGpuOCAll( 0 ) );
  return 0;
}

static int cmdGpuOCState( ucc::UccdClient &c )
{
  auto ocAvail = c.getNvidiaOCAvailable();
  if ( !ocAvail || !*ocAvail )
  {
    std::puts( "NVIDIA GPU overclocking: not available" );
    return 0;
  }
  auto state = c.getNvidiaOCState( 0 );
  if ( !state )
  {
    std::fputs( "Error: Could not retrieve NVIDIA OC state\n", stderr );
    return 1;
  }
  std::puts( "=== NVIDIA OC State ===" );
  printJSON( *state );
  return 0;
}

// --- Auto-OC ---

static int cmdGpuAutoOC( ucc::UccdClient &c, const char *component )
{
  std::string comp = component ? component : "both";

  // Verify valid component
  if ( comp != "core" && comp != "vram" && comp != "both" )
  {
    std::fprintf( stderr, "Invalid component: %s (expected: core, vram, both)\n", comp.c_str() );
    return 1;
  }

  std::printf( "Starting auto-OC scan (%s)...\n", comp.c_str() );
  std::puts( "IMPORTANT: Keep a GPU-intensive workload running during the scan!" );
  std::puts( "  (e.g., a game, benchmark, or 'gpu_burn')" );
  std::puts( "Press Ctrl+C to cancel.\n" );

  // Subscribe to progress before starting
  std::atomic_bool done{ false };
  int exitCode = 0;

  c.subscribeAutoOCProgress( [&]( const std::string &json )
  {
    // Parse and display nicely
    auto doc = QJsonDocument::fromJson( QByteArray::fromStdString( json ) );
    if ( doc.isObject() )
    {
      auto obj = doc.object();
      std::string phase   = obj["phase"].toString().toStdString();
      std::string msg     = obj["message"].toString().toStdString();
      int         offset  = obj["currentOffset"].toInt();
      int         best    = obj["bestStable"].toInt();
      int         iter    = obj["iteration"].toInt();
      int         maxIter = obj["maxIterations"].toInt();
      int         temp    = obj["temp"].toInt();
      int         gpuClk  = obj["gpuClock"].toInt();
      int         gpuUtil = obj["gpuUtil"].toInt();

      std::printf( "\r[%s] offset=%+d best=%+d step=%d/%d temp=%d°C clk=%dMHz util=%d%% %s",
                   phase.c_str(), offset, best, iter, maxIter, temp, gpuClk, gpuUtil,
                   msg.c_str() );
      std::fflush( stdout );
    }
  } );

  c.subscribeAutoOCFinished( [&]( int coreOffset, int vramOffset, bool success, const std::string &msg )
  {
    std::printf( "\n\n" );
    if ( success )
    {
      std::puts( "=== Auto-OC Complete ===" );
      if ( coreOffset > 0 )
        std::printf( "  Core offset: +%d MHz\n", coreOffset );
      if ( vramOffset > 0 )
        std::printf( "  VRAM offset: +%d MHz\n", vramOffset );
      std::printf( "  %s\n", msg.c_str() );
      exitCode = 0;
    }
    else
    {
      std::fprintf( stderr, "Auto-OC failed: %s\n", msg.c_str() );
      exitCode = 1;
    }
    done = true;
  } );

  if ( !c.startAutoOC( comp, 0 ) )
  {
    std::fputs( "Error: Could not start auto-OC (check uccd logs)\n", stderr );
    return 1;
  }

  // Process D-Bus events until finished
  while ( !done )
  {
    QCoreApplication::processEvents( QEventLoop::WaitForMoreEvents, 200 );
  }

  return exitCode;
}

static int cmdGpuAutoOCStop( ucc::UccdClient &c )
{
  if ( c.stopAutoOC() )
    std::puts( "Auto-OC stop requested" );
  else
    std::fputs( "Error: Could not stop auto-OC\n", stderr );
  return 0;
}

static int cmdGpuAutoOCStatus( ucc::UccdClient &c )
{
  auto running = c.getAutoOCRunning();
  if ( !running.has_value() )
  {
    std::fputs( "Error: Could not query auto-OC status\n", stderr );
    return 1;
  }
  std::printf( "Auto-OC: %s\n", *running ? "running" : "idle" );
  return 0;
}

// --- ODM Performance Profile ---

static int cmdODMList( ucc::UccdClient &c )
{
  auto profiles = c.getAvailableODMProfiles();
  if ( !profiles || profiles->empty() )
  {
    std::puts( "ODM performance profiles: not available on this hardware" );
    return 0;
  }
  std::puts( "Available ODM performance profiles:" );
  for ( const auto &p : *profiles )
    std::printf( "  %s\n", p.c_str() );

  auto current = c.getODMPerformanceProfile();
  if ( current && !current->empty() )
    std::printf( "\nActive: %s\n", current->c_str() );

  return 0;
}

static int cmdODMGet( ucc::UccdClient &c )
{
  auto current = c.getODMPerformanceProfile();
  if ( !current || current->empty() )
  {
    std::puts( "ODM performance profile: not available" );
    return 0;
  }
  std::printf( "%s\n", current->c_str() );
  return 0;
}

static int cmdODMSet( ucc::UccdClient &c, const char *profile )
{
  ok( c.setODMPerformanceProfile( profile ) );
  return 0;
}

// --- State Map ---

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
  if ( obj.contains( "keyboardBacklightControlEnabled" ) )
    std::printf( "    %-24s %s\n", "Keyboard backlight:", obj["keyboardBacklightControlEnabled"].toBool() ? "enabled" : "disabled" );
  if ( obj.contains( "fahrenheit" ) )
    std::printf( "    %-24s %s\n", "Temperature unit:", obj["fahrenheit"].toBool() ? "Fahrenheit" : "Celsius" );

  // Charging
  if ( obj.contains( "chargingProfile" ) )
  {
    QString cp = obj["chargingProfile"].toString();
    if ( !cp.isEmpty() )
      std::printf( "    %-24s %s\n", "Charging profile:", cp.toStdString().c_str() );
  }
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

// --- CPU Info ---

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

// --- Power Limits ---

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

// ---------------------------------------------------------------------------
// Usage / Help
// ---------------------------------------------------------------------------

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
    "GPU info & overclocking:\n"
    "  gpu                           Show GPU sensors, NVIDIA metrics & power control\n"
    "  gpu info                      Same as 'gpu' (explicit form)\n"
    "  gpu profile list              List GPU OC profiles (built-in + custom)\n"
    "  gpu profile get <ID>          Show a GPU OC profile's data\n"
    "  gpu profile set <ID>          Apply a GPU OC profile by ID\n"
    "  gpu profile reset             Reset all GPU OC (clocks, power limits)\n"
    "  gpu oc-state                  Show current NVIDIA OC state (JSON)\n"
    "  gpu auto-oc [core|vram|both]  Auto-tune GPU/VRAM clock offset (binary search)\n"
    "  gpu auto-oc stop              Cancel a running auto-OC scan\n"
    "  gpu auto-oc status            Check if auto-OC is running\n"
    "\n"
    "ODM performance profile:\n"
    "  odm list                      List available ODM performance profiles\n"
    "  odm get                       Show current ODM performance profile\n"
    "  odm set <PROFILE>             Set ODM performance profile\n"
    "\n"
    "Keyboard backlight:\n"
    "  keyboard info                 Show keyboard backlight capabilities\n"
    "  keyboard get                  Show current per-zone backlight states\n"
    "  keyboard set <JSON>           Set per-zone backlight states (JSON array)\n"
    "  keyboard color <R> <G> <B> [BRIGHTNESS]\n"
    "                                Set uniform color (0-255 each, brightness default 128)\n"
    "  keyboard profiles             List custom keyboard profiles\n"
    "  keyboard activate <ID>        Activate a keyboard profile by ID\n"
    "\n"
    "Hardware controls:\n"
    "  brightness get                Get display brightness (0-100)\n"
    "  brightness set <VALUE>        Set display brightness (0-100)\n"
    "  webcam get                    Get webcam status\n"
    "  webcam set <on|off>           Enable/disable webcam\n"
    "  fnlock get                    Get Fn Lock status\n"
    "  fnlock set <on|off>           Enable/disable Fn Lock\n"
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
    "  charging set-profile <DESC>   Set charging profile\n"
    "  charging set-priority <DESC>  Set charging priority\n"
    "  charging set-thresholds <START> <END>\n"
    "                                Set charge start/end thresholds (%)\n"
    "\n"
    "System info:\n"
    "  cpu                           Show CPU info and capabilities\n"
    "  power-limits                  Show ODM power limits (TDP values)\n"
    "\n"
    "General:\n"
    "  --help, -h                    Show this help\n"
    "  --version, -v                 Show version\n"
    "  --json                        Force JSON output for status commands\n"
  );
}

// ---------------------------------------------------------------------------
// Argument parsing helpers
// ---------------------------------------------------------------------------

static bool matchArg( const char *arg, const char *name )
{
  return std::strcmp( arg, name ) == 0;
}

static bool parseBool( const char *s, bool &out )
{
  if ( matchArg( s, "on" ) || matchArg( s, "true" ) || matchArg( s, "1" ) || matchArg( s, "yes" ) || matchArg( s, "enable" ) || matchArg( s, "enabled" ) )
  {
    out = true;
    return true;
  }
  if ( matchArg( s, "off" ) || matchArg( s, "false" ) || matchArg( s, "0" ) || matchArg( s, "no" ) || matchArg( s, "disable" ) || matchArg( s, "disabled" ) )
  {
    out = false;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Status JSON mode
// ---------------------------------------------------------------------------

static int cmdStatusJSON( ucc::UccdClient &c )
{
  QJsonObject root;

  root["connected"] = c.isConnected();

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

  // Extended NVIDIA metrics
  auto ge1 = c.getDGpuComputeUtilPct();    if ( ge1 && *ge1 >= 0 ) gpu["computeUtilPct"]    = *ge1;
  auto ge2 = c.getDGpuMemoryUtilPct();     if ( ge2 && *ge2 >= 0 ) gpu["memoryUtilPct"]     = *ge2;
  auto ge3 = c.getDGpuVramUsedMiB();       if ( ge3 && *ge3 >= 0 ) gpu["vramUsedMiB"]       = *ge3;
  auto ge4 = c.getDGpuVramTotalMiB();      if ( ge4 && *ge4 >= 0 ) gpu["vramTotalMiB"]      = *ge4;
  auto ge5 = c.getDGpuPerfLimitReason();   if ( ge5 && !ge5->empty() ) gpu["perfLimitReason"] = QString::fromStdString( *ge5 );
  auto ge6 = c.getDGpuEncoderUtilPct();    if ( ge6 && *ge6 >= 0 ) gpu["encoderUtilPct"]    = *ge6;
  auto ge7 = c.getDGpuDecoderUtilPct();    if ( ge7 && *ge7 >= 0 ) gpu["decoderUtilPct"]    = *ge7;
  auto ge8 = c.getDGpuCurrentPstate();     if ( ge8 && *ge8 >= 0 ) gpu["currentPstate"]     = *ge8;
  auto ge9 = c.getDGpuGrClockOffsetMHz();  if ( ge9 && *ge9 != 0 ) gpu["grClockOffsetMHz"]  = *ge9;
  auto ge10 = c.getDGpuMemClockOffsetMHz(); if ( ge10 && *ge10 != 0 ) gpu["memClockOffsetMHz"] = *ge10;
  auto ge11 = c.getDGpuVramFrequencyMHz(); if ( ge11 && *ge11 >= 0 ) gpu["vramFrequencyMHz"]  = *ge11;
  auto ge12 = c.getDGpuCoreVoltageMv();    if ( ge12 && *ge12 >= 0 ) gpu["coreVoltageMv"]     = *ge12;
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
  auto w = c.getWebcamEnabled();     if ( w ) hw["webcamEnabled"]     = *w;
  auto f = c.getFnLock();            if ( f ) hw["fnLock"]            = *f;
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

  // Charging — mirror GUI logic
  auto chargingProfilesAvail = c.getChargingProfilesAvailable();
  bool hasChargingJ = false;
  if ( chargingProfilesAvail )
  {
    QJsonDocument chDoc = QJsonDocument::fromJson( QByteArray::fromStdString( *chargingProfilesAvail ) );
    hasChargingJ = chDoc.isArray() && !chDoc.array().isEmpty();
  }
  if ( hasChargingJ )
  {
    QJsonObject ch;
    auto cp = c.getCurrentChargingProfile();
    if ( cp && !cp->empty() ) ch["profile"]  = QString::fromStdString( *cp );
    auto cr = c.getCurrentChargingPriority();
    if ( cr && !cr->empty() ) ch["priority"] = QString::fromStdString( *cr );
    auto ct = c.getChargeType();
    if ( ct && *ct != "Unknown" && *ct != "N/A" && !ct->empty() )
      ch["type"] = QString::fromStdString( *ct );

    auto endAvailJ = c.getChargeEndAvailableThresholds();
    bool hasThrJ = false;
    if ( endAvailJ )
    {
      QJsonDocument td = QJsonDocument::fromJson( QByteArray::fromStdString( *endAvailJ ) );
      hasThrJ = td.isArray() && !td.array().isEmpty();
    }
    if ( hasThrJ )
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

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

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

  // --- Dispatch commands ---

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

  // keyboard ...
  if ( matchArg( cmd, "keyboard" ) || matchArg( cmd, "kb" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli keyboard <info|get|set|color|profiles|activate>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "info" ) || matchArg( sub, "caps" ) )
      return cmdKeyboardInfo( client );
    if ( matchArg( sub, "get" ) || matchArg( sub, "show" ) )
      return cmdKeyboardGet( client );
    if ( matchArg( sub, "set" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli keyboard set <JSON>\n", stderr ); return 1; }
      return cmdKeyboardSet( client, args[2] );
    }
    if ( matchArg( sub, "profiles" ) || matchArg( sub, "profile-list" ) || matchArg( sub, "ls" ) )
      return cmdKeyboardProfileList( client );
    if ( matchArg( sub, "activate" ) || matchArg( sub, "use" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli keyboard activate <PROFILE_ID>\n", stderr ); return 1; }
      return cmdKeyboardProfileSet( client, args[2] );
    }
    if ( matchArg( sub, "color" ) )
    {
      if ( args.size() < 5 )
      {
        std::fputs( "Usage: ucc-cli keyboard color <R> <G> <B> [BRIGHTNESS]\n", stderr );
        return 1;
      }
      int r = std::atoi( args[2] );
      int g = std::atoi( args[3] );
      int b = std::atoi( args[4] );
      int brightness = ( args.size() > 5 ) ? std::atoi( args[5] ) : 128;
      return cmdKeyboardColor( client, r, g, b, brightness );
    }
    std::fprintf( stderr, "Unknown keyboard subcommand: %s\n", sub );
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

  // webcam ...
  if ( matchArg( cmd, "webcam" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli webcam <get|set>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "get" ) )
      return cmdWebcamGet( client );
    if ( matchArg( sub, "set" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli webcam set <on|off>\n", stderr ); return 1; }
      bool v;
      if ( !parseBool( args[2], v ) ) { std::fputs( "Error: expected on/off\n", stderr ); return 1; }
      return cmdWebcamSet( client, v );
    }
    std::fprintf( stderr, "Unknown webcam subcommand: %s\n", sub );
    return 1;
  }

  // fnlock ...
  if ( matchArg( cmd, "fnlock" ) || matchArg( cmd, "fn-lock" ) )
  {
    if ( args.size() < 2 )
    {
      std::fputs( "Usage: ucc-cli fnlock <get|set>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "get" ) )
      return cmdFnLockGet( client );
    if ( matchArg( sub, "set" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli fnlock set <on|off>\n", stderr ); return 1; }
      bool v;
      if ( !parseBool( args[2], v ) ) { std::fputs( "Error: expected on/off\n", stderr ); return 1; }
      return cmdFnLockSet( client, v );
    }
    std::fprintf( stderr, "Unknown fnlock subcommand: %s\n", sub );
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
      std::fputs( "Usage: ucc-cli charging <status|set-profile|set-priority|set-thresholds>\n", stderr );
      return 1;
    }
    const char *sub = args[1];
    if ( matchArg( sub, "status" ) )
      return cmdChargingStatus( client );
    if ( matchArg( sub, "set-profile" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli charging set-profile <DESCRIPTOR>\n", stderr ); return 1; }
      return cmdChargingSetProfile( client, args[2] );
    }
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

  // gpu [info | profile <list|get|set|reset> | oc-state]
  if ( matchArg( cmd, "gpu" ) )
  {
    if ( args.size() < 2 )
      return cmdGpuInfo( client );  // bare "gpu" → info

    const char *sub = args[1];
    if ( matchArg( sub, "info" ) )
      return cmdGpuInfo( client );
    if ( matchArg( sub, "oc-state" ) || matchArg( sub, "ocstate" ) )
      return cmdGpuOCState( client );

    if ( matchArg( sub, "auto-oc" ) || matchArg( sub, "autooc" ) || matchArg( sub, "auto_oc" ) )
    {
      if ( args.size() >= 3 )
      {
        if ( matchArg( args[2], "stop" ) || matchArg( args[2], "cancel" ) )
          return cmdGpuAutoOCStop( client );
        if ( matchArg( args[2], "status" ) )
          return cmdGpuAutoOCStatus( client );
        // component argument
        return cmdGpuAutoOC( client, args[2] );
      }
      return cmdGpuAutoOC( client, "both" );
    }

    if ( matchArg( sub, "profile" ) || matchArg( sub, "prof" ) )
    {
      if ( args.size() < 3 )
      {
        std::fputs( "Usage: ucc-cli gpu profile <list|get|set|reset>\n", stderr );
        return 1;
      }
      const char *sub2 = args[2];
      if ( matchArg( sub2, "list" ) || matchArg( sub2, "ls" ) )
        return cmdGpuProfileList( client );
      if ( matchArg( sub2, "get" ) || matchArg( sub2, "show" ) )
      {
        if ( args.size() < 4 ) { std::fputs( "Usage: ucc-cli gpu profile get <GPU_PROFILE_ID>\n", stderr ); return 1; }
        return cmdGpuProfileGet( client, args[3] );
      }
      if ( matchArg( sub2, "set" ) || matchArg( sub2, "activate" ) || matchArg( sub2, "apply" ) )
      {
        if ( args.size() < 4 ) { std::fputs( "Usage: ucc-cli gpu profile set <GPU_PROFILE_ID>\n", stderr ); return 1; }
        return cmdGpuProfileSet( client, args[3] );
      }
      if ( matchArg( sub2, "reset" ) || matchArg( sub2, "clear" ) )
        return cmdGpuProfileReset( client );
      std::fprintf( stderr, "Unknown gpu profile subcommand: %s\n", sub2 );
      return 1;
    }
    std::fprintf( stderr, "Unknown gpu subcommand: %s\n", sub );
    return 1;
  }

  // odm [list | get | set <PROFILE>]
  if ( matchArg( cmd, "odm" ) )
  {
    if ( args.size() < 2 )
      return cmdODMList( client );  // bare "odm" → list

    const char *sub = args[1];
    if ( matchArg( sub, "list" ) || matchArg( sub, "ls" ) )
      return cmdODMList( client );
    if ( matchArg( sub, "get" ) || matchArg( sub, "show" ) )
      return cmdODMGet( client );
    if ( matchArg( sub, "set" ) )
    {
      if ( args.size() < 3 ) { std::fputs( "Usage: ucc-cli odm set <PROFILE>\n", stderr ); return 1; }
      return cmdODMSet( client, args[2] );
    }
    std::fprintf( stderr, "Unknown odm subcommand: %s\n", sub );
    return 1;
  }

  // power-limits
  if ( matchArg( cmd, "power-limits" ) )
    return cmdPowerLimits( client );

  std::fprintf( stderr, "Unknown command: %s\nTry: ucc-cli --help\n", cmd );
  return 1;
}
