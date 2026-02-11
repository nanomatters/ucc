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
#include <cmath>
#include <sstream>
#include <string>
#include <iostream>

// helper function to compute speed at an arbitrary temp by linear interpolation between key points
int32_t computeSpeedAtTemp(const std::vector<std::pair<int32_t,int32_t>>& keyPoints, int32_t temp)
{
  if ( keyPoints.empty() )
    return 0;
  else if ( temp <= keyPoints.front().first )
    return keyPoints.front().second;
  else if ( temp >= keyPoints.back().first )
    return keyPoints.back().second;

  // find enclosing segment
  for ( size_t i = 0; i + 1 < keyPoints.size(); ++i )
  {
    const int32_t t1 = keyPoints[ i ].first;
    const int32_t t2 = keyPoints[ i + 1 ].first;
    const int32_t s1 = keyPoints[ i ].second;
    const int32_t s2 = keyPoints[ i + 1 ].second;

    if ( temp == t1 )
        return s1;
    else if ( t1 < temp )
        continue;

    else if ( temp <= t2 )
    {
      if ( t2 == t1 )
        return s1;

      const float frac = static_cast< float >( temp - t1 ) / static_cast< float >( t2 - t1 );
      return static_cast< int32_t >( std::lround( static_cast< float >( s1 ) + frac * static_cast< float >( s2 - s1 ) ) );
    }
  }

  return keyPoints.back().second;
}

const std::vector< FanProfile > defaultFanProfiles = {
  // Silent profile
  FanProfile(
    "Silent",
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 0 }, { 55, 0 }, { 60, 0 }, { 65, 20 }, { 70, 28 }, { 75, 40 },
      { 80, 53 }, { 85, 65 }, { 90, 83 }, { 95, 96 }, { 100, 100 }
    },
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 0 }, { 55, 0 }, { 60, 10 }, { 65, 24 }, { 70, 34 }, { 75, 46 },
      { 80, 58 }, { 85, 70 }, { 90, 91 }, { 95, 95 }, { 100, 100 }
    },
    {
      { 35, 1 }, { 50, 2 }, { 65, 3 }, { 75, 4 }
    },
    // Water cooler fan: average of CPU and GPU
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 0 }, { 55, 0 }, { 60, 5 }, { 65, 22 }, { 70, 31 }, { 75, 43 },
      { 80, 56 }, { 85, 68 }, { 90, 87 }, { 95, 96 }, { 100, 100 }
    }
  ),

  // Quiet profile
  FanProfile(
    "Quiet",
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 0 }, { 55, 10 }, { 60, 20 }, { 65, 24 }, { 70, 33 }, { 75, 46 },
      { 80, 55 }, { 85, 68 }, { 90, 85 }, { 95, 96 }, { 100, 100 }
    },
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 0 }, { 55, 10 }, { 60, 20 }, { 65, 26 }, { 70, 35 }, { 75, 46 },
      { 80, 55 }, { 85, 68 }, { 90, 90 }, { 95, 95 }, { 100, 100 }
    },
    {
      { 35, 1 }, { 50, 2 }, { 65, 3 }, { 75, 4 }
    },
    // Water cooler fan: average of CPU and GPU
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 0 }, { 55, 10 }, { 60, 20 }, { 65, 25 }, { 70, 34 }, { 75, 46 },
      { 80, 55 }, { 85, 68 }, { 90, 88 }, { 95, 96 }, { 100, 100 }
    }
  ),

  // Balanced profile
  FanProfile(
    "Balanced",
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 17 }, { 55, 25 }, { 60, 31 }, { 65, 38 }, { 70, 50 }, { 75, 55 },
      { 80, 65 }, { 85, 78 }, { 90, 88 }, { 95, 96 }, { 100, 100 }
    },
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 17 }, { 55, 25 }, { 60, 31 }, { 65, 38 }, { 70, 50 }, { 75, 55 },
      { 80, 65 }, { 85, 78 }, { 90, 90 }, { 95, 95 }, { 100, 100 }
    },
    {
      { 35, 1 }, { 50, 2 }, { 65, 3 }, { 75, 4 }
    },
    // Water cooler fan: average of CPU and GPU
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 0 }, { 45, 0 },
      { 50, 17 }, { 55, 25 }, { 60, 31 }, { 65, 38 }, { 70, 50 }, { 75, 55 },
      { 80, 65 }, { 85, 78 }, { 90, 89 }, { 95, 96 }, { 100, 100 }
    }
  ),

  // Cool profile
  FanProfile(
    "Cool",
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 3 }, { 45, 20 },
      { 50, 25 }, { 55, 29 }, { 60, 35 }, { 65, 43 }, { 70, 50 }, { 75, 58 },
      { 80, 72 }, { 85, 85 }, { 90, 93 }, { 95, 96 }, { 100, 100 }
    },
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 5 }, { 45, 26 },
      { 50, 31 }, { 55, 36 }, { 60, 41 }, { 65, 46 }, { 70, 52 }, { 75, 62 },
      { 80, 71 }, { 85, 79 }, { 90, 97 }, { 95, 100 }, { 100, 100 }
    },
    {
      { 35, 1 }, { 50, 2 }, { 65, 3 }, { 75, 4 }
    },
    // Water cooler fan: average of CPU and GPU
    {
      { 20, 0 }, { 25, 0 }, { 30, 0 }, { 35, 0 }, { 40, 4 }, { 45, 23 },
      { 50, 28 }, { 55, 33 }, { 60, 38 }, { 65, 45 }, { 70, 51 }, { 75, 60 },
      { 80, 72 }, { 85, 82 }, { 90, 95 }, { 95, 98 }, { 100, 100 }
    }
  ),

  // Freezy profile
  FanProfile(
    "Freezy",
    {
      { 20, 20 }, { 25, 20 }, { 30, 21 }, { 35, 23 }, { 40, 26 }, { 45, 30 },
      { 50, 40 }, { 55, 40 }, { 60, 45 }, { 65, 50 }, { 70, 55 }, { 75, 60 },
      { 80, 73 }, { 85, 85 }, { 90, 91 }, { 95, 96 }, { 100, 100 }
    },
    {
      { 20, 25 }, { 25, 25 }, { 30, 25 }, { 35, 25 }, { 40, 30 }, { 45, 35 },
      { 50, 40 }, { 55, 45 }, { 60, 50 }, { 65, 60 }, { 70, 65 }, { 75, 70 },
      { 80, 75 }, { 85, 85 }, { 90, 95 }, { 95, 98 }, { 100, 100 }
    },
    {
      { 35, 1 }, { 50, 2 }, { 65, 3 }, { 75, 4 }
    },
    // Water cooler fan: average of CPU and GPU
    {
      { 20, 23 }, { 25, 23 }, { 30, 23 }, { 35, 24 }, { 40, 28 }, { 45, 33 },
      { 50, 40 }, { 55, 43 }, { 60, 48 }, { 65, 55 }, { 70, 60 }, { 75, 65 },
      { 80, 74 }, { 85, 85 }, { 90, 93 }, { 95, 97 }, { 100, 100 }
    }
  )
};


