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

#include "UccProfile.hpp"
#include <string>
#include <vector>
#include <map>

// default custom profile IDs
inline constexpr const char *defaultCustomProfileID = "__default_custom_profile__";
inline constexpr const char *defaultMobileCustomProfileID = "__default_mobile_custom_profile__";

/**
 * @brief TUXEDO device types
 *
 * Enumeration of supported TUXEDO devices for device-specific profile handling
 */
enum class UniwillDeviceID
{
  IBP17G6,
  PULSE1403,
  PULSE1404,
  IBP14G6_TUX,
  IBP14G6_TRX,
  IBP14G6_TQF,
  IBP14G7_AQF_ARX,
  IBPG8,
  IBPG10AMD,
  PULSE1502,
  AURA14G3,
  AURA15G3,
  POLARIS1XA02,
  POLARIS1XI02,
  POLARIS1XA03,
  POLARIS1XI03,
  POLARIS1XA05,
  STELLARIS1XA03,
  STELLARIS1XI03,
  STELLARIS1XI04,
  STEPOL1XA04,
  STELLARIS1XI05,
  STELLARIS1XA05,
  STELLARIS16I06,
  STELLARIS17I06,
  STELLSL15A06,
  STELLSL15I06,
  STELLARIS16A07,
  STELLARIS16I07,
  SIRIUS1601,
  SIRIUS1602,
  
  // XMG models (via SchenkerTechnologiesGmbH vendor string)
  XNE16E25,                   // XMG NEO 16" Intel (Gen7, 2025)
  XNE16A25,                   // XMG NEO 16" AMD (Gen7, 2025)
};

// pre-defined profiles
extern const UccProfile maxEnergySave;
extern const UccProfile silent;
extern const UccProfile office;
extern const UccProfile highPerformance;
extern const UccProfile highPerformance25WcTGP;

// legacy profiles (for unknown devices)
extern const UccProfile legacyDefault;
extern const UccProfile legacyCoolAndBreezy;
extern const UccProfile legacyPowersaveExtreme;
extern const std::vector< UccProfile > legacyProfiles;

// default custom profiles
extern const UccProfile defaultCustomProfile;
extern const UccProfile defaultMobileCustomProfileTDP;
extern const UccProfile defaultMobileCustomProfileCl;
extern const UccProfile defaultCustomProfile25WcTGP;

// device-specific profile mappings
extern const std::map< UniwillDeviceID, std::vector< UccProfile > > deviceProfiles;
extern const std::map< UniwillDeviceID, std::vector< UccProfile > > deviceCustomProfiles;

