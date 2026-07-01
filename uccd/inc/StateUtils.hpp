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

#include "TccSettings.hpp"
#include "PowerSupplyController.hpp"
#include "SysfsNode.hpp"
#include <filesystem>
#include <iostream>
#include <syslog.h>

inline ProfileState determineState() noexcept
{
  ProfileState state = ProfileState::AC; // Default to AC

  try
  {
    namespace fs = std::filesystem;
    const fs::path pathPowerSupplies = "/sys/class/power_supply";

    if ( !SysfsNode< std::string >::exists( pathPowerSupplies ) )
    {
      return state;
    }

    // Find a 'Mains' type power supply
    for ( const fs::path &entry : SysfsNode< std::string >::directoryEntries( pathPowerSupplies ) )
    {
      if ( SysfsNode< std::string >( entry / "type" ).read().value_or( "" ) == "Mains" )
      {
        // Found AC power supply, check if it's online
        const auto online = SysfsNode< int64_t >( entry / "online" ).read();
        if ( !online )
          continue;

        if ( *online == 1 )
        {
          state = ProfileState::AC;
        }
        else
        {
          state = ProfileState::BAT;
        }

        break; // Found what we need
      }
    }
  }
  catch ( const std::exception &e )
  {
    syslog( LOG_ERR, "[State] Exception determining power state: %s", e.what() );
  }

  return state;
}

inline std::string profileStateToString( ProfileState state ) noexcept
{
  switch ( state )
  {
    case ProfileState::AC:
      return "power_ac";
    case ProfileState::BAT:
      return "power_bat";
    case ProfileState::WC:
      return "power_wc";
    default:
      return "power_ac";
  }
}
