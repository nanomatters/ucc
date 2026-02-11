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
#include <filesystem>
#include <iostream>

inline ProfileState determineState() noexcept
{
  ProfileState state = ProfileState::AC; // Default to AC
  
  try
  {
    namespace fs = std::filesystem;
    const fs::path pathPowerSupplies = "/sys/class/power_supply";
    
    if ( !fs::exists( pathPowerSupplies ) )
    {
      return state;
    }
    
    // Find a 'Mains' type power supply
    for ( const auto &entry : fs::directory_iterator( pathPowerSupplies ) )
    {
      if ( !entry.is_directory() )
        continue;
      
      fs::path typePath = entry.path() / "type";
      if ( !fs::exists( typePath ) )
        continue;
      
      // Read type
      std::ifstream typeFile( typePath );
      if ( !typeFile )
        continue;
      
      std::string type;
      std::getline( typeFile, type );
      
      if ( type == "Mains" )
      {
        // Found AC power supply, check if it's online
        fs::path onlinePath = entry.path() / "online";
        if ( !fs::exists( onlinePath ) )
          continue;
        
        std::ifstream onlineFile( onlinePath );
        if ( !onlineFile )
          continue;
        
        std::string online;
        std::getline( onlineFile, online );
        
        if ( online == "1" )
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
