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

#include "platform/uniwill/UniwillDefaultProfiles.hpp"

#include <map>

namespace ucc::hal
{

namespace
{

UccProfile makeMaxEnergySave()
{
  UccProfile profile( DefaultProfileIDs::MaxEnergySave, DefaultProfileIDs::MaxEnergySave );
  profile.display.brightness = 40;
  profile.display.useBrightness = true;
  profile.display.refreshRate = 60;
  profile.display.useRefRate = false;
  profile.display.xResolution = 1920;
  profile.display.yResolution = 1080;
  profile.display.useResolution = false;
  profile.cpu.noTurbo = false;
  profile.webcam.status = true;
  profile.webcam.useStatus = true;
  profile.fan.useControl = true;
  profile.fan.fanProfile = DefaultFanProfileIDs::Silent;
  profile.odmProfile.name = "power_save";
  profile.odmPowerLimits.tdpValues = { 5, 10, 15 };
  return profile;
}

UccProfile makeSilent()
{
  UccProfile profile( DefaultProfileIDs::Quiet, DefaultProfileIDs::Quiet );
  profile.display.brightness = 50;
  profile.display.useBrightness = true;
  profile.display.refreshRate = -1;
  profile.display.useRefRate = false;
  profile.cpu.noTurbo = false;
  profile.webcam.status = true;
  profile.webcam.useStatus = true;
  profile.fan.useControl = true;
  profile.fan.fanProfile = DefaultFanProfileIDs::Silent;
  profile.fan.autoControlWC = true;
  profile.odmProfile.name = "power_save";
  profile.odmPowerLimits.tdpValues = { 10, 15, 25 };
  return profile;
}

UccProfile makeOffice()
{
  UccProfile profile( DefaultProfileIDs::Office, DefaultProfileIDs::Office );
  profile.display.brightness = 60;
  profile.display.useBrightness = true;
  profile.display.refreshRate = -1;
  profile.display.useRefRate = false;
  profile.cpu.noTurbo = false;
  profile.webcam.status = true;
  profile.webcam.useStatus = true;
  profile.fan.useControl = true;
  profile.fan.fanProfile = DefaultFanProfileIDs::Quiet;
  profile.fan.autoControlWC = true;
  profile.odmProfile.name = "enthusiast";
  profile.odmPowerLimits.tdpValues = { 25, 35, 35 };
  return profile;
}

UccProfile makeHighPerformance()
{
  UccProfile profile( DefaultProfileIDs::HighPerformance, DefaultProfileIDs::HighPerformance );
  profile.display.brightness = 60;
  profile.display.useBrightness = true;
  profile.display.refreshRate = -1;
  profile.display.useRefRate = false;
  profile.cpu.noTurbo = false;
  profile.webcam.status = true;
  profile.webcam.useStatus = true;
  profile.fan.useControl = true;
  profile.fan.fanProfile = DefaultFanProfileIDs::Balanced;
  profile.fan.autoControlWC = true;
  profile.odmProfile.name = "overboost";
  profile.odmPowerLimits.tdpValues = { 60, 60, 70 };
  return profile;
}

UccProfile makeHighPerformance25WcTGP()
{
  UccProfile profile = makeHighPerformance();
  return profile;
}

UccProfile makeDefaultCustomProfile()
{
  UccProfile profile( defaultCustomProfileID, "TUXEDO Defaults" );
  profile.description = "Edit profile to change behaviour";
  profile.display.brightness = 100;
  profile.display.useBrightness = false;
  profile.display.refreshRate = -1;
  profile.display.useRefRate = false;
  profile.cpu.noTurbo = false;
  profile.webcam.status = true;
  profile.webcam.useStatus = true;
  profile.fan.useControl = true;
  profile.fan.fanProfile = DefaultFanProfileIDs::Balanced;
  profile.fan.autoControlWC = true;
  profile.odmPowerLimits.tdpValues = { 60, 60, 70 };
  return profile;
}

UccProfile makeDefaultMobileCustomProfileTDP()
{
  UccProfile profile( defaultMobileCustomProfileID, "TUXEDO Mobile Default" );
  profile.description = "Edit profile to change behaviour";
  profile.display.brightness = 100;
  profile.display.useBrightness = false;
  profile.display.refreshRate = -1;
  profile.display.useRefRate = false;
  profile.cpu.scalingMaxFrequency = 3500000;
  profile.cpu.noTurbo = false;
  profile.webcam.status = true;
  profile.webcam.useStatus = true;
  profile.fan.useControl = true;
  profile.fan.fanProfile = DefaultFanProfileIDs::Balanced;
  profile.fan.autoControlWC = true;
  profile.odmPowerLimits.tdpValues = { 15, 25, 50 };
  return profile;
}

UccProfile makeDefaultMobileCustomProfileCl()
{
  UccProfile profile = makeDefaultMobileCustomProfileTDP();
  return profile;
}

UccProfile makeDefaultCustomProfile25WcTGP()
{
  UccProfile profile( defaultCustomProfileID, "TUXEDO Defaults" );
  profile.description = "Edit profile to change behaviour";
  profile.display.brightness = 100;
  profile.display.useBrightness = false;
  profile.display.refreshRate = -1;
  profile.display.useRefRate = false;
  profile.cpu.noTurbo = false;
  profile.webcam.status = true;
  profile.webcam.useStatus = true;
  profile.fan.useControl = true;
  profile.fan.fanProfile = DefaultFanProfileIDs::Balanced;
  profile.fan.autoControlWC = true;
  profile.odmPowerLimits.tdpValues = { 15, 25, 50 };
  return profile;
}

} // namespace

std::vector< UccProfile > getUniwillFallbackDefaultProfiles()
{
  return { makeMaxEnergySave(), makeSilent(), makeOffice(), makeHighPerformance() };
}

std::vector< UccProfile > getUniwillDefaultProfiles( UniwillDeviceID deviceId )
{
  const UccProfile maxEnergySave = makeMaxEnergySave();
  const UccProfile silent = makeSilent();
  const UccProfile office = makeOffice();
  const UccProfile highPerformance = makeHighPerformance();
  const UccProfile highPerformance25WcTGP = makeHighPerformance25WcTGP();

  const std::map< UniwillDeviceID, std::vector< UccProfile > > deviceProfiles =
  {
    { UniwillDeviceID::IBP14G6_TUX, { maxEnergySave, silent, office } },
    { UniwillDeviceID::IBP14G6_TRX, { maxEnergySave, silent, office } },
    { UniwillDeviceID::IBP14G6_TQF, { maxEnergySave, silent, office } },
    { UniwillDeviceID::IBP14G7_AQF_ARX, { maxEnergySave, silent, office } },
    { UniwillDeviceID::IBPG8, { maxEnergySave, silent, office } },
    { UniwillDeviceID::PULSE1502, { maxEnergySave, silent, office } },
    { UniwillDeviceID::POLARIS1XI02, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::POLARIS1XI03, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::POLARIS1XA02, { maxEnergySave, silent, office, highPerformance25WcTGP } },
    { UniwillDeviceID::POLARIS1XA03, { maxEnergySave, silent, office, highPerformance25WcTGP } },
    { UniwillDeviceID::POLARIS1XA05, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS1XI03, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS1XI04, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS1XI05, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS1XA03, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::STEPOL1XA04, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS1XA05, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS16I06, { silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS17I06, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::XNE16E25, { silent, office, highPerformance } },
    { UniwillDeviceID::XNE16A25, { maxEnergySave, silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS16I07, { silent, office, highPerformance } },
    { UniwillDeviceID::STELLARIS16A07, { maxEnergySave, silent, office, highPerformance } },
  };

  if ( auto it = deviceProfiles.find( deviceId ); it != deviceProfiles.end() )
    return it->second;

  return getUniwillFallbackDefaultProfiles();
}

UccProfile getUniwillDefaultCustomProfile( UniwillDeviceID deviceId )
{
  const UccProfile defaultCustomProfile = makeDefaultCustomProfile();
  const UccProfile defaultMobileCustomProfileTDP = makeDefaultMobileCustomProfileTDP();
  const UccProfile defaultMobileCustomProfileCl = makeDefaultMobileCustomProfileCl();
  const UccProfile defaultCustomProfile25WcTGP = makeDefaultCustomProfile25WcTGP();

  const std::map< UniwillDeviceID, std::vector< UccProfile > > deviceCustomProfiles =
  {
    { UniwillDeviceID::IBPG8, { defaultCustomProfile, defaultMobileCustomProfileTDP } },
    { UniwillDeviceID::AURA14G3, { defaultCustomProfile, defaultMobileCustomProfileCl } },
    { UniwillDeviceID::AURA15G3, { defaultCustomProfile, defaultMobileCustomProfileCl } },
    { UniwillDeviceID::POLARIS1XA02, { defaultCustomProfile25WcTGP } },
    { UniwillDeviceID::POLARIS1XA03, { defaultCustomProfile25WcTGP } },
    { UniwillDeviceID::STELLARIS1XA03, { defaultCustomProfile25WcTGP } },
  };

  if ( auto it = deviceCustomProfiles.find( deviceId ); it != deviceCustomProfiles.end() && !it->second.empty() )
    return it->second.front();

  return defaultCustomProfile;
}

UccProfile getUniwillDefaultCustomProfile()
{
  return makeDefaultCustomProfile();
}

std::vector< FanProfile > getUniwillDefaultFanProfiles()
{
  return defaultFanProfiles;
}

} // namespace ucc::hal
