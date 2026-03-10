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

#include "hal/IProfileProvider.hpp"
#include "profiles/FanProfile.hpp"

#include <algorithm>
#include <syslog.h>
#include <string>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Generic (fallback) profile provider for desktops and unsupported devices.
 *
 * Always detects successfully, but at low priority (0) so that platform-specific
 * providers like UniwillProfileProvider take precedence when available.
 *
 * Builds simple default profiles at runtime that only use generic features:
 * CPU frequency/governor, fan curves, display brightness.  No ODM profiles,
 * no water cooler, no battery-specific settings.
 */
class GenericProfileProvider : public IProfileProvider
{
public:
  GenericProfileProvider() = default;

  std::string name() const override { return "GenericProfileProvider"; }
  int priority() const override { return 0; }

  bool detect() override
  {
    syslog( LOG_INFO, "[GenericProfileProvider] Generic profile provider available (fallback)" );
    return true;
  }

  std::vector< UccProfile > getDefaultProfiles() const override
  {
    return {
      buildProfile( DefaultProfileIDs::Office, "Default", "fan-platform-default", "balance_performance", false ),
    };
  }

  UccProfile getDefaultCustomProfile() const override
  {
    UccProfile profile( "__default_custom_profile__", "Defaults" );
    profile.description = "Edit profile to change behaviour";

    profile.display.brightness = 100;
    profile.display.useBrightness = false;
    profile.display.refreshRate = -1;
    profile.display.useRefRate = false;

    profile.cpu.noTurbo = false;

    profile.webcam.status = true;
    profile.webcam.useStatus = false;

    profile.fan.useControl = true;
    profile.fan.fanProfile = "fan-platform-default";
    profile.fan.sameSpeed = true;
    profile.fan.autoControlWC = false;
    profile.fan.enableWaterCooler = false;

    return profile;
  }

  std::vector< FanProfile > getDefaultFanProfiles(
    const std::vector< ucc::hal::FanZone > &zones ) const override
  {
    auto normalizeToCanonicalCurveModel = []( const std::vector< ucc::hal::FanCurvePoint > &input ) {
      std::vector< ucc::hal::FanCurvePoint > sorted = input;
      std::sort( sorted.begin(), sorted.end(),
                 []( const ucc::hal::FanCurvePoint &a, const ucc::hal::FanCurvePoint &b ) {
                   return a.temp < b.temp;
                 } );

      if ( sorted.empty() )
        return std::vector< ucc::hal::FanCurvePoint >{};

      std::vector< ucc::hal::FanCurvePoint > normalized;
      normalized.reserve( 17 );
      for ( int temp = 20; temp <= 100; temp += 5 )
      {
        normalized.emplace_back( temp, ucc::hal::interpolateCurve( sorted, temp ) );
      }
      return normalized;
    };

    FanProfile platformDefault;
    platformDefault.id = "fan-platform-default";
    platformDefault.name = "BIOS / Platform Default [Built-in]";

    for ( const auto &zone : zones )
    {
      platformDefault.zoneCurves.push_back(
        ucc::hal::FanZoneCurve( zone.id,
                                normalizeToCanonicalCurveModel( zone.curve ),
                                zone.hysteresisDeg,
                                zone.enabled,
                                zone.thermalSourceId ) );
    }

    if ( platformDefault.zoneCurves.empty() )
      return {};

    return { platformDefault };
  }

private:
  static UccProfile buildProfile( const char *id,
                                  const char *displayName,
                                  const char *fanProfileId,
                                  const char *epp,
                                  bool noTurbo )
  {
    UccProfile p( id, displayName );

    p.display.brightness = 100;
    p.display.useBrightness = false;
    p.display.refreshRate = -1;
    p.display.useRefRate = false;

    p.cpu.noTurbo = noTurbo;
    p.cpu.energyPerformancePreference = epp;

    p.webcam.status = true;
    p.webcam.useStatus = false;

    p.fan.useControl = true;
    p.fan.fanProfile = fanProfileId;
    p.fan.sameSpeed = true;
    p.fan.autoControlWC = false;
    p.fan.enableWaterCooler = false;

    // No ODM profile — generic hardware has none
    // No TDP values — will be filled by fillDeviceSpecificDefaults() if available
    // No water cooler, no charging control

    return p;
  }
};

} // namespace ucc::hal