std::string getFanProfileJson(const std::string &name)
{
  const FanProfile *fp = nullptr;
  for (const auto &p : defaultFanProfiles) {
    if (p.name == name) {
      fp = &p;
      break;
    }
  }

  if (!fp) return "{}";

  std::string json = "{";
  
  // tableCPU - return the sampled entries (already at 5°C steps from 20 to 100)
  json += "\"tableCPU\":[";
  for ( size_t i = 0; i < fp->tableCPU.size(); ++i )
  {
    const auto &entry = fp->tableCPU[i];
    json += "{\"temp\":" + std::to_string(entry.temp) + ",\"speed\":" + std::to_string(entry.speed) + "}";
    if ( i + 1 < fp->tableCPU.size() ) json += ",";
  }
  json += "],";
  
  // tableGPU - return the sampled entries (already at 5°C steps from 20 to 100)
  json += "\"tableGPU\":[";
  for ( size_t i = 0; i < fp->tableGPU.size(); ++i )
  {
    const auto &entry = fp->tableGPU[i];
    json += "{\"temp\":" + std::to_string(entry.temp) + ",\"speed\":" + std::to_string(entry.speed) + "}";
    if ( i + 1 < fp->tableGPU.size() ) json += ",";
  }
  json += "],";
  
  // tablePump - return the entries
  json += "\"tablePump\":[";
  for ( size_t i = 0; i < fp->tablePump.size(); ++i )
  {
    const auto &entry = fp->tablePump[i];
    json += "{\"temp\":" + std::to_string(entry.temp) + ",\"speed\":" + std::to_string(entry.speed) + "}";
    if ( i + 1 < fp->tablePump.size() ) json += ",";
  }
  json += "],";

  // tableWaterCoolerFan - return the entries
  json += "\"tableWaterCoolerFan\":[";
  for ( size_t i = 0; i < fp->tableWaterCoolerFan.size(); ++i )
  {
    const auto &entry = fp->tableWaterCoolerFan[i];
    json += "{\"temp\":" + std::to_string(entry.temp) + ",\"speed\":" + std::to_string(entry.speed) + "}";
    if ( i + 1 < fp->tableWaterCoolerFan.size() ) json += ",";
  }
  json += "]";
  
  json += "}";
  return json;
}

FanProfile getDefaultFanProfileByName( const std::string &name )
{
  for (const auto &p : defaultFanProfiles) {
    if (p.name == name) return p;
  }
  // Fallback to Balanced if not found
  for (const auto &p : defaultFanProfiles) {
    if (p.name == "Balanced") return p;
  }
  // If Balanced missing, return first element
  if (!defaultFanProfiles.empty()) return defaultFanProfiles[0];

  return FanProfile("Balanced");
}

// Setting built-in fan profiles is not supported. This function exists to satisfy the DBus
// SetFanProfile call but will refuse to overwrite built-ins. If callers need to store
// custom fan profiles they should use the UCC settings (ProfileManager) instead.
bool setFanProfileJson(const std::string &name, [[maybe_unused]] const std::string &json)
{
  std::cerr << "[FanProfiles] setFanProfileJson called for '" << name << "' - operation not supported\n";
  return false;
}
