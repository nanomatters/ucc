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

#include "profiles/FanProfile.hpp"
#include <iostream>
#include <string>

using ucc::hal::FanCurvePoint;
using ucc::hal::FanZoneCurve;

// ---------------------------------------------------------------------------
//  Helper: build a FanZoneCurve from a zone ID and curve points
// ---------------------------------------------------------------------------
static FanZoneCurve makeCurve( const char *zoneId,
                               std::vector< FanCurvePoint > curve )
{
  return FanZoneCurve( zoneId, std::move( curve ) );
}

// ---------------------------------------------------------------------------
//  Built-in fan profiles
// ---------------------------------------------------------------------------
const std::vector< FanProfile > defaultFanProfiles = {

  // ---- Silent ----
  FanProfile(
    DefaultFanProfileIDs::Silent,
    "Silent [Built-in]",
    {
      makeCurve( "zone-cpu",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,0},{60,0},
          {65,20},{70,28},{75,40},{80,53},{85,65},{90,83},{95,96},{100,100} } ),
      makeCurve( "zone-gpu",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,0},{60,10},
          {65,24},{70,34},{75,46},{80,58},{85,70},{90,91},{95,95},{100,100} } ),
      makeCurve( "zone-case",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,0},{60,5},
          {65,22},{70,31},{75,43},{80,56},{85,68},{90,87},{95,96},{100,100} } ),
      makeCurve( "zone-pump",
        { {20,30},{30,30},{50,35},{65,45},{75,60},{85,80},{90,100} } ),
      makeCurve( "zone-misc",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,0},{60,5},
          {65,22},{70,31},{75,43},{80,56},{85,68},{90,87},{95,96},{100,100} } ),
      makeCurve( "wc-fan",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,0},{60,5},
          {65,22},{70,31},{75,43},{80,56},{85,68},{90,87},{95,96},{100,100} } ),
      makeCurve( "wc-pump",
        { {35,1},{50,2},{65,3},{75,4} } ),
    }
  ),

  // ---- Quiet ----
  FanProfile(
    DefaultFanProfileIDs::Quiet,
    "Quiet [Built-in]",
    {
      makeCurve( "zone-cpu",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,10},{60,20},
          {65,24},{70,33},{75,46},{80,55},{85,68},{90,85},{95,96},{100,100} } ),
      makeCurve( "zone-gpu",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,10},{60,20},
          {65,26},{70,35},{75,46},{80,55},{85,68},{90,90},{95,95},{100,100} } ),
      makeCurve( "zone-case",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,10},{60,20},
          {65,25},{70,34},{75,46},{80,55},{85,68},{90,88},{95,96},{100,100} } ),
      makeCurve( "zone-pump",
        { {20,30},{30,30},{50,35},{65,45},{75,60},{85,80},{90,100} } ),
      makeCurve( "zone-misc",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,10},{60,20},
          {65,25},{70,34},{75,46},{80,55},{85,68},{90,88},{95,96},{100,100} } ),
      makeCurve( "wc-fan",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,0},{55,10},{60,20},
          {65,25},{70,34},{75,46},{80,55},{85,68},{90,88},{95,96},{100,100} } ),
      makeCurve( "wc-pump",
        { {35,1},{50,2},{65,3},{75,4} } ),
    }
  ),

  // ---- Balanced ----
  FanProfile(
    DefaultFanProfileIDs::Balanced,
    "Balanced [Built-in]",
    {
      makeCurve( "zone-cpu",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,17},{55,25},{60,31},
          {65,38},{70,50},{75,55},{80,65},{85,78},{90,88},{95,96},{100,100} } ),
      makeCurve( "zone-gpu",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,17},{55,25},{60,31},
          {65,38},{70,50},{75,55},{80,65},{85,78},{90,90},{95,95},{100,100} } ),
      makeCurve( "zone-case",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,17},{55,25},{60,31},
          {65,38},{70,50},{75,55},{80,65},{85,78},{90,89},{95,96},{100,100} } ),
      makeCurve( "zone-pump",
        { {20,30},{30,30},{50,35},{65,50},{75,65},{85,85},{90,100} } ),
      makeCurve( "zone-misc",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,17},{55,25},{60,31},
          {65,38},{70,50},{75,55},{80,65},{85,78},{90,89},{95,96},{100,100} } ),
      makeCurve( "wc-fan",
        { {20,0},{25,0},{30,0},{35,0},{40,0},{45,0},{50,17},{55,25},{60,31},
          {65,38},{70,50},{75,55},{80,65},{85,78},{90,89},{95,96},{100,100} } ),
      makeCurve( "wc-pump",
        { {35,1},{50,2},{65,3},{75,4} } ),
    }
  ),

  // ---- Cool ----
  FanProfile(
    DefaultFanProfileIDs::Cool,
    "Cool [Built-in]",
    {
      makeCurve( "zone-cpu",
        { {20,0},{25,0},{30,0},{35,0},{40,3},{45,20},{50,25},{55,29},{60,35},
          {65,43},{70,50},{75,58},{80,72},{85,85},{90,93},{95,96},{100,100} } ),
      makeCurve( "zone-gpu",
        { {20,0},{25,0},{30,0},{35,0},{40,5},{45,26},{50,31},{55,36},{60,41},
          {65,46},{70,52},{75,62},{80,71},{85,79},{90,97},{95,100},{100,100} } ),
      makeCurve( "zone-case",
        { {20,0},{25,0},{30,0},{35,0},{40,4},{45,23},{50,28},{55,33},{60,38},
          {65,45},{70,51},{75,60},{80,72},{85,82},{90,95},{95,98},{100,100} } ),
      makeCurve( "zone-pump",
        { {20,30},{30,35},{50,45},{65,60},{75,75},{85,90},{90,100} } ),
      makeCurve( "zone-misc",
        { {20,0},{25,0},{30,0},{35,0},{40,4},{45,23},{50,28},{55,33},{60,38},
          {65,45},{70,51},{75,60},{80,72},{85,82},{90,95},{95,98},{100,100} } ),
      makeCurve( "wc-fan",
        { {20,0},{25,0},{30,0},{35,0},{40,4},{45,23},{50,28},{55,33},{60,38},
          {65,45},{70,51},{75,60},{80,72},{85,82},{90,95},{95,98},{100,100} } ),
      makeCurve( "wc-pump",
        { {35,1},{50,2},{65,3},{75,4} } ),
    }
  ),

  // ---- Freezy ----
  FanProfile(
    DefaultFanProfileIDs::Freezy,
    "Freezy [Built-in]",
    {
      makeCurve( "zone-cpu",
        { {20,20},{25,20},{30,21},{35,23},{40,26},{45,30},{50,40},{55,40},{60,45},
          {65,50},{70,55},{75,60},{80,73},{85,85},{90,91},{95,96},{100,100} } ),
      makeCurve( "zone-gpu",
        { {20,25},{25,25},{30,25},{35,25},{40,30},{45,35},{50,40},{55,45},{60,50},
          {65,60},{70,65},{75,70},{80,75},{85,85},{90,95},{95,98},{100,100} } ),
      makeCurve( "zone-case",
        { {20,23},{25,23},{30,23},{35,24},{40,28},{45,33},{50,40},{55,43},{60,48},
          {65,55},{70,60},{75,65},{80,74},{85,85},{90,93},{95,97},{100,100} } ),
      makeCurve( "zone-pump",
        { {20,35},{30,40},{50,50},{65,65},{75,80},{85,95},{90,100} } ),
      makeCurve( "zone-misc",
        { {20,23},{25,23},{30,23},{35,24},{40,28},{45,33},{50,40},{55,43},{60,48},
          {65,55},{70,60},{75,65},{80,74},{85,85},{90,93},{95,97},{100,100} } ),
      makeCurve( "wc-fan",
        { {20,23},{25,23},{30,23},{35,24},{40,28},{45,33},{50,40},{55,43},{60,48},
          {65,55},{70,60},{75,65},{80,74},{85,85},{90,93},{95,97},{100,100} } ),
      makeCurve( "wc-pump",
        { {35,1},{50,2},{65,3},{75,4} } ),
    }
  ),
};

