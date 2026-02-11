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

#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>

#include "profiles/UccProfile.hpp"

std::string generateProfileId()
{
  // generate random component using base36 encoding
  std::random_device rd;
  std::mt19937 gen( rd() );
  std::uniform_int_distribution< uint32_t > dist( 0, 36 * 36 * 36 * 36 * 36 - 1 );

  uint32_t randomValue = dist( gen );

  // convert to base36
  static constexpr const char *base36 = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::string randomPart;
  
  for ( int i = 0; i < 5; ++i )
  {
    randomPart = base36[randomValue % 36] + randomPart;
    randomValue /= 36;
  }

  // generate timestamp component in base36
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast< std::chrono::milliseconds >( 
    now.time_since_epoch() 
  ).count();

  std::string timestampPart;
  while ( timestamp > 0 )
  {
    timestampPart = base36[timestamp % 36] + timestampPart;
    timestamp /= 36;
  }

  return randomPart + timestampPart;
}

const std::map< std::string, std::string > profileImageMap =
{
  { LegacyDefaultProfileIDs::Default, "icon_profile_performance.svg" },
  { LegacyDefaultProfileIDs::CoolAndBreezy, "icon_profile_breezy.svg" },
  { LegacyDefaultProfileIDs::PowersaveExtreme, "icon_profile_energysaver.svg" },
  { "custom", "icon_profile_custom.svg" },
  { DefaultProfileIDs::MaxEnergySave, "icon_profile_energysaver.svg" },
  { DefaultProfileIDs::Quiet, "icon_profile_quiet4.svg" },
  { DefaultProfileIDs::Office, "icon_profile_default.svg" },
  { DefaultProfileIDs::HighPerformance, "icon_profile_performance.svg" },
};