// ---------------------------------------------------------------------------
//  Serialization — getFanProfileJson
// ---------------------------------------------------------------------------
static void appendCurveJson( std::string &json,
                             const std::vector< FanCurvePoint > &curve )
{
  json += '[';
  for ( size_t i = 0; i < curve.size(); ++i )
  {
    const auto &p = curve[i];
    json += "{\"temp\":" + std::to_string( p.temp )
          + ",\"speed\":" + std::to_string( p.speed ) + "}";
    if ( i + 1 < curve.size() )
      json += ',';
  }
  json += ']';
}

std::string getFanProfileJson( const std::string &idOrName,
                               const std::unordered_map< std::string, std::string > &hwDeviceTypes )
{
  const FanProfile *fp = nullptr;
  for ( const auto &p : defaultFanProfiles )
    if ( p.id == idOrName ) { fp = &p; break; }
  if ( !fp )
    for ( const auto &p : defaultFanProfiles )
      if ( p.name == idOrName ) { fp = &p; break; }

  if ( !fp ) return "{}";

  std::string json = "{";
  json += "\"id\":\"" + fp->id + "\",";
  json += "\"name\":\"" + fp->name + "\",";

  // Helper: derive a human-readable name from the zone ID
  auto zoneDisplayName = []( const std::string &zid ) -> std::string {
    if ( zid == WellKnownZoneIDs::CPU )    return "CPU Fans";
    if ( zid == WellKnownZoneIDs::GPU )    return "GPU Fans";
    if ( zid == WellKnownZoneIDs::Case )   return "Case Fans";
    if ( zid == WellKnownZoneIDs::Pump )   return "Pump(s)";
    if ( zid == WellKnownZoneIDs::Misc )   return "Other Fans";
    if ( zid == WellKnownZoneIDs::WCFan )  return "Water Cooler Fan";
    if ( zid == WellKnownZoneIDs::WCPump ) return "Water Cooler Pump";
    return zid;
  };

  // Helper: derive a device type string — prefer actual hardware type,
  //         fall back to well-known-ID defaults.
  auto zoneDeviceTypeStr = [&hwDeviceTypes]( const std::string &zid ) -> std::string {
    if ( auto it = hwDeviceTypes.find( zid ); it != hwDeviceTypes.end() )
      return it->second;
    if ( zid == WellKnownZoneIDs::Pump )   return "pump";
    if ( zid == WellKnownZoneIDs::WCPump ) return "stagedPump";
    return "fan";
  };

  json += "\"zones\":[";
  bool firstZone = true;
  for ( size_t z = 0; z < fp->zoneCurves.size(); ++z )
  {
    const auto &zc = fp->zoneCurves[z];

    // When hardware info is available, skip zones that have no hardware
    if ( !hwDeviceTypes.empty() && hwDeviceTypes.find( zc.zoneId ) == hwDeviceTypes.end() )
      continue;

    if ( !firstZone )
      json += ',';
    firstZone = false;

    json += "{\"id\":\"" + zc.zoneId + "\"";
    json += ",\"name\":\"" + zoneDisplayName( zc.zoneId ) + "\"";
    json += ",\"deviceType\":\"" + zoneDeviceTypeStr( zc.zoneId ) + "\"";
    json += ",\"hysteresisDeg\":" + std::to_string( zc.hysteresisDeg );
    json += ",\"enabled\":" + std::string( zc.enabled ? "true" : "false" );
    if ( !zc.thermalSourceId.empty() )
      json += ",\"thermalSourceId\":\"" + zc.thermalSourceId + "\"";
    json += ",\"curve\":";
    appendCurveJson( json, zc.curve );
    json += "}";
  }
  json += "]";

  json += "}";
  return json;
}

// ---------------------------------------------------------------------------
//  Lookup helpers
// ---------------------------------------------------------------------------
FanProfile getDefaultFanProfile( const std::string &idOrName )
{
  for ( const auto &p : defaultFanProfiles )
    if ( p.id == idOrName ) return p;
  for ( const auto &p : defaultFanProfiles )
    if ( p.name == idOrName ) return p;
  for ( const auto &p : defaultFanProfiles )
    if ( p.id == DefaultFanProfileIDs::Balanced ) return p;
  if ( !defaultFanProfiles.empty() ) return defaultFanProfiles[0];

  return FanProfile( DefaultFanProfileIDs::Balanced, "Balanced" );
}

bool setFanProfileJson( const std::string &idOrName, [[maybe_unused]] const std::string &json )
{
  std::cerr << "[FanProfiles] setFanProfileJson called for '" << idOrName << "' - operation not supported\n";
  return false;
}
